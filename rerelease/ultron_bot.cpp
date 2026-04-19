// MVP Claude bot that drives the human player's character.
//
// Architecture:
//   - Engine calls ClientThink(edict_t*, usercmd_t*) for the human each frame.
//   - p_client.cpp calls Ultron_Bot_Command() before it caches *ucmd into
//     client->buttons / client->cmd, so mutating *ucmd here drives pmove,
//     weapon fire, view angles, everything — no SVF_BOT flag needed.
//   - Aim is forced via client->ps.pmove.delta_angles (see Edict_ForceLookAtPoint
//     at bots/bot_exports.cpp:158 for the reference pattern).
//   - Movement is view-yaw-relative: we project the desired world direction
//     onto the ground plane forward/right basis, then set forwardmove/sidemove.
//
// Fairness rule (strict, always-on for MVP):
//   Only LoS-visible enemies are read. No sv_entity_t peeking. No omniscient
//   health/armor/inventory reads on enemies. Last-seen origin decays in 2s.

#include "g_local.h"
#include "ultron_bot.h"

cvar_t *ultron_play_self = nullptr;
cvar_t *ultron_eval_seconds = nullptr;

// Tuning knobs — no rebuild needed, just `ultron_bot_fire_cone 3` in console
// or `+set ultron_bot_fire_cone 3` at launch.
cvar_t *ultron_bot_fire_cone       = nullptr;  // degrees; fire if view-forward within this cone of target
cvar_t *ultron_bot_move_speed      = nullptr;  // units/sec; Q2 run is ~400
cvar_t *ultron_bot_strafe_period   = nullptr;  // ms between strafe direction flips
cvar_t *ultron_bot_backpedal_dist  = nullptr;  // units; back away when closer than this
cvar_t *ultron_bot_memory_ms       = nullptr;  // how long we remember a lost target
cvar_t *ultron_bot_no_fire         = nullptr;  // 1 = never press BUTTON_ATTACK
cvar_t *ultron_bot_no_move         = nullptr;  // 1 = zero out forward/side
cvar_t *ultron_bot_no_strafe       = nullptr;  // 1 = don't auto-strafe during combat
cvar_t *ultron_bot_debug           = nullptr;  // 1 = emit verbose per-decision logs
cvar_t *ultron_bot_wander          = nullptr;  // 1 = explore map when no enemy
cvar_t *ultron_bot_wander_probe    = nullptr;  // forward-traceline probe distance for wall avoidance

static edict_t *g_Ultron_human = nullptr;

// Wall-clock start for the eval harness, captured on the human's first
// ClientBegin. Used to auto-quit after ultron_eval_seconds and to stamp the
// [eval] telemetry lines.
static gtime_t g_eval_start  = 0_ms;
static gtime_t g_last_telem  = 0_ms;
static bool    g_quit_issued = false;

// Rolling counters so the harness can see the bot is actually doing things.
static uint32_t g_fire_ticks     = 0;  // ticks we held BUTTON_ATTACK
static uint32_t g_target_ticks   = 0;  // ticks we had a visible enemy
static uint32_t g_nothing_ticks  = 0;  // ticks with no target and no memory

// Wander/exploration state. When no target is visible and no memory to
// pursue, the bot roams the map with wall-avoidance and periodic yaw changes.
static float   g_wander_yaw       = 0.0f;
static gtime_t g_wander_yaw_until = 0_ms;
static vec3_t  g_last_pos         = {};
static gtime_t g_last_pos_check   = 0_ms;
static gtime_t g_stuck_since      = 0_ms;

// Per-human memory of the last-seen enemy position.
struct bot_memory_t {
    edict_t *target     = nullptr;
    vec3_t   last_seen  = {};
    gtime_t  last_seen_at = 0_ms;
};
static bot_memory_t g_mem;

// Compile-time defaults; runtime values come from cvars below.
constexpr gtime_t   DEFAULT_MEMORY_WINDOW = 2_sec;
constexpr float     DEFAULT_FIRE_CONE_DEG = 8.0f;
constexpr float     DEFAULT_BACKPEDAL_DIST = 200.0f;
constexpr float     DEFAULT_MOVE_SPEED     = 400.0f;
constexpr int64_t   DEFAULT_STRAFE_FLIP_MS = 500;

// Resolve the live values each frame so cvar changes take effect immediately.
static inline float  CvarF(cvar_t *cv, float  def) { return cv ? cv->value : def; }
static inline int64_t CvarI(cvar_t *cv, int64_t def) { return cv ? cv->integer : def; }

