// tfu: run Lua in the game's own Lua 5.1 VM (dev menu console + remote file console).
//
// Addresses (default.xex sha1 9BE35A81..., found 2026-11-17 from luaB_pcall / luaB_loadstring in the base lib table):
//   lua_pcall(L, nargs, nresults, errfunc)      sub_824DC5E8   (calls luaD_pcall 0x824D99D8 with f_call 0x824D6E60)
//   luaL_loadbuffer(L, buf, size, chunkname)    sub_824F0910   (reader getS 0x824CD3B0 -> luaD_protectedparser 0x824F0598)
//   lua_tolstring(L, idx, len*)                 sub_824D4D88
//   lua_State: +8 top, +12 base, +16 l_G
// Safe point: we hook lua_pcall. Every time the game is about to run a Lua callback (game thread, VM consistent) we
// first run anything queued, with the original lua_pcall, on the same L. Our chunks run protected; the stack top is
// restored afterwards, so the game's own call proceeds untouched.
// Sources of commands:
//   - dev menu (TfuLuaQueue)
//   - run\lua_in.txt: whole file is one chunk; it is deleted after it runs; results go to logs\lua_out.txt
//     (lets Claude drive experiments from outside while you play).
// A chunk starting with '=' is an expression: its tostring() result is shown (e.g. "=GetHealth(GetPlayer())").
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#endif
#include <map>
#include <random>
#include <cmath>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

