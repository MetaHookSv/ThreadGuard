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
