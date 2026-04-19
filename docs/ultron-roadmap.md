# Ultron roadmap: from MVP to "indistinguishable from human"

## Status: Phases 2, 3, 4, 5, 6, 7, 8, 9 all shipped in one burst (see git log `f358f09..b3a5505`). Polish + tuning ongoing.



## Goal

Ultron reaches a state where:

1. A Quake II spectator cannot tell it's a bot.
2. It plays at **one notch below pro-athlete level** — loses to top humans but dominates casuals and stock Kex bots.
3. It **improves per map as it plays** — retains knowledge across matches, gets stronger.

## Where we are (v1.x shipped)

- `usercmd_t` intercept on the human slot; engine routes input to Ultron each tick.
- Input is hard-killed while `ultron_play_self=1` — keyboard / mouse inert in every state.
- Perception: nearest LoS-visible enemy via `visible(self, other)`, 2s last-seen memory.
- Aim: rate-limited rotation (`ultron_bot_turn_speed` 540 deg/s) + hand-tremor jitter — no more infinite-sensitivity snap. Natural reaction lag (~0.7s target-acquire → fire observed in eval).
- Fire: cone-based (`ultron_bot_fire_cone` 8°), driven by smoothed view.
- Movement (combat): walk toward target, 500ms strafe metronome, back-pedal when < 200u.
- Movement (idle): `DriveWander` — forward + wall-probe traceline + random yaw + stuck detection.
- Autostart: on DLL load, bootstraps `q2dm1` DM vs one engine bot, handles respawn + intermission + rematch loop autonomously.
- Eval harness: bounded-duration runs, scenario presets (`stationary`, `passive`, `noaim`, `hardmode`, `crippled`), `-Runs N` aggregation, log compare.
- Dev ergonomics: windowed 800×600, mouse released, `cmdlist`/`cvarlist` dumped to `docs/kex-*.txt` (186 commands + 579 cvars).

**Current eval (baseline 15s):** `target_ticks=640 / fire_ticks=613 / score=0 / health=85`. Ultron sees, turns to, and fires at enemies — but rarely scores frags (wide cone, no lead, no weapon switch, engine bot has much better brain still).

## What "one notch below pro-athlete" looks like

A demo viewer should see Ultron:

