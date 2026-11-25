// tfu: last-chance crash reporter (Windows).
// The SDK's own vectored handler runs first and handles the normal faults (MMIO, write watches).
// Anything it passes on reaches this handler, which writes the host call stack, symbolized from
// tfu.pdb, to the log AND to logs/crash_<time>.txt (the async log may not flush before exit).
// Recompiled functions call each other as plain C++ calls, so the host stack reads as the guest
// call chain: sub_82XXXXXX frames, plus any of our hooks (BsTickP2, REX_FUNC wrappers) involved.
// Only crash-type codes are reported, at most 3 per run; behaviour is unchanged (continue search).
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS   // fopen/localtime are fine here; silences the build-log noise
#include <windows.h>
#include <dbghelp.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <rex/logging.h>
#include <csignal>
#include <cstdlib>
#include <exception>

namespace {
std::atomic<int> g_crash_reports{0};

std::string Frame(HANDLE proc, DWORD64 pc) {
  alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
  auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = 255;
  DWORD64 disp = 0;
  if (SymFromAddr(proc, pc, &disp, si)) return fmt::format("{}+0x{:X}", si->Name, disp);
  HMODULE mod = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &mod) && mod) {
    GetModuleFileNameA(mod, name, MAX_PATH);
    const char* b = std::strrchr(name, '\\');
    return fmt::format("{}+0x{:X}", b ? b + 1 : name, pc - reinterpret_cast<DWORD64>(mod));
  }
  return fmt::format("{:016X}", pc);
}

LONG CALLBACK CrashVeh(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
      code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW &&
      code != EXCEPTION_PRIV_INSTRUCTION)
    return EXCEPTION_CONTINUE_SEARCH;
  if (g_crash_reports.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;

  HANDLE proc = GetCurrentProcess(), th = GetCurrentThread();
  CONTEXT c = *ep->ContextRecord;
  STACKFRAME64 sf{};
  sf.AddrPC.Offset = c.Rip;    sf.AddrPC.Mode = AddrModeFlat;
  sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
  std::string out = "\n    " + Frame(proc, c.Rip) + "   <- faulting instruction";
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &c, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset)
      break;
    if (i == 0) continue;  // first walked frame is the faulting one, already printed
    out += "\n    " + Frame(proc, sf.AddrPC.Offset);
  }
  const ULONG_PTR* info = ep->ExceptionRecord->ExceptionInformation;
  std::string head = fmt::format("CRASH code {:08X} {} address {:016X} (thread {}):", code,
                                 code == EXCEPTION_ACCESS_VIOLATION ? (info[0] ? "writing" : "reading") : "at",
                                 code == EXCEPTION_ACCESS_VIOLATION ? info[1] : c.Rip, GetCurrentThreadId());
  REXLOG_ERROR("tfu: {}{}", head, out);
  char path[64];
  std::time_t t = std::time(nullptr);
  std::strftime(path, sizeof(path), "logs\\crash_%Y%m%d_%H%M%S.txt", std::localtime(&t));
  if (FILE* f = std::fopen(path, "a")) { std::fprintf(f, "%s%s\n\n", head.c_str(), out.c_str()); std::fclose(f); }
  return EXCEPTION_CONTINUE_SEARCH;
}
// 11-08: the game also died with no fault at all (abort / terminate / exit). Record those too, with the
// calling thread's stack, straight to logs\exit_<time>.txt (the async log loses its last lines on exit).
void WriteExitReport(const char* why) {
  HANDLE proc = GetCurrentProcess(), th = GetCurrentThread();
  CONTEXT c{}; RtlCaptureContext(&c);
  STACKFRAME64 sf{};
  sf.AddrPC.Offset = c.Rip; sf.AddrPC.Mode = AddrModeFlat; sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
  std::string out;
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &c, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset) break;
    out += "\n    " + Frame(proc, sf.AddrPC.Offset);
  }
  char path[64]; std::time_t t = std::time(nullptr);
  std::strftime(path, sizeof(path), "logs\\exit_%Y%m%d_%H%M%S.txt", std::localtime(&t));
  if (FILE* f = std::fopen(path, "a")) { std::fprintf(f, "EXIT via %s (thread %lu):%s\n\n", why, GetCurrentThreadId(), out.c_str()); std::fclose(f); }
}
void OnAbortSignal(int) { WriteExitReport("abort() / SIGABRT"); }
void OnTerminate() { WriteExitReport("std::terminate (uncaught C++ exception?)"); std::abort(); }
void OnAtExit() { WriteExitReport("exit() / normal process exit"); }
}  // namespace

