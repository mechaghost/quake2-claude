// MVP Claude bot that drives the human player's character.
//
// Architecture:
//   - Engine calls ClientThink(edict_t*, usercmd_t*) for the human each frame.
//   - p_client.cpp calls MyMod_Bot_Command() before it caches *ucmd into
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
#include "mymod_bot.h"

cvar_t *mymod_play_self = nullptr;
cvar_t *mymod_eval_seconds = nullptr;

static edict_t *g_mymod_human = nullptr;

// Wall-clock start for the eval harness, captured on the human's first
// ClientBegin. Used to auto-quit after mymod_eval_seconds and to stamp the
// [eval] telemetry lines.
static gtime_t g_eval_start  = 0_ms;
static gtime_t g_last_telem  = 0_ms;
static bool    g_quit_issued = false;

// Rolling counters so the harness can see the bot is actually doing things.
static uint32_t g_fire_ticks     = 0;  // ticks we held BUTTON_ATTACK
static uint32_t g_target_ticks   = 0;  // ticks we had a visible enemy
static uint32_t g_nothing_ticks  = 0;  // ticks with no target and no memory

// Per-human memory of the last-seen enemy position.
struct bot_memory_t {
    edict_t *target     = nullptr;
    vec3_t   last_seen  = {};
    gtime_t  last_seen_at = 0_ms;
};
static bot_memory_t g_mem;

constexpr gtime_t   MEMORY_WINDOW = 2_sec;
constexpr float     FIRE_CONE_DEG = 8.0f;   // fire if aim is within this cone
constexpr float     BACKPEDAL_DIST = 200.0f;
constexpr float     MOVE_SPEED     = 400.0f; // Q2 normal run speed
constexpr int64_t   STRAFE_FLIP_MS = 500;    // strafe direction flip period

// ---------------------------------------------------------------------------
// Identity

void MyMod_OnClientConnect(edict_t *ent, bool isBot) {
    if (!isBot && !g_mymod_human) {
        g_mymod_human = ent;
        gi.Com_PrintFmt("[mymod] human bound to client slot {}\n", ent->s.number);
    }
}

void MyMod_OnClientDisconnect(edict_t *ent) {
    if (ent == g_mymod_human) {
        g_mymod_human = nullptr;
        g_mem = {};
        gi.Com_Print("[mymod] human disconnected; identity cleared\n");
    }
    // Also drop any target we'd memorized pointing at a disconnecting edict.
    if (ent == g_mem.target) g_mem.target = nullptr;
}

// Fire bot_add once the human is fully in the level. Firing bot_add from
// InitGame crashed the engine (access violation at 0x0 inside the engine
// while mid-map-init). Deferring to ClientBegin avoids that.
void MyMod_OnClientBegin(edict_t *ent) {
    if (!ent || !ent->client) return;
    if (ent->svflags & SVF_BOT) return;           // only trigger off the human
    if (!deathmatch || !deathmatch->integer) return;
    int count = 0;
    for (auto *p : active_players()) { (void)p; count++; }
    if (count < 2) {
        gi.Com_Print("[mymod] spawning enemy bot via addbot\n");
        gi.AddCommandString("addbot\n");
    }
    if (g_eval_start == 0_ms) {
        g_eval_start = level.time;
        gi.Com_PrintFmt("[eval] start t={}\n", level.time.milliseconds());
    }
}

bool MyMod_IsHuman(edict_t *ent) {
    return ent != nullptr && ent == g_mymod_human;
}

// ---------------------------------------------------------------------------
// Init

void MyMod_Bot_Init() {
    mymod_play_self    = gi.cvar("mymod_play_self",    "1", CVAR_NOFLAGS);
    mymod_eval_seconds = gi.cvar("mymod_eval_seconds", "0", CVAR_NOFLAGS);
    g_mymod_human  = nullptr;
    g_mem          = {};
    g_eval_start   = 0_ms;
    g_last_telem   = 0_ms;
    g_quit_issued  = false;
    g_fire_ticks = g_target_ticks = g_nothing_ticks = 0;
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
    vec3_t wd = goal_world - self->s.origin;
    wd[2] = 0.0f;
    float dist_xy = wd.normalize();

    vec3_t fwd, rt;
    AngleVectors(vec3_t{ 0.0f, face_yaw, 0.0f }, fwd, rt, nullptr);

    float forward = std::clamp(wd.dot(fwd) * MOVE_SPEED, -MOVE_SPEED, MOVE_SPEED);

    // Back-pedal when uncomfortably close.
    if (dist_xy < BACKPEDAL_DIST && forward > 0.0f) {
        forward = -forward;
    }

    // Strafe direction flips every STRAFE_FLIP_MS; this dominates sidemove
    // during combat because circle-strafing is generally better than
    // face-strafing toward the target.
    int phase = (int)(level.time.milliseconds() / STRAFE_FLIP_MS);
    float side = (phase & 1) ? MOVE_SPEED : -MOVE_SPEED;

    ucmd->forwardmove = forward;
    ucmd->sidemove    = side;
}

// ---------------------------------------------------------------------------
// Fire

// Returns true if self's current view forward is within FIRE_CONE_DEG of the
// direction to target_point.
static bool AimWithinCone(edict_t *self, const vec3_t &target_point) {
    vec3_t eye = EyePos(self);
    vec3_t to_target = (target_point - eye).normalized();

    vec3_t view_fwd;
    AngleVectors(self->client->v_angle, view_fwd, nullptr, nullptr);

    float dot = view_fwd.dot(to_target);
    // cos(8 deg) ~ 0.9903
    float cos_thresh = cosf(FIRE_CONE_DEG * PIf / 180.0f);
    return dot >= cos_thresh;
}

// ---------------------------------------------------------------------------
// Top-level

void MyMod_Bot_Command(edict_t *self, usercmd_t *ucmd) {
    if (!mymod_play_self || !mymod_play_self->integer) return;
    if (!self || !self->client) return;

    // Eval harness: auto-quit once the deadline passes, and stamp periodic
    // telemetry lines the harness can parse.
    if (mymod_eval_seconds && mymod_eval_seconds->integer > 0 && g_eval_start != 0_ms)
    {
        float elapsed = (level.time - g_eval_start).seconds<float>();
        if (!g_quit_issued && elapsed >= (float)mymod_eval_seconds->integer)
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

    // Neutral input baseline. We'll selectively fill in what we want.
    ucmd->buttons     = BUTTON_NONE;
    ucmd->forwardmove = 0.0f;
    ucmd->sidemove    = 0.0f;
    ucmd->angles      = {}; // mouse delta zero; we drive view via delta_angles

    // Perception: nearest visible enemy, with 2s "last seen" memory.
    edict_t *vis = FindVisibleEnemy(self);
    if (vis) {
        g_mem.target       = vis;
        g_mem.last_seen    = EyePos(vis);
        g_mem.last_seen_at = level.time;
    } else if (g_mem.target && (level.time - g_mem.last_seen_at) > MEMORY_WINDOW) {
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
        // No target, no memory — stand still. MVP: no wandering/patrol.
        return;
    }
    if (target) g_target_ticks++;

    vec3_t desired = AimAtPoint(self, aim_point);

    // Walk toward visible target; if only remembered, also walk toward last-seen.
    DriveMovement(self, ucmd, aim_point, desired[YAW]);

    // Fire only when we actually see the target AND our view is near it.
    if (target && AimWithinCone(self, aim_point)) {
        ucmd->buttons |= BUTTON_ATTACK;
        g_fire_ticks++;
    }
}
