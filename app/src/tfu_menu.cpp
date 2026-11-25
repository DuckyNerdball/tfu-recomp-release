// tfu: in-game dev menu (v1). L3+R3 (both stick clicks) or F9 toggles. While open the game gets no pad input.
// D-pad up/down: select   left/right: change value   A / Enter: run   B / Esc: close.
// Ported from the BioShock Infinite recomp menu (same input gate, same XInput read).
//
// v1 items
//   Player model: redirect the costume pack the game loads for Darth Vader (or any player costume) to another of
//   the 81 costume packs on the discs. Done in the VFS (prefix symlink '<mount>\ExternalPacks\<from>.actor.xml' ->
//   '<to>.actor.xml'), so no file on disk changes and packs the game holds open do not matter (v1 copied files and
//   failed with 'file in use' mid-level). Active swaps are kept in run\costume_redirects.txt and re-applied at
//   startup. Takes effect when the game next opens the pack: pause > restart checkpoint, load a save, or relaunch.
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#endif
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <map>

#include <imgui.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/input/input_system.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/filesystem/vfs.h>

namespace fs = std::filesystem;

void TfuLuaQueue(const std::string& code);   // tfu_lua.cpp
std::string TfuLuaLast();
bool TfuLuaReady();
std::string TfuCheat(const std::string& cmd);   // persistent cheats (run\tfu_settings.toml)
std::string TfuCheatSummary();
bool TfuPuppetActive();   // tfu_lua.cpp: puppet mode drives an AI with the pad - the game must not see it

namespace {
std::atomic<bool> g_open{false};
std::mutex g_msg_mu;
std::string g_msg = "Ready.";
void Msg(const std::string& s) { { std::lock_guard<std::mutex> l(g_msg_mu); g_msg = s; } REXLOG_INFO("tfu menu: {}", s); }
std::string MsgGet() { std::lock_guard<std::mutex> l(g_msg_mu); return g_msg; }

// ---- costume packs -------------------------------------------------------------------------------------------------
const fs::path kExt = "game_all/ExternalPacks";

std::vector<std::string> ListCostumes() {
  std::vector<std::string> v;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(kExt, ec)) {
    std::string n = e.path().filename().string();
    const std::string suf = ".actor.xml_x360.lp";
    if (n.rfind("player", 0) == 0 && n.size() > suf.size() && n.compare(n.size() - suf.size(), suf.size(), suf) == 0)
      v.push_back(n.substr(0, n.size() - suf.size()));
  }
  std::sort(v.begin(), v.end());
  return v;
}
bool StartsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

const fs::path kRedirLog = "run/costume_redirects.txt";
std::map<std::string, std::string> g_redir;   // from -> to (active)

