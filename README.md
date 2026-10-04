# ThreadGuard

## Wait for background threads before a module is unloaded

GoldSrc's Win32 thread code can still be running when the module that created a thread is unloaded, which leads to crashes or long hangs on shutdown and on `_restart`. ThreadGuard hooks `CreateThread` / `WaitForSingleObject` / `Sleep` inside the managed modules, records every thread handle they create, and waits for those threads to terminate before the module is released.

Managed modules:

| Module | Notes |
|---|---|
| `hw.dll` (engine) | always |
| `GameUI.dll` | skipped when it uses the `steam_api.dll` callback model and does not import `CreateThread` |
| `ServerBrowser.dll` | same as `GameUI.dll` |
| `server.dll` | Sven Co-op (`svencoop` game directory) only |

It also fixes a Valve bug where the `_restart` command did not shut the server down properly, leaking resources such as `CSteam3Server`: the hooked `_restart` runs `shutdownserver` first.

# Install

1. Download and install [MetaHookSv](https://github.com/hzqst/MetaHookSv).

2. Build or download .dll, put it into `/SteamLibrary/steamapps/common/Sven Co-op/svencoop/metahook/plugins` directory.

3. Add `ThreadGuard.dll` in `/SteamLibrary/steamapps/common/Sven Co-op/svencoop/metahook/configs/plugins.lst` as a newline.

4. Keep the `svencoop/metahook/gamedata/threadguard` directory shipped next to the plugin: it carries the `eng` global the plugin resolves at load time.

5. Enjoy.

## F5 debugging (optional)

Install MetaHook and enable this plugin in the game's `plugins.lst` first. Configure a standalone Visual Studio Win32 solution:

```powershell
cmake -S . -B build/launch -G "Visual Studio 17 2022" -A Win32 -DMETAHOOKSV_ENABLE_LAUNCH_GAME=ON
```

Open the solution, select **LaunchGame** and press **F5**. **DeployGame** builds this plugin and its dependencies, stages Install, and copies plugin DLLs/PDBs/resources before the native debugger starts the existing game launcher. Root launchers/runtime files and plugin lists remain unchanged. Set VS to build before running and **Do not launch** on build errors; stop the game before redeploying. Ordinary builds do not deploy.

`METAHOOKSV_GAME_DIRECTORY` defaults to Steam discovery; `METAHOOKSV_GAME_APPID` defaults to `225840`. Set `METAHOOKSV_GAME_MOD` for a custom mod and `METAHOOKSV_GAME_ARGUMENTS` for extra arguments. Debug and Release are supported.

The shared module uses `METAHOOKSV_LAUNCH_GAME_MODULE_DIR`, the surrounding MetaHookSv checkout, or a pinned source archive. Without Installer sources, it downloads the self-contained CLI from GitHub `latest` (no .NET required); `METAHOOKSV_INSTALLER_RELEASE` selects a fixed tag, and `METAHOOKSV_INSTALLER_CLI_EXECUTABLE` supplies an offline EXE. Plugin mode requires v20261004c or later. Valid caches under `build/launch/launch-game/installer/<release>` are reused without update checks; select another tag or clear that private cache to upgrade. `GH_TOKEN`/`GITHUB_TOKEN` may be supplied through the environment if GitHub API rate limits prevent the first download. The feature defaults OFF and performs no extra downloads when disabled.

# Build

Requirements: Windows, Visual Studio 2022, CMake 3.21 or newer, Python 3.8 or newer. The first configure downloads VC-LTL 5.3.1 into `thirdparty/cache` and synchronizes the gamedata catalog.

1. Run `scripts\build-ThreadGuard-x86-Release.bat` (or `scripts\build-ThreadGuard-x86-Debug.bat`).

2. The plugin, its PDB and the gamedata catalog are installed to `install\x86\<Configuration>\svencoop\metahook`.

3. Copy `ThreadGuard.dll` and `gamedata\threadguard` into `svencoop/metahook`, as described in Install.

The MetaHook SDK is fetched automatically at a pinned commit. To build against a local MetaHook source tree instead, pass it on the command line or export the same environment variable before configuring:

```
scripts\build-ThreadGuard-x86-Release.bat -DMETAHOOK_SOURCE_PATH=D:\MetaHook
```

The path is the repository root that provides `include/metahook.h`, `include/HLSDK` and `include/Interface`.

ThreadGuard resolves its symbols from the MetaHook gamedata catalog, links no third-party library and needs no Capstone headers. Pass `-DTHREADGUARD_SYNC_GAMEDATA=OFF` to build without downloading gamedata.