extern "C" void __imp__sub_824DC5E8(PPCContext& __restrict ctx, uint8_t* base);   // lua_pcall
extern "C" void __imp__sub_824F0910(PPCContext& __restrict ctx, uint8_t* base);   // luaL_loadbuffer
extern "C" void __imp__sub_824D4D88(PPCContext& __restrict ctx, uint8_t* base);   // lua_tolstring
extern "C" void __imp__sub_824CCBF8(PPCContext& __restrict ctx, uint8_t* base);   // lua_settop
extern "C" void __imp__sub_827AF6A8(PPCContext& __restrict ctx, uint8_t* base);   // Lua arg -> actor handle (r4 = stack index)
extern "C" void __imp__sub_822872B0(PPCContext& __restrict ctx, uint8_t* base);   // actor handle -> character object (0 if none)
extern "C" void __imp__sub_82278018(PPCContext& __restrict ctx, uint8_t* base);
// Pack system (traced 2026-11-18; PackSys = *(0x82FC9B84)). Used by @loadpack, see LoadPack().
extern "C" void __imp__sub_821DC388(PPCContext& __restrict ctx, uint8_t* base);   // (PackSys, char* pack, 0): mount sub-pack, tables only -> 1
extern "C" void __imp__sub_821D3FA0(PPCContext& __restrict ctx, uint8_t* base);   // (char* pack) -> loaded sub-pack object or 0
extern "C" void __imp__sub_821D4020(PPCContext& __restrict ctx, uint8_t* base);   // (sub): read data block into PackSys+15128 arena
extern "C" void __imp__sub_821D40C8(PPCContext& __restrict ctx, uint8_t* base);   // (sub): read data block into level heap PackSys+180/184
extern "C" void __imp__sub_821DCF00(PPCContext& __restrict ctx, uint8_t* base);   // (PackSys, sub+16, 1): fix up + link assets
extern "C" void __imp__sub_821D7378(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8227F250(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82498640(PPCContext& __restrict ctx, uint8_t* base);   // (char* bank/pack name): load costume audio bank   // (PlayerMgr, idx) -> player idx actor handle or 0   // (char* asset) -> loaded asset record or 0   // spawner handle -> spawner object (0 if none)

namespace {
struct Job { std::string code; bool from_file; int target = -1; std::vector<uint32_t> done; int passes = 0; };   // target: -1 any L, -2 every L, k = k-th L seen
std::vector<uint32_t> g_states;   // distinct lua_States seen in lua_pcall, in order (game thread only)
std::mutex g_mu;
std::deque<Job> g_jobs;
std::string g_last = "Lua: idle (no command run yet)";
std::atomic<uint32_t> g_L{0};
std::atomic<bool> g_have_jobs{false};

uint32_t Rd32(uint8_t* base, uint32_t a) { uint32_t v; std::memcpy(&v, base + a, 4); return _byteswap_ulong(v); }
void Wr32(uint8_t* base, uint32_t a, uint32_t v) { v = _byteswap_ulong(v); std::memcpy(base + a, &v, 4); }
std::string RdStr(uint8_t* base, uint32_t a, size_t max = 200000) {
  if (!a) return "";
  std::string s; for (size_t i = 0; i < max; ++i) { char c = char(base[a + i]); if (!c) break; s += c; } return s;
}

float RdF(uint8_t* base, uint32_t a) { uint32_t v = Rd32(base, a); float f; std::memcpy(&f, &v, 4); return f; }
void WrF(uint8_t* base, uint32_t a, float f) { uint32_t v; std::memcpy(&v, &f, 4); Wr32(base, a, v); }

// Player body capsule. Read off GetCharacterCapsuleRadiusAndHeight (0x827BD018): actor handle h -> entry
// *(0x82F99EB0) + (h & 0xFFFF) * 64, entry+8 == h, character object = entry+56; +156 radius, +152 height (floats).
// scale 0 = just report. Originals are remembered per object so repeated presses do not compound.
std::string Capsule(uint8_t* base, uint32_t h, double scale) {
  static std::vector<std::pair<uint32_t, float>> orig;
  // the getter (0x827AF6A8) switches on lua_type and returns 0 for nil/other types - no lua_error, so it is safe here
  if (!h) return "error: no player right now (menu or loading screen?)";
  uint32_t tbl = Rd32(base, 0x82F99EB0u);
  if (!tbl) return "error: actor table not set up";
  uint32_t e = tbl + ((h & 0xFFFFu) << 6);
  if (Rd32(base, e + 8) != h) return fmt::format("error: stale actor handle {:08X}", h);
  uint32_t obj = Rd32(base, e + 56);
  if (!obj) return "error: player has no character controller";
  float r = RdF(base, obj + 156), ht = RdF(base, obj + 152);
  float r0 = r;
  bool found = false;
  for (auto& [o, v] : orig) if (o == obj) { r0 = v; found = true; }
  if (!found) orig.push_back({obj, r});
  if (scale > 0.0) { WrF(base, obj + 156, float(r0 * scale)); r = RdF(base, obj + 156); }
  return fmt::format("body capsule: radius {:.3f} (original {:.3f}, {:.0f}%), height {:.3f}  [obj {:08X}]",
                     r, r0, r0 > 0 ? 100.0 * r / r0 : 0.0, ht, obj);
}

// ---- persistent cheats (QoL 2) ---------------------------------------------------------------------------------
// run\tfu_settings.toml, flat 'key = value'. A tick (game thread, inside the lua_pcall hook) keeps them applied:
//   god            SetInvincible(0,true) every tick (idempotent; survives death/checkpoint without knowing about them)
//   infinite_force energy float = max, native: obj = sub_822872B0(handle); +808 energy, +812 max (read off GetEnergy
//                  0x825C20A8 -> 0x82801510 'lfs f1,4(r3)' on obj+804, GetEnergyMax -> 'lfs f1,8(r3)'). The game's
//                  ABILITYFUNC_INFINITEENERGY ran fine but did nothing (2026-11-17 20:22, Force still drained).
//   timescale      SetTimeScale(x) when the player handle changes (level load / respawn) - not every tick, so the
//                  game's own slow-mo moments are not fought
//   body_scale     capsule radius scale, re-applied when the player handle changes
struct Settings { bool god = false, inf_force = false; double timescale = 1.0, body = 1.0, spawn_range = 1.0; int spawn_mult = 1; };
std::mutex g_set_mu;
Settings g_set;
std::atomic<bool> g_set_loaded{false};
const char* kSettingsPath = "run/tfu_settings.toml";
uint32_t g_last_player = 0;          // game thread only
std::atomic<bool> g_need_ts{false};  // apply timescale on the next tick (set by menu or handle change)

void LoadSettings() {
  Settings s;
  std::ifstream f(kSettingsPath);
  std::string line;
  while (std::getline(f, line)) {
    auto eq = line.find('=');
    if (eq == std::string::npos || line[0] == '#' || line[0] == '[') continue;
    auto trim = [](std::string x) { while (!x.empty() && isspace((unsigned char)x.back())) x.pop_back();
                                     while (!x.empty() && isspace((unsigned char)x[0])) x.erase(0, 1); return x; };
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    if (k == "god") s.god = v == "true";
    else if (k == "infinite_force") s.inf_force = v == "true";
    else if (k == "timescale") s.timescale = std::atof(v.c_str());
    else if (k == "body_scale") s.body = std::atof(v.c_str());
    else if (k == "spawn_multiplier") s.spawn_mult = std::clamp(std::atoi(v.c_str()), 1, 99);   // 99 = endless
    else if (k == "spawn_range") s.spawn_range = std::clamp(std::atof(v.c_str()), 1.0, 20.0);
  }
  if (s.timescale <= 0.0) s.timescale = 1.0;
  if (s.body <= 0.0) s.body = 1.0;
  { std::lock_guard<std::mutex> l(g_set_mu); g_set = s; }
  g_set_loaded = true;
  REXLOG_INFO("tfu cheats: settings god={} infinite_force={} timescale={} body_scale={}", s.god, s.inf_force, s.timescale, s.body);
}
void SaveSettings(const Settings& s) {
  std::error_code ec; std::filesystem::create_directories("run", ec);
  std::ofstream f(kSettingsPath, std::ios::trunc);
  f << "# tfu cheats - written by the dev menu, re-applied on every level load / respawn\n"
    << "god = " << (s.god ? "true" : "false") << "\n"
    << "infinite_force = " << (s.inf_force ? "true" : "false") << "\n"
    << "timescale = " << s.timescale << "\n"
    << "body_scale = " << s.body << "\n"
    << "spawn_multiplier = " << s.spawn_mult << "\n"
    << "spawn_range = " << s.spawn_range << "\n";
}
Settings GetSettings() { std::lock_guard<std::mutex> l(g_set_mu); return g_set; }
bool AnyCheat(const Settings& s) { return s.god || s.inf_force || s.timescale != 1.0 || s.body != 1.0 || s.spawn_mult > 1 || s.spawn_range != 1.0; }

void SetLast(const std::string& s) { std::lock_guard<std::mutex> l(g_mu); g_last = s; }
void Out(const std::string& line) {
  std::error_code ec; std::filesystem::create_directories("logs", ec);
  std::ofstream("logs/lua_out.txt", std::ios::app) << line << "\n";
}

// Runs one chunk on L with a scratch area carved below the caller's guest stack pointer.
// Native debug jobs for the live loop (no Lua):
//   @peek <hexaddr> <nwords>         dump big-endian words
//   @poke <hexaddr> <hexvalue>       write one word
//   @spawnobj <hexhandle>            spawner handle (tostring of GetSpawner's userdata) -> object via sub_82278018,
//                                    dumps object +0..+0x70 and its definition (*(obj+20)) +0x60..+0xD0.
//   ResetSpawner -> sub_82276CD8(obj): clears +40/+44/+56/+60/+96, +64 = 4, +48 = *(def+112), +52 = byte def+205.
// strtoul-based: never throws (std::stoul on bad console input would throw on the game thread)
bool Hex(const std::string& t, uint32_t& out) {
  if (t.empty()) return false;
  char* e = nullptr; unsigned long v = std::strtoul(t.c_str(), &e, 16);
  if (!e || *e) return false;
  out = uint32_t(v); return true;
}
bool Readable(uint8_t* base, uint32_t a, uint32_t n);   // below: VirtualQuery-checked guest range
}  // namespace
void TfuLuaQueue(const std::string& code);
namespace {
std::string NativeDebug(PPCContext& ctx, uint8_t* base, const std::string& cmd) {
  auto words = [&](uint32_t a, int n) {
    std::string o;
    for (int i = 0; i < n; ++i) { if (i % 8 == 0) o += fmt::format("\n  +{:03X} {:08X}:", i * 4, a + i * 4); o += fmt::format(" {:08X}", Rd32(base, a + i * 4)); }
    return o;
  };
  auto ok_addr = [](uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u; };
  std::istringstream in(cmd);
  std::string op; in >> op;
  if (op == "@peek") {
    std::string a; int n = 16; in >> a >> n;
    uint32_t addr = 0;
    if (!Hex(a, addr)) return "error: @peek <hexaddr> [n]";
    n = std::clamp(n, 1, 128);
    if (!Readable(base, addr, uint32_t(n * 4))) return fmt::format("error: {:08X} not mapped", addr);
    return fmt::format("peek {:08X}:", addr) + words(addr, std::clamp(n, 1, 128));
  }
  if (op == "@poke") {
    std::string a, v; in >> a >> v;
    uint32_t addr = 0, val = 0;
    if (!Hex(a, addr) || !Hex(v, val)) return "error: @poke <hexaddr> <hexvalue>";
    if (!Readable(base, addr, 4)) return fmt::format("error: {:08X} not mapped", addr);
    uint32_t old = Rd32(base, addr); Wr32(base, addr, val);
    return fmt::format("poke {:08X}: {:08X} -> {:08X}", addr, old, val);
  }
  if (op == "@spawnobj") {
    std::string a; in >> a;
    uint32_t h = 0;
    if (!Hex(a, h)) return "error: @spawnobj <hexhandle>";
    PPCContext c = ctx;
    c.r1.u32 = (ctx.r1.u32 - 0x400) & ~0xFu;
    c.r3.u64 = h;
    __imp__sub_82278018(c, base);
    uint32_t obj = c.r3.u32;
    if (!obj || !Readable(base, obj, 0x70)) return fmt::format("spawner {:08X}: no object ({:08X})", h, obj);
    uint32_t def = Rd32(base, obj + 20);
    std::string o = fmt::format("spawner {:08X} obj {:08X} def {:08X}", h, obj, def) + words(obj, 28);
    if (Readable(base, def + 0x60, 0x70)) o += fmt::format("\n def+60:") + words(def + 0x60, 28);
    return o;
  }
  // @scan <text>          every committed guest page is searched for the ASCII text (case-sensitive); up to 40 hits
  // @scanptr <hex> [hex]  aligned big-endian words equal to value (or within [value, value2]); up to 40 hits
  // Pages are found with VirtualQuery on the host mapping, so uncommitted guest memory is never touched.
  if (op == "@scan" || op == "@scanptr") {
    std::string arg, arg2;
    std::vector<uint8_t> needle;
    uint32_t lo = 0, hi = 0;
    if (op == "@scan") {
      std::getline(in, arg);
      while (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
      if (arg.empty()) return "error: @scan needs text";
      needle.assign(arg.begin(), arg.end());
    } else {
      in >> arg >> arg2;
      if (!Hex(arg, lo)) return "error: @scanptr <hex> [hex]";
      hi = lo;
      if (!arg2.empty() && !Hex(arg2, hi)) return "error: @scanptr <hex> [hex]";
    }
    std::vector<uint32_t> hits;
    uint64_t scanned = 0;
    for (uint64_t ga = 0x00010000ull; ga < 0xFFFF0000ull && hits.size() < 40;) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(base + ga, &mbi, sizeof(mbi))) break;
      uint64_t rbase = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base);
      uint64_t rend = rbase + mbi.RegionSize;
      bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                      (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
      if (readable && rend > ga) {
        uint64_t a0 = std::max(ga, rbase), a1 = std::min<uint64_t>(rend, 0xFFFF0000ull);
        const uint8_t* p0 = base + a0;
        size_t len = size_t(a1 - a0);
        scanned += len;
        if (op == "@scan") {
          const uint8_t* q = p0;
          const uint8_t* e = p0 + len;
          while (hits.size() < 40) {
            auto it = std::search(q, e, needle.begin(), needle.end());
            if (it == e) break;
            hits.push_back(uint32_t(a0 + (it - p0)));
            q = it + 1;
          }
        } else {
          for (size_t i = (4 - (a0 & 3)) & 3; i + 4 <= len && hits.size() < 40; i += 4) {
            uint32_t v; std::memcpy(&v, p0 + i, 4); v = _byteswap_ulong(v);
            if (v >= lo && v <= hi) hits.push_back(uint32_t(a0 + i));
          }
        }
      }
      ga = std::max(rend, ga + 0x1000);
    }
    std::string o = fmt::format("{} {} {}: {} hit(s) in {:.0f} MB", op, arg, arg2, hits.size(), scanned / 1048576.0);
    for (uint32_t h : hits) {
      o += fmt::format("\n  {:08X}:", h);
      uint32_t a = h & ~3u;
      for (int k = -2; k < 6; ++k) o += fmt::format(" {:08X}", Rd32(base, a + 4 * k));
      if (op == "@scan") o += "  \"" + RdStr(base, h, 48) + "\"";
    }
    return o;
  }
  return "error: unknown native job " + op;
}

// Spawner activation distances (2026-11-17 22:5x, @spawnobj): spawner object = sub_82278018(handle), def = *(obj+20),
// def+0xA0 / def+0xA4 = floats 180/200 on the Wookiee spawner (30/50 on another) - spawns only start when the player
// is that close. Scale them by mult from remembered originals (a def is recognised as already scaled when it holds
// orig*mult; anything else is treated as a new original, so re-used addresses after a level load are safe).
std::string ApplySpawnRange(PPCContext& ctx, uint8_t* base, const std::string& handles, double mult) {
  struct Range { float o0, o1, last0; };
  static std::map<uint32_t, Range> orig;
  auto ok_addr = [](uint32_t a) { return a >= 0x40000000u && a < 0xA0000000u; };
  std::istringstream in(handles);
  std::string hx;
  int spawners = 0, changed = 0;
  float s0 = 0, s1 = 0;
  while (in >> hx) {
    uint32_t h = 0;
    if (!Hex(hx, h)) continue;
    // sub_82278018 looks the handle up and returns 0 unless the entry type is 3 (spawner), so any actor handle
    // is safe to pass; dormant spawners (the ones range is meant to wake) only appear this way.
    PPCContext c = ctx;
    c.r1.u32 = (ctx.r1.u32 - 0x2400) & ~0xFu;   // below RunChunk's 0x2000 scratch area
    c.r3.u64 = h;
    __imp__sub_82278018(c, base);
    uint32_t obj = c.r3.u32;
    if (!obj || !ok_addr(obj)) continue;
    uint32_t def = Rd32(base, obj + 20);
    if (!Readable(base, obj, 0x18) || !Readable(base, def, 0xB0)) continue;
    ++spawners;
    float a = RdF(base, def + 0xA0), b = RdF(base, def + 0xA4);
    if (!(a > 0.0f && a < 1.0e6f && b > 0.0f && b < 1.0e6f)) continue;
    auto it = orig.find(def);
    bool known = it != orig.end() && (std::fabs(a - it->second.last0) < 0.01f || std::fabs(a - it->second.o0) < 0.01f);
    if (!known) { orig[def] = {a, b, a}; it = orig.find(def); }
    float na = float(it->second.o0 * mult), nb = float(it->second.o1 * mult);
    if (std::fabs(a - na) > 0.01f || std::fabs(b - nb) > 0.01f) { WrF(base, def + 0xA0, na); WrF(base, def + 0xA4, nb); ++changed; s0 = na; s1 = nb; }
    it->second.last0 = na;
  }
  if (changed) REXLOG_INFO("tfu cheats: spawn range x{}: {} spawners seen, {} rescaled (e.g. {:.0f}/{:.0f})", mult, spawners, changed, s0, s1);
  return fmt::format("spawn range x{}: {} spawners, {} rescaled", mult, spawners, changed);
}

// ---- play any character in any level (2026-11-17 23:1x, proven in Kashyyyk_Prologue) -----------------------------
// The story campaign (Scum/runtime/prefs/TheForceUnleashed.scumcampaign.xml, resident from gp_x360.lp) holds one
// CampaignLevelState per level, 0x40 apart: +0 mName char*, +4/+5/+6 flags, +8 mLevelCostume char*
// ('playerDarthVader.actor.xml' for the prologue, 'player.actor.xml' elsewhere), +0xC mNameNextLevel char*.
// The level loads the template named by +8, moveset included. Repointing +8 before a level load = play as that
// character. Session-only: originals are remembered and 'restore' puts them back; nothing is saved.
template <typename F>
void ForEachGuestPage(uint8_t* base, F&& f) {   // f(guest_addr, host_ptr, len) for every committed readable range
  for (uint64_t ga = 0x00010000ull; ga < 0xFFFF0000ull;) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + ga, &mbi, sizeof(mbi))) break;
    uint64_t rbase = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base), rend = rbase + mbi.RegionSize;
    bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
                    (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
    if (readable && rend > ga) {
      uint64_t a0 = std::max(ga, rbase), a1 = std::min<uint64_t>(rend, 0xA0000000ull);   // skip physical mirrors
      if (a1 > a0 && !f(uint32_t(a0), base + a0, size_t(a1 - a0))) return;
    }
    if (rend >= 0xA0000000ull) return;
    ga = std::max(rend, ga + 0x1000);
  }
}
// True if guest [a, a+n) is committed, readable host memory. Every speculative read of a pointer we did not get from
// the game itself goes through this: guest address space is reserved but not all committed, and touching an
// uncommitted page is a host access violation = crash.
bool Readable(uint8_t* base, uint32_t a, uint32_t n) {
  // >= 0x10000: our own SystemHeapAlloc strings live in the 4 KB heap below 0x40000000
  if (a < 0x00010000u || uint64_t(a) + n >= 0xA0000000ull) return false;
  for (uint64_t p = a & ~0xFFFull; p < uint64_t(a) + n; p += 0x1000) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base + p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
      return false;
    uint64_t rend = uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize;
    if (rend >= uint64_t(a) + n) return true;
    p = rend - 0x1000;   // continue from the next region
  }
  return true;
}
bool IsText(uint8_t* base, uint32_t a, size_t minlen, size_t maxlen) {
  if (!Readable(base, a, uint32_t(maxlen + 1))) return false;
  size_t n = 0;
  for (; n <= maxlen; ++n) { uint8_t c = base[a + n]; if (!c) break; if (c < 32 || c > 126) return false; }
  return n >= minlen && n <= maxlen;
}
bool EndsWith(const std::string& s, const std::string& e) { return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0; }
bool IsLevelEntry(uint8_t* base, uint32_t e) {
  if (!Readable(base, e, 0x40)) return false;
  uint32_t n = Rd32(base, e), c = Rd32(base, e + 8);
  return IsText(base, n, 3, 64) && IsText(base, c, 8, 80) && EndsWith(RdStr(base, c, 80), ".actor.xml");
}
struct LevelTable { std::vector<uint32_t> entries; std::map<uint32_t, uint32_t> orig; std::map<std::string, uint32_t> strs; };
LevelTable g_lt;   // game thread only