// ---------------------------------------------------------------------------
// Identity

void Ultron_OnClientConnect(edict_t *ent, bool isBot) {
    if (!isBot && !g_Ultron_human) {
        g_Ultron_human = ent;
        gi.Com_PrintFmt("[ultron] human bound to client slot {}\n", ent->s.number);
    }
}

void Ultron_OnClientDisconnect(edict_t *ent) {
    if (ent == g_Ultron_human) {
        g_Ultron_human = nullptr;
        g_mem = {};
        gi.Com_Print("[ultron] human disconnected; identity cleared\n");
    }
    // Also drop any target we'd memorized pointing at a disconnecting edict.
    if (ent == g_mem.target) g_mem.target = nullptr;
}

// Fire bot_add once the human is fully in the level. Firing bot_add from
// InitGame crashed the engine (access violation at 0x0 inside the engine
// while mid-map-init). Deferring to ClientBegin avoids that.
void Ultron_OnClientBegin(edict_t *ent) {
    if (!ent || !ent->client) return;
    if (ent->svflags & SVF_BOT) return;           // only trigger off the human
    if (!deathmatch || !deathmatch->integer) return;
    int count = 0;
    for (auto *p : active_players()) { (void)p; count++; }
    if (count < 2) {
        gi.Com_Print("[ultron] spawning enemy bot via addbot\n");
        gi.AddCommandString("addbot\n");
    }
    if (g_eval_start == 0_ms) {
        g_eval_start = level.time;
        gi.Com_PrintFmt("[eval] start t={}\n", level.time.milliseconds());
    }
}

bool Ultron_IsHuman(edict_t *ent) {
    return ent != nullptr && ent == g_Ultron_human;
}

// ---------------------------------------------------------------------------
// Init

void Ultron_Bot_Init() {
    ultron_play_self       = gi.cvar("ultron_play_self",       "1",   CVAR_NOFLAGS);
    ultron_eval_seconds    = gi.cvar("ultron_eval_seconds",    "0",   CVAR_NOFLAGS);
    ultron_bot_fire_cone   = gi.cvar("ultron_bot_fire_cone",   "8",   CVAR_NOFLAGS);
    ultron_bot_move_speed  = gi.cvar("ultron_bot_move_speed",  "400", CVAR_NOFLAGS);
    ultron_bot_strafe_period = gi.cvar("ultron_bot_strafe_period", "500", CVAR_NOFLAGS);
    ultron_bot_backpedal_dist = gi.cvar("ultron_bot_backpedal_dist","200", CVAR_NOFLAGS);
    ultron_bot_memory_ms   = gi.cvar("ultron_bot_memory_ms",   "2000", CVAR_NOFLAGS);
    ultron_bot_no_fire     = gi.cvar("ultron_bot_no_fire",     "0",   CVAR_NOFLAGS);
    ultron_bot_no_move     = gi.cvar("ultron_bot_no_move",     "0",   CVAR_NOFLAGS);
    ultron_bot_no_strafe   = gi.cvar("ultron_bot_no_strafe",   "0",   CVAR_NOFLAGS);
    ultron_bot_debug       = gi.cvar("ultron_bot_debug",       "0",   CVAR_NOFLAGS);
    ultron_bot_wander      = gi.cvar("ultron_bot_wander",      "1",   CVAR_NOFLAGS);
    ultron_bot_wander_probe= gi.cvar("ultron_bot_wander_probe","128", CVAR_NOFLAGS);
    g_Ultron_human  = nullptr;
    g_mem          = {};
    g_eval_start   = 0_ms;
    g_last_telem   = 0_ms;
    g_quit_issued  = false;
    g_fire_ticks = g_target_ticks = g_nothing_ticks = 0;
    g_wander_yaw = 0.0f;
    g_wander_yaw_until = 0_ms;
    g_last_pos = {};
    g_last_pos_check = 0_ms;
    g_stuck_since = 0_ms;
}

// ---------------------------------------------------------------------------
// Perception

