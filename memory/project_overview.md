---
title: project_overview
type: note
permalink: threadguard/project-overview
---

# ThreadGuard

ThreadGuard is a MetaHook plugin that intercepts the thread APIs (`CreateThread`,
`WaitForSingleObject`, `Sleep`) inside target modules and centrally waits for those threads to
terminate during shutdown, reducing crashes and long hangs caused by background threads that keep
running after their module is unloaded.

## Provenance

This repository is the standalone ThreadGuard plugin, extracted from MetaHookSv
(`Plugins/ThreadGuard/`) into its own CMake workspace, aligned with the standalone Renderer,
PrecacheManager and HeapPatch projects. This note was migrated from MetaHookSv
`memory/ThreadGuard.md` and adapted to the new layout: plugin sources moved to `src/`, the
`ThreadGuard.vcxproj` / `MetaHook.sln` integration was replaced by CMake plus a self-owned gamedata
catalog. The symbol-by-symbol inventory that the source note links as `threadguard-privatevars`
remains in the source repository (`memory/privatevars/threadguard-privatevars.md`, `metahooksv`
project) and is summarized here. The `metahooksv` Basic Memory project belongs to the source
repository; notes here use the `threadguard` project and the `threadguard/` permalink prefix.

## Responsibilities and entry points

- `src/plugins.cpp`: `IPluginsV4` lifecycle. `LoadEngine` checks the host API version, collects the
  file system and engine type/buildnum, records `g_MainThreadId`, registers `DllLoadNotification` and
  resolves the engine `IEngine*` slot; `LoadClient` copies the export table and installs the
  `_restart` command hook; `ExitGame` calls `Engine_WaitForShutdown`; `Shutdown` unregisters the
  DLL-notification callback.
- `src/privatehook.cpp`: the module routing layer — per-module hook installation/uninstallation, the
  `FreeLibrary` IAT hooks, and the termination entry points gated by the engine state.
- `src/ThreadManager.cpp`, `src/ThreadManager.h`: the thread management layer — the manager registry,
  the API wrappers and `CThreadManager` itself.
- `src/exportfuncs.cpp`: `EngineCommand_InstallHook` and the `_restart` fix.
- `src/enginedef.h`: the `DLL_INACTIVE` / `DLL_ACTIVE` / `DLL_PAUSED` / `DLL_CLOSE` / `DLL_TRANS` /
  `DLL_RESTART` state constants.

Managed modules and the hooks each receives:

| Module | Flags | Extra |
| --- | --- | --- |
| engine (`hw.dll`, or the blob engine) | `CreateThread \| WaitForSingleObject \| Sleep` | `FreeLibrary` IAT hook, only when the game directory is `svencoop` |
| `GameUI.dll` | `CreateThread \| WaitForSingleObject` | `FreeLibrary` IAT hook |
| `ServerBrowser.dll` | `CreateThread \| WaitForSingleObject` | — |
| `server.dll` | `CreateThread` only | enabled only when the game directory is `svencoop` |

`GameUI.dll` and `ServerBrowser.dll` are skipped when the module imports `steam_api.dll` without
importing `kernel32.dll!CreateThread` — the comment in the code is "It use callback instead of
creating stupid thread", i.e. such a module is considered to use the callback model.

## Architecture

```mermaid
flowchart TD
    A[IPluginsV4::LoadEngine] --> B[RegisterLoadDllNotificationCallback]
    A --> C[Engine_FillAddress resolves the eng global from gamedata]
    B --> D[DllLoadNotification]
    D -->|engine load| E[Engine_InstallHook: CreateThread + WaitForSingleObject + Sleep, FreeLibrary IAT hook under svencoop]
    D -->|GameUI.dll / ServerBrowser.dll| F[InstallHook: CreateThread + WaitForSingleObject]
    D -->|server.dll under svencoop| G[InstallHook: CreateThread]
    E --> H[NewCreateThread / NewWaitForSingleObject / NewSleep]
    F --> H
    G --> H
    H --> I[FindThreadManagerByVirtualAddress(_ReturnAddress)]
    I --> J[CThreadManager::OnCreateThread / OnWaitForSingleObject / OnSleep]
    K[IPluginsV4::ExitGame or the hooked FreeLibrary] --> L[StartTermination]
    L --> M[WaitForAliveThreadsToShutdown]
```

Wrapper behavior:

- `NewCreateThread` calls the real `CreateThread` first, then attributes the call to a manager with
  `FindThreadManagerByVirtualAddress(_ReturnAddress())` — the read-only `.text` range of each managed
  module decides ownership. The returned handle is duplicated (`DuplicateHandle` with
  `THREAD_ALL_ACCESS` / `DUPLICATE_SAME_ACCESS`) and the duplicate is handed to
  `OnCreateThread`, so the manager owns a handle whose lifetime it controls; the original handle is
  returned to the caller unchanged.
