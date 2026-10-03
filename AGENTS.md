# AGENTS.md

This file provides guidance and important rules working with code in this repository.

## When coding / building plan

- Use a progressive disclosure approach for agent coding in this repository: start from high-level
  information in the Basic Memory knowledge base first, and only locate/read specific files or
  symbols when necessary, instead of expanding a large amount of context at once.

#### Basic Memory knowledge base (project-scoped, `memory/`)

- Notes live in `memory/` (markdown with YAML frontmatter: `title`/`type`/`permalink`), tracked in git.
- This repository contains the standalone ThreadGuard plugin, extracted from MetaHookSv
  `Plugins/ThreadGuard`. Its notes were migrated from MetaHookSv and adapted to the CMake workspace;
  see `memory/project_overview.md` for scope and provenance.
- Basic Memory is registered as MCP server `basic-memory`, pinned to the `threadguard` project
  (project-level `.mcp.json`, mirrored by `.codex/config.toml`). The `metahooksv` project belongs to
  the source repository and also holds the engine-private symbol inventory
  (`memory/privatevars/threadguard-privatevars.md`), which is summarized but not copied here.
- Prefer Basic Memory MCP tools (`search_notes` / `read_note` / `write_note` / `edit_note`) only when
  their project resolves to this repository's `memory/` directory. Verify the project binding before
  writing; when no matching project is available, read and edit the local markdown files directly.
- Notes use the `threadguard/` permalink prefix to distinguish them from the source repository.
- Historical records are not current evidence: the migrated note retains MetaHookSv paths
  (`Plugins/ThreadGuard/`, `ThreadGuard.vcxproj`), while the current sources are `src/<file>`. Do not
  extend an old statement to a new change without checking the code.

#### High-level information in this repository (read corresponding notes first)

- Project overview, provenance, module routing, termination gates and dependencies: `project_overview`

#### When notes are insufficient: source entry points (query and read on demand)

- Build: `CMakeLists.txt`, `cmake/Sources.cmake` (explicit compile list: 4 plugin units plus the SDK's
  `interface.cpp`), `cmake/Dependencies.cmake`, `cmake/VCLTL.cmake`,
  `scripts/build-ThreadGuard-x86-{Debug,Release}.bat`
- Plugin sources: `src/`; lifecycle `src/plugins.cpp`, module routing and hooks `src/privatehook.cpp`,
  manager registry and API wrappers `src/ThreadManager.cpp`, `_restart` fix `src/exportfuncs.cpp`,
  `IEngine` state constants `src/enginedef.h`
- Public API / interface: `src/ThreadManager.h` declares `IThreadManager` and the three
  `hookflag_*` values; MetaHook's `include/metahook.h`, `include/HLSDK/`, `include/Interface/` and
  `IEngine.h` are consumed as an SDK (the launcher is never built here)
- gamedata: `scripts/manifests/threadguard.json` (the single `engine` / `eng` global),
  `scripts/sync-gamedata.py`, `scripts/validate-gamedata.py`; the build-time sync prunes the upstream
  catalog into the nested `metahook/gamedata/threadguard/` directory, which the host launcher merges
- Docs: `README.md` / `README.zh-CN.md`. There is no `docs/` directory, no assets and no test suite
- External sources, all read-only inputs: `METAHOOK_SOURCE_PATH` (must provide `include/metahook.h`,
  `include/HLSDK` and `include/Interface`). Empty paths fall back to pinned FetchContent; VC-LTL
  5.3.1 is downloaded into `thirdparty/cache`. No third-party library is linked and no Capstone
  headers are needed
- Build output: `build/x86/<configuration>/`; install output: `install/x86/<configuration>/svencoop/`.
  Neither is tracked, and nothing is deployed to the game automatically

#### Progressive disclosure key points

- Read notes first, then locate a single file/symbol; do not read the whole repository at once.
- Prefer correctly scoped Basic Memory MCP tools for knowledge retrieval; otherwise use the local
  notes before reading source.
- Prefer Context7 for external dependency/library usage (query on demand).

## Repository rules

- Preserve the MetaHook API, plugin exports and calling conventions. Match the naming, indentation and
  comment style of the files you touch.
- Resolve engine symbols only through the host gamedata contract. Do not add a signature-scan
  fallback for `eng`; keep the catalog name `eng` (not `engine`) and the fatal
  `Could not resolve gamedata symbol: ...` diagnostic.
- Keep the wrapper semantics explicit: `NewCreateThread` must still duplicate the handle before
  handing it to a manager and must return the original handle, and the `Sleep(1)` /
  `WaitForSingleObject(0)` short-circuits must stay gated on `StartTermination` and, for `Sleep`, on
  the non-main-thread check.
- Keep the termination gate (`DLL_CLOSE` / `DLL_RESTART` via `(*eng)->GetState()`) and the
  per-module hook flags intact; the `svencoop` game-directory gates for `server.dll` and the engine
  `FreeLibrary` hook are deliberate.
- The fixed `MAXIMUM_WAIT_OBJECTS` handle pool and its "already signaled slot" reclamation are part of
  the design; a full pool must keep failing loudly rather than silently dropping a thread.
- The `#if 0` closed-thread path is intentionally disabled. Do not re-enable it as part of an
  unrelated change.
- When gamedata usage changes, update `scripts/manifests/threadguard.json` in the same change.
- Do not modify external sources or third-party sources; MetaHook is a read-only build input.
- The plugin builds for MSVC x86 only. Keep the static CRT / VC-LTL, C++20 and warning-level settings
  in `CMakeLists.txt` in sync with the other standalone plugin repositories.
- Verification distinguishes build checks from a real game run: there is no test suite here, and the
  hook routing, the shutdown wait and the `_restart` fix can only be observed in a live game. Claims
  about in-game behavior must not be made without evidence. Documentation changes need content, path
  and format checks, not a plugin rebuild.

## Explore SKILLs

- Project-level skills, when present, live in `.claude/skills` no matter what harness tool is being
  used.
