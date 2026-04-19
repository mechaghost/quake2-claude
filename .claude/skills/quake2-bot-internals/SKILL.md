---
name: quake2-bot-internals
description: Architecture reference for the Claude-driven player bot in this Quake II Rerelease mod. Use when modifying the bot's perception, aim, fire, movement, weapon selection, or when adding new capabilities (pathfinding, items, hearing). Also use when diagnosing why the bot isn't playing correctly. Captures the load-bearing invariants that are easy to get wrong (don't flip SVF_BOT, delta_angles not ucmd->angles, ±400 move speed, SVF_BOT gate on bot_exports).
tools: Read, Edit, Grep, Glob, Bash
---

# Bot architecture reference

The bot lives in `rerelease/mymod_bot.cpp` / `mymod_bot.h` and hooks into three places in the stock game DLL.

## Core design decision: intercept `usercmd_t`, don't flip SVF_BOT

The human's character is driven by mutating `*ucmd` inside `ClientThink` before the engine reads it. The human's `edict_t` is **never** marked `SVF_BOT` because the engine routes `ClientThink` only to non-SVF_BOT edicts — flipping that flag silences input entirely.

**Entry point**: `p_client.cpp` `ClientThink(edict_t*, usercmd_t*)` at line ~3150. Our hook runs **before** `client->oldbuttons = client->buttons;` (around line 3162) so our synthesized buttons participate in the latched-buttons edge detect used by weapon fire.

**Gate**:
```cpp
if (mymod_play_self && mymod_play_self->integer
    && MyMod_IsHuman(ent)
    && !level.intermissiontime
    && !ent->client->awaiting_respawn
    && !ent->client->resp.spectator
    && !ent->deadflag)
{
    MyMod_Bot_Command(ent, ucmd);
}
```

## Identity (`MyMod_IsHuman`)

`&g_edicts[1]` is **unreliable** — bots can race the human for slot 1. We cache the first non-bot edict to connect:
- `MyMod_OnClientConnect(ent, isBot)` called from `p_client.cpp` `ClientConnect` (after SVF_BOT is set, line ~2906).
- `MyMod_OnClientDisconnect(ent)` called from `p_client.cpp` `ClientDisconnect` (top of function).
- `MyMod_IsHuman(ent)` compares against the cached pointer.

## ClientBegin hook

DM mode is a separate path in `p_client.cpp`: `ClientBegin` returns early to `ClientBeginDeathmatch`, so our SP-path hook at the bottom of `ClientBegin` never runs for DM. We call `MyMod_OnClientBegin(ent)` from **both** `ClientBegin` and `ClientBeginDeathmatch` (near the end of each).

`MyMod_OnClientBegin`:
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

`mymod_bot.cpp` maintains three rolling counters incremented in `MyMod_Bot_Command`:
- `g_fire_ticks++` — BUTTON_ATTACK set this frame.
- `g_target_ticks++` — target in LoS this frame.
- `g_nothing_ticks++` — no target, no memory.

Stamped to stdout every ~1 sec as `[eval] t=X score=S health=H fire_ticks=F target_ticks=TT idle_ticks=I`. The `eval.ps1` harness parses these.

## Cvars

- `mymod_play_self` (default 1) — intercept on/off. Toggle in console to hand control back to human.
- `mymod_autostart` (default 1) — run the DM-on-q2dm1 bootstrap in `InitGame`.
- `mymod_eval_seconds` (default 0) — auto-quit after N seconds of human gameplay; also enables telemetry.
- `mymod_bootstrapped` (internal, auto-set) — guards re-entry on DLL reload within one engine process.

## Key files and line refs

| Need | File | Reference point |
|------|------|-----------------|
| Intercept point | `rerelease/p_client.cpp` | `ClientThink`, line ~3150 |
| Identity cache | `rerelease/p_client.cpp` | `ClientConnect` ~2906, `ClientDisconnect` top |
| DM ClientBegin hook | `rerelease/p_client.cpp` | `ClientBeginDeathmatch` end |
| SP ClientBegin hook | `rerelease/p_client.cpp` | `ClientBegin` before last brace |
| Bootstrap cvars | `rerelease/g_main.cpp` | `PreInitGame` (latched), `InitGame` (non-latched + gamemap) |
| Bot brain | `rerelease/mymod_bot.cpp` | `MyMod_Bot_Command` |
| LoS helper | `rerelease/g_ai.cpp` | `visible()` line 392 |
| Aim reference | `rerelease/bots/bot_exports.cpp` | `Edict_ForceLookAtPoint` line 158 |
| SVF_BOT gate trap | `rerelease/bots/bot_exports.cpp` | line 17 |
| Stock bot API | `rerelease/game.h` | game_import_t ~1989, game_export_t ~2138 |

## Explicitly deferred (scope decisions made during MVP planning)

These are not implemented; when the user asks for them, do not assume they work:

- **Weapon switching** — use `ucmd->impulse` or `item->use` direct call, not `Bot_SetWeapon`.
- **Pathfinding** — `gi.GetPathToGoal(PathRequest&, PathInfo&)` at `game.h:1995`. Preallocate a `std::array<vec3_t, 256>` (or 512 like `bots/bot_debug.cpp:30`); `request.moveDist` should be ~32 for humanoid granularity. Check `PathReturnCode::NoNavAvailable` for maps without nav.
- **Item memory** — scan `g_edicts` for `ent->item && (ent->svflags & SVF_RESPAWNING)`; respawn time = `ent->nextthink - level.time`.
- **Hearing proxy** — per-frame scan `g_edicts` for `s.event ∈ {EV_MUZZLEFLASH, EV_MUZZLEFLASH2, EV_FOOTSTEP}` within ~1500u for a "heard" memory hint. Engine bots likely have direct audio knowledge; this proxies it.
- **Projectile lead** — required when we switch away from hitscan. Compute target velocity from `other->s.origin` delta frames, offset aim by `velocity * distance / projectile_speed`.
- **Reaction-time / aim jitter** — only add if we're dominating too hard. Default 0, calibrate up.