std::vector<std::string> RedirectKeys(const std::string& name) {
  std::vector<std::string> keys = {"game:\\ExternalPacks\\" + name + ".actor.xml", "d:\\ExternalPacks\\" + name + ".actor.xml"};
  auto* vfs = rex::Runtime::instance() ? rex::Runtime::instance()->file_system() : nullptr;
  std::string mount;
  if (vfs && vfs->FindSymbolicLink("game:", mount)) keys.push_back(mount + "\\ExternalPacks\\" + name + ".actor.xml");
  return keys;
}
std::string RedirectTarget(const std::string& key, const std::string& from, const std::string& to) {
  return key.substr(0, key.size() - (from + ".actor.xml").size()) + to + ".actor.xml";
}
void Unredirect(const std::string& from) {
  auto* vfs = rex::Runtime::instance() ? rex::Runtime::instance()->file_system() : nullptr;
  if (!vfs) return;
  for (auto& k : RedirectKeys(from)) vfs->UnregisterSymbolicLink(k);
  g_redir.erase(from);
}
void SaveRedirects() {
  std::error_code ec;
  fs::create_directories(kRedirLog.parent_path(), ec);
  std::ofstream f(kRedirLog, std::ios::trunc);
  for (auto& [a, b] : g_redir) f << a << " " << b << "\n";
}
bool SwapCostume(const std::string& from, const std::string& to, std::string& err) {
  auto* vfs = rex::Runtime::instance() ? rex::Runtime::instance()->file_system() : nullptr;
  if (!vfs) { err = "runtime not ready"; return false; }
  Unredirect(from);
  if (from != to) {
    if (g_redir.count(to)) { err = to + " is itself redirected; restore it first (chains loop)"; return false; }
    std::error_code ec;
    if (!fs::exists(kExt / (to + ".actor.xml_x360.lp"), ec)) { err = "no packs named " + to; return false; }
    for (auto& k : RedirectKeys(from)) vfs->RegisterSymbolicLink(k, RedirectTarget(k, from, to));
    g_redir[from] = to;
  }
  SaveRedirects();
  return true;
}
std::string CurrentSwap(const std::string& from) { auto it = g_redir.find(from); return it == g_redir.end() ? "" : it->second; }
// "Play as X": every player template the game might load (each level picks its own: Hoth forces playerhoth, the
// prologue playerdarthvader, story levels whatever costume is equipped) is redirected to X. Live EquipCostume alone
// changes nothing on screen (2026-11-17 test on Hoth) - the model comes from the pack loaded at level/checkpoint load.
std::vector<std::string> ListCostumes();
int SwapAll(const std::string& to, std::string& err) {
  auto* vfs = rex::Runtime::instance() ? rex::Runtime::instance()->file_system() : nullptr;
  if (!vfs) { err = "runtime not ready"; return 0; }
  std::error_code ec;
  if (!fs::exists(kExt / (to + ".actor.xml_x360.lp"), ec)) { err = "no packs named " + to; return 0; }
  for (auto it = g_redir.begin(); it != g_redir.end();) { std::string f = it->first; ++it; Unredirect(f); }
  int n = 0;
  for (auto& c : ListCostumes()) {
    if (c == to) continue;
    for (auto& k : RedirectKeys(c)) vfs->RegisterSymbolicLink(k, RedirectTarget(k, c, to));
    g_redir[c] = to; ++n;
  }
  SaveRedirects();
  return n;
}
void RestoreAll() {
  for (auto it = g_redir.begin(); it != g_redir.end();) { std::string f = it->first; ++it; Unredirect(f); }
  SaveRedirects();
}
std::string Lower(std::string s) { for (auto& ch : s) ch = char(tolower((unsigned char)ch)); return s; }
void LoadRedirects() {   // startup: re-apply what was active last session
  std::ifstream f(kRedirLog);
  std::string a, b, err;
  while (f >> a >> b) if (a != b && !SwapCostume(a, b, err)) REXLOG_WARN("tfu menu: redirect {} -> {} failed: {}", a, b, err);
  if (!g_redir.empty()) REXLOG_INFO("tfu menu: {} model redirect(s) active from last session", g_redir.size());
}


// ---- menu content --------------------------------------------------------------------------------------------------
// An item with empty code is a section header (not selectable). Special codes:
//   @as <template>   play as that character in every level (all player templates redirected) + EquipCostume
//   @restoreall      undo play-as      @unlockall   everything the game can unlock      @run   run the console text
struct Item { std::string label; std::string code; };
struct Tab { const char* name; std::vector<Item> items; };