- `NewWaitForSingleObject` only short-circuits `dwMilliseconds == 0`; while terminating,
  `OnWaitForSingleObject` returns true and the wrapper reports `WAIT_OBJECT_0` immediately instead of
  polling. Every other timeout forwards to the real `WaitForSingleObject`.
- `NewSleep` only short-circuits `dwMilliseconds == 1`; while terminating, `OnSleep` returns true for
  any thread except `g_MainThreadId`, and the wrapper calls `ExitThread(0)`. Other sleep durations
  forward to the real `Sleep`.
- `StartTermination` only sets `m_bStartTermination`, which is what enables the two short-circuits
  above.
- `WaitForAliveThreadsToShutdown` snapshots the alive handles under `m_ThreadListLock`, calls
  `WaitForMultipleObjects(numThreads, hThreads, TRUE, INFINITE)`, clears the array and finally
  `CloseHandle`s each handle.

Termination trigger:

- `Engine_WaitForShutdown` (from `ExitGame`) waits for the engine manager only when
  `GetEngineDLLState()` is `DLL_CLOSE` or `DLL_RESTART`; the state comes from
  `(*eng)->GetState()` — `eng` is dereferenced exactly once, and the function reports `DLL_INACTIVE`
  when the slot is null.
- `NewFreeLibrary_Engine` (installed in the engine's import table, `svencoop` only) waits for the
  `server.dll` manager when the module being freed is the tracked one; `NewFreeLibrary_GameUI` does
  the same for the `ServerBrowser.dll` manager. Both then forward to the real `FreeLibrary`. This is
  how the plugin catches the unload of modules that it does not own.
- `ServerDLL_WaitForShutdown` and `ServerBrowser_WaitForShutdown` apply the same
  `DLL_CLOSE` / `DLL_RESTART` gate as the engine path.

`_restart` command fix:

- `EngineCommand_InstallHook` (from `LoadClient`) looks up `shutdownserver` with `FindCmd`, stores its
  function and then installs `Host_Quit_Restart_f` over `_restart` with `HookCmd`. The replacement
  runs `shutdownserver` first and then the original `_restart` function, fixing Valve's incomplete
  server shutdown that leaked resources such as `CSteam3Server`. A missing `shutdownserver` command
  is fatal (`Command "shutdownserver" not found!`).

## Dependencies

- **MetaHook API** (>= 109, enforced by a `static_assert` in `src/privatehook.cpp`): the gamedata trio
  `ResolveGameSymbol` / `GetModuleCRC64` / `GetGameSymbolStatusString` (through `GamedataResolvePtr`),
  `RegisterLoadDllNotificationCallback` / `UnregisterLoadDllNotificationCallback`, `IATHook` /
  `BlobIATHook` / `UnHook`, `HookCmd` / `FindCmd`, `GetModuleBase`,
  `GetBlobModuleImageBase`, `GetSectionByName`, `GetEngineModule`,
  `GetBlobEngineModule`, `GetGameDirectory`, and `SysError`.
- **Engine interface**: `IEngine::GetState()` (`IEngine.h`) from the MetaHook SDK, through the
  gamedata-resolved `IEngine**` slot.
- **Win32 threading and module APIs**: `CreateThread`, `DuplicateHandle`,
  `WaitForSingleObject`, `WaitForMultipleObjects`, `Sleep`, `ExitThread`, `CloseHandle`,
  `FreeLibrary`, `GetCurrentThreadId`.
- **Build-only inputs**: the MetaHook source tree (read-only) and VC-LTL 5.3.1. No third-party library
  is linked, and no Capstone headers are needed.

## Repository layout

- `src/plugins.cpp`, `src/plugins.h` — plugin lifecycle, globals and the gamedata helper.
- `src/privatehook.cpp`, `src/privatehook.h` — module routing, hooks and the termination gate.
- `src/ThreadManager.cpp`, `src/ThreadManager.h` — manager registry, API wrappers, `CThreadManager`.
- `src/exportfuncs.cpp`, `src/exportfuncs.h` — the `_restart` command fix.
- `src/enginedef.h` — `IEngine` state constants.
- `CMakeLists.txt`, `cmake/Sources.cmake` (explicit compile list), `cmake/Dependencies.cmake`,
  `cmake/VCLTL.cmake` — build.
- `scripts/build-ThreadGuard-x86-{Debug,Release}.bat` — configure/build/install entry points.
- `scripts/manifests/threadguard.json`, `scripts/sync-gamedata.py`, `scripts/validate-gamedata.py` —
  gamedata synchronization and validation.
- `README.md`, `README.zh-CN.md` — install, managed-module table and build documentation. There is no
  `docs/` directory, no assets and no test suite.

## Build and data flow

`scripts/build-ThreadGuard-x86-{Debug,Release}.bat` → CMake (Visual Studio 17 2022, `-A Win32`) →
compile the DLL → install. The build uses MSVC x86 / C++20, a static CRT and VC-LTL 5.3.1, with the
explicit compile list in `cmake/Sources.cmake` (4 plugin units plus the SDK's `interface.cpp`).
`scripts/manifests/threadguard.json` → `scripts/sync-gamedata.py` → pruned catalog under
`build/x86/<Configuration>/assets/svencoop/metahook/gamedata/threadguard`, validated before the
plugin target builds; disable with `-DTHREADGUARD_SYNC_GAMEDATA=OFF`.
The manifest declares the single `engine` / `eng` global for 11 engine snapshots (`cof-5936`,
`hl-10210`, `hl-3248`, `hl-3266`, `hl-3329`, `hl-3647`, `hl-4554`, `hl-6153`, `hl-8684`,
`svencoop-10257`, `svencoop-8948`); catalog coverage is not a validation statement.
Install output is `install/x86/<Configuration>/svencoop/metahook/{plugins,gamedata/threadguard}` (DLL
plus PDB); nothing is deployed into the game automatically.

## Notes

- The thread-handle pool is a fixed `HANDLE m_hAliveThread[MAXIMUM_WAIT_OBJECTS]`. When it is full,
  `OnCreateThread` tries to reclaim a slot whose handle is already signaled
  (`WaitForSingleObject(handle, 0)`); if that also fails it calls
  `SysError("Failed to insert thread to thread manager!")`.
- Only two call shapes are intercepted: `WaitForSingleObject` with `dwMilliseconds == 0` and
  `Sleep(1)`. All other waits and sleeps run uncontrolled, which is deliberate — the plugin only
  targets the busy-wait and polling loops that would otherwise keep a terminating thread alive.
- Actual waiting for thread termination happens only when `GetEngineDLLState()` is `DLL_CLOSE` or
  `DLL_RESTART`; on a level transition or a normal frame the state gate keeps the plugin out of the
  way.
- Attribution uses `_ReturnAddress()` against each manager's `.text` section, so a module that
  forwards thread creation through a helper outside its own `.text` (or through a delay-loaded import
  that bypasses the IAT) is not tracked. `FindThreadManagerByVirtualAddress` takes a mutex over the
  registry while it scans.
- Hooks are installed per import table (`IATHook` for modules, `BlobIATHook` for the blob engine), so
  only calls that go through that module's import table are intercepted.
- `server.dll` tracking is intentionally limited to `CreateThread` — the comment in the code is
  "Fuck off the CPlayerDatabase_RunThread" — and, like the engine `FreeLibrary` hook, is enabled only
  when the game directory is `svencoop`.
- The `m_hClosedThread` array, `FindClosedThread` / `AddClosedThread` /
  `FindAndRemoveSignaledClosedThread`, `WaitForClosedThreadsToShutdown` and the related
  `DllMain` / `CloseHandle` / `TerminateThread` hook members are disabled with `#if 0`: only the
  alive-thread path is currently maintained.
- Since the #855 change the address resolution is gamedata-only: `Engine_FillAddress` resolves the
  GLOBAL `eng` with the real module base and aborts with a specific
  `Could not resolve gamedata symbol: eng (module engine, <status>)` diagnostic (symbol, status,
  engine buildnum) instead of the old silent-null `CEngine not found`. Mirror-space conversion,
  section preparation and the unused no-argument `Engine_InstallHook` / `Engine_UninstallHook`
  declarations are gone. The catalog symbol name is `eng`, not `engine`; the plugin variable and the
  validator's required-symbol entry use the same name.
- `LoadEngine` also rejects a host whose `MetaHookAPIVersion` is lower than the SDK constant with a
  `Sys_Error`, so a mismatched launcher fails fast rather than resolving against a different API.

## Callers (optional)

- The host MetaHook loader drives `Init` / `LoadEngine` / `LoadClient` / `ExitGame` / `Shutdown` and
  loads `metahook/plugins/ThreadGuard.dll` from `plugins.lst`.
- The DLL-notification system registered in `LoadEngine` calls `DllLoadNotification` on every engine
  and target-module load/unload, which is what installs and removes the hooks.
- The engine render/client loop reaches the wrappers through the hooked import entries; the engine's
  own `_restart` command dispatch reaches `Host_Quit_Restart_f`.
- `ExitGame` calls `Engine_WaitForShutdown(g_pMetaHookAPI->GetEngineModule(),
  g_pMetaHookAPI->GetBlobEngineModule())`, and the hooked `FreeLibrary` paths trigger
  `ServerDLL_WaitForShutdown` / `ServerBrowser_WaitForShutdown` before module release.
- The host launcher merges `metahook/gamedata/threadguard` into its gamedata catalog.

## External documentation

`README.md` and `README.zh-CN.md` cover installation, the managed-module table, the `_restart` fix
and the build. The engine-private symbol inventory lives in the source repository's
`memory/privatevars/threadguard-privatevars.md`.
