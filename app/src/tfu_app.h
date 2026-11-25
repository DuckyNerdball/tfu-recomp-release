// tfu - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS   // std::getenv is fine here
#endif
#include <cstdlib>
#include <rex/logging/macros.h>
#include <rex/rex_app.h>

void BsInstallCrashTrace();
void TfuCreateMenu(rex::ui::ImGuiDrawer* drawer);   // tfu_menu.cpp: L3+R3 / F9 dev menu
void TfuInstallMenuInputGate();                       // tfu_menu.cpp: after the runtime exists   // crash_trace.cpp: symbolized host stack -> logs\crash_*.txt

class TfuApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TfuApp>(new TfuApp(ctx, "tfu",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  void OnPostInitLogging() override {
    BsInstallCrashTrace();
    // tools\run.ps1 -Diag sets TFU_DIAG=1: kernel (xam content/device/UI), file system and sys calls at trace,
    // so save/load (XamShowDeviceSelectorUI, XamContentCreateEx, save:\ opens) shows up in the log.
    if (const char* d = std::getenv("TFU_DIAG"); d && d[0] == '1') {
      for (auto cat : {rex::log::krnl(), rex::log::fs(), rex::log::sys()})
        if (auto* l = rex::GetLoggerRaw(cat)) l->set_level(spdlog::level::trace);
      REXLOG_INFO("tfu: TFU_DIAG on - krnl/fs/sys logging at trace");
    }
  }
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  void OnPostSetup() override { TfuInstallMenuInputGate(); }
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override { TfuCreateMenu(drawer); }
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