#define L(s) "F\"" s "\""
const std::vector<Item> kPlayer = {
  {"Survival", ""},
  {"God mode on/off (saved)", "@cheat god"},
  {"Heal to full", L("OverrideHealth") "(" L("GetPlayer") "(), " L("GetMaxHealth") "(" L("GetPlayer") "()))"},
  {"Max health x10", L("SetMaxHealth") "(" L("GetPlayer") "(), " L("GetMaxHealth") "(" L("GetPlayer") "()) * 10)\n"
                     L("OverrideHealth") "(" L("GetPlayer") "(), " L("GetMaxHealth") "(" L("GetPlayer") "()))"},
  {"Combat", ""},
  {"Infinite Force on/off (saved)", "@cheat force"},
  {"Damage boost", L("ExecuteAbilityFunction") "(0, \"ABILITYFUNC_DAMAGEINCREASE\", \"\")"},
  {"Body collision (experimental: tighter = slip past enemies and corners)", ""},
  {"Show body size", "@capsule 0"},
  {"Tighter body (85%, saved)", "@cheat body 0.85"},
  {"Much tighter body (70%, saved)", "@cheat body 0.70"},
  {"Normal body (saved)", "@cheat body 1.0"},
  {"Info", ""},
  {"Show health / Force", "=" L("GetHealth") "(" L("GetPlayer") "()) .. \" / \" .. " L("GetMaxHealth") "(" L("GetPlayer")
                          "()) .. \"   Force \" .. " L("GetEnergy") "(" L("GetPlayer") "()) .. \" / \" .. " L("GetEnergyMax") "(" L("GetPlayer") "())"},
};
const std::vector<Item> kPowers = {
  {"Force power ranks", ""},
  {"ALL powers to max rank", L("SetForceRanks") "(0, 3)"},
  {"Lightning max", L("SetForceLightningRank") "(0, 3)"},
  {"Push max", L("SetForcePushRank") "(0, 3)"},
  {"Grip max", L("SetForceGripRank") "(0, 3)"},
  {"Repulse max", L("SetForceRepulseRank") "(0, 3)"},
  {"Saber throw max", L("SetSaberThrowRank") "(0, 3)"},
  {"Super powers (multiplayer-encounter buffs)", ""},
  {"Super Lightning", L("ExecuteAbilityFunction") "(0, \"ABILITYFUNC_MPENC_SUPER_LIGHTNING\", \"\")"},
  {"Super Push", L("ExecuteAbilityFunction") "(0, \"ABILITYFUNC_MPENC_SUPER_PUSH\", \"\")"},
  {"Super Grip", L("ExecuteAbilityFunction") "(0, \"ABILITYFUNC_MPENC_SUPER_GRIP\", \"\")"},
  {"Sith Rage", L("ExecuteAbilityFunction") "(0, \"ABILITYFUNC_MPENC_SITH_RAGE\", \"\")"},
  {"One-shot abilities", ""},
  {"Force Explode", L("ExecuteAbilityFunction") "(0, \"ABILITYFUNC_FORCE_EXPLODE\", \"\")"},
  {"Lightning Shield", L("ExecuteAbilityFunction") "(0, \"ABILITYFUNC_LIGHTNING_SHIELD\", \"\")"},
};
const std::vector<Item> kUnlock = {
  {"Unlocks (stick to your save)", ""},
  {"All upgrades / combos / talents + powers max", "@unlockall"},
  {"The rest: type these in the game's own cheat code screen", ""},
  {"   SOHNDANN all costumes   HURRIKANE all saber crystals   MOLDYCROW all combos", ""},
  {"   JOCASTA all talents   KATARN maxed powers   OSSUS databank   SITHSPAWN Sith Master", ""},
  {"   Lightsaber = super saber   Mastermind = Emperor   KORRIBAN = Sith Stalker", ""},
};
const std::vector<Item> kWorld = {
  {"Enemies", ""},
  {"Kill all enemies", L("KillAll") "()"},
  {"Puppet mode (experimental): play as the nearest enemy with ITS moves", ""},
  {"Take control of the nearest enemy (stick moves, X/Y attacks, A jump, B dodge, Back releases)", "@puppet on"},
  {"Release the puppet", "@puppet off"},
  {"More enemies (saved; each enemy spawner fires extra waves - hostile factions only)", ""},
  {"Enemy spawns x2", "@cheat spawn 2"},
  {"Enemy spawns x3", "@cheat spawn 3"},
  {"Enemy spawns x5 (chaos)", "@cheat spawn 5"},
  {"ENDLESS waves (every kill is replaced, all hostile spawners re-fire every second)", "@cheat spawn 99"},
  {"Enemy spawns normal", "@cheat spawn 1"},
  // Spawn range removed 2026-11-18: x5 rescaled all 58 spawners, 0 extra spawns in 15 s (encounters are trigger-driven)
  {"Enemy variety (this level, new spawns): shuffle which enemy type each spawner makes", ""},
  {"Mix enemy types (troopers from Wookiee spawners and so on)", "@spawnmix"},
  {"Restore enemy types", "@spawnmix restore"},
  {"Combat pace (whole game, enemies too)", ""},
  {"Turbo 1.15x (snappier fights, saved)", "@cheat ts 1.15"},
  {"Turbo 1.3x (DMC pace, saved)", "@cheat ts 1.3"},
  {"Normal speed (saved)", "@cheat ts 1.0"},
  {"Time (this moment only, not saved)", ""},
  {"Bullet time 0.1x", L("SetTimeScale") "(0.1)"},
  {"Slow motion 0.3x", L("SetTimeScale") "(0.3)"},
  {"Fast 2x", L("SetTimeScale") "(2.0)"},
  {"Possess (experimental - may crash)", ""},
  // the game's _G is strict (no new globals): keep our state with rawget/rawset (2026-11-17 20:36 errors)
  {"Take control of your lock-on target", "if rawget(_G,'__tfu_me') == nil then rawset(_G,'__tfu_me'," L("GetPlayer") "()) end\nlocal t = " L("GetTarget")
     "()\nif t == nil then return 'no target - lock on first' end\n" L("SetPlayerController") "(t, 0)\nreturn 'possessed ' .. tostring(t)"},
  {"Give control back", "local me = rawget(_G,'__tfu_me')\nif me then " L("SetPlayerController") "(me, 0) rawset(_G,'__tfu_me',nil) end\nreturn 'back'"},
};
const std::vector<Item> kConsole = {
  {"Lua console (type above, all 1588 functions in docs\\lua_signatures.txt)", ""},
  {"Run the text above   (start with = to show a value)", "@run"},
  {"Debug", ""},
  {"List Lua globals (to logs\\lua_out.txt)", "=(function() local t={} for k in pairs(_G) do t[#t+1]=tostring(k) end table.sort(t) return #t .. ' globals: ' .. table.concat(t, ' ') end)()"},
  {"Player faction id", "=" L("GetRenFactionId") "(" L("GetPlayer") "())"},
  {"Combat research (DMC project)", ""},
  {"Show animation block names on screen", L("DisplayBlockNames") "(true)\nreturn 'block names on - fight something'"},
  {"Hide animation block names", L("DisplayBlockNames") "(false)"},
  {"Current anim length (player)", "=" L("GetCurrentAnimTotalTime") "(" L("GetPlayer") "())"},
  {"Dump player chore blocks (to logs\\lua_out.txt)", "=(function() local p=" L("GetPlayer") "() local o={} "
     "for _,c in ipairs({'','default','Default','melee','Melee','saber','Saber','combat','Combat','locomotion','Locomotion'}) do "
     "local ok,n=pcall(" L("GetChoreBlockCount") ",p,c) if ok and n and n>0 then o[#o+1]=c..':'..n "
     "for i=0,n-1 do local ok2,nm=pcall(" L("GetChoreBlockNameByIndex") ",p,c,i) o[#o+1]='  '..tostring(nm) end end end "
     "return #o==0 and 'no chores matched' or table.concat(o,'\\n') end)()"},
};
#undef L