bool FindLevelTable(uint8_t* base) {
  if (!g_lt.entries.empty() && IsLevelEntry(base, g_lt.entries.front())) return true;
  g_lt.entries.clear();   // keep orig (true originals) and strs (allocations) across re-scans
  // 1) every "Kashyyyk_Prologue\0" that starts a string
  static const char kName[] = "Kashyyyk_Prologue";
  std::vector<uint32_t> names;
  ForEachGuestPage(base, [&](uint32_t ga, const uint8_t* p, size_t len) {
    const uint8_t* q = p; const uint8_t* e = p + len;
    while (names.size() < 64) {
      auto it = std::search(q, e, kName, kName + sizeof(kName));   // includes the terminator
      if (it == e) break;
      uint32_t a = ga + uint32_t(it - p);
      if (it == p || it[-1] == 0) names.push_back(a);
      q = it + 1;
    }
    return names.size() < 64;
  });
  // 2) a word pointing at one of them whose struct is a level entry
  uint32_t entry = 0;
  ForEachGuestPage(base, [&](uint32_t ga, const uint8_t* p, size_t len) {
    for (size_t i = (4 - (ga & 3)) & 3; i + 4 <= len; i += 4) {
      uint32_t v; std::memcpy(&v, p + i, 4); v = _byteswap_ulong(v);
      if (std::find(names.begin(), names.end(), v) != names.end() && IsLevelEntry(base, ga + uint32_t(i))) { entry = ga + uint32_t(i); return false; }
    }
    return true;
  });
  if (!entry) return false;
  uint32_t first = entry;
  while (IsLevelEntry(base, first - 0x40)) first -= 0x40;
  for (uint32_t e = first; IsLevelEntry(base, e) && g_lt.entries.size() < 64; e += 0x40) {
    g_lt.entries.push_back(e);
    if (!g_lt.orig.count(e)) g_lt.orig[e] = Rd32(base, e + 8);   // never overwrite a true original with our pointer
  }
  REXLOG_INFO("tfu levels: campaign table {:08X}, {} levels (prologue entry {:08X})", first, g_lt.entries.size(), entry);
  return true;
}
// Exact casing of a costume as registered in ExternalPacks\List.txt ("playerDarthVader.actor.xml costume ...").
std::string CostumeFileName(const std::string& lower) {
  std::ifstream f("game_all/ExternalPacks/List.txt");
  std::string line;
  while (std::getline(f, line)) {
    std::string tok = line.substr(0, line.find(' '));
    std::string l = tok; for (auto& ch : l) ch = char(tolower((unsigned char)ch));
    if (l == lower + ".actor.xml") return tok;
  }
  return lower + ".actor.xml";
}
uint32_t GuestString(uint8_t* base, const std::string& s) {
  auto it = g_lt.strs.find(s);
  if (it != g_lt.strs.end()) return it->second;
  auto* rt = rex::Runtime::instance();
  if (!rt || !rt->memory()) return 0;
  uint32_t a = rt->memory()->SystemHeapAlloc(uint32_t(s.size() + 1 + 15) & ~15u);
  if (!a) return 0;
  std::memcpy(base + a, s.c_str(), s.size() + 1);
  g_lt.strs[s] = a;   // never freed: the campaign table may point at it for the rest of the session
  return a;
}
std::string LevelCostume(uint8_t* base, std::string arg) {
  while (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
  if (arg.empty()) return "error: @levelcostume <template>|restore|list";
  if (!FindLevelTable(base)) return "error: campaign level table not found (story campaign not loaded?)";
  if (arg == "list") {
    std::string o = fmt::format("{} levels:", g_lt.entries.size());
    for (uint32_t e : g_lt.entries) o += fmt::format("\n  {:08X} {} -> {}", e, RdStr(base, Rd32(base, e), 64), RdStr(base, Rd32(base, e + 8), 80));
    return o;
  }
  if (arg == "restore") {
    // only entries that are still level entries of the CURRENT table (a moved table must not get stale writes)
    for (auto& [e, c] : g_lt.orig)
      if (std::find(g_lt.entries.begin(), g_lt.entries.end(), e) != g_lt.entries.end() && IsLevelEntry(base, e)) Wr32(base, e + 8, c);
    REXLOG_INFO("tfu levels: original level costumes restored ({} levels)", g_lt.orig.size());
    return fmt::format("restored {} levels", g_lt.orig.size());
  }
  std::string file = CostumeFileName(arg);
  uint32_t str = GuestString(base, file);
  if (!str) return "error: could not allocate guest string";
  for (uint32_t e : g_lt.entries) Wr32(base, e + 8, str);
  REXLOG_INFO("tfu levels: every story level now loads {} ({} levels, string {:08X})", file, g_lt.entries.size(), str);
  return fmt::format("{} levels now load {} - takes effect on the next level load", g_lt.entries.size(), file);
}

// Mid-level costume pack load = what RoninLoadWorld (sub_82206528, non-reload path) does for each name on the
// pending costume list 0x82FD44CC: sub_821DC388 mount -> sub_821D3FA0 find -> sub_821D4020 + sub_821D40C8 read data
// -> sub_821DCF00 fix up. Pack name = List.txt name ('playerEmperorPalpatine.actor.xml'); file <name>_x360.lp.
// Risk: sub_821D40C8 silently skips if the level heap (PackSys+180/184) is too small; sub_821D4020 errors if the
// +15128 arena is full. Both are logged before/after. Audio banks are NOT loaded (separate list 0x82FD44D8).
// Data reads (disassembly 2026-11-18):
//   sub_821D4020(sub): n = sub+3280; arena = PackSys+15128 (+4 base, +12 capacity, +16 used); if used+n > capacity it
//     calls sub_827186E0 (error path) - so we pre-check and never call it without room. Success: sub+3344 = data.
//   sub_821D40C8(sub): n = align16(sub+3264) from the level heap (PackSys+180 cursor, +184 bytes left); SILENTLY skips
//     if the cursor is 0 or n > left - linking a pack without its data = garbage pointers. Success: sub+3352 = data.
// We check both budgets after the mount (sizes come from the pack header read by the mount) and refuse to read,
// link or swap unless everything fits. A pack mounted but not completed is remembered and never used again.
std::vector<std::string> g_bad_packs;   // game thread only

bool LoadPack(PPCContext& ctx, uint8_t* base, std::string pack, std::string& o) {
  while (!pack.empty() && pack[0] == ' ') pack.erase(0, 1);
  if (pack.empty()) { o = "error: @loadpack <template or pack name>"; return false; }
  if (pack.find(".actor.xml") == std::string::npos) pack = CostumeFileName(pack);
  if (std::find(g_bad_packs.begin(), g_bad_packs.end(), pack) != g_bad_packs.end()) {
    o = "error: " + pack + " was mounted without its data earlier this session - not safe, restart the game";
    return false;
  }
  const uint32_t ps = Rd32(base, 0x82FC9B84u);
  if (!Readable(base, ps, 15128 + 32) || !Readable(base, ps + 16720, 4)) { o = fmt::format("error: PackSys {:08X} not readable", ps); return false; }
  const uint32_t name = GuestString(base, pack);
  if (!name) { o = "error: could not allocate guest string"; return false; }
  const uint32_t ar = ps + 15128;
  auto budget = [&] {
    return fmt::format("arena {:X}/{:X} (base {:08X}), level heap left {:X} (cursor {:08X})",
                       Rd32(base, ar + 16), Rd32(base, ar + 12), Rd32(base, ar + 4), Rd32(base, ps + 184), Rd32(base, ps + 180));
  };
  o = fmt::format("loadpack {}  before: {}", pack, budget());
  PPCContext c = ctx;
  const uint32_t sp = (ctx.r1.u32 - 0x400) & ~0xFu;
  auto call = [&](auto fn, uint32_t a3, uint32_t a4 = 0, uint32_t a5 = 0) {
    c = ctx; c.r1.u32 = sp; c.r3.u64 = a3; c.r4.u64 = a4; c.r5.u64 = a5;
    fn(c, base);
    return c.r3.u32;
  };
  if (uint32_t sub0 = call(__imp__sub_821D3FA0, name)) {   // mounted by the game itself = complete
    o += fmt::format("\n  already loaded (sub {:08X})", sub0);
    return true;
  }
  const uint32_t ok = call(__imp__sub_821DC388, ps, name, 0);
  const uint32_t sub = call(__imp__sub_821D3FA0, name);
  o += fmt::format("\n  mount -> {} sub {:08X}", ok, sub);
  if (!(ok & 0xFF) || !sub || !Readable(base, sub, 3392)) { o += "\n  FAILED to mount (name not in ExternalPacks\\List.txt?)"; return false; }
  const uint32_t na = Rd32(base, sub + 3280), nh = (Rd32(base, sub + 3264) + 15) & ~15u;
  const bool arena_ok = na == 0 || (Rd32(base, ar + 4) && uint64_t(Rd32(base, ar + 16)) + na <= Rd32(base, ar + 12));
  const bool heap_ok = nh == 0 || (Rd32(base, ps + 180) && nh <= Rd32(base, ps + 184));
  o += fmt::format("\n  needs arena {:X} ({}), level heap {:X} ({})", na, arena_ok ? "fits" : "NO ROOM", nh, heap_ok ? "fits" : "NO ROOM");
  // Mid-level both pools are normally exhausted (2026-11-18 test: arena 5.6 MB free of the 11.3 MB Starkiller needs,
  // level heap closed: cursor 0). Then the reads are pointed at OUR memory: each read takes its destination from a
  // bump cursor (arena +4 / level heap +180), so for exactly one call we aim the cursor at our buffer and put the
  // game's values back right after. Game thread, synchronous: nothing else allocates in between. The arena data
  // holds textures the GPU reads -> physical heap, 4 KB aligned. Buffers are never freed (live for the session).
  auto* rt = rex::Runtime::instance();
  auto* mem = rt ? rt->memory() : nullptr;
  bool own_a = false, own_h = false;
  if (na) {
    if (arena_ok) call(__imp__sub_821D4020, sub);
    else {
      uint32_t pad = Rd32(base, ar + 20);   // per-allocation padding the game adds after each block
      if (pad > 0x100000u) { o += fmt::format("\n  arena pad {:X} looks wrong - not touching it", pad); g_bad_packs.push_back(pack); return false; }
      const uint32_t buf = mem ? mem->SystemHeapAlloc(na + pad + 0x1000, 0x1000, rex::memory::kSystemHeapPhysical) : 0;
      if (!buf) { o += "\n  could not allocate a physical buffer for the arena data"; g_bad_packs.push_back(pack); return false; }
      const uint32_t c4 = Rd32(base, ar + 4), c12 = Rd32(base, ar + 12), c16 = Rd32(base, ar + 16);
      Wr32(base, ar + 4, buf); Wr32(base, ar + 16, 0); Wr32(base, ar + 12, na + pad + 0x1000);
      call(__imp__sub_821D4020, sub);
      Wr32(base, ar + 4, c4); Wr32(base, ar + 12, c12); Wr32(base, ar + 16, c16);
      own_a = true;
      o += fmt::format("\n  arena data -> our physical buffer {:08X} ({:X} bytes)", buf, na);
    }
  }
  if (nh) {
    if (heap_ok) call(__imp__sub_821D40C8, sub);
    else {
      const uint32_t buf = mem ? mem->SystemHeapAlloc(nh, 0x1000) : 0;
      if (!buf) { o += "\n  could not allocate a buffer for the level-heap data"; g_bad_packs.push_back(pack); return false; }
      const uint32_t c180 = Rd32(base, ps + 180), c184 = Rd32(base, ps + 184);
      Wr32(base, ps + 180, buf); Wr32(base, ps + 184, nh);
      call(__imp__sub_821D40C8, sub);
      Wr32(base, ps + 180, c180); Wr32(base, ps + 184, c184);
      own_h = true;
      o += fmt::format("\n  level-heap data -> our buffer {:08X} ({:X} bytes)", buf, nh);
    }
  }
  const bool read_ok = (na == 0 || Rd32(base, sub + 3344)) && (nh == 0 || Rd32(base, sub + 3352));
  if (!read_ok) {
    g_bad_packs.push_back(pack);
    o += fmt::format("\n  READ FAILED (data ptrs {:08X} / {:08X}) - not linked", Rd32(base, sub + 3344), Rd32(base, sub + 3352));
    REXLOG_WARN("tfu packs: {}", o);
    return false;
  }
  if (own_a || own_h) o += "\n  (read into our own memory; game pools untouched)";
  call(__imp__sub_821DCF00, ps, sub + 16, 1);
  o += fmt::format("\n  read + linked. after: {}", budget());
  REXLOG_INFO("tfu packs: {}", o);
  return true;
}

// Live costume swap through the game's own (unused, dev-leftover) PlayerMgr routine sub_8227F510 "Costume change",
// run every frame: if byte mgr+24 is set it clears it, formats "%s.actor.xml" from char[256] mgr+32, mounts that pack
// (sub_821DC388 on PackSys - returns 1 because we loaded it already), takes player mgr+28's position/rotation,
// destroys the old actor (sub_827F0D48), creates the new one (sub_82269788) and binds the index (sub_8227F330).
// mgr = *(0x83034120) (verified in the disassembly 2026-11-18). Swaps only if LoadPack fully succeeded and a player 0
// exists (asked from the PlayerMgr itself, sub_8227F250(mgr, 0), not from our cheat tick).
std::string SwapNow(PPCContext& ctx, uint8_t* base, std::string arg) {
  while (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
  if (arg.empty()) return "error: @swapnow <template>";
  const uint32_t mgr = Rd32(base, 0x83034120u);
  if (!Readable(base, mgr, 0x200)) return fmt::format("error: PlayerMgr {:08X} not readable", mgr);
  PPCContext c = ctx;
  c.r1.u32 = (ctx.r1.u32 - 0x400) & ~0xFu; c.r3.u64 = mgr; c.r4.u64 = 0;
  __imp__sub_8227F250(c, base);
  const uint32_t player = c.r3.u32;
  if (!player) return "error: no player 0 right now (main menu or loading) - pick at the main menu instead";
  if (base[mgr + 24]) return "error: a costume change is already pending";
  std::string file = arg.find(".actor.xml") == std::string::npos ? CostumeFileName(arg) : arg;
  std::string stem = file.substr(0, file.size() - std::string(".actor.xml").size());
  if (stem.empty() || stem.size() > 200) return "error: bad name " + file;
  std::string o;
  if (!LoadPack(ctx, base, file, o)) return o + "\n  swap NOT queued";
  std::memcpy(base + mgr + 32, stem.c_str(), stem.size() + 1);
  Wr32(base, mgr + 28, 0);   // player index 0
  base[mgr + 24] = 1;        // next frame: sub_8227F510 swaps
  REXLOG_INFO("tfu swap: costume change queued -> {} (PlayerMgr {:08X}, old player {:08X})", stem, mgr, player);
  return o + fmt::format("\n  swap queued: {} (old player {:08X}) - happens next frame", stem, player);
}

// Costume voice/sound banks (2026-11-18): RoninLoadWorld (sub_82206528) walks the costume audio list 0x82FD44D8 -
// filled by sub_82204448 with the costume PACK name for List.txt lines marked 'includes audio' - and calls
// sub_82498640(char* name) for each (-> sub_82498450(*(0x83073A24), name, 0), synchronous). A mid-level swap loads
// the pack but not this, hence no voice. Returns the loader's r3 for the log.
std::string LoadAudio(PPCContext& ctx, uint8_t* base, std::string arg) {
  while (!arg.empty() && arg[0] == ' ') arg.erase(0, 1);
  if (arg.empty()) return "error: @loadaudio <template or pack name>";
  std::string pack = arg.find(".actor.xml") == std::string::npos ? CostumeFileName(arg) : arg;
  const uint32_t name = GuestString(base, pack);
  if (!name) return "error: could not allocate guest string";
  PPCContext c = ctx;
  c.r1.u32 = (ctx.r1.u32 - 0x400) & ~0xFu; c.r3.u64 = name;
  __imp__sub_82498640(c, base);
  const uint32_t r = c.r3.u32;
  REXLOG_INFO("tfu audio: sub_82498640('{}') -> {:08X}", pack, r);
  return fmt::format("loadaudio {} -> {:08X}", pack, r);
}

// ---- Puppet mode (2026-11-18): drive an AI character with the pad ------------------------------------------------
// Proven live in Kashyyyk_Prologue: SetScriptControlBrain(e) + RequestMoveToTarget(e, target) makes it walk there;
// ExecuteAbility(e, "CHORE_JUMP") plays its own chore (Y 29.2 -> 13.6); DisableBrainExceptMoveSet blocked move orders.
// The camera follows the player, so the player (hidden, body shrunk) is the move target: 3 m ahead of the puppet in the
// stick direction (camera-relative: forward = puppet - GetEye()), or on the puppet when idle. While active the game
// gets no pad input (menu input gate) and we read XInput ourselves. Back button / menu / puppet death releases.
std::atomic<bool> g_puppet{false};
// test hook (unattended runs): '@pupinput lx ly buttons ms' replaces the pad for ms milliseconds (game thread only)
struct PupOverride { float lx = 0, ly = 0; uint16_t buttons = 0; std::chrono::steady_clock::time_point until{}; };
PupOverride g_pup_ovr;
struct Pad { float lx = 0, ly = 0; uint16_t buttons = 0; bool ok = false; };
Pad ReadPadFull() {
  struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
  using Fn = DWORD(WINAPI*)(DWORD, XState*);
  static Fn get = [] {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
      if (HMODULE m = LoadLibraryA(dll)) if (auto f = GetProcAddress(m, "XInputGetState")) return reinterpret_cast<Fn>(f);
    return Fn(nullptr);
  }();
  Pad p;
  if (!get) return p;
  for (DWORD i = 0; i < 4; ++i) {
    XState st{};
    if (get(i, &st) != 0) continue;
    p.ok = true; p.buttons |= st.buttons;
    float x = st.lx / 32767.0f, y = st.ly / 32767.0f;
    if (std::fabs(x) > std::fabs(p.lx)) p.lx = std::clamp(x, -1.0f, 1.0f);
    if (std::fabs(y) > std::fabs(p.ly)) p.ly = std::clamp(y, -1.0f, 1.0f);
  }
  if (p.lx * p.lx + p.ly * p.ly < 0.25f * 0.25f) p.lx = p.ly = 0;
  return p;
}
const char* kPupXyz =
    "local function xyz(a) local s=tostring(F\"GetPosition\"(a)) local x,y,z=s:match(\"%[([%-%d%.e]+),([%-%d%.e]+),([%-%d%.e]+)\") "
    "return tonumber(x),tonumber(y),tonumber(z) end\n";
std::string PuppetStartSrc() {
  return std::string(kPupXyz) +
    "local me=F\"GetPlayer\"() if me==nil then return 'error: no player' end local mx,my,mz=xyz(me)\n"
    // candidates: not the player, health > 0, IsAlive not false, not the player's faction (0), nearest first; the
    // first one with a real chore set (>= 8 animations: props/turrets/corpses have few or none) wins
    "local cand={} for _,a in pairs(F\"GetAllRens\"()) do if a~=me then local ok,h=pcall(F\"GetHealth\",a)\n"
    " local oka,al=pcall(F\"IsAlive\",a)\n"
    " if ok and type(h)=='number' and h>0 and not (oka and al==false) then local okf,f=pcall(F\"GetRenFactionId\",a) if okf and f and f~=0 then\n"
    "  local x,y,z=xyz(a) if x then cand[#cand+1]={a=a,d=(x-mx)^2+(y-my)^2+(z-mz)^2} end end end end end\n"
    "table.sort(cand,function(p,q) return p.d<q.d end)\n"
    "local best,bd for i=1,math.min(#cand,12) do local c=cand[i] local okc,t=pcall(F\"GetAllChoreNames\",c.a) local n=0\n"
    " if okc and type(t)=='table' then for _ in pairs(t) do n=n+1 end end if n>=8 then best,bd=c.a,c.d break end end\n"
    "if not best then return 'error: no living AI character nearby' end\n"
    "local ok,err=pcall(F\"SetScriptControlBrain\",best) if not ok then return 'error: SetScriptControlBrain '..tostring(err) end\n"
    "local names={} local okn,t=pcall(F\"GetAllChoreNames\",best) if okn and type(t)=='table' then for _,v in pairs(t) do names[#names+1]=tostring(v) end end\n"
    "local function find(pats,skip) for _,n in ipairs(names) do for _,p in ipairs(pats) do if n:find(p) and n~=skip and not n:find('REACT') and not n:find('OVERLAY')\n"
    // things that happen TO the character (trooper X was CHORE_CHOKE_ARMS = being choked), not moves it makes
    "  and not n:find('ARMS') and not n:find('DEATH') and not n:find('GETUP') and not n:find('ELECTROCUTE') and not n:find('RELOAD') then return n end end end end\n"
    "local atk={'ATK','ATTACK','SWING','MELEE','PUNCH','STRIKE','SLASH'} local rng={'FIRE','THROW','SHOOT','SHIELD','DODGE_FORWARD'}\n"
    "local a1=find(atk) or find(rng)\n"
    "local a2=find(atk,a1) or find(rng,a1) or a1\n"
    "local P={e=best,me=me,n=0,a1=a1,a2=a2,jump=find({'CHORE_JUMP','DODGE_JUMP_UP','DODGE_JUMP_FWD'}),dl=find({'DODGE_LEFT'}),dr=find({'DODGE_RIGHT'}),moving=false}\n"
    "rawset(_G,'__tfu_pup',P) local okh,rh=pcall(F\"HideFromLayer\",me)\n"
    "return 'puppet '..tostring(best)..string.format(' at %.1f m, %d chores, hide=%s/%s. X=%s Y=%s A=%s B=%s/%s',math.sqrt(bd),#names,"
    "tostring(okh),tostring(rh),tostring(a1),tostring(a2),tostring(P.jump),tostring(P.dl),tostring(P.dr))";
}
std::string PuppetStopSrc() {
  return "local P=rawget(_G,'__tfu_pup') rawset(_G,'__tfu_pup',nil) if not P then return 'puppet: none' end\n"
         "pcall(F\"RestoreOriginalBrain\",P.e) pcall(F\"UnhideFromLayer\",P.me)\n"
         "return 'puppet released'";
}
// One driver step: stick (lx, ly), buttons pressed this step.
std::string PuppetDriveSrc(const Pad& pad, uint16_t pressed) {
  std::string a;
  if (pressed & 0x4000) a += "fire(P.a1)\n";                                     // X
  if (pressed & 0x8000) a += "fire(P.a2)\n";                                     // Y
  if (pressed & 0x1000) a += "fire(P.jump)\n";                                   // A
  if (pressed & 0x2000) a += pad.lx < 0 ? "fire(P.dl)\n" : "fire(P.dr)\n";      // B
  return std::string(kPupXyz) +
    "local P=rawget(_G,'__tfu_pup') if not P then return 'nopup' end local e,me=P.e,F\"GetPlayer\"() P.n=P.n+1\n"
    "local ok,h=pcall(F\"GetHealth\",e) if not ok or type(h)~='number' or h<=0 then return 'dead' end\n"
    "local function fire(c) if c then pcall(F\"ExecuteAbility\",e,c) end end\n" + a +
    "local px,py,pz=xyz(e) if not px then return 'nopos' end\n"
    "local es=tostring(F\"GetEye\"()) local ex,ey,ez=es:match(\"%[([%-%d%.e]+),([%-%d%.e]+),([%-%d%.e]+)\") ex,ez=tonumber(ex),tonumber(ez)\n"
    "local fx,fz=px-(ex or px),pz-(ez or pz-1) local l=math.sqrt(fx*fx+fz*fz) if l<0.01 then fx,fz=0,1 else fx,fz=fx/l,fz/l end\n"
    "local rx,rz=-fz,fx\n" +
    fmt::format("local lx,ly={:.3f},{:.3f}\n", pad.lx, pad.ly) +
    // live-tunable: rawset(_G,'__tfu_pupcfg',{spd=1,every=10,dist=6}). 22:18 run: re-ordering every 0.2 s at 0.5
    // left the puppet at 0.00-0.03 m/step; the one proven walk was a single order at 1.0. The target (hidden player)
    // keeps moving ahead every step; the order itself is only re-sent on a big direction change or every 'every' steps.
    // 22:59 unattended run: the hidden player was only re-placed on stick changes, so once it stood past a ledge it
    // free-fell (y -130, camera in the white void) and the puppet had nowhere to walk. Now it is re-placed EVERY step
    // at the puppet's own height: ahead while moving, just above the puppet's head while idle (camera stays on it).
    // DIST 10: RequestMoveToTarget treats anything within ~4 m as arrived (23:3x lab: marker 4 m away -> 0.0-0.2 m
    // moved in any direction; 8 m -> 4.7 m then stopped 3.3 m short); 10 m ahead -> 13 m in 2 s.
    "local C=rawget(_G,'__tfu_pupcfg') or {} local SPD,EVERY,DIST,UP=C.spd or 1.0,C.every or 10,C.dist or 10,C.up or 2.4\n"
    // camera basis is LOCKED while the stick is held (23:1x run: forward/right moved 0.3 m in 2 s, back/left 5-6.5 m -
    // the camera chases the target ahead, so a basis re-read every step kept flipping and re-planning the walk)
    "if lx*lx+ly*ly>0.0001 then\n"
    " if not P.moving or not P.bx then P.bx,P.bz=fx,fz end local bx,bz=P.bx,P.bz local brx,brz=-bz,bx\n"
    " local dx,dz=bx*ly+brx*lx,bz*ly+brz*lx local dl=math.sqrt(dx*dx+dz*dz) if dl>0 then dx,dz=dx/dl,dz/dl end\n"
    " F\"SetPosition\"(me,F\"Vector\"(px+dx*DIST,py+0.3,pz+dz*DIST,1)) pcall(F\"NotifyTeleport\",me)\n"
    " local turn=not P.dx or (P.dx*dx+P.dz*dz)<0.7\n"
    " if not P.moving or turn or P.n%EVERY==0 then local okm,rm=pcall(F\"RequestMoveToTarget\",e,me,SPD,0) P.rm=tostring(rm) P.dx,P.dz=dx,dz end P.moving=true\n"
    "else\n"
    // idle: hold the hidden player just above the puppet's head every step (not level with it - an actor teleported
    // onto another shoves it sideways, 2026-11-18 20:0x 'he just shifts to the side')
    " F\"SetPosition\"(me,F\"Vector\"(px,py+UP,pz,1)) pcall(F\"NotifyTeleport\",me) P.moving=false P.dx=nil P.bx=nil\n"
    "end\n"
    "local d=0 if P.lx then d=math.sqrt((px-P.lx)^2+(pz-P.lz)^2) end P.lx,P.lz=px,pz\n"
    "return string.format('ok %.2f move=%s',d,tostring(P.rm))";
}

// Enemy variety (proven 2026-11-18 02:3x, Kashyyyk_Prologue): spawner def+0xA8 is the template hash it spawns -
// every Wookiee spawner 741CB56D, every trooper spawner 6D0E78FB. Writing the trooper hash into a Wookiee spawner
// (GateWookiee DDC900BC), KillAll + ResetSpawner -> 14 TROOPERS on the Wookiee faction (6), classified by chore list
// (CHORE_OPERATE_TURRET vs CHORE_SABER_ATK). The faction comes from the spawner, so swapped types fight on the
// spawner's side. Only templates already used by this level's spawners are loaded, so only those are shuffled.
std::string SpawnMix(PPCContext& ctx, uint8_t* base, const std::string& handles, bool restore) {
  static std::map<uint32_t, uint32_t> orig;   // def -> original hash (game thread)
  std::vector<uint32_t> defs;
  std::istringstream in(handles);
  std::string hx;
  while (in >> hx) {
    uint32_t h = 0;
    if (!Hex(hx, h)) continue;
    PPCContext c = ctx;
    c.r1.u32 = (ctx.r1.u32 - 0x2400) & ~0xFu;
    c.r3.u64 = h;
    __imp__sub_82278018(c, base);   // 0 unless the handle is a spawner
    const uint32_t obj = c.r3.u32;
    if (!obj || !Readable(base, obj, 0x18)) continue;
    const uint32_t def = Rd32(base, obj + 20);
    if (!Readable(base, def, 0xB0)) continue;
    if (std::find(defs.begin(), defs.end(), def) == defs.end()) defs.push_back(def);
  }
  if (restore) {
    int n = 0;
    for (uint32_t d : defs) if (auto it = orig.find(d); it != orig.end()) { Wr32(base, d + 0xA8, it->second); ++n; }
    REXLOG_INFO("tfu variety: restored {} of {} spawners", n, defs.size());
    return fmt::format("enemy types restored on {} spawners", n);
  }
  std::vector<uint32_t> types;
  for (uint32_t d : defs) {
    if (!orig.count(d)) orig[d] = Rd32(base, d + 0xA8);
    const uint32_t t = orig[d];
    if (t && std::find(types.begin(), types.end(), t) == types.end()) types.push_back(t);
  }
  if (types.size() < 2) return fmt::format("only {} enemy type(s) in this level's {} spawners - nothing to mix", types.size(), defs.size());
  static std::mt19937 rng{std::random_device{}()};
  int changed = 0;
  for (uint32_t d : defs) {
    const uint32_t cur = orig[d];
    uint32_t pick = types[rng() % types.size()];
    if (pick == cur) pick = types[(std::find(types.begin(), types.end(), cur) - types.begin() + 1) % types.size()];
    Wr32(base, d + 0xA8, pick);
    ++changed;
  }
  std::string tl;
  for (uint32_t t : types) tl += fmt::format(" {:08X}", t);
  REXLOG_INFO("tfu variety: {} spawners re-typed among {} types:{}", changed, types.size(), tl);
  return fmt::format("{} spawners now spawn a different type ({} types in this level:{}) - new spawns only", changed, types.size(), tl);
}

std::string RunChunk(PPCContext& ctx, uint8_t* base, uint32_t L, const std::string& src_in) {
  if (src_in.rfind("@loadaudio", 0) == 0) return LoadAudio(ctx, base, src_in.substr(10));
  if (src_in.rfind("@pupinput", 0) == 0) {
    std::istringstream in(src_in.substr(9));
    float lx = 0, ly = 0; unsigned b = 0; int ms = 0;
    in >> lx >> ly >> std::hex >> b >> std::dec >> ms;
    g_pup_ovr = {std::clamp(lx, -1.0f, 1.0f), std::clamp(ly, -1.0f, 1.0f), uint16_t(b),
                 std::chrono::steady_clock::now() + std::chrono::milliseconds(std::clamp(ms, 0, 20000))};
    return fmt::format("pupinput lx {} ly {} buttons {:04X} for {} ms", lx, ly, b, ms);
  }
  if (src_in == "@puppet on" || src_in == "@puppet off") {
    const bool on = src_in == "@puppet on";
    if (on && g_puppet) return "puppet: already active";
    std::string r = RunChunk(ctx, base, L, on ? PuppetStartSrc() : PuppetStopSrc());
    if (on && r.rfind("puppet ", 0) == 0) {
      g_puppet = true;
      TfuLuaQueue("@capsule 0.05");   // the hidden player must not shove the puppet
      REXLOG_INFO("tfu puppet: {}", r);
    } else if (!on) {
      g_puppet = false;
      TfuLuaQueue("@capsule 1.0");
      REXLOG_INFO("tfu puppet: {}", r);
    }
    return r;
  }
  if (src_in.rfind("@swapnow", 0) == 0) return SwapNow(ctx, base, src_in.substr(8));
  if (src_in.rfind("@loadpack", 0) == 0) { std::string o; LoadPack(ctx, base, src_in.substr(9), o); return o; }
  if (src_in.rfind("@levelcostume", 0) == 0) return LevelCostume(base, src_in.substr(13));
  if (src_in.rfind("@peek", 0) == 0 || src_in.rfind("@poke", 0) == 0 || src_in.rfind("@spawnobj", 0) == 0 ||
      src_in.rfind("@scan", 0) == 0)
    return NativeDebug(ctx, base, src_in);
  // "@capsule <scale>": native job - get the player actor through Lua, then scale its body capsule radius
  const bool capsule = src_in.rfind("@capsule", 0) == 0;
  const double cap_scale = capsule ? std::atof(src_in.c_str() + 8) : 0.0;
  // "@tick": persistent-cheat maintenance (see Settings). Lua part leaves the player on top of the stack.
  // "@spawnrange <x>": Lua lists every actor handle; native code scales the activation distances of every spawner
  // among them (see ApplySpawnRange).
  const bool srange = src_in.rfind("@spawnrange", 0) == 0;
  const bool smix = src_in.rfind("@spawnmix", 0) == 0;
  const bool smix_restore = smix && src_in.find("restore") != std::string::npos;
  const double srange_mult = srange ? std::atof(src_in.c_str() + 11) : 1.0;
  const bool tick = src_in == "@tick";
  const Settings cs = tick ? GetSettings() : Settings{};
  std::string tick_src;
  if (tick) {
    tick_src = "local p=F\"GetPlayer\"() if p==nil then return nil end\n";
    if (cs.god) tick_src += "F\"SetInvincible\"(0,true)\n";
    if (g_need_ts.load()) tick_src += fmt::format("F\"SetTimeScale\"({})\n", cs.timescale);
    // Spawn multiplier (proven 2026-11-17 22:2x, Kashyyyk_Prologue: ResetSpawner on the 8 live spawners took living
    // actors 19 -> 27; resets do not stack within a frame, a spawner refills up to its own alive cap). Only HOSTILE
    // factions: a faction is hostile once one of its actors targets the player (GetCurrentAITarget == player) - the
    // first blind test also doubled Vader's own troopers (factions 6/7 fight each other, player is 0). Each spawner is
    // reset at most (mult-1) times per level; state lives in rawset globals (strict _G) and clears when the player
    // handle changes (level load).
    // paused while puppeting: resetting the puppet's own spawner hands it back to the AI
    if (cs.spawn_mult > 1 && !g_puppet) tick_src += fmt::format(
      "do local M={} local G=rawget\n"
      "local S=G(_G,'__tfu_sp') if S==nil or S.me~=p then S={{me=p,hostile={{}},resets={{}}}} rawset(_G,'__tfu_sp',S) rawset(_G,'__tfu_sp_n',0) end\n"
      "local fh,ff,ft,fs,fr=F\"GetHealth\",F\"GetRenFactionId\",F\"GetCurrentAITarget\",F\"GetSpawner\",F\"ResetSpawner\"\n"
      "local done,seen=0,{{}}\n"
      "for _,a in pairs(F\"GetAllRens\"()) do if a~=p then local ok,h=pcall(fh,a)\n"
      " if ok and type(h)=='number' and h>0 then local okf,f=pcall(ff,a) if not okf or f==nil then f=-1 end\n"
      "  if not S.hostile[f] then local okt,t=pcall(ft,a) if okt and t==p then S.hostile[f]=true end end\n"
      "  if S.hostile[f] and done<8 then local oks,sp=pcall(fs,a) if oks and sp~=nil then local k=tostring(sp) local n=S.resets[k] or 0\n"
      "   if not seen[k] and (M>=99 or n<M-1) then seen[k]=true pcall(fr,sp) S.resets[k]=n+1 done=done+1 end end end end end end\n"
      "if done>0 then rawset(_G,'__tfu_sp_n',(G(_G,'__tfu_sp_n') or 0)+done) end end\n", cs.spawn_mult);
    tick_src += "return p, tostring(rawget(_G,'__tfu_sp_n') or 0)";   // 2 results: player, spawn resets so far
  }
  const std::string src = capsule ? std::string("return F\"GetPlayer\"()") : tick ? tick_src
      : (srange || smix) ? std::string("local t={} for _,a in pairs(F\"GetAllRens\"()) do local h=tostring(a):match('(%x+)$') if h then t[#t+1]=h end end return table.concat(t,' ')")
      : src_in;
  bool expr = !src.empty() && src[0] == '=';
  // F"Name" finds a game function in whichever global table holds it (player.*, ai.*, game.*, ...): the bindings
  // are not globals (2026-11-17: 'Accesing non-existent global [SetInvincible]'; it lives in player and ai).
  static const std::string kPrelude =
      "local function F(n) local g=rawget(_G,n) if type(g)=='function' then return g end "
      "for k,t in pairs(_G) do if type(t)=='table' and k~='_G' then local f=rawget(t,n) "
      "if type(f)=='function' then return f end end end error('no game function '..n,2) end\n";
  std::string code = kPrelude + (expr ? "return tostring(" + src.substr(1) + ")" : src);
  if (code.size() > 7000) return "error: chunk too long (7000 bytes max)";
  PPCContext c = ctx;
  uint32_t sp = (ctx.r1.u32 - 0x2000) & ~0xFu;
  uint32_t buf = sp + 0x100, name = sp + 0x1F00;
  std::memcpy(base + buf, code.c_str(), code.size() + 1);
  std::memcpy(base + name, "=tfu", 5);
  Wr32(base, sp, ctx.r1.u32);   // back chain
  c.r1.u32 = sp;
  // stack depth as an INDEX: the Lua stack can be reallocated while our chunk runs (crash 2026-11-17 00:35:54:
  // restoring the old absolute top pointer left the game's next lua_pcall on a freed stack)
  int32_t saved_idx = int32_t(Rd32(base, L + 8) - Rd32(base, L + 12)) / 8;
  // No garbage collection while our chunk runs (crash 2026-11-17 00:42:04: right after a console chunk the game's
  // next script read a freed table, i.e. a collection triggered by OUR allocations freed something the engine still
  // used - it only collects at points it controls). global_State: +64 GCthreshold, +68 totalbytes. Afterwards the
  // threshold moves up by what we allocated, so the game's own collection timing is unchanged.
  const uint32_t G = Rd32(base, L + 16);
  const uint32_t gc_threshold = Rd32(base, G + 64), gc_total0 = Rd32(base, G + 68);
  Wr32(base, G + 64, 0xFFFFFFF0u);
  // luaL_loadbuffer(L, buf, size, "=tfu")
  c.r3.u64 = L; c.r4.u64 = buf; c.r5.u64 = code.size(); c.r6.u64 = name;
  __imp__sub_824F0910(c, base);
  int st = int(c.r3.u32);
  std::string res;
  if (st == 0) {
    // lua_pcall(L, 0, 1, 0): one result even for statement chunks, so 'return "msg"' shows in the menu (nil -> "ok")
    c.r1.u32 = sp; c.r3.u64 = L; c.r4.u64 = 0; c.r5.u64 = tick ? 2 : 1; c.r6.u64 = 0;
    __imp__sub_824DC5E8(c, base);
    st = int(c.r3.u32);
  }
  if (st == 0 && tick) {
    c.r1.u32 = sp; c.r3.u64 = L; c.r4.u64 = 0xFFFFFFFEu;   // player (or nil -> 0), 2nd from top
    __imp__sub_827AF6A8(c, base);
    const uint32_t h = c.r3.u32;
    {   // spawn multiplier progress -> log when it changes (proof that resets happened)
      c.r1.u32 = sp; c.r3.u64 = L; c.r4.u64 = 0xFFFFFFFFu; c.r5.u64 = 0;
      __imp__sub_824D4D88(c, base);
      static std::string last_n = "0";
      std::string n = RdStr(base, c.r3.u32, 32);
      if (!n.empty() && n != last_n) { REXLOG_INFO("tfu cheats: spawn x{}: {} hostile spawner reset(s) so far this level", cs.spawn_mult, n); last_n = n; }
    }
    if (h) g_need_ts = false;   // the SetTimeScale above ran (a player existed, chunk succeeded)
    if (!h) g_last_player = 0;  // menu/loading: next player counts as a new level even if the handle repeats
    res = "tick";
    if (h && h != g_last_player) {
      REXLOG_INFO("tfu cheats: player handle {:08X} -> {:08X} (level load/respawn): god={} infinite_force={} timescale={} body={}",
                  g_last_player, h, cs.god, cs.inf_force, cs.timescale, cs.body);
      g_last_player = h;
      if (cs.timescale != 1.0) g_need_ts = true;   // next tick
      if (cs.body != 1.0) REXLOG_INFO("tfu cheats: {}", Capsule(base, h, cs.body));
    }
    if (h && cs.inf_force) {
      c.r1.u32 = sp; c.r3.u64 = h;
      __imp__sub_822872B0(c, base);
      const uint32_t obj = c.r3.u32;
      if (obj) {
        const float e = RdF(base, obj + 808), mx = RdF(base, obj + 812);
        static int logged = 0;
        if (mx > 0.0f && mx < 100000.0f && e < mx) {
          if (logged < 3) { ++logged; REXLOG_INFO("tfu cheats: Force refill {:.1f} -> {:.1f} (obj {:08X})", e, mx, obj); }
          WrF(base, obj + 808, mx);
        }
      }
    }
  } else if (st == 0 && smix) {
    c.r1.u32 = sp; c.r3.u64 = L; c.r4.u64 = 0xFFFFFFFFu; c.r5.u64 = 0;
    __imp__sub_824D4D88(c, base);
    res = SpawnMix(ctx, base, RdStr(base, c.r3.u32), smix_restore);
  } else if (st == 0 && srange) {
    c.r1.u32 = sp; c.r3.u64 = L; c.r4.u64 = 0xFFFFFFFFu; c.r5.u64 = 0;
    __imp__sub_824D4D88(c, base);
    res = ApplySpawnRange(ctx, base, RdStr(base, c.r3.u32), srange_mult);
  } else if (st == 0 && capsule) {
    c.r1.u32 = sp; c.r3.u64 = L; c.r4.u64 = 0xFFFFFFFFu;   // the player actor GetPlayer() left on top of the stack
    __imp__sub_827AF6A8(c, base);
    res = Capsule(base, c.r3.u32, cap_scale);
  } else {
    // top of stack: error message (st != 0) or the expression's string
    c.r1.u32 = sp; c.r3.u64 = L; c.r4.u64 = 0xFFFFFFFFu; c.r5.u64 = 0;
    __imp__sub_824D4D88(c, base);
    std::string s = RdStr(base, c.r3.u32);
    res = st == 0 ? (s.empty() && !expr ? std::string("ok") : s) : "error: " + (s.empty() ? std::string("(no message, status ") + std::to_string(st) + ")" : s);
  }
  c.r1.u32 = sp; c.r3.u64 = L; c.r4.u64 = uint32_t(saved_idx);   // lua_settop(L, saved_idx)
  __imp__sub_824CCBF8(c, base);
  uint32_t gc_total1 = Rd32(base, G + 68);
  {
    const uint64_t t = uint64_t(gc_threshold) + (gc_total1 > gc_total0 ? gc_total1 - gc_total0 : 0);
    Wr32(base, G + 64, t > 0xFFFFFFF0ull ? 0xFFFFFFF0u : uint32_t(t));   // saturate: a wrap would force a collection now
  }
  return res;
}

Job MakeJob(std::string code, bool from_file) {
  Job j; j.from_file = from_file;
  // "#*" prefix: run on every distinct lua_State; "#3" prefix: only on the 3rd state seen (0-based)
  if (code.rfind("#*", 0) == 0) { j.target = -2; code = code.substr(2); }
  else if (code.size() > 1 && code[0] == '#' && isdigit((unsigned char)code[1])) {
    size_t e = 1; while (e < code.size() && isdigit((unsigned char)code[e])) ++e;
    j.target = e - 1 <= 4 ? std::atoi(code.substr(1, e - 1).c_str()) : 9999; code = code.substr(e);
  }
  while (!code.empty() && (code[0] == ' ' || code[0] == '\n' || code[0] == '\r')) code.erase(0, 1);
  j.code = code;
  return j;
}
void PollFile() {
  static auto last = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::milliseconds(500)) return;
  last = now;
  std::error_code ec;
  if (!std::filesystem::exists("run/lua_in.txt", ec)) return;
  std::ifstream f("run/lua_in.txt"); std::stringstream ss; ss << f.rdbuf(); f.close();
  std::filesystem::remove("run/lua_in.txt", ec);
  std::string code = ss.str();
  while (!code.empty() && (code.back() == '\n' || code.back() == '\r' || code.back() == ' ')) code.pop_back();
  if (code.empty()) return;
  std::lock_guard<std::mutex> l(g_mu);
  g_jobs.push_back(MakeJob(code, true));
  g_have_jobs = true;
}
}  // namespace

