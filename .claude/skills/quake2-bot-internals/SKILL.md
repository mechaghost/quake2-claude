---
name: quake2-bot-internals
description: Architecture reference for the Claude-driven player bot in this Quake II Rerelease mod. Use when modifying the bot's perception, aim, fire, movement, weapon selection, or when adding new capabilities (pathfinding, items, hearing). Also use when diagnosing why the bot isn't playing correctly. Captures the load-bearing invariants that are easy to get wrong (don't flip SVF_BOT, delta_angles not ucmd->angles, ±400 move speed, SVF_BOT gate on bot_exports).
tools: Read, Edit, Grep, Glob, Bash
---

# Bot architecture reference

The bot lives in `rerelease/ultron_bot.cpp` / `ultron_bot.h` and hooks into three places in the stock game DLL.

## Input is killed unconditionally for the human slot

When `ultron_play_self=1`, `Ultron_Bot_Command` **always** runs for the human's `ClientThink` — including during intermission, respawn, dead, and spectator states. The function's first action is to zero `ucmd->buttons`, `ucmd->angles`, `ucmd->forwardmove`, `ucmd->sidemove`. This guarantees no keyboard or mouse input from the user ever reaches pmove / weapon fire / view angles.

In non-combat states (intermission / respawn / deadflag / spectator), the bot pulses `BUTTON_ATTACK` on a cadence to:
- Exit intermission (~1 pulse per 6s after `intermissiontime + 5s`).
- Respawn (~1 pulse per 1s while `awaiting_respawn` or `deadflag`).
- Do nothing while spectator (just the zeroed input).

So the user can leave the game running fully hands-off and watch the Claude bot play indefinitely — even match-end → intermission → next map → respawn loops.

## Core design decision: intercept `usercmd_t`, don't flip SVF_BOT

The human's character is driven by mutating `*ucmd` inside `ClientThink` before the engine reads it. The human's `edict_t` is **never** marked `SVF_BOT` because the engine routes `ClientThink` only to non-SVF_BOT edicts — flipping that flag silences input entirely.

**Entry point**: `p_client.cpp` `ClientThink(edict_t*, usercmd_t*)` at line ~3150. Our hook runs **before** `client->oldbuttons = client->buttons;` (around line 3162) so our synthesized buttons participate in the latched-buttons edge detect used by weapon fire.

**Gate**:
```cpp
if (ultron_play_self && ultron_play_self->integer
    && Ultron_IsHuman(ent)
    && !level.intermissiontime
    && !ent->client->awaiting_respawn
    && !ent->client->resp.spectator
    && !ent->deadflag)
{
    Ultron_Bot_Command(ent, ucmd);
}
```

## Identity (`Ultron_IsHuman`)

`&g_edicts[1]` is **unreliable** — bots can race the human for slot 1. We cache the first non-bot edict to connect:
- `Ultron_OnClientConnect(ent, isBot)` called from `p_client.cpp` `ClientConnect` (after SVF_BOT is set, line ~2906).
- `Ultron_OnClientDisconnect(ent)` called from `p_client.cpp` `ClientDisconnect` (top of function).
- `Ultron_IsHuman(ent)` compares against the cached pointer.

## ClientBegin hook

DM mode is a separate path in `p_client.cpp`: `ClientBegin` returns early to `ClientBeginDeathmatch`, so our SP-path hook at the bottom of `ClientBegin` never runs for DM. We call `Ultron_OnClientBegin(ent)` from **both** `ClientBegin` and `ClientBeginDeathmatch` (near the end of each).

`Ultron_OnClientBegin`:
1. Fires `addbot` if `active_players().count < 2` (the correct command is `addbot`, not `bot_add` / `sv_addbot`).
2. Starts the eval clock (`g_eval_start = level.time`) on first human ClientBegin.

## Aim via `delta_angles` (NOT `ucmd->angles`)

Engine computes `viewangles = cmd.angles + delta_angles`. To force the view direction, we write:

```cpp
self->client->ps.pmove.delta_angles = desired - self->client->resp.cmd_angles;
self->client->ps.viewangles = desired;
self->client->v_angle       = desired;
self->s.angles              = { 0.0f, desired[YAW], 0.0f };
ucmd->angles                = {};  // zero incoming "mouse" delta
```

This mirrors `Edict_ForceLookAtPoint` in `bots/bot_exports.cpp:158`, but without calling that function (it only gates on `client != nullptr`, so it would work, but keeping the math inline lets us adjust later).

**Gotcha**: if we wrote `ucmd->angles = desired` instead, the engine would treat it as absolute input *and* add `delta_angles` on top → doubled rotation. Always use `delta_angles` for forced aim.

## Movement (view-relative)

`forwardmove` / `sidemove` are `float` (in `usercmd_t`) but interpreted in the yaw-relative frame established by `pm.viewangles` (which pmove derives from `cmd.angles + delta_angles`). Q2 normal run speed is **±400**, not ±127.

Transform world-space desired direction into local:
```cpp
vec3_t wd = goal_world - self->s.origin; wd[2] = 0.0f; wd.normalize();
vec3_t fwd, rt;
AngleVectors({0.0f, face_yaw, 0.0f}, fwd, rt, nullptr);
ucmd->forwardmove = std::clamp(wd.dot(fwd) * 400.f, -400.f, 400.f);
ucmd->sidemove    = std::clamp(wd.dot(rt)  * 400.f, -400.f, 400.f);
```

## Fire

Set `ucmd->buttons |= BUTTON_ATTACK` when view-forward is within cone-of-N-degrees of the target direction. Weapon cooldown (`weapon_fire_finished`) is already enforced by `p_weapon.cpp`, so holding the button is fine.

## Fatal trap: SVF_BOT gate on `Bot_SetWeapon` / `Bot_UseItem` / `Bot_TriggerEdict`

`bots/bot_exports.cpp:17-19`:
```cpp
if ( ( bot->svflags & SVF_BOT ) == 0 ) {
    return;
}
```

Our human doesn't have SVF_BOT, so **calling `Bot_SetWeapon(self, ...)` silently no-ops.** If weapon switching is needed:
- Use `ucmd->impulse = weapon_id` — the normal Q2 impulse dispatcher path works for player edicts.
- OR call `item->use(self, item)` directly, bypassing the gate (what `Bot_SetWeapon` does internally).

Same gate on `Bot_UseItem` and `Bot_TriggerEdict`. `Bot_PickedUpItem` and `Edict_ForceLookAtPoint` do NOT have the gate — they're safe.

## Perception (strict fairness rule)

**Never read `sv_entity_t` fields of other entities.** That's the engine-side data fed to stock bots and is an unfair information source vs. what a human sees. Allowed:
- `visible(self, other, false)` at `g_ai.cpp:392` — LoS traceline, eye to eye.
- Own `client->pers.inventory[]` / `self->health` / `self->client->resp.score`.
- Positions (`other->s.origin`) for in-LoS entities.
- Last-seen position + decay timer for lost-contact (our memory window is 2 sec).

Forbidden (marks us as cheating):
- `other->sv.health`, `other->sv.armor_value`, `other->sv.inventory[]`, etc.
- Full-map enemy location without LoS.
- `gi.GetPathToGoal(...)` is **allowed** — stock bots use the same API, so it's level.

## Telemetry for eval harness

`ultron_bot.cpp` maintains three rolling counters incremented in `Ultron_Bot_Command`:
- `g_fire_ticks++` — BUTTON_ATTACK set this frame.
- `g_target_ticks++` — target in LoS this frame.
- `g_nothing_ticks++` — no target, no memory.

Stamped to stdout every ~1 sec as `[eval] t=X score=S health=H fire_ticks=F target_ticks=TT idle_ticks=I`. The `eval.ps1` harness parses these.

## Cvars

**Control:**
- `ultron_play_self` (default 1) — intercept on/off. Toggle in console to hand control back to human.
- `ultron_autostart` (default 1) — run the DM-on-q2dm1 bootstrap in `InitGame`.
- `ultron_eval_seconds` (default 0) — auto-quit after N seconds of human gameplay; also enables telemetry.
- `ultron_bootstrapped` (internal, auto-set) — guards re-entry on DLL reload within one engine process.

**Tuning knobs (live — no rebuild needed):**
- `ultron_bot_fire_cone` (8.0) — fire threshold in degrees.
- `ultron_bot_move_speed` (400) — units/sec; Q2 run speed.
- `ultron_bot_strafe_period` (500) — ms between strafe-direction flips.
- `ultron_bot_backpedal_dist` (200) — back away when closer than this many units.
- `ultron_bot_memory_ms` (2000) — how long we remember a lost target.
- `ultron_bot_no_fire` (0) — 1 disables BUTTON_ATTACK (movement-only isolation).
- `ultron_bot_no_move` (0) — 1 zeroes forward/sidemove (aim-only isolation).
- `ultron_bot_no_strafe` (0) — 1 disables combat strafe dodge.
- `ultron_bot_debug` (0) — 1 emits per-decision logs at ~4 Hz.

Use via `+set ultron_bot_fire_cone 3` at launch, or from `eval.bat -Cvars @{ultron_bot_fire_cone=3}`. The code reads them each frame so console changes take effect immediately.

## Key files and line refs

| Need | File | Reference point |
|------|------|-----------------|
| Intercept point | `rerelease/p_client.cpp` | `ClientThink`, line ~3150 |
| Identity cache | `rerelease/p_client.cpp` | `ClientConnect` ~2906, `ClientDisconnect` top |
| DM ClientBegin hook | `rerelease/p_client.cpp` | `ClientBeginDeathmatch` end |
| SP ClientBegin hook | `rerelease/p_client.cpp` | `ClientBegin` before last brace |
| Bootstrap cvars | `rerelease/g_main.cpp` | `PreInitGame` (latched), `InitGame` (non-latched + gamemap) |
| Bot brain | `rerelease/ultron_bot.cpp` | `Ultron_Bot_Command` |
| LoS helper | `rerelease/g_ai.cpp` | `visible()` line 392 |
| Aim reference | `rerelease/bots/bot_exports.cpp` | `Edict_ForceLookAtPoint` line 158 |
| SVF_BOT gate trap | `rerelease/bots/bot_exports.cpp` | line 17 |
| Stock bot API | `rerelease/game.h` | game_import_t ~1989, game_export_t ~2138 |

## State machine (Phase 8, shipped)

`Ultron_Bot_Command` dispatches through `DecideState()` → one of:

| State | When | Handler |
|---|---|---|
| `UST_INTERMISSION` | intermissiontime != 0 | button-pulse to advance |
| `UST_RESPAWN`       | dead or awaiting_respawn | button-pulse to respawn |
| `UST_SPECTATOR`     | resp.spectator | no-op |
| `UST_COMBAT`        | visible enemy + health > 30 | aim+lead+fire, strafe |
| `UST_HEAL`          | visible enemy + health ≤ 30 | path to health item / run away |
| `UST_HUNT`          | no enemy + last-seen or heard | path to last-known |
| `UST_LOOT`          | no enemy + pickup within 1600u | path to best item |
| `UST_WANDER`        | nothing known | random explore |

Each is its own `static void State_*` function in `ultron_bot.cpp`. Clean insertion points for future per-phase tuning.

## Aim quality (Phase 2, shipped)

- `WeaponFireCone(item_id)` — 2.5° (rail), 3° (BFG), 4° (RL splash), 5° (chain/hyper), 6° (blaster), 10° (SSG spray).
- `WeaponProjectileSpeed(item_id)` — 0 for hitscan, 650 (RL), 600 (GL), 800 (ionripper), 1000 (blaster/hyper/BFG).
- `LeadPoint(shooter, target, speed)` — `target.origin + target.velocity * (dist / speed)`, capped 1.5s.
- `AimSmooth(yaw, pitch, frametime)` — rate-limits rotation to `ultron_bot_turn_speed` deg/s (default 540). Plus `ultron_bot_aim_jitter` degrees of hand-tremor noise.
- `UpdateDamageSense(self)` — tracks `self->health` delta; on damage, stamps `g_damage_from` + `g_damage_at`. HEAL state uses the direction to run opposite the threat.

## Weapon switching (Phase 4, shipped)

`Ultron_SelectWeapon(self, item_id)` skips the `Bot_SetWeapon` SVF_BOT gate by lifting its internal logic: validate, check inventory, `item->use(self, item)`. Safe to call every frame.

`PickBestWeapon(self, range)` returns the first usable (owned + has ammo) weapon from a range-banded preference list. Called from `State_Combat` each frame.

## Item awareness (Phase 5, shipped)

`FindBestPickup(self, max_dist, require_los)` scans `g_edicts[maxclients+1 .. num_edicts]`, filters to pickupable items (`inuse && item && !SVF_NOCLIENT`), ranks by `ItemWeight(self, ent) / distance`.

`ItemWeight` priorities: quad 180, invuln 170, quadfire 150, mega 120+, red armor 100, unowned weapon 80, combat armor 70, large health 60+, medium health 40+, small health 15+, shards/ammo/extras lower. Health weights add HP deficit so they scale up when hurt.

`FindBestHealthPickup(self, max_dist)` is HEAL-specific (heals + armor only).

## Pathfinding (Phase 3, shipped)

`gi.GetPathToGoal(req, info)` via wrapper `PlanTo(goal)` → `g_plan.next_waypoint`. `MoveTowardGoal(goal, frametime)` queries path, aims at next waypoint, drives view-relative movement. Replans on goal drift >96u or every 500ms, whichever first. Falls back to straight-line when no nav.

## Perception augment (Phase 6, shipped)

- `UpdateHearing(self)` scans active_players each frame for `s.event ∈ {EV_FOOTSTEP, EV_OTHER_FOOTSTEP, EV_PLAYER_TELEPORT, EV_FALL*}` within 1500u → stamps `g_mem.heard_at`. HUNT uses this when last-seen is stale.
- `ExtrapolateLastSeen()` — `g_mem.last_seen + g_mem.last_seen_vel * min(elapsed, 1s)`. HUNT aims at the extrapolated position so Ultron rounds corners where the enemy actually is.
- `g_mem.last_seen_vel` captured each sight from `other->velocity`.

## Persistent brain (Phase 7, shipped) — THE LEARNING FEATURE

Per-map JSON file at `%USERPROFILE%\Saved Games\Nightdive Studios\Quake II\Ultron\brain\<map>.json`.

Schema (will grow):
```json
{
  "map": "q2dm1",
  "games_played": 12,
  "total_frags": 34,
  "total_deaths": 47,
  "total_shots": 0,
  "items": [
    { "classname": "item_health", "x": ..., "y": ..., "z": ..., "seen_count": 5072 }
  ]
}
```

- `LoadBrain(mapname)` runs from `Ultron_OnClientBegin` once per human spawn.
- `SaveBrain()` runs every 30s in `BrainTick`, plus on `Ultron_OnClientDisconnect`.
- `BrainNoteItemSight(e)` called inside `FindBestPickup` scan.
- `BrainNoteDeath()` called from `UpdateDamageSense` on health <= 0 transition.
- `games_played` bumped once per map load (gated by `g_counted_this_game`).
- Score deltas in `BrainTick` roll `total_frags` forward.

Uses jsoncpp (already linked) + `std::filesystem` for the brain directory.

## Cvars summary

**Control:**
- `ultron_play_self` (1) — intercept on/off.
- `ultron_autostart` (1) — DM bootstrap in InitGame.
- `ultron_eval_seconds` (0) — auto-quit + telemetry; set by eval harness.
- `ultron_bootstrapped` (internal) — one-shot reentry guard.

**Tuning (live — no rebuild needed):**
- `ultron_bot_fire_cone` (0 = auto per-weapon) — override global degrees.
- `ultron_bot_move_speed` (400) — Q2 run speed.
- `ultron_bot_strafe_period` (500ms).
- `ultron_bot_backpedal_dist` (200u).
- `ultron_bot_memory_ms` (2000).
- `ultron_bot_turn_speed` (540 deg/s).
- `ultron_bot_aim_jitter` (0.3°).
- `ultron_bot_no_fire` / `no_move` / `no_strafe` (0) — isolation toggles.
- `ultron_bot_auto_weapon` (1) — weapon switching.
- `ultron_bot_wander` (1) / `ultron_bot_wander_probe` (128u).
- `ultron_bot_debug` (0) — per-decision logs at ~4 Hz.

## Still open (not yet implemented)

- **Strafe-jumping** — Q2 air-control speed boost trick; the plan says ~600 u/s vs 320 u/s walking.
- **Brain-derived priors** — the brain tracks data, but no decision yet reads it. Next obvious wins: start match pathing to highest-value `items[]` entry, time mega/red armor respawns.
- **Tactical grid / hot-spot weights** — score deaths_here vs frags_here per 128u cell.
- **Chat / trash talk** — Kex bots have `bot_chat_enable`; we should emit too.
- **Reaction-time variance** — fixed at 0 right now (no self-handicap).
- **Damage / hit stats** — we track fire_ticks + target_ticks but not "did this shot hit something." Would need to hook `T_Damage` to credit kills to Ultron.