// Characters: friendly name + group for each player template on the discs (69). Unknown ones land in "Other".
struct CharInfo { const char* tmpl; const char* name; int group; };
const char* kGroups[] = {"Villains & Sith", "Jedi & heroes", "Starkiller outfits", "Troopers & others", "Other"};
const CharInfo kChars[] = {
  {"playerdarthvader", "Darth Vader", 0}, {"playeremperorpalpatine", "Emperor Palpatine", 0},
  {"playercountdooku", "Count Dooku", 0}, {"playerdarthmaul", "Darth Maul", 0}, {"playerdarthsion", "Darth Sion", 0},
  {"playersithassassin", "Sith Assassin", 0}, {"playerultimateevil", "Sith Stalker armor", 0},
  {"playerevilboss", "Dark Stalker (DLC)", 0}, {"playeruberevil", "Sith Lord robes (DLC)", 0},
  {"playerjangofett", "Jango Fett", 0}, {"playerozziksturn", "Ozzik Sturn", 0},
  {"playeranakin", "Anakin Skywalker", 1}, {"playerobiwan", "Obi-Wan Kenobi", 1}, {"playerbenkenobi", "Ben Kenobi", 1},
  {"playerquigonjinn", "Qui-Gon Jinn", 1}, {"playermacewindu", "Mace Windu", 1}, {"playerkitfisto", "Kit Fisto", 1},
  {"playerplokoon", "Plo Koon", 1}, {"playerkiadimundi", "Ki-Adi-Mundi", 1}, {"playerlukejedi", "Luke Skywalker (Jedi)", 1},
  {"playerluketattooine", "Luke Skywalker (Tatooine)", 1}, {"playerawardkota", "Rahm Kota", 1},
  {"playerkota", "Rahm Kota (story)", 1}, {"playerdrunkkota", "Rahm Kota (drunk)", 1}, {"playerexiledjedibonus", "Exiled Jedi", 1},
  {"playerbailorgana", "Bail Organa", 1}, {"playergarmbeliblis", "Garm Bel Iblis", 1}, {"playerc3po", "C-3PO", 1},
  {"playerproxy", "PROXY", 1}, {"playerblueglowy", "Force ghost", 1},
  {"player", "Starkiller (default)", 2}, {"playeracttwo", "Starkiller - act 2", 2}, {"playertraining", "Training gear", 2},
  {"playerhoodedrobe", "White hooded robe", 2}, {"playerultimategood", "Ceremonial Jedi robes", 2},
  {"playerjeditemple", "Jedi Temple outfit", 2}, {"playerhoth", "Hoth outfit", 2}, {"playertatooine", "Tatooine outfit", 2},
  {"playercloudcity", "Cloud City outfit", 2}, {"playerdeathstar", "Death Star outfit", 2}, {"playerempirical", "Empirical outfit", 2},
  {"playerfeluciaact1", "Felucia outfit", 2}, {"playerfeluciaact2", "Felucia outfit 2", 2}, {"playerraxusact1", "Raxus Prime outfit", 2},
  {"playerraxusact2", "Raxus Prime outfit 2", 2}, {"playerkashyyykact2", "Kashyyyk outfit", 2}, {"playerclonewars", "Clone Wars outfit", 2},
  {"playerwhitearmor", "Utility combat suit", 2}, {"playerveteran", "Rebel veteran", 2}, {"playerimpcomndo", "Imperial commando", 2},
  {"playerstormtrooper", "Stormtrooper", 3}, {"playertroopercommander", "Stormtrooper commander", 3},
  {"playershadowtrooper", "Shadow trooper", 3}, {"playersnowtrooper", "Snowtrooper", 3}, {"playerscouttrooper", "Scout trooper", 3},
  {"playerjumptrooper", "Jump trooper", 3}, {"playerkashyyyktrooper", "Kashyyyk trooper", 3}, {"playerevotrooper", "EVO trooper", 3},
  {"playershadowevotrooper", "Shadow EVO trooper", 3}, {"playerincinerator", "Incinerator trooper", 3},
  {"playergunner", "Imperial gunner", 3}, {"playerimperialofficer", "Imperial officer", 3}, {"playerclonetrooper", "Clone trooper", 3},
  {"playerprotorebel", "Rebel soldier", 3}, {"playerprotorebelelite", "Rebel elite", 3}, {"playerprotorebelsaboteur", "Rebel saboteur", 3},
  {"playerstatuemelee", "Sith training droid (melee)", 3}, {"playerstatueranged", "Sith training droid (ranged)", 3},
  {"playerdrexlfighter", "Drexl fighter", 3},
};
std::string CharName(const std::string& t) {
  for (auto& c : kChars) if (t == c.tmpl) return c.name;
  return StartsWith(t, "player") ? t.substr(6) : t;
}
// Costumes from the DLC catalogs (octobercat / novembercat / jtcostumecat / hot / tat). Equipping one in a STORY
// profile made the save unloadable: "The content for this save game is not available" (2026-11-17 20:43, Dooku).
const char* kDlcCostumes[] = {
  "playeranakin", "playerdarthsion", "playerhoodedrobe", "playerimpcomndo", "playerkiadimundi", "playerkitfisto",
  "playerlukejedi", "playerobiwan", "playersithassassin", "playerveteran",                                  // October
  "playerbenkenobi", "playerblueglowy", "playerc3po", "playerclonetrooper", "playerclonewars", "playercountdooku",
  "playerjangofett", "playerluketattooine", "playerplokoon", "playerwhitearmor",                            // November
  "playerdarthmaul", "playerevilboss", "playerjeditemple", "playermacewindu", "playerquigonjinn",
  "playerstatuemelee", "playerstatueranged", "playertraining", "playeruberevil",                          // Jedi Temple
  "playerhoth", "playertatooine"};