void TfuLuaQueue(const std::string& code) {
  std::lock_guard<std::mutex> l(g_mu);
  g_jobs.push_back(MakeJob(code, false));
  g_have_jobs = true;
}
std::string TfuLuaLast() { std::lock_guard<std::mutex> l(g_mu); return g_last; }
bool TfuPuppetActive() { return g_puppet.load(); }
bool TfuLuaReady() { return g_L.load() != 0; }

// Menu side of the persistent cheats. cmd: "god" / "force" toggle, "ts <x>", "body <x>". Saves, applies now.
std::string TfuCheat(const std::string& cmd) {
  if (!g_set_loaded) LoadSettings();
  Settings s = GetSettings();
  std::string msg, post;
  if (cmd == "god") {
    s.god = !s.god;
    if (!s.god) post = "F\"SetInvincible\"(0, false)";
    msg = std::string("God mode ") + (s.god ? "ON" : "OFF");
  } else if (cmd == "force") {
    s.inf_force = !s.inf_force;
    msg = std::string("Infinite Force ") + (s.inf_force ? "ON" : "OFF");
  } else if (cmd.rfind("ts ", 0) == 0) {
    s.timescale = std::atof(cmd.c_str() + 3); if (s.timescale <= 0) s.timescale = 1.0;
    post = fmt::format("F\"SetTimeScale\"({})", s.timescale);   // now; the tick re-applies after loads
    msg = fmt::format("Game speed {}x", s.timescale);
  } else if (cmd.rfind("range ", 0) == 0) {
    s.spawn_range = std::clamp(std::atof(cmd.c_str() + 6), 1.0, 20.0);
    post = fmt::format("@spawnrange {}", s.spawn_range);
    msg = s.spawn_range != 1.0 ? fmt::format("Spawn range x{} (encounters further ahead wake up)", s.spawn_range)
                               : std::string("Spawn range normal");
  } else if (cmd.rfind("spawn ", 0) == 0) {
    s.spawn_mult = std::clamp(std::atoi(cmd.c_str() + 6), 1, 99);
    msg = s.spawn_mult > 1 ? fmt::format("Enemy spawns x{} (hostile only; starts once enemies target you)", s.spawn_mult)
                           : std::string("Enemy spawns normal");
  } else if (cmd.rfind("body ", 0) == 0) {
    s.body = std::atof(cmd.c_str() + 5); if (s.body <= 0) s.body = 1.0;
    post = fmt::format("@capsule {}", s.body);
    msg = fmt::format("Body size {:.0f}%", s.body * 100);
  } else return "unknown cheat " + cmd;
  { std::lock_guard<std::mutex> l(g_set_mu); g_set = s; }
  if (!post.empty()) TfuLuaQueue(post);   // after publishing, so a tick in between cannot undo it
  SaveSettings(s);
  REXLOG_INFO("tfu cheats: {} (saved)", msg);
  return msg + " (saved - stays on after death, reload and relaunch)";
}
std::string TfuCheatSummary() {
  if (!g_set_loaded) LoadSettings();
  Settings s = GetSettings();
  return fmt::format("Saved cheats: god {}  |  infinite Force {}  |  speed {}x  |  body {:.0f}%  |  spawns x{}  |  range x{}",
                     s.god ? "ON" : "off", s.inf_force ? "ON" : "off", s.timescale, s.body * 100, s.spawn_mult, s.spawn_range);
}

