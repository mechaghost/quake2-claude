# Kex engine command & cvar reference

This file catalogs the commands and cvars the Quake II Rerelease Kex engine exposes, focused on what we use (or are likely to use) in this mod project. The authoritative dumps are in [kex-cmds.txt](kex-cmds.txt) (186 entries) and [kex-cvars.txt](kex-cvars.txt) (579 entries), produced by running `dump-kex-vocab.bat` which asks the engine to emit `cmdlist` + `cvarlist` on boot and then `quit`.

## How to refresh

Engine updates may add or rename cvars. To regenerate:

```
dump-kex-vocab.bat
```

This launches the engine headlessly (640×480 windowed, no intro), dumps both lists to `%USERPROFILE%\Saved Games\Nightdive Studios\Quake II\stdout.txt`, carves out the command/cvar sections, and writes them to `docs/kex-cmds.txt` + `docs/kex-cvars.txt`.

## Commands we use or care about

| Command | Purpose |
|---|---|
| `addbot` | Spawn one Kex bot into the current server. The right command — `bot_add` / `sv_addbot` do not exist. |
| `kickbot` | Remove a bot by name. |
| `bot_stuffcmd` | Send a console command to a bot. |
| `bot_reloadsettings` | Reload bot config files. |
| `map` / `gamemap` | Load a map. `gamemap` preserves DM state better; `map` is simpler. |
| `nav_save` / `nav_reset` / `nav_edit` | Navigation data editor tools. Useful if we ship custom maps. |
| `quit` | Shut the engine down. Our eval harness relies on this. |
| `cmdlist` / `cvarlist` | Dump all commands / cvars. How this doc was made. |
| `wait` | Defer next-command execution one frame. Useful to chain startup commands. |
| `vid_restart` | Reload video subsystem after changing `v_*` cvars. |

## Cvars we set from the launcher

| Cvar | Current value | Why |
|---|---|---|
| `v_windowmode` | `0` (windowed) | Kex-specific; `r_fullscreen`/`vid_fullscreen` don't exist on this engine. |
| `v_width` / `v_height` | `800` / `600` | Small dev window. |
| `g_showintromovie` | `0` | Skip studio/logo videos. |
| `g_nativeMouse` | `1` | Use OS cursor instead of SDL relative-mouse grab. |
| `deathmatch` | `1` | Force DM mode (latched; set in PreInitGame). |
| `coop` | `0` | Force DM mode. |
| `fraglimit` | `20` | DM end condition. |
| `g_dm_same_level` | `1` | Rematch stays on q2dm1 instead of cycling. |
| `ultron_eval_seconds` | `N` (harness-set) | Trigger telemetry + auto-quit in our mod. |
| `ultron_play_self` | `1` | Enable the usercmd intercept. |
| `ultron_autostart` | `1` | Run the DM bootstrap in InitGame. |

## Bot cvars worth knowing

Many of these are debug/dev tools for the engine's own bot system. We compete against the engine's bots, so being able to cripple them for A/B testing is useful.

| Cvar | Default | Effect |
|---|---|---|
| `bot_skill` | `2` | Difficulty 0–? (seen as `2` in config). |
| `bot_numBots` | `-1` | Target bot count (−1 = auto/none). |
| `bot_minClients` | `-1` | Fill bots up to this many total clients. Alternative to our `addbot` loop. |
| `bot_pause` | `0` | Freeze all engine bots. Use during debug to inspect positions. |
| `bot_aim_disabled` | `0` | Stock bots stop aiming. Claude vs. stationary bots. |
| `bot_aim_instant` | `0` | Stock bots get perfect aim — hard-mode test. |
| `bot_combatDisabled` | `0` | Stock bots don't fight back. Pure movement/target test. |
| `bot_move_disable` | `0` | Stock bots don't move. |
| `bot_senses_disabled` | `0` | Stock bots can't see. |
| `bot_weapons_disable` | `0` | Stock bots can't use weapons. |
| `bot_jump_disable` | `0` | Stock bots can't jump. |
| `bot_meleeDisabled` | `0` | Stock bots can't melee. |
| `bot_followPlayer` | `0` | Stock bots follow the human — good for aim-practice. |
| `bot_respawnTime` | `0` | Delay before bots respawn. |
| `bot_attackHumansOnly` | `0` | Stock bots ignore other bots. |
| `bot_chat_enable` | `1` | Whether bots emit chat lines. |
| `bot_debugSystem` / `bot_aim_debug` / `bot_move_debug` | `0` | Render on-screen bot debug. |
| `bot_drawNavNodes` / `bot_showPaths` / `bot_showBots` | `0` | Visualize nav + bot state. |

## Suggested alternative to our `addbot` loop

Instead of firing `addbot` from `ClientBegin` when `active_players().count < 2`, we could just set:

```cpp
gi.cvar_set("bot_minClients", "2");   // in InitGame
```

...which tells the engine to keep at least 2 clients in the game at all times, auto-spawning bots as needed. Less code, better rematch behavior (auto-refills after a disconnect). Not switching today since the existing path works, but noting for v2.

## Mouse grab notes

There is **no** `in_nograb` cvar in this Kex build (not in the 579-cvar dump). Our belt-and-suspenders `+set in_nograb 1` is a no-op. The actual mouse-release knob is `g_nativeMouse 1`. If the mouse is still captured in windowed mode, other candidates to try (present in the dump):

| Cvar | Guess |
|---|---|
| `g_nativeMouse` | Already using; `1` = OS cursor. |
| `cl_mousesmooth` | Smoothing, not grab. |
| `m_pitch` / `m_yaw` | Per-axis sensitivity (can zero to neutralize). |
| `freelook` | `0` disables mouse look entirely. |

## Video cvars worth knowing

| Cvar | Notes |
|---|---|
| `v_windowmode` | 0 = windowed, 1 = fullscreen. |
| `v_width` / `v_height` | Window dimensions. |
| `v_refresh` | Refresh rate. |
| `v_vsync` | Vsync on/off. |
| `v_displaymonitor` | Which monitor in multi-display setups. |
| `v_windowXPos` / `v_windowYPos` | Window placement. |
| `r_resolutionscale` | Dynamic resolution (0–1). |
| `r_rhirenderfamily` | `vulkan` or `d3d11`. |
| `cl_engineFPS` | Max framerate. Default 165. Lower for deterministic eval. |

## Render-cosmetic cvars (for screen recording)

| Cvar | Effect |
|---|---|
| `cl_skipHud` | Hide HUD. |
| `cl_skipCrossHair` | Hide crosshair. |
| `cl_muzzleflashes` | Disable muzzle flashes. |
| `r_bloom` | Bloom on/off. |
| `r_motionBlur` | Motion blur. |
| `r_shadows` | Shadows. |
| `con_showfps` | Show FPS counter. |

## Convention: per-player cvars have `_0`/`_1`/… suffixes

From the Steam guide: `cl_run_0`, `hand_0`, `fov_0`, `crosshair_0`, `bobskip_0`, `crosshaircolor_0`. The suffix is the split-screen player index. Most of our work uses player 0 (the human slot).