// ---- hang watchdog + kill switch (ported from BS2, 11-08) ---------------------------------------------------
// BsHeartbeat() from the game-thread tick (BsRunGives). A watcher thread: if the game thread is silent for 15 s
// (long enough that normal level loads do not trip it), samples its host stack 3x 700 ms apart into logs/hang_<time>.txt
// (symbolized like crashes: sub_82xxxxxx frames = guest call chain). At most 2 dumps per hang-free stretch.
// "kill=1" in run\knobs.txt TerminateProcess()es the game from the watcher - works while the game thread is
// frozen, so an unattended test loop never needs someone at the PC. Remove the line before the next launch.
namespace {
std::atomic<HANDLE> g_hb_thread{nullptr};
std::atomic<long long> g_hb_ms{0};
std::atomic<bool> g_hb_armed{false};   // armed once the player is captured (in a level): the front end's attract-mode
                                       // movie blocks the game thread ~15 s by design (false HANG 11-08 14:51)
long long NowMs() { return (long long)GetTickCount64(); }
std::string WalkThread(HANDLE th) {
  HANDLE proc = GetCurrentProcess();
  CONTEXT c{}; c.ContextFlags = CONTEXT_FULL;
  if (SuspendThread(th) == (DWORD)-1) return " (suspend failed)";
  std::string out;
  if (GetThreadContext(th, &c)) {
    STACKFRAME64 sf{};
    sf.AddrPC.Offset = c.Rip; sf.AddrPC.Mode = AddrModeFlat; sf.AddrFrame.Offset = c.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
    sf.AddrStack.Offset = c.Rsp; sf.AddrStack.Mode = AddrModeFlat;
    out += "\n    " + Frame(proc, c.Rip) + "   <- pc";
    for (int i = 0; i < 40; ++i) {
      if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &c, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || !sf.AddrPC.Offset) break;
      if (i == 0) continue;
      out += "\n    " + Frame(proc, sf.AddrPC.Offset);
    }
  } else out = " (no context)";
  ResumeThread(th);
  return out;
}
void CheckKill() {
  if (FILE* kf = std::fopen("run\\knobs.txt", "r")) { char ln[128]; bool kill = false;
    while (std::fgets(ln, sizeof ln, kf)) if (std::strncmp(ln, "kill=1", 6) == 0) kill = true;
    std::fclose(kf);
    if (kill) { REXLOG_ERROR("tfu: kill=1 in knobs.txt - terminating"); TerminateProcess(GetCurrentProcess(), 3); } }
}
DWORD WINAPI HangWatch(void*) {
  int dumps = 0;
  for (;;) {
    Sleep(500);
    CheckKill();
    HANDLE th = g_hb_thread.load(); long long hb = g_hb_ms.load();
    if (!th || !hb || !g_hb_armed.load() || NowMs() - hb < 15000) continue;
    if (dumps >= 2) { Sleep(5000); continue; }
    ++dumps;
    char path[64]; std::time_t t = std::time(nullptr);
    std::strftime(path, sizeof(path), "logs\\hang_%Y%m%d_%H%M%S.txt", std::localtime(&t));
    std::string all = fmt::format("HANG: game thread silent for {} ms\n", NowMs() - hb);
    for (int k = 0; k < 3; ++k) { all += fmt::format("sample {}:{}\n", k, WalkThread(th)); Sleep(700); }
    REXLOG_ERROR("tfu: {}", all);
    if (FILE* f = std::fopen(path, "a")) { std::fprintf(f, "%s\n", all.c_str()); std::fclose(f); }
    while (g_hb_ms.load() == hb) { Sleep(1000); CheckKill(); }   // wait until it ticks again before re-arming
    dumps = 0;
  }
}
}  // namespace
void BsHangArm(bool on) { g_hb_armed = on; }
void BsHeartbeat() {
  if (!g_hb_thread.load()) {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, THREAD_ALL_ACCESS, FALSE, 0);
    g_hb_thread = h;
    CreateThread(nullptr, 0, HangWatch, nullptr, 0, nullptr);
    REXLOG_INFO("tfu: hang watchdog on (15 s, logs\\hang_*.txt); kill=1 in run\\knobs.txt closes the game");
  }
  g_hb_ms = NowMs();
}

void BsInstallCrashTrace() {
  SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
  SymInitialize(GetCurrentProcess(), nullptr, TRUE);
  AddVectoredExceptionHandler(0, CrashVeh);  // 0 = after the SDK's handler
  std::signal(SIGABRT, OnAbortSignal);
  std::set_terminate(OnTerminate);
  std::atexit(OnAtExit);
  REXLOG_INFO("tfu: crash reporter installed (logs\\crash_*.txt)");
}
#else
void BsInstallCrashTrace() {}
void BsHeartbeat() {}
void BsHangArm(bool) {}
#endif