- Track moving targets smoothly, not in snap-jumps.
- Lead rockets and grenades (not just point straight at the enemy's current position).
- Switch to the right weapon for the current range (SSG close, rocket mid, rail far).
- Pick up items — health, armor, weapons, ammo — as a natural part of movement.
- Not walk repeatedly into the same wall.
- Retreat and heal when low.
- React to being shot (turn toward the damage direction).
- Bunny-hop / strafe-jump through corridors for speed, not just walk.
- Know the map — head straight for mega-health when it's about to respawn, control red armor, etc.

None of that involves ML. All of it is reachable with heuristics + memory + the engine APIs we already have.

## Architectural principles

1. **Every behavior layer is toggle-able by cvar.** Ablation is how we know what's improving.
2. **Fairness bar: HUD-equivalent perception.** Never read `sv_entity_t` of other edicts. Stock bots do, so we get to use nav (`GetPathToGoal`) which is the same engine tool a human learning the map effectively has.
3. **Persistence lives in `Saved Games\…\Ultron\brain\<map>.json`.** Written via jsoncpp (already linked). Crash-safe: write periodically, not only on shutdown.
4. **State machine over monolithic function.** `Ultron_Bot_Command` dispatches to one of: `Combat`, `Hunt`, `Loot`, `Heal`, `Wander`, `Respawn`, `Intermission`. Each state owns its input generation.
5. **Eval-driven.** Every phase lands with a before/after metric. If it doesn't move the needle, we revert.

## Phased plan

### Phase 2 — Aim quality (1-2 weeks)

Goal: miss less. Concretely: **fire_efficiency** = hits / fire_ticks should rise from ~0 to ≥ 0.1.

**2.1 Projectile lead.** Rocket speed is ~650 u/s (from `g_weapon.cpp` constants). For non-hitscan weapons, aim at `target.origin + target.velocity * (distance / projectile_speed)` instead of `target.origin`. Target velocity derived from per-frame origin deltas we already track for stuck detection — extend to cover enemies too.

**2.2 Weapon-specific fire cone.** 8° is right for SSG spray, wildly wrong for railgun. Add a cone table:

| Weapon | Cone | Notes |
|---|---|---|
| Blaster | 6° | Projectile; needs lead |
| Shotgun / Super Shotgun | 12° | Spread weapons |
| Machinegun / Chaingun | 5° | Hitscan, burst |
| Hyperblaster | 5° | Projectile, fast |
| Rocket launcher | 3° + lead | Splash forgives small miss |
| Grenade launcher | needs arc | Special case |
| Railgun | 2° | Hitscan, needs precision |

**2.3 Pitch-aware aim.** We aim at `target.origin + viewheight.z` today. At close range with vertical separation, we need steeper pitch. Already mostly handled by `vectoangles` — confirm it works on stairs / ramps.

**2.4 Pain-aim reactivity.** When taking damage, bias the next-frame aim toward the damage source (approx from `client->damage_from` — needs to exist; check `g_combat.cpp`). If no source vector, rotate 180° as a "who hit me?" probe.

**Telemetry additions:** `hits_given`, `hits_taken`, `damage_given`, `damage_taken`. Derived: accuracy, K/D.

### Phase 3 — Movement quality (1-2 weeks)

Goal: Ultron stops looking like it's on rails. Concretely: human spectator can't tell from movement alone.

**3.1 Pathfinding** via `gi.GetPathToGoal(PathRequest&, PathInfo&)`. Already used by engine bots. Wrap it:
```cpp
struct PathPlan {
    std::array<vec3_t, 64> waypoints;
    int32_t count = 0;
    int32_t current_wp = 0;
    gtime_t replan_at = 0_ms;
};
```
Replan every 1s or on reaching within 48u of current waypoint. Feed `waypoint[current_wp]` as movement target instead of enemy origin when in `Hunt` / `Loot` states.

**3.2 Strafe-jumping.** Q2 air control mechanic: alternating forward + side + jump + aim-with-movement gives ~600 u/s vs ~320 u/s walking. Pmove rewards this. Implement as a movement mode: when destination > 300u away and ground is flat, enter strafe-jump loop. Bind to `ultron_bot_strafe_jump` cvar so we can A/B.

**3.3 Adaptive dodge.** Replace the 500ms strafe metronome with range-varied:
- Close (<200u): crouch + circle-strafe, tighter radius.
- Mid (200-600u): strafe + occasional jump.
- Far (>600u): back-pedal strafe while tracking.
And randomize periods (300-700ms) so it doesn't read as a pattern.

**3.4 Retreat behavior.** Health < 40: switch from `Combat` to `Heal`. Break LoS with current enemy, path toward nearest known health item.

**Telemetry additions:** `distance_traveled`, `unique_cells_visited` (grid 128u), `times_stuck`.

### Phase 4 — Weapon switching (1 week)

Blocked on the SVF_BOT gate in `Bot_SetWeapon` — already noted in the bot-internals skill. Two paths:

**4.1 `ucmd->impulse` = weapon_id.** Kex's ucmd_t in the rerelease doesn't have `impulse` (we confirmed). Needs another path.

**4.2 Direct `item->use(self, item)` call.** This is what `Bot_SetWeapon` does internally after the SVF_BOT gate. We lift that logic, skipping the gate.

Implementation:
```cpp
static void Ultron_SelectWeapon(edict_t *self, int weapon_id) {
    gitem_t *item = &itemlist[weapon_id];
    if (!item || !item->use) return;
    if (self->client->pers.inventory[weapon_id] <= 0) return;
    item->use(self, item);
}
```

**Selection logic:** each frame in combat, compute best weapon for `(distance, own_ammo, visible_target)`. Only switch if the new pick is meaningfully better (hysteresis) to avoid weapon-change thrash.

**Telemetry additions:** `weapon_changes`, per-weapon fire counts.

### Phase 5 — Item awareness (1-2 weeks)

Goal: Ultron picks up health, armor, ammo as a natural part of moving; times key items.

**5.1 Item scan.** Each frame (or every 5 frames), iterate `g_edicts[game.maxclients+1 .. globals.num_edicts]` looking for `ent->item && ent->inuse && !ent->item_picked_up_by[self_idx]`. Classify by `ent->item->flags` / category.

**5.2 Item priorities.** Weighted value:

| Item | Weight |
|---|---|
| Megahealth | 100 |
| Red armor | 90 |
| Yellow armor | 60 |
| Any weapon we don't own | 80 |
| Quad damage | 150 |
| Invulnerability | 120 |
| Rocket ammo (if own RL) | 40 |
| Shells (if own shotgun and low) | 30 |
| Small health | 10 |

**5.3 "Best reachable item" goal.** When in `Loot` state (or `Hunt` with nothing closer to fight), pathfind to argmax over items of `weight(item) / distance(self, item)`. If we're within 300u and see the item, drop the pathfinding and just walk straight.

**5.4 Respawn timer memory.** When an item goes `SVF_RESPAWNING` (flag set at `g_items.cpp:181`), note `ent->nextthink` as the respawn time. Persist per-item to brain file.

### Phase 6 — Perception augment (1 week)

**6.1 Hearing proxy.** Scan active edicts each frame for `s.event` = MUZZLEFLASH, MUZZLEFLASH2, FOOTSTEP, or JUMP within ~1500u, and pickup events. Add sound source to memory with 2s decay. Use for threat orientation (turn toward unseen gunfire) but **not** for aim (no x-ray).

**6.2 Enemy trajectory prediction.** When an enemy goes out of LoS, continue to update `g_mem.last_seen` by extrapolating velocity for 1s, then hold. Shoots around corners correctly when they're about to emerge.

**6.3 Damage-direction awareness.** On `pain` event (we take damage), the `damage_from` vector tells us who hit us. Bias the next target search toward that direction. If no LoS on anyone there, face that way while strafe-dodging (combat posture).

### Phase 7 — Self-training memory (the big one, 2-3 weeks)

Goal: `brain/q2dm1.json` grows each match. Next match starts smarter.

**7.1 Persistent brain file.**

Format:
```json
{
  "map": "q2dm1",
  "games_played": 12,
  "last_updated_ms": 1234567890000,
  "items": [
    { "classname": "item_megahealth", "origin": [-480, 608, 40], "avg_respawn_ms": 30000, "seen_count": 12 }
  ],
  "tactical_grid": [
    { "cell": [12, 7, 1], "frags_here": 8, "deaths_here": 2, "weight": 6 }
  ],
  "route_edges": [
    { "from": [12, 7, 1], "to": [13, 7, 1], "traversed": 34 }
  ],
  "weapon_hits": { "rocketlauncher": { "shots": 412, "hits": 63 } }
}
```

Grid = integer cells of `origin / 128`. Coarse enough to be memory-efficient, fine enough to be tactically meaningful.

**7.2 Write triggers.** Flush brain file:
- Every 30s during a match (crash-safe).
- On human ClientDisconnect.
- On map change.

**7.3 Read triggers.** Load brain file on `SpawnEntities` per new map. Apply priors:
- Item positions known? Skip exploration, path directly.
- Tactical grid has a heavily-weighted cell? Bias `Hunt` / `Wander` goals toward it.
- Death-heavy cell? Actively avoid unless objective-necessary.

**7.4 Ongoing updates.** Each event:
- We frag someone → `frags_here` on our current grid cell +1.
- We die → `deaths_here` on our last alive cell +1.
- We witness an item pickup → update `avg_respawn_ms` (exponential moving average).
- We move from cell A to cell B → `route_edges[(A,B)].traversed += 1`.

**7.5 Respect the fairness bar.** The brain file is just a persistent version of what we've already *observed*. No peeking at `sv_entity_t`. Every entry must have come from a sensed event (LoS sight, heard event, pickup we made).

### Phase 8 — State machine (1 week, actually slots in early)

Replace the current monolith in `Ultron_Bot_Command` with a clean dispatch:

```
enum UltronState { COMBAT, HUNT, LOOT, HEAL, WANDER, RESPAWN, INTERMISSION, SPECTATE };

state = decide_next_state(...);
switch (state) { ... per-state handler generates ucmd ... }
```

Transition rules:
- See enemy AND health > 30 → COMBAT
- See enemy AND health ≤ 30 → HEAL
- No enemy AND have last-seen AND LoS-path-possible → HUNT
- Loot-value * inverse-distance > threshold → LOOT
- Otherwise → WANDER
- Dead / awaiting-respawn / intermission override everything.

This goes **in parallel** with Phase 2 rather than blocking it, since the rules are cheap.

### Phase 9 — Eval rigor (ongoing)

**9.1 Baseline suite.** `eval-suite.bat` runs a fixed sequence:
- `stationary` (bot can't move): pure aim test.
- `passive` (bot doesn't shoot back): movement + targeting test.
- `baseline` (normal 1v1): overall.
- `hardmode` (perfect-aim enemy): ceiling test.

Records aggregate stats per scenario to `eval-logs/suite-<timestamp>.json`. We compare suite-to-suite to decide if Phase N work shipped an improvement.

**9.2 Match-level stats.** Extend telemetry with K/D, accuracy, damage-dealt, damage-taken, items picked up, distance traveled.

**9.3 Longitudinal.** Track suite scores over git commit SHAs. `eval-longitudinal.ps1` charts "baseline avg_score over the last N commits" so we can see the learning curve.

### Phase 10 — Polish (ongoing)

- Chat lines (Kex bots emit `bot_chat` — we can too) so Ultron trash-talks.
- Customized player model / colors for showmanship.
- View model adjustments (correct weapon pose).
- Realistic reaction-time scaling based on how "fresh" we are (mock cognitive load).

## Rough timeline

Working at v1 pace (MVP was ~1 day), assume each 1-week phase is ~4-6 actual sessions.

| Order | Phase | Duration |
|---|---|---|
| 1 | **Phase 8 state machine** (light) | 0.5 week |
| 2 | **Phase 2 aim quality** | 1-2 weeks |
| 3 | **Phase 4 weapon switching** | 1 week |
| 4 | **Phase 9 eval baseline suite** | 0.5 week |
| 5 | **Phase 3 movement** | 1-2 weeks |
| 6 | **Phase 5 items** | 1-2 weeks |
| 7 | **Phase 6 perception** | 1 week |
| 8 | **Phase 7 brain/persistence** | 2-3 weeks |
| 9 | **Phase 10 polish** | ongoing |

Total first pass: ~10-14 weeks of ambient work. Anchor check-ins: after Phase 4 (Ultron can win `stationary`), after Phase 5 (Ultron beats `passive`), after Phase 7 (Ultron visibly improves between sessions on same map).

## Open questions

1. **Bar for "indistinguishable"**: is it enough if 3 non-expert friends can't tell, or should it hold up to a Quake veteran watching?
2. **Training scope**: persist per-map only (9-10 key DM maps), or also persist strategies that generalize across maps?
3. **Shared memory vs. per-match**: do we want the brain to keep growing indefinitely, or reset periodically to avoid over-fitting?
4. **Bots-of-bots**: eventually spawn multiple Claude bots and watch them fight each other — useful for generating training data faster than real-time play?
5. **Out-of-DLL ML later**: if the heuristic ceiling turns out to be sub-pro, do we want an offline training loop that tunes cvars from eval logs (e.g. CMA-ES over `turn_speed`, `fire_cone`, dodge periods)?

## What I'd propose right now

Start with **Phase 8 state machine** and **Phase 2 aim quality** in the same branch. State machine is a precondition for everything after — without it, each new phase means more "if" tangles in `Ultron_Bot_Command`. Aim quality is the highest-leverage individual improvement because fire_ticks are already high but score is ~0.

Skip directly to Phase 7 (persistence) if the user wants to see the "learning" feature land early even at the cost of Ultron being mid-skill. The learning effect will be more dramatic per-session, just starting from a lower floor.

**Next concrete step**: either

(A) I do Phase 8 + Phase 2.1 (lead prediction) + Phase 2.2 (weapon-specific cones) in one commit, then we eval and see score move from 0 → small-positive.

(B) I skip ahead to Phase 7 bones (brain file read/write scaffolding + per-match stats) so by end of session the "plays better next game" feedback is visible to the user, even if the bot is still a bad shot.

User picks.