bool IsDlcCostume(const std::string& t) { for (auto* c : kDlcCostumes) if (t == c) return true; return false; }
// run.ps1 sets TFU_CAMPAIGN. "story" = guard on; "storyx" = the experiment that may make DLC costumes safe; dlc = off.
bool StoryGuard() { const char* c = std::getenv("TFU_CAMPAIGN"); return c && std::string(c) == "story"; }

std::vector<Item> BuildCharacters() {
  std::vector<Item> v;
  if (StoryGuard()) v.push_back({"STORY PROFILE: [DLC] outfits are locked - they make the save unloadable", ""});
  // ReLoadWorld removed: crashed 2026-11-17 01:28:38 (null read in 0x82552B48 during the reload, with redirects on).
  v.push_back({"Play as: swaps you IN PLACE now, and every story level loads this character (session only)", ""});
  v.push_back({"Restore original characters", "@restoreall"});
  auto all = ListCostumes();
  for (int g = 0; g < 5; ++g) {
    std::vector<std::string> names;
    for (auto& t : all) {
      int grp = 4;
      for (auto& c : kChars) if (t == c.tmpl) grp = c.group;
      if (grp == g) names.push_back(t);
    }
    if (names.empty()) continue;
    std::string hdr = kGroups[g];
    if (g == 0 || g == 1) hdr += "   (look only - they use Starkiller's moves)";
    v.push_back({hdr, ""});
    std::stable_sort(names.begin(), names.end(), [](auto& a, auto& b) {
      int ia = 999, ib = 999, i = 0;
      for (auto& c : kChars) { if (a == c.tmpl) ia = i; if (b == c.tmpl) ib = i; ++i; }
      return ia < ib; });
    for (auto& t : names) v.push_back({CharName(t) + (StoryGuard() && IsDlcCostume(t) ? "   [DLC]" : ""), "@as " + t});
  }
  return v;
}