extern "C" REX_FUNC(sub_824DC5E8) {
  static thread_local bool inside = false;
  uint32_t L = ctx.r3.u32;
  if (!inside) {
    if (!g_L.exchange(L)) REXLOG_INFO("tfu lua: first lua_pcall seen, L={:08X} - console ready", L);
    int li = int(std::find(g_states.begin(), g_states.end(), L) - g_states.begin());
    if (li == int(g_states.size()) && g_states.size() < 64) {
      g_states.push_back(L);
      REXLOG_INFO("tfu lua: lua_State #{} = {:08X} (global_State {:08X})", li, L, Rd32(base, L + 16));
      Out("# lua_State #" + std::to_string(li) + " = " + fmt::format("{:08X}", L) + "  G=" + fmt::format("{:08X}", Rd32(base, L + 16)));
    }
    PollFile();
    if (g_have_jobs.load()) {
      inside = true;
      std::deque<Job> keep;
      for (;;) {
        Job j;
        {
          std::lock_guard<std::mutex> l(g_mu);
          if (g_jobs.empty()) break;
          j = g_jobs.front(); g_jobs.pop_front();
        }
        bool run = j.target == -1 || j.target == li || (j.target == -2 && std::find(j.done.begin(), j.done.end(), L) == j.done.end());
        if (!run) { if (++j.passes < 2000) keep.push_back(j); else Out("> dropped (no such lua_State): " + j.code); continue; }
        std::string r = RunChunk(ctx, base, L, j.code);
        std::string first = j.code.substr(0, j.code.find('\n'));
        SetLast("[L#" + std::to_string(li) + "] " + first.substr(0, 80) + "  ->  " + r);
        REXLOG_INFO("tfu lua: [L#{}] {} -> {}", li, first.substr(0, 120), r.substr(0, 400));
        Out("> [L#" + std::to_string(li) + "] " + j.code + "\n" + r);
        if (j.target == -2) { j.done.push_back(L); if (j.done.size() < g_states.size()) keep.push_back(j); }   // "#*": every state seen so far
      }
      {
        std::lock_guard<std::mutex> l(g_mu);
        for (auto& k : keep) g_jobs.push_back(k);
        g_have_jobs = !g_jobs.empty();
      }
      inside = false;
    }
    // persistent cheats: maintenance tick on the main state (1 Hz) + native Force top-up every frame
    if (!g_set_loaded) LoadSettings();
    if (li == 0 && g_last_player) {
      static auto last_fill = std::chrono::steady_clock::now();
      auto now = std::chrono::steady_clock::now();
      if (now - last_fill >= std::chrono::milliseconds(15) && GetSettings().inf_force) {
        last_fill = now;
        PPCContext c = ctx;
        c.r1.u32 = (ctx.r1.u32 - 0x400) & ~0xFu;
        c.r3.u64 = g_last_player;
        __imp__sub_822872B0(c, base);   // stale handle -> 0
        if (uint32_t obj = c.r3.u32) {
          const float e = RdF(base, obj + 808), mx = RdF(base, obj + 812);
          if (mx > 0.0f && mx < 100000.0f && e < mx) WrF(base, obj + 808, mx);
        }
      }
    }
    if (li == 0) {
      static auto last_tick = std::chrono::steady_clock::now();
      const Settings s = GetSettings();
      auto now = std::chrono::steady_clock::now();
      static auto last_range = std::chrono::steady_clock::now();
      if (s.spawn_range != 1.0 && now - last_range >= std::chrono::seconds(5) && g_last_player) {
        last_range = now;
        std::lock_guard<std::mutex> l(g_mu);
        g_jobs.push_back(MakeJob(fmt::format("@spawnrange {}", s.spawn_range), false));
        g_have_jobs = true;
      }
      if (AnyCheat(s) && now - last_tick >= std::chrono::milliseconds(s.spawn_mult >= 99 ? 500 : 1000)) {
        last_tick = now;
        inside = true;
        std::string r = RunChunk(ctx, base, L, "@tick");
        inside = false;
        static int errs = 0;
        if (r != "tick" && r != "ok" && errs < 5) { ++errs; REXLOG_WARN("tfu cheats: tick -> {}", r.substr(0, 300)); }
      }
    }
    // puppet driver, 10 Hz
    if (li == 0 && g_puppet) {
      static auto last_drive = std::chrono::steady_clock::now();
      static uint16_t prev_buttons = 0;
      auto now = std::chrono::steady_clock::now();
      if (now - last_drive >= std::chrono::milliseconds(100)) {
        last_drive = now;
        Pad pad = ReadPadFull();
        if (now < g_pup_ovr.until) { pad.lx = g_pup_ovr.lx; pad.ly = g_pup_ovr.ly; pad.buttons = g_pup_ovr.buttons; pad.ok = true; }
        uint16_t pressed = pad.buttons & ~prev_buttons;
        prev_buttons = pad.buttons;
        if (pressed & 0x0020) {   // Back: release
          TfuLuaQueue("@puppet off");
        } else {
          inside = true;
          std::string r = RunChunk(ctx, base, L, PuppetDriveSrc(pad, pressed));
          inside = false;
          static int perr = 0;
          // once a second while puppeting: what the pad said and how far the puppet moved (m per 0.1 s step)
          static auto last_log = now;
          static uint16_t held = 0;
          held |= pressed;
          if (now - last_log >= std::chrono::seconds(1)) {
            last_log = now;
            REXLOG_INFO("tfu puppet: pad ok={} stick {:.2f},{:.2f} pressed {:04X} -> {}", pad.ok, pad.lx, pad.ly, held, r);
            held = 0;
          }
          if (r == "dead" || r == "nopup") { REXLOG_INFO("tfu puppet: {} -> releasing", r); TfuLuaQueue("@puppet off"); g_puppet = false; }
          else if (r.rfind("ok", 0) != 0 && perr < 5) { ++perr; REXLOG_WARN("tfu puppet: drive -> {}", r.substr(0, 300)); }
        }
      }
    }
  }
  __imp__sub_824DC5E8(ctx, base);
}

