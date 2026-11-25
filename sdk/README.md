# SDK patch

`rexglue-tfu.patch` is the set of changes this project needs on top of the public ReXGlue SDK
(https://github.com/rexglue/rexglue-sdk), made against the commit in `BASE_COMMIT.txt`. It adds, among others:
the pixel-shader constant options used by the look presets (`ps_const_mul`, `ps_const_log`, `skip_pixel_shaders`,
`sharp_composite_ps`), the draw/hitch logging used by `-Probe`, and the input gate (`SetActiveCallbacks`) the
in-game menu uses. It contains SDK source changes only, no game data. ReXGlue is BSD-licensed (derived from Xenia);
its licence text stays with the SDK checkout.

`tools\Get-Sdk.ps1` downloads the SDK at that commit, applies the patch and builds it into `.deps\rexglue-sdk`.
`Play.ps1` runs it automatically when no built SDK is found.

To refresh the patch after changing the SDK locally (run inside the SDK folder):
```powershell
git diff HEAD --ignore-submodules=all --binary -- . ':!thirdparty' | Out-File -Encoding ascii <repo>\sdk\rexglue-tfu.patch
git rev-parse HEAD | Out-File -Encoding ascii <repo>\sdk\BASE_COMMIT.txt
```