// ---- input ---------------------------------------------------------------------------------------------------------
struct PadState { WORD buttons = 0; bool ok = false; };
PadState ReadPad() {
  struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
  using Fn = DWORD(WINAPI*)(DWORD, XState*);
  static Fn get = [] {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"})
      if (HMODULE m = LoadLibraryA(dll)) if (auto f = GetProcAddress(m, "XInputGetState")) return reinterpret_cast<Fn>(f);
    return Fn(nullptr);
  }();
  PadState p;
  if (!get) return p;
  for (DWORD i = 0; i < 4; ++i) { XState s{}; if (get(i, &s) == 0) { p.buttons |= s.buttons; p.ok = true; } }
  return p;
}
bool Foreground() { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }

class Menu final : public rex::ui::ImGuiDialog {
 public:
  explicit Menu(rex::ui::ImGuiDrawer* d) : ImGuiDialog(d) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    constexpr WORD kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kL3 = 0x0040, kR3 = 0x0080,
                   kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000;
    const bool fg = Foreground();
    PadState pad = fg ? ReadPad() : PadState{};
    WORD now = pad.buttons, pressed = now & ~prev_, released = prev_ & ~now;
    prev_ = now;
    auto key = [&](int vk) { bool d = fg && (GetAsyncKeyState(vk) & 0x8000); bool e = d && !kdown_[vk]; kdown_[vk] = d; return e; };
    const bool typing = io.WantTextInput;
    bool k_toggle = key(VK_F9), k_up = key(VK_UP), k_down = key(VK_DOWN), k_left = key(VK_LEFT), k_right = key(VK_RIGHT),
         k_ok = key(VK_RETURN), k_esc = key(VK_ESCAPE), k_tab = key(VK_TAB), k_pgup = key(VK_PRIOR), k_pgdn = key(VK_NEXT);
    bool open = g_open;
    bool combo = (now & (kL3 | kR3)) == (kL3 | kR3) && (pressed & (kL3 | kR3));
    if (combo || k_toggle) { open = !open; close_on_b_ = false; if (open) Build(); }
    if (open && (pressed & kB)) close_on_b_ = true;
    if (open && close_on_b_ && (released & kB)) { open = false; close_on_b_ = false; }
    if (open && k_esc) open = false;
    g_open = open;
    if (!open) return;