// ---- live costume swap: finish what the dev routine leaves out (2026-11-18) ---------------------------------------
// sub_8227F510 (PlayerMgr 'Costume change', every frame) destroys the player and creates the new one, but skips the
// post-spawn init sub_82552B48(rec, idx) that a level start runs (sub_8227F938 InitPostWorldLoad). That init fetches
// the player's profile object (sub_824F1318 on *(0x830A7B80)) into rec+11732 and attaches controller/camera/input.
// Without it the same frame's player update (sub_8227EF40 -> 82558BD0 -> 82550AD8) read *(rec+11732)+0x94 = NULL+0x94
// -> crash 00:54:15 (logs/crash_20261118_005415.txt). So: run the original, then the init on the new record; if the
// profile pointer is still 0, fall back to the old record's value.
extern "C" void __imp__sub_8227F510(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82550AA0(PPCContext& __restrict ctx, uint8_t* base);   // actor handle -> player record (type 1) or 0
extern "C" void __imp__sub_82552B48(PPCContext& __restrict ctx, uint8_t* base);   // (record, idx) post-spawn init

extern "C" REX_FUNC(sub_8227F510) {
  const uint32_t mgr = ctx.r3.u32;
  const bool pending = mgr && base[mgr + 24] != 0;
  uint32_t idx = 0, old_h = 0, old_rec = 0, old_prof = 0;
  auto call = [&](auto fn, uint32_t a3, uint32_t a4 = 0) {
    PPCContext c = ctx;
    c.r1.u32 = (ctx.r1.u32 - 0x200) & ~0xFu;
    c.r3.u64 = a3; c.r4.u64 = a4;
    fn(c, base);
    return c.r3.u32;
  };
  if (pending) {
    idx = Rd32(base, mgr + 28);
    old_h = call(__imp__sub_8227F250, mgr, idx);
    old_rec = old_h ? call(__imp__sub_82550AA0, old_h) : 0;
    old_prof = old_rec ? Rd32(base, old_rec + 11732) : 0;
    REXLOG_INFO("tfu swap: running costume change '{}' idx {} old player {:08X} rec {:08X} profile {:08X}",
                RdStr(base, mgr + 32, 200), idx, old_h, old_rec, old_prof);
  }
  __imp__sub_8227F510(ctx, base);
  if (!pending) return;
  const uint32_t new_h = call(__imp__sub_8227F250, mgr, idx);
  const uint32_t new_rec = new_h ? call(__imp__sub_82550AA0, new_h) : 0;
  if (!new_rec) {
    REXLOG_WARN("tfu swap: no player record after the change (new handle {:08X}) - not initialising", new_h);
    return;
  }
  call(__imp__sub_82552B48, new_rec, idx);
  uint32_t prof = Rd32(base, new_rec + 11732);
  if (!prof && old_prof) { Wr32(base, new_rec + 11732, old_prof); prof = old_prof; }
  REXLOG_INFO("tfu swap: new player {:08X} rec {:08X} initialised, profile {:08X}", new_h, new_rec, prof);
  // The init places the new player at the spawn point. If the menu stored the old position, put the player back there
  // on the next Lua call (game thread). Verified live 2026-11-18: SetPosition(p,pos) + NotifyTeleport(p) teleports the
  // player exactly; MoveToPosition(p,pos,0) returned ok but did nothing (the character controller overrides it).
  TfuLuaQueue("local pos=rawget(_G,'__tfu_swap_pos') rawset(_G,'__tfu_swap_pos',nil) local p=F\"GetPlayer\"()\n"
              "if p==nil or pos==nil then return 'swap: no stored position' end\n"
              "local ok,e=pcall(F\"SetPosition\",p,pos) pcall(F\"NotifyTeleport\",p)\n"
              "return 'swap: moved back to '..tostring(pos)..' ok='..tostring(ok)..(ok and '' or (' '..tostring(e)))");
}

// ---- Press Start crash guard (2026-11-24) ---------------------------------------------------------------------------
// sub_8272B180 (draw one render item, loader/render thread) starts with, in render mode 5 (*(RS+6704), RS=*(0x830F3AB0))
// when RS+172 is set and item+48 == 0:   t = *(RS+6772); if (u16 item+112 < u16 t+8) flag = u8 (*(t+24) + idx*8 + 5)
// At Press Start that table is sometimes not built yet: t == 0 (read 0x8, 2026-11-17) or t+24 == 0/garbage (read
// idx*8+5 = 0xDD with idx 27; 11 crashes 11-18..11-24, any profile, with or without other hooks). It is a race, not a
// save or hook problem. Guard: when the table is not usable, skip this item for this frame instead of crashing.
extern "C" void __imp__sub_8272B180(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_8272B180) {
  auto r32 = [base](uint32_t a) { uint32_t v; std::memcpy(&v, base + a, 4); return _byteswap_ulong(v); };
  auto r16 = [base](uint32_t a) { uint16_t v; std::memcpy(&v, base + a, 2); return (uint32_t)_byteswap_ushort(v); };
  const uint32_t rs = r32(0x830F3AB0), item = ctx.r3.u32;
  if (rs >= 0x10000 && item >= 0x10000 && r32(rs + 6704) == 5 && base[rs + 172] != 0 && r32(item + 48) == 0) {
    const uint32_t t = r32(rs + 6772);
    bool bad = t < 0x10000;
    if (!bad && r16(item + 112) < r16(t + 8) && r32(t + 24) < 0x10000) bad = true;
    if (bad) {
      static std::atomic<uint32_t> n{0};
      const uint32_t k = ++n;
      if (k <= 5 || (k % 5000) == 0)
        REXLOG_INFO("tfu fix: render item {:08X} skipped, pass table not ready (t={:08X}) - skip #{}", item, t, k);
      return;
    }
  }
  __imp__sub_8272B180(ctx, base);
}
