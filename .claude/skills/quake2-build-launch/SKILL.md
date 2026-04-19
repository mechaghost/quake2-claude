---
name: quake2-build-launch
description: Build the Quake II Rerelease mod DLL and launch the game with the right settings (windowed, no intros, mouse free). Use when the user asks to build, compile, rebuild, launch, run, start, or play the mod. Use on any code change that needs to ship to the running engine. Covers the MSBuild + vcpkg pipeline and the Kex-specific cvars we've proven out.
tools: Read, Bash, Edit
---

# Building and launching the quake2-claude mod

This repo compiles a Quake II Rerelease game DLL (`game_x64.dll`) and drops it where the Kex engine picks it up.

## One-shot commands (default case)

Rebuild-if-needed + launch:
```
modlaunch.bat
```

Force full rebuild + launch:
```
modlaunch.bat -Build
```

Build only (no launch, for CI-style verification):
```
powershell -File modlaunch.ps1 -NoBuild:$false -NoLaunch
```

## Pipeline facts

- **Source**: `rerelease/` in this repo. Active files are `.cpp`/`.h`; `original/` is read-only reference.
- **Build tool**: MSBuild 2022 at `C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe`.
- **Deps**: vcpkg at `D:\dev\vcpkg` (fmt + jsoncpp, manifest mode via `rerelease/vcpkg.json`). The launcher sets `VCPKG_ROOT` env var before MSBuild.
- **Output**: DLL drops into `%USERPROFILE%\Saved Games\Nightdive Studios\Quake II\mymod\game_x64.dll` (mod name from `modname.txt`, currently `mymod`).
- **OutDir/IntDir**: launcher passes forward-slash paths to avoid cmd-quoting bugs with trailing backslashes (see `modlaunch.ps1` comments).

## What the launcher does under the hood

From `modlaunch.ps1`:
- Reads mod name from `modname.txt`.
- Smart-skip build if newest `.cpp/.h/.hpp` is older than the DLL (ignores `x64/`, `vcpkg_installed/`, etc.).
- Starts `D:\SteamLibrary\steamapps\common\Quake 2\rerelease\quake2ex_steam.exe` with these args unless overridden:
  - `+set game mymod` — load our mod dir.
  - `+set deathmatch 1 +map q2dm1` — bypass intros and land in DM directly.
  - `+set v_windowmode 0 +set v_width 800 +set v_height 600` — **Kex-specific** windowed mode (the `r_*`/`vid_*` cvars are ignored on this engine).
  - `+set g_showintromovie 0` — skip studio/logo videos.
  - `+set g_nativeMouse 1 +set in_nograb 1` — release the mouse cursor so it's not grabbed by the engine.

## Launcher flags

```
modlaunch.bat -WithIntro             # keep the studio logos
modlaunch.bat -Fullscreen            # force fullscreen (default is windowed)
modlaunch.bat -StartMap q2dm3        # load a different map
modlaunch.bat -EvalSeconds 30        # mod auto-quits after 30s (drives eval harness)
modlaunch.bat -Config Debug          # Debug DLL (default is Release)
modlaunch.bat -NoBuild               # skip build, just launch
modlaunch.bat -NoLaunch              # build only
modlaunch.bat -Clean                 # wipe x64/, vcpkg_installed/, and the staged DLL
```

## Troubleshooting

- **"MSBuild not found"** → install VS 2022 Community with C++ workload, or update `$MSBuild` in `modlaunch.ps1`.
- **vcpkg errors** → run `D:\dev\vcpkg\vcpkg.exe integrate install` once.
- **`/utf-8` error in fmt/base.h** → should be patched in `rerelease/game.vcxproj` (both Debug+Release ClCompile).
- **`formatter::format` cannot convert `this` pointer** → fmt v12 wants `const` on `format()`; patched in `g_local.h:3543` and `q_vec3.h:539`.
- **Game launches fullscreen despite `-Fullscreen:$false`** → wrong cvars. Kex uses `v_*` (not `r_*`/`vid_*`). See `modlaunch.ps1` for the working set.
- **Mouse grabbed in windowed mode** → set `g_nativeMouse 1`; also try `in_nograb 1` as fallback.
- **CRASHLOG.TXT at `D:\SteamLibrary\steamapps\common\Quake 2\rerelease\CRASHLOG.TXT`** holds the last engine crash backtrace.

## Silent launch without build (for repeated playtests)

```
run.bat
```
Reads `modname.txt`, launches the engine with `+set game <mod>`, exits immediately. No build, no PowerShell wrapper.

## Commit policy for this project

User granted standing authorization to auto-commit and auto-push to `origin` (never `upstream` — that's id-Software's repo). Ship meaningful units of work eagerly; no approval needed. See memory file `feedback_auto_commit_push.md`.