    // ---- navigation: LB/RB, D-pad left/right, Tab, PgUp/PgDn = tab; D-pad / arrows up/down = item ----
    const int nt = int(tabs_.size());
    int dt = ((pressed & kRB) || (pressed & kRight) || k_tab || k_pgdn || (!typing && k_right)) -
             ((pressed & kLB) || (pressed & kLeft) || k_pgup || (!typing && k_left));
    if (dt) { tab_ = (tab_ + nt + dt) % nt; scroll_ = true; }
    auto& items = tabs_[tab_].items;
    int& sel = sel_[tab_];
    auto selectable = [&](int i) { return i >= 0 && i < int(items.size()) && !items[i].code.empty(); };
    if (!selectable(sel)) { for (sel = 0; sel < int(items.size()) && !selectable(sel); ++sel) {} }
    int dy = ((pressed & kDown) || k_down) - ((pressed & kUp) || k_up);
    if (dy) {
      int n = int(items.size()), s = sel;
      for (int step = 0; step < n; ++step) { s = (s + n + dy) % n; if (selectable(s)) break; }
      sel = s; scroll_ = true;
    }
    int run = -1;
    if (((pressed & kA) || (k_ok && !typing)) && !combo) run = sel;

    // ---- draw ----
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::Begin("THE FORCE UNLEASHED  -  CHEATS##tfumenu", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.4f);
      // tab bar
      for (int t = 0; t < nt; ++t) {
        if (t) ImGui::SameLine();
        bool on = t == tab_;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button((std::string(" ") + tabs_[t].name + " ##t" + std::to_string(t)).c_str())) { tab_ = t; scroll_ = true; }
        if (on) ImGui::PopStyleColor();
      }
      ImGui::Separator();
      if (tab_ == kCharTab) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "Playing as: %s",
                           cur_.empty() ? "original characters" : CharName(cur_).c_str());
      }
      if (tab_ == 0 || tab_ == 4) ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", TfuCheatSummary().c_str());
      if (tab_ == kConsoleTab) {
        ImGui::SetNextItemWidth(1000.0f);
        ImGui::InputText("##lua", lua_, sizeof(lua_));
      }
      ImGui::BeginChild("##items", ImVec2(1000.0f, vp->WorkSize.y * 0.5f));
      std::string act = cur_;
      for (int i = 0; i < int(items.size()); ++i) {
        const Item& it = items[i];
        if (it.code.empty()) {
          if (i) ImGui::Spacing();
          ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", it.label.c_str());
          continue;
        }
        bool active = !act.empty() && it.code == "@as " + act;
        std::string text = std::string(active ? "  > " : "    ") + it.label + "##i" + std::to_string(i);
        if (ImGui::Selectable(text.c_str(), sel == i)) { sel = i; run = i; }
        if (sel == i && scroll_) ImGui::SetScrollHereY(0.5f);
      }
      scroll_ = false;
      ImGui::EndChild();
      ImGui::Separator();
      ImGui::TextWrapped("%s", MsgGet().c_str());
      ImGui::TextDisabled("Result: %s", TfuLuaLast().c_str());
      ImGui::PopFont();
      ImGui::TextDisabled("LB/RB or D-pad left/right: tab   Up/Down: move   A: select   B: close   (L3+R3 / F9)%s",
                          TfuLuaReady() ? "" : "   [game scripts not running yet]");
      if (!pad.ok) ImGui::TextDisabled("Keyboard: Tab/PgUp/PgDn or arrows, Enter, Esc");
    }
    ImGui::End();
    if (run >= 0 && selectable(run)) Do(items[run]);
  }

 private:
  static constexpr int kCharTab = 2, kConsoleTab = 5;
  void Build() {
    if (!tabs_.empty()) return;
    tabs_ = {{"Player", kPlayer}, {"Powers", kPowers}, {"Characters", BuildCharacters()},
             {"Unlock", kUnlock}, {"World", kWorld}, {"Console", kConsole}};
    sel_.assign(tabs_.size(), 0);
  }
  void Do(const Item& it) {
    std::string code = it.code;
    if (code == "@run") {
      if (lua_[0]) { TfuLuaQueue(lua_); Msg(std::string("Ran: ") + lua_); }
      return;
    }
    if (code.rfind("@cheat ", 0) == 0) { Msg(TfuCheat(code.substr(7))); return; }
    if (code == "@restoreall") {
      RestoreAll();
      TfuLuaQueue("@levelcostume restore");
      cur_.clear();
      Msg("Original characters back (Vader in the prologue, Starkiller after). Applies on the next level load.");
      return;
    }
    if (code.rfind("@as ", 0) == 0) {
      // Campaign level table swap (tfu_lua.cpp LevelCostume): every story level's forced costume -> this template,
      // moveset included. Replaces EquipCostume (never changed the model live, and DLC picks poisoned story saves)
      // and the VFS file redirect (never produced a visible change; crashed once with ReLoadWorld).
      std::string t = code.substr(4);
      if (StoryGuard() && IsDlcCostume(t)) {
        Msg("Blocked: " + CharName(t) + " is a DLC outfit - the story-only profile does not register it. Use -Campaign all.");
        return;
      }
      // 1) every later level load (campaign table), 2) right now if a player exists: load the pack into our own
      //    memory and run the game's dev 'Costume change' in place (tfu_lua.cpp SwapNow + sub_8227F510 hook).
      //    Verified 2026-11-18: Vader -> Starkiller -> Vader mid-prologue; new character has no voice (audio banks).
      TfuLuaQueue("@levelcostume " + t);
      // remember where the player stands: the post-spawn init after the swap puts the new player at the spawn point
      // (2026-11-18: swap back to Vader landed at the level start); the sub_8227F510 hook queues the move back.
      TfuLuaQueue("local p=F\"GetPlayer\"() if p~=nil then rawset(_G,'__tfu_swap_pos',F\"GetPosition\"(p)) end");
      TfuLuaQueue("@swapnow " + t);
      Msg("Playing as " + CharName(t) + ": swaps in place now if you are in a level (a second's hitch, no voice lines"
          " for characters the level did not load), and for every level load this session.");
      cur_ = t;
      return;
    }
    if (code == "@unlockall") {
      // UnlockPackage('Unlock_AllCostumes', ...) etc. return false (2026-11-17 01:12): those are cheat-table
      // tags, not packages. Costumes/crystals come from the game's cheat code screen (Unlock tab lists them).
      code = "local p = F\"GetPlayer\"()\nF\"UnlockAllPackages\"(p)\nF\"SetForceRanks\"(0, 3)\n"
             "return 'all upgrade packages unlocked, powers max'";
    }
    TfuLuaQueue(code);
    Msg("Done: " + it.label);
  }
  std::vector<Tab> tabs_;
  std::vector<int> sel_;
  int tab_ = 0;
  bool scroll_ = false;
  char lua_[512] = "=F\"GetHealth\"(F\"GetPlayer\"())";
  WORD prev_ = 0;
  bool kdown_[256] = {};
  bool close_on_b_ = false;
  bool force_ = false;
  std::string cur_;
};

rex::ui::ImGuiDrawer* g_drawer = nullptr;
}  // namespace

void TfuCreateMenu(rex::ui::ImGuiDrawer* drawer) {
  g_drawer = drawer;
  new Menu(drawer);   // registers itself with the drawer, lives for the session
  REXLOG_INFO("tfu: dev menu ready - L3+R3 or F9");
}
void TfuInstallMenuInputGate() {
  auto* rt = rex::Runtime::instance();
  auto* in = rt ? static_cast<rex::input::InputSystem*>(rt->input_system()) : nullptr;
  if (!in || !g_drawer) { REXLOG_WARN("tfu: menu input gate not installed"); return; }
  in->SetActiveCallbacks([] { return !g_open.load() && !TfuPuppetActive(); },
                         [] { return !g_open.load() && !g_drawer->GetIO().WantCaptureMouse; });
  REXLOG_INFO("tfu: menu input gate installed");
  RestoreAll();   // redirects are session-only: a bad one must never survive into the next boot (crash 2026-11-17 01:28)
}