// Pick the nearest visible enemy. Returns nullptr if none visible.
static edict_t *FindVisibleEnemy(edict_t *self) {
    edict_t *best = nullptr;
    float best_dist2 = FLT_MAX;
    for (auto *other : active_players()) {
        if (other == self) continue;
        if (!other->inuse) continue;
        if (other->deadflag) continue;
        if (other->client == nullptr) continue;
        if (other->client->resp.spectator) continue;
        if (!visible(self, other, false)) continue;
        float d2 = (other->s.origin - self->s.origin).lengthSquared();
        if (d2 < best_dist2) {
            best_dist2 = d2;
            best = other;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Aim

// Compute eye position: entity origin + viewheight in Z.
static vec3_t EyePos(edict_t *ent) {
    vec3_t p = ent->s.origin;
    if (ent->client) {
        p += ent->client->ps.viewoffset;
    } else {
        p[2] += (float)ent->viewheight;
    }
    return p;
}

// Force self's view to look at 'point'. Mirrors Edict_ForceLookAtPoint in
// bots/bot_exports.cpp but keeps the player's cmd_angles (so the engine-side
// input accumulator stays sane).
static vec3_t AimAtPoint(edict_t *self, const vec3_t &point) {
    vec3_t eye   = EyePos(self);
    vec3_t ideal = (point - eye).normalized();
    vec3_t desired = vectoangles(ideal);
    if (desired[PITCH] < -180.0f) desired[PITCH] = anglemod(desired[PITCH] + 360.0f);

    // viewangles = cmd.angles + delta_angles (see p_move.cpp:1530).
    // Drive delta_angles so that, for whatever cmd.angles the engine passes in
    // next frame, the viewangles resolve to 'desired'.
    self->client->ps.pmove.delta_angles = desired - self->client->resp.cmd_angles;
    // Snap the rendered/logical angles so the camera matches immediately.
    self->client->ps.viewangles = desired;
    self->client->v_angle       = desired;
    self->s.angles              = { 0.0f, desired[YAW], 0.0f };
    return desired;
}

// ---------------------------------------------------------------------------
// Movement

// Fill ucmd->forwardmove/sidemove to move toward goal_world from self, given
// the yaw we're about to face. Side-strafes at a fixed cadence for dodging.
static void DriveMovement(edict_t *self, usercmd_t *ucmd, const vec3_t &goal_world, float face_yaw) {
    if (CvarI(ultron_bot_no_move, 0)) { ucmd->forwardmove = ucmd->sidemove = 0.0f; return; }

    const float   speed       = CvarF(ultron_bot_move_speed, DEFAULT_MOVE_SPEED);
    const float   backpedal   = CvarF(ultron_bot_backpedal_dist, DEFAULT_BACKPEDAL_DIST);
    const int64_t strafe_ms   = CvarI(ultron_bot_strafe_period, DEFAULT_STRAFE_FLIP_MS);
    const bool    no_strafe   = CvarI(ultron_bot_no_strafe, 0) != 0;

    vec3_t wd = goal_world - self->s.origin;
    wd[2] = 0.0f;
    float dist_xy = wd.normalize();

    vec3_t fwd, rt;
    AngleVectors(vec3_t{ 0.0f, face_yaw, 0.0f }, fwd, rt, nullptr);

    float forward = std::clamp(wd.dot(fwd) * speed, -speed, speed);

    // Back-pedal when uncomfortably close.
    if (dist_xy < backpedal && forward > 0.0f) {
        forward = -forward;
    }

    float side = 0.0f;
    if (!no_strafe && strafe_ms > 0) {
        int phase = (int)(level.time.milliseconds() / strafe_ms);
        side = (phase & 1) ? speed : -speed;
    } else {
        // Natural sidemove toward goal (no dodging).
        side = std::clamp(wd.dot(rt) * speed, -speed, speed);
    }

    ucmd->forwardmove = forward;
    ucmd->sidemove    = side;
}

// ---------------------------------------------------------------------------
// Fire

// Returns true if self's current view forward is within ultron_bot_fire_cone
// degrees of the direction to target_point.
static bool AimWithinCone(edict_t *self, const vec3_t &target_point) {
    vec3_t eye = EyePos(self);
    vec3_t to_target = (target_point - eye).normalized();

    vec3_t view_fwd;
    AngleVectors(self->client->v_angle, view_fwd, nullptr, nullptr);

    float dot = view_fwd.dot(to_target);
    float cone_deg = CvarF(ultron_bot_fire_cone, DEFAULT_FIRE_CONE_DEG);
    float cos_thresh = cosf(cone_deg * PIf / 180.0f);
    return dot >= cos_thresh;
}

// ---------------------------------------------------------------------------
// Explore / wander

// Aim the camera at 'yaw' degrees (ground plane). Used by both combat and
// wander. Mirrors the Edict_ForceLookAtPoint pattern.
static void SetAimYawPitch(edict_t *self, float yaw, float pitch) {
    vec3_t desired = { pitch, yaw, 0.0f };
    self->client->ps.pmove.delta_angles = desired - self->client->resp.cmd_angles;
    self->client->ps.viewangles = desired;
    self->client->v_angle       = desired;
    self->s.angles              = { 0.0f, yaw, 0.0f };
}

// Simple wander: pick a yaw, walk forward, avoid walls, rotate on stuck.
// Runs when no target is known. Exits with ucmd filled for this frame.
static void DriveWander(edict_t *self, usercmd_t *ucmd) {
    const float speed = CvarF(ultron_bot_move_speed, DEFAULT_MOVE_SPEED);
    const float probe = CvarF(ultron_bot_wander_probe, 128.0f);

    // Initialize on first entry — face whichever way we're currently looking.
    if (g_wander_yaw_until == 0_ms) {
        g_wander_yaw = self->client->v_angle[YAW];
        g_wander_yaw_until = level.time + gtime_t::from_ms(1500 + (int64_t)frandom(2500.0f));
        g_last_pos = self->s.origin;
        g_last_pos_check = level.time;
    }

    bool pick_new = false;

    // Wall probe: traceline forward at eye height. If something blocks within
    // the probe distance, we need to turn.
    {
        vec3_t eye = EyePos(self);
        vec3_t fwd;
        AngleVectors(vec3_t{ 0.0f, g_wander_yaw, 0.0f }, fwd, nullptr, nullptr);
        vec3_t probe_end = eye + fwd * probe;
        trace_t tr = gi.traceline(eye, probe_end, self, MASK_SOLID);
        if (tr.fraction < 0.9f) pick_new = true;
    }

    // Timed refresh — even if the path is clear, human-like movement has some
    // zig-zag rather than barreling straight.
    if (level.time >= g_wander_yaw_until) pick_new = true;

    // Stuck detection: sample position every 500ms; if we've moved <24 units
    // for >= 1 second straight, we're stuck on geometry — force a turn.
    if ((level.time - g_last_pos_check).seconds<float>() >= 0.5f) {
        g_last_pos_check = level.time;
        float moved = (self->s.origin - g_last_pos).length();
        g_last_pos = self->s.origin;
        if (moved < 24.0f) {
            if (g_stuck_since == 0_ms) g_stuck_since = level.time;
            else if ((level.time - g_stuck_since).seconds<float>() >= 1.0f) {
                pick_new = true;
                g_stuck_since = 0_ms;
            }
        } else {
            g_stuck_since = 0_ms;
        }
    }

    if (pick_new) {
        // Pick a random yaw delta in the range [30, 165] degrees with random
        // sign. Magnitude is enough to dodge walls without spinning in place.
        float sign  = (frandom() < 0.5f) ? -1.0f : 1.0f;
        float delta = frandom(30.0f, 165.0f);
        g_wander_yaw = anglemod(g_wander_yaw + sign * delta);
        g_wander_yaw_until = level.time + gtime_t::from_ms(1200 + (int64_t)frandom(2800.0f));
    }

    // Aim where we're walking (with a slight downward tilt so we don't stare at the ceiling).
    SetAimYawPitch(self, g_wander_yaw, 0.0f);

    // Forward at full speed; no strafe during exploration.
    ucmd->forwardmove = speed;
    ucmd->sidemove    = 0.0f;

    // Occasional jump to break pathing on stairs.
    if (frandom() < 0.01f) {
        ucmd->buttons |= BUTTON_JUMP;
    }
}

// ---------------------------------------------------------------------------
// Top-level

void Ultron_Bot_Command(edict_t *self, usercmd_t *ucmd) {
    if (!ultron_play_self || !ultron_play_self->integer) return;
    if (!self || !self->client) return;

    // ALWAYS zero the human's incoming input first — no keyboard or mouse from
    // the user ever reaches pmove / weapon fire / view angles while play_self
    // is on. This runs unconditionally regardless of match state.
    ucmd->buttons     = BUTTON_NONE;
    ucmd->forwardmove = 0.0f;
    ucmd->sidemove    = 0.0f;
    ucmd->angles      = {}; // mouse delta zero; we drive view via delta_angles

    // Eval harness: auto-quit once the deadline passes, and stamp periodic
    // telemetry lines the harness can parse.
    if (ultron_eval_seconds && ultron_eval_seconds->integer > 0 && g_eval_start != 0_ms)
    {
        float elapsed = (level.time - g_eval_start).seconds<float>();
        if (!g_quit_issued && elapsed >= (float)ultron_eval_seconds->integer)
        {
            g_quit_issued = true;
            gi.Com_PrintFmt("[eval] done elapsed={:.2f} score={} health={} fire_ticks={} target_ticks={} idle_ticks={}\n",
                elapsed, self->client->resp.score, self->health,
                g_fire_ticks, g_target_ticks, g_nothing_ticks);
            gi.AddCommandString("quit\n");
            return;
        }
        if ((level.time - g_last_telem).seconds<float>() >= 1.0f)
        {
            g_last_telem = level.time;
            gi.Com_PrintFmt("[eval] t={:.1f} score={} health={} fire_ticks={} target_ticks={} idle_ticks={}\n",
                elapsed, self->client->resp.score, self->health,
                g_fire_ticks, g_target_ticks, g_nothing_ticks);
        }
    }

    // Non-combat states: emit a pulsed BUTTON_ATTACK on a cadence so the user
    // never has to touch the keyboard. Returns after — no perception/aim/fire
    // logic should run in these states.
    if (level.intermissiontime != 0_ms)
    {
        // Intermission skip: any button after intermission_time + 5s.
        if ((level.time - level.intermissiontime) > 5_sec
            && ((level.time.milliseconds() / 200) % 30) == 0)   // ~1 pulse every 6s
        {
            ucmd->buttons |= BUTTON_ATTACK;
        }
        return;
    }
    if (self->client->awaiting_respawn || self->deadflag)
    {
        // Respawn: short pulse once a second.
        if ((level.time.milliseconds() / 200) % 5 == 0)
            ucmd->buttons |= BUTTON_ATTACK;
        return;
    }
    if (self->client->resp.spectator)
    {
        return;
    }

    // Perception: nearest visible enemy, with configurable last-seen memory.
    const gtime_t memory_window = gtime_t::from_ms((int64_t)CvarI(ultron_bot_memory_ms, 2000));
    edict_t *vis = FindVisibleEnemy(self);
    if (vis) {
        g_mem.target       = vis;
        g_mem.last_seen    = EyePos(vis);
        g_mem.last_seen_at = level.time;
    } else if (g_mem.target && (level.time - g_mem.last_seen_at) > memory_window) {
        g_mem.target = nullptr;
    }

    edict_t *target = vis;
    vec3_t   aim_point;
    bool     have_aim = false;

    if (target) {
        aim_point = EyePos(target);
        have_aim = true;
    } else if (g_mem.target) {
        // Aim at last-seen position as a best-effort orientation hint.
        aim_point = g_mem.last_seen;
        have_aim = true;
    }

    if (!have_aim) {
        g_nothing_ticks++;
        // No target, no memory — wander the map (was: stand still). Gated on
        // ultron_bot_wander so a tuning pass can disable for A/B.
        if (CvarI(ultron_bot_wander, 1)) {
            DriveWander(self, ucmd);
        }
        return;
    }
    // Fresh visible target invalidates the wander plan; next time we lose
    // sight we want a fresh random yaw, not the stale one from last wander.
    g_wander_yaw_until = 0_ms;
    g_stuck_since      = 0_ms;
    if (target) g_target_ticks++;

    vec3_t desired = AimAtPoint(self, aim_point);

    // Walk toward visible target; if only remembered, also walk toward last-seen.
    DriveMovement(self, ucmd, aim_point, desired[YAW]);

    // Fire only when we actually see the target AND our view is near it.
    bool want_fire = target && AimWithinCone(self, aim_point);
    if (want_fire && !CvarI(ultron_bot_no_fire, 0)) {
        ucmd->buttons |= BUTTON_ATTACK;
        g_fire_ticks++;
    }

    // Verbose per-frame dev log, when enabled. Rate-limit to ~4 Hz.
    if (CvarI(ultron_bot_debug, 0))
    {
        static gtime_t last_dbg = 0_ms;
        if ((level.time - last_dbg).milliseconds() >= 250)
        {
            last_dbg = level.time;
            vec3_t dir = aim_point - EyePos(self);
            float dist = dir.length();
            gi.Com_PrintFmt("[ultron/dbg] tgt={} vis={} dist={:.0f} fwd={:.2f} side={:.2f} fire={}\n",
                target ? target->s.number : -1, vis ? 1 : 0,
                dist, ucmd->forwardmove, ucmd->sidemove, want_fire ? 1 : 0);
        }
    }
}
