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
#include "json/json.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
  // Hand-declared rather than pulling in all of <windows.h> (which fights
  // with other game headers). These are pure Win32 user32 entry points.
  extern "C" __declspec(dllimport) int __stdcall ClipCursor(const void *rect);
  extern "C" __declspec(dllimport) int __stdcall ShowCursor(int show);
#endif

// Fight the engine's mouse-grab. Win32-only, silently no-ops elsewhere.
// The Kex engine calls SDL_SetRelativeMouseMode during gameplay which
// clips the OS cursor to the window (via ClipCursor on Windows) and hides
// it. We unclip + reshow it every frame. Cheap, effective.
void Ultron_FreeMouseCursor() {
#if defined(_WIN32)
    ClipCursor(nullptr);
    // ShowCursor uses an internal counter; loop until visible (>= 0 means
    // "cursor is shown"). Most frames this is a single call.
    for (int i = 0; i < 8; i++) {
        if (ShowCursor(1) >= 0) break;
    }
#endif
}

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
cvar_t *ultron_bot_turn_speed      = nullptr;  // degrees/sec max view rotation; below-pro = ~540
cvar_t *ultron_bot_aim_jitter      = nullptr;  // tiny random noise added to aim each frame (deg)
cvar_t *ultron_bot_auto_weapon     = nullptr;  // 1 = auto-switch to best weapon for range

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

// Smoothed aim state. delta_angles is rate-limited so the view doesn't
// teleport to the target (which reads as infinite sensitivity). Initialized
// from the player's actual view on first bot tick so there's no pop.
static float g_cur_yaw   = 0.0f;
static float g_cur_pitch = 0.0f;
static bool  g_aim_init  = false;

// Per-human memory of the last-seen enemy position + augmented senses.
struct bot_memory_t {
    edict_t *target        = nullptr;
    vec3_t   last_seen     = {};
    vec3_t   last_seen_vel = {};    // velocity at last sight — used for extrapolation
    gtime_t  last_seen_at  = 0_ms;

    // Hearing proxy: any nearby enemy footstep / teleport event we detected.
    vec3_t   heard_at      = {};
    gtime_t  heard_at_time = 0_ms;
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
// Brain state (declared up here because Ultron_On* callbacks touch it).
// Function definitions live further down in the "Persistent brain" section.

struct UltronItemMemory {
    std::string classname;
    vec3_t      origin       = {};
    int         seen_count   = 0;
};
struct UltronBrain {
    std::string                   map;
    int                           games_played = 0;
    int                           total_frags  = 0;
    int                           total_deaths = 0;
    int                           total_shots  = 0;
    std::vector<UltronItemMemory> items;
};
static UltronBrain g_brain;
static gtime_t     g_brain_last_save = 0_ms;
static bool        g_brain_loaded    = false;
static int32_t     g_last_score_seen = 0;
static bool        g_counted_this_game = false;

static void SaveBrain();
static void LoadBrain(const char *mapname);
static void BrainNoteItemSight(edict_t *e);
static void BrainNoteDeath();
static void BrainTick(edict_t *self);

// ---------------------------------------------------------------------------
// Identity

// Disable every knob that lets the user's mouse / keyboard touch the view.
// Called when the human binds to Ultron. We don't restore these because the
// bot is the point; if the user wants to play normally, they flip
// ultron_play_self 0 and manually re-set sensitivity.
static void Ultron_SuppressUserInput() {
    gi.AddCommandString("seta sensitivity 0\n");
    gi.AddCommandString("seta m_pitch 0\n");
    gi.AddCommandString("seta m_yaw 0\n");
    gi.AddCommandString("seta m_side 0\n");
    gi.AddCommandString("seta m_forward 0\n");
    gi.AddCommandString("seta cl_mousesmooth 0\n");
    gi.AddCommandString("seta freelook 0\n");
}

void Ultron_OnClientConnect(edict_t *ent, bool isBot) {
    if (!isBot && !g_Ultron_human) {
        g_Ultron_human = ent;
        Ultron_SuppressUserInput();
        gi.Com_PrintFmt("[ultron] human bound to client slot {}\n", ent->s.number);
    }
}

void Ultron_OnClientDisconnect(edict_t *ent) {
    if (ent == g_Ultron_human) {
        SaveBrain();
        g_Ultron_human = nullptr;
        g_mem = {};
        g_aim_init = false;
        g_wander_yaw_until = 0_ms;
        g_counted_this_game = false;  // next bind on new map starts a new game count
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
    // Guard against spawning another bot when one already exists in the match.
    // Each Ultron_OnClientBegin fires once per map load for the human; on the
    // second map load (autostart reload) the first bot has already connected.
    static gtime_t last_addbot_at = 0_ms;
    if (count < 2 && (last_addbot_at == 0_ms ||
                      (level.time - last_addbot_at).seconds<float>() > 3.0f)) {
        gi.Com_Print("[ultron] spawning enemy bot via addbot\n");
        gi.AddCommandString("addbot\n");
        last_addbot_at = level.time;
    }
    if (g_eval_start == 0_ms) {
        g_eval_start = level.time;
        gi.Com_PrintFmt("[eval] start t={} deathmatch={}\n",
            level.time.milliseconds(), deathmatch->integer);
    }
    // Load the map-specific brain file now that level.mapname is populated.
    if (!g_brain_loaded || g_brain.map != level.mapname) {
        LoadBrain(level.mapname);
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
    // 0 = per-weapon auto cone (recommended). Non-zero forces a global cone.
    ultron_bot_fire_cone   = gi.cvar("ultron_bot_fire_cone",   "0",   CVAR_NOFLAGS);
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
    ultron_bot_turn_speed  = gi.cvar("ultron_bot_turn_speed",  "540", CVAR_NOFLAGS);
    ultron_bot_aim_jitter  = gi.cvar("ultron_bot_aim_jitter",  "0.3", CVAR_NOFLAGS);
    ultron_bot_auto_weapon = gi.cvar("ultron_bot_auto_weapon", "1",   CVAR_NOFLAGS);
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
    g_cur_yaw = g_cur_pitch = 0.0f;
    g_aim_init = false;
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

// Hearing proxy. Scan other clients for audible events (footstep, teleport)
// within 'radius' units. Stamps g_mem.heard_at when we catch one. This is
// intentionally cheap — O(maxclients) per frame.
static void UpdateHearing(edict_t *self) {
    const float radius = 1500.0f;
    const float radius2 = radius * radius;
    for (auto *other : active_players()) {
        if (other == self) continue;
        if (!other->inuse || other->deadflag) continue;
        if (!other->client || other->client->resp.spectator) continue;
        if (other->s.event != EV_FOOTSTEP &&
            other->s.event != EV_OTHER_FOOTSTEP &&
            other->s.event != EV_PLAYER_TELEPORT &&
            other->s.event != EV_FALL &&
            other->s.event != EV_FALLSHORT &&
            other->s.event != EV_FALLFAR) continue;
        float d2 = (other->s.origin - self->s.origin).lengthSquared();
        if (d2 > radius2) continue;
        g_mem.heard_at      = other->s.origin;
        g_mem.heard_at_time = level.time;
    }
}

// Trajectory-extrapolate the last-seen origin using captured velocity.
// Used by State_Hunt to "lead" the chase around corners, so Ultron rounds
// the corner where the enemy actually is rather than where they stood.
static vec3_t ExtrapolateLastSeen() {
    if (g_mem.last_seen_at == 0_ms) return g_mem.last_seen;
    float t = (level.time - g_mem.last_seen_at).seconds<float>();
    t = std::clamp(t, 0.0f, 1.0f);  // only extrapolate for 1s max
    return g_mem.last_seen + g_mem.last_seen_vel * t;
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

// Shortest-path signed delta from a -> b in degrees, result in [-180, 180].
static float AngleShortestDelta(float a, float b) {
    float d = b - a;
    while (d >  180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

// Rate-limited aim. Rotates g_cur_yaw / g_cur_pitch toward (desired_yaw,
// desired_pitch) at most ultron_bot_turn_speed degrees per frame-second,
// then commits the smoothed angles to the client's viewangles + delta_angles.
// frametime_s is taken from ucmd->msec so smoothing is correct at any client
// tick rate.
static void AimSmooth(edict_t *self, float desired_yaw, float desired_pitch, float frametime_s) {
    if (!g_aim_init) {
        g_cur_yaw   = self->client->v_angle[YAW];
        g_cur_pitch = self->client->v_angle[PITCH];
        g_aim_init  = true;
    }

    float turn_speed = CvarF(ultron_bot_turn_speed, 540.0f);
    float max_step   = turn_speed * std::max(frametime_s, 0.001f);

    float dyaw   = AngleShortestDelta(g_cur_yaw,   desired_yaw);
    float dpitch = AngleShortestDelta(g_cur_pitch, desired_pitch);
    dyaw   = std::clamp(dyaw,   -max_step, max_step);
    dpitch = std::clamp(dpitch, -max_step, max_step);
    g_cur_yaw   = anglemod(g_cur_yaw + dyaw);
    g_cur_pitch = std::clamp(g_cur_pitch + dpitch, -89.0f, 89.0f);

    // Tiny hand-tremor jitter so the aim isn't perfectly rigid.
    float jitter = CvarF(ultron_bot_aim_jitter, 0.3f);
    float yaw_out   = g_cur_yaw   + (jitter > 0.0f ? crandom() * jitter : 0.0f);
    float pitch_out = g_cur_pitch + (jitter > 0.0f ? crandom() * jitter : 0.0f);
    pitch_out = std::clamp(pitch_out, -89.0f, 89.0f);

    vec3_t cur = { pitch_out, yaw_out, 0.0f };
    self->client->ps.pmove.delta_angles = cur - self->client->resp.cmd_angles;
    self->client->ps.viewangles = cur;
    self->client->v_angle       = cur;
    self->s.angles              = { 0.0f, yaw_out, 0.0f };
}

// Aim at a point in world space. Returns (pitch, yaw, 0) of the ideal look
// direction; caller uses this for downstream fire-cone checks.
static vec3_t AimAtPoint(edict_t *self, const vec3_t &point, float frametime_s) {
    vec3_t eye   = EyePos(self);
    vec3_t ideal = (point - eye).normalized();
    vec3_t desired = vectoangles(ideal);
    if (desired[PITCH] < -180.0f) desired[PITCH] = anglemod(desired[PITCH] + 360.0f);
    AimSmooth(self, desired[YAW], desired[PITCH], frametime_s);
    return desired;
}

// Forward decl: defined later in the movement section.
static void DriveMovement(edict_t *self, usercmd_t *ucmd, const vec3_t &goal_world, float face_yaw);

// ---------------------------------------------------------------------------
// Persistent brain (per-map JSON file)
//
// Each map gets its own file at:
//   %USERPROFILE%\Saved Games\Nightdive Studios\Quake II\Ultron\brain\<map>.json
//
// The brain grows every match we play on the map: games, total frags,
// deaths, and observed item positions (with sighting counts). This is the
// scaffolding for the "Ultron gets stronger the more he plays" feature;
// downstream phases will mine it for priors (hot items at match start,
// tactical positions, etc).

static std::string BrainDir() {
    const char *up = std::getenv("USERPROFILE");
    std::string base = up ? up : ".";
    std::filesystem::path p = std::filesystem::path(base) / "Saved Games" / "Nightdive Studios" / "Quake II" / "Ultron" / "brain";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p.string();
}

static std::string BrainPath(const std::string &map) {
    return (std::filesystem::path(BrainDir()) / (map + ".json")).string();
}

static void LoadBrain(const char *mapname) {
    g_brain = {};
    g_brain.map = mapname ? mapname : "unknown";
    g_brain_loaded = true;
    g_counted_this_game = false;

    std::string path = BrainPath(g_brain.map);
    std::ifstream f(path);
    if (!f.good()) {
        gi.Com_PrintFmt("[ultron/brain] fresh file: {}\n", path);
        return;
    }

    Json::Value root;
    Json::CharReaderBuilder rb;
    std::string errs;
    if (!Json::parseFromStream(rb, f, &root, &errs)) {
        gi.Com_PrintFmt("[ultron/brain] parse failed for {}: {}\n", path, errs);
        return;
    }

    g_brain.games_played = root.get("games_played", 0).asInt();
    g_brain.total_frags  = root.get("total_frags",  0).asInt();
    g_brain.total_deaths = root.get("total_deaths", 0).asInt();
    g_brain.total_shots  = root.get("total_shots",  0).asInt();

    for (const auto &it : root["items"]) {
        UltronItemMemory m;
        m.classname  = it.get("classname", "").asString();
        m.origin[0]  = it.get("x", 0.0f).asFloat();
        m.origin[1]  = it.get("y", 0.0f).asFloat();
        m.origin[2]  = it.get("z", 0.0f).asFloat();
        m.seen_count = it.get("seen_count", 0).asInt();
        g_brain.items.push_back(m);
    }

    gi.Com_PrintFmt("[ultron/brain] loaded: map={} games={} frags={} deaths={} items={}\n",
        g_brain.map, g_brain.games_played, g_brain.total_frags,
        g_brain.total_deaths, (int)g_brain.items.size());
}

static void SaveBrain() {
    if (!g_brain_loaded || g_brain.map.empty()) return;

    Json::Value root;
    root["map"]          = g_brain.map;
    root["games_played"] = g_brain.games_played;
    root["total_frags"]  = g_brain.total_frags;
    root["total_deaths"] = g_brain.total_deaths;
    root["total_shots"]  = g_brain.total_shots;

    Json::Value items(Json::arrayValue);
    for (const auto &m : g_brain.items) {
        Json::Value it;
        it["classname"]  = m.classname;
        it["x"]          = m.origin[0];
        it["y"]          = m.origin[1];
        it["z"]          = m.origin[2];
        it["seen_count"] = m.seen_count;
        items.append(it);
    }
    root["items"] = items;

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "  ";
    std::ofstream f(BrainPath(g_brain.map));
    if (!f.good()) return;
    f << Json::writeString(wb, root);
    g_brain_last_save = level.time;
}

// Call when we see an item on the map (via FindBestPickup scan). Adds
// a new entry if we haven't recorded this classname at this position
// yet, otherwise bumps the sighting count.
static void BrainNoteItemSight(edict_t *e) {
    if (!g_brain_loaded) return;
    if (!e || !e->item || !e->classname) return;
    for (auto &m : g_brain.items) {
        if (m.classname == e->classname &&
            (m.origin - e->s.origin).length() < 64.0f) {
            m.seen_count++;
            return;
        }
    }
    UltronItemMemory m;
    m.classname  = e->classname;
    m.origin     = e->s.origin;
    m.seen_count = 1;
    g_brain.items.push_back(m);
}

// Per-frame brain updater: bumps games_played once per match, totals
// frags/deaths from score deltas and deadflag transitions, saves every
// 30 seconds of wall time.
static void BrainTick(edict_t *self) {
    if (!g_brain_loaded) return;

    if (!g_counted_this_game) {
        g_brain.games_played++;
        g_counted_this_game = true;
        g_last_score_seen = self->client->resp.score;
        gi.Com_PrintFmt("[ultron/brain] game #{} on {} (lifetime frags={} deaths={})\n",
            g_brain.games_played, g_brain.map,
            g_brain.total_frags, g_brain.total_deaths);
    }

    // Score deltas -> frag count.
    int32_t now_score = self->client->resp.score;
    if (now_score > g_last_score_seen) {
        g_brain.total_frags += (now_score - g_last_score_seen);
    }
    g_last_score_seen = now_score;

    // Periodic save to survive crashes.
    if ((level.time - g_brain_last_save).seconds<float>() >= 30.0f) {
        SaveBrain();
    }
}

// Death transition — incremented from UpdateDamageSense which already
// tracks health across frames. Called from there.
static void BrainNoteDeath() {
    if (!g_brain_loaded) return;
    g_brain.total_deaths++;
}

// ---------------------------------------------------------------------------
// Pathfinding via gi.GetPathToGoal

// Cached plan so we don't re-query the engine each frame (cheap but not free).
struct ultron_plan_t {
    vec3_t  goal          = {};       // world-space end goal
    vec3_t  next_waypoint = {};       // current sub-goal to walk toward
    bool    has_plan      = false;
    gtime_t replan_at     = 0_ms;
};
static ultron_plan_t g_plan;

// Replan the path to 'goal' if our cached plan is stale or if goal drifted.
// Returns true if g_plan.next_waypoint is now usable for movement.
static bool PlanTo(edict_t *self, const vec3_t &goal) {
    const bool goal_moved = (goal - g_plan.goal).length() > 96.0f;
    if (g_plan.has_plan && !goal_moved && level.time < g_plan.replan_at) {
        return true;
    }

    std::array<vec3_t, 256> points;
    PathRequest req{};
    req.start = self->s.origin;
    req.goal  = goal;
    req.moveDist = 32.0f;   // matches humanoid step granularity
    req.pathFlags = PathFlags::All;
    req.nodeSearch.minHeight = 64.0f;
    req.nodeSearch.maxHeight = 64.0f;
    req.nodeSearch.radius    = 512.0f;
    req.pathPoints.array = points.data();
    req.pathPoints.count = (int64_t)points.size();

    PathInfo info{};
    if (!gi.GetPathToGoal(req, info) || info.numPathPoints <= 0) {
        g_plan.has_plan = false;
        g_plan.replan_at = level.time + 500_ms;  // don't hammer on failure
        return false;
    }

    // Use firstMovePoint which the engine fills for the initial step. If
    // that is zero, fall back to the first array entry.
    vec3_t wp = info.firstMovePoint;
    if (wp.length() < 1.0f) wp = points[0];

    g_plan.goal          = goal;
    g_plan.next_waypoint = wp;
    g_plan.has_plan      = true;
    g_plan.replan_at     = level.time + 500_ms;  // re-query twice a second
    return true;
}

// Walk toward waypoint if we have one, else toward raw goal. Useful for
// states that know a goal but not a path: try path, fall back to line.
static void MoveTowardGoal(edict_t *self, usercmd_t *ucmd, const vec3_t &goal, float frametime_s) {
    vec3_t aim_point = goal + vec3_t{0, 0, 16.0f};
    if (PlanTo(self, goal)) aim_point = g_plan.next_waypoint + vec3_t{0, 0, 16.0f};
    vec3_t desired = AimAtPoint(self, aim_point, frametime_s);
    DriveMovement(self, ucmd, aim_point, desired[YAW]);
}

// ---------------------------------------------------------------------------
// Item awareness

// Weight of an item for general "pick this up" reasoning. Higher = more
// valuable. Returns 0 for items we don't want (already have plenty of).
static float ItemWeight(edict_t *self, edict_t *item_ent) {
    if (!item_ent->item) return 0.0f;
    item_id_t id = item_ent->item->id;
    gclient_t *c = self->client;

    // Powerups = always grab (Quad / Invuln are match-winners).
    if (id == IT_ITEM_QUAD)             return 180.0f;
    if (id == IT_ITEM_QUADFIRE)         return 150.0f;
    if (id == IT_ITEM_INVULNERABILITY)  return 170.0f;

    // Health: highest when we're hurt.
    float hp_deficit = std::max(0.0f, 100.0f - self->health);
    if (id == IT_HEALTH_MEGA)   return 120.0f + hp_deficit * 0.5f;
    if (id == IT_HEALTH_LARGE)  return  60.0f + hp_deficit * 0.4f;
    if (id == IT_HEALTH_MEDIUM) return  40.0f + hp_deficit * 0.3f;
    if (id == IT_HEALTH_SMALL)  return  15.0f + hp_deficit * 0.2f;

    // Armor. Body (red) is top; combat/jacket scale down; shards are low.
    if (id == IT_ARMOR_BODY)    return 100.0f;
    if (id == IT_ARMOR_COMBAT)  return  70.0f;
    if (id == IT_ARMOR_JACKET)  return  40.0f;
    if (id == IT_ARMOR_SHARD)   return  10.0f;

    // Weapons: big weight if we don't own it, low if we already have one.
    if (item_ent->item->flags & IF_WEAPON) {
        if (c->pers.inventory[id] <= 0) return 80.0f;
        return 8.0f;  // extra copy — mostly just ammo
    }

    // Ammo: only valuable if we have the weapon that uses it and are low.
    if (item_ent->item->flags & IF_AMMO) {
        int cur = c->pers.inventory[id];
        if (cur < 50) return 20.0f;
        return 3.0f;
    }

    // Default trickle value so "something we've never seen" isn't zero.
    return 5.0f;
}

// Only items that are on the map AND currently pickupable (not respawning)
// AND not already consumed by us in coop/instance.
static bool ItemIsPickupable(edict_t *self, edict_t *ent) {
    if (!ent->inuse) return false;
    if (!ent->item)  return false;
    if (ent->svflags & SVF_NOCLIENT) return false;      // hidden / respawning
    if (ent->solid   == SOLID_NOT)   return false;
    return true;
}

// Scan the edict array for the best reachable pickup. "Reachable" here
// means visible via eye-to-origin traceline — the cheap proxy for nav.
// Phase 3 (pathfinding) replaces this LoS check with a real path query.
static edict_t *FindBestPickup(edict_t *self, float max_dist, bool require_los) {
    edict_t *best = nullptr;
    float    best_score = 0.0f;
    const int first = game.maxclients + 1;  // skip player edicts
    for (int i = first; i < (int)globals.num_edicts; i++) {
        edict_t *e = &g_edicts[i];
        if (!ItemIsPickupable(self, e)) continue;
        BrainNoteItemSight(e);  // feed the brain's map knowledge
        float w = ItemWeight(self, e);
        if (w <= 0.0f) continue;
        float d = (e->s.origin - self->s.origin).length();
        if (d > max_dist) continue;
        if (require_los) {
            vec3_t eye = EyePos(self);
            trace_t tr = gi.traceline(eye, e->s.origin + vec3_t{0, 0, 16.0f}, self, MASK_SOLID);
            if (tr.fraction < 0.99f && tr.ent != e) continue;
        }
        // Weight / distance is the usual "value density" heuristic.
        float score = w / std::max(1.0f, d);
        if (score > best_score) {
            best_score = score;
            best       = e;
        }
    }
    return best;
}

// "Health-only" variant for HEAL state — armor counts as a heal proxy
// because damage eats armor first.
static edict_t *FindBestHealthPickup(edict_t *self, float max_dist) {
    edict_t *best = nullptr;
    float    best_score = 0.0f;
    const int first = game.maxclients + 1;
    for (int i = first; i < globals.num_edicts; i++) {
        edict_t *e = &g_edicts[i];
        if (!ItemIsPickupable(self, e)) continue;
        item_id_t id = e->item->id;
        bool is_heal = (id == IT_HEALTH_SMALL || id == IT_HEALTH_MEDIUM ||
                        id == IT_HEALTH_LARGE || id == IT_HEALTH_MEGA ||
                        id == IT_ARMOR_BODY   || id == IT_ARMOR_COMBAT ||
                        id == IT_ARMOR_JACKET || id == IT_ARMOR_SHARD);
        if (!is_heal) continue;
        float d = (e->s.origin - self->s.origin).length();
        if (d > max_dist) continue;
        float w = ItemWeight(self, e);
        float score = w / std::max(1.0f, d);
        if (score > best_score) { best_score = score; best = e; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Weapon knowledge

// Current weapon id, or IT_NULL if unarmed / switching.
static item_id_t CurrentWeaponId(edict_t *self) {
    if (!self->client->pers.weapon) return IT_NULL;
    return self->client->pers.weapon->id;
}

// Fire cone (degrees) appropriate for the current weapon. Cone is how far
// off-axis the aim can be before we decide to press attack. Too wide =
// spray; too tight = we can't hit moving targets at all.
static float WeaponFireCone(item_id_t id) {
    switch (id) {
        case IT_WEAPON_RAILGUN:      return 2.5f;   // hitscan precision
        case IT_WEAPON_MACHINEGUN:
        case IT_WEAPON_CHAINGUN:     return 5.0f;   // hitscan auto-fire
        case IT_WEAPON_HYPERBLASTER:
        case IT_WEAPON_IONRIPPER:    return 5.0f;
        case IT_WEAPON_BLASTER:      return 6.0f;
        case IT_WEAPON_RLAUNCHER:    return 4.0f;   // splash forgives some miss
        case IT_WEAPON_GLAUNCHER:
        case IT_WEAPON_PROXLAUNCHER: return 6.0f;   // arc compensation
        case IT_WEAPON_SHOTGUN:
        case IT_WEAPON_SSHOTGUN:     return 10.0f;  // spread forgives wide
        case IT_WEAPON_BFG:          return 3.0f;
        default:                     return 8.0f;
    }
}

// Effective projectile speed (units/sec). 0 means hitscan — no lead.
static float WeaponProjectileSpeed(item_id_t id) {
    switch (id) {
        case IT_WEAPON_BLASTER:      return 1000.0f;
        case IT_WEAPON_HYPERBLASTER: return 1000.0f;
        case IT_WEAPON_IONRIPPER:    return 800.0f;
        case IT_WEAPON_RLAUNCHER:    return 650.0f;   // g_weapon.cpp:1295
        case IT_WEAPON_GLAUNCHER:    return 600.0f;
        case IT_WEAPON_BFG:          return 400.0f;
        // hitscan / melee / unknown → no lead
        default: return 0.0f;
    }
}

// Switch to weaponIndex now, bypassing the SVF_BOT gate in Bot_SetWeapon.
// Mirrors the internal logic of bots/bot_exports.cpp:42-60. Safe to call
// every frame; it short-circuits when the weapon is already wielded or
// pending, and when we don't own it.
static void Ultron_SelectWeapon(edict_t *self, item_id_t weapon_id) {
    if (weapon_id <= IT_NULL || weapon_id >= IT_TOTAL) return;
    gclient_t *client = self->client;
    if (!client) return;
    if (!client->pers.inventory[weapon_id]) return;
    if (client->pers.weapon && client->pers.weapon->id == weapon_id) return;
    if (client->newweapon   && client->newweapon->id   == weapon_id) return;
    gitem_t *item = &itemlist[weapon_id];
    if (!(item->flags & IF_WEAPON)) return;
    if (!item->use) return;
    client->no_weapon_chains = true;
    item->use(self, item);
}

// Pick the best weapon for a given engagement range. Only considers weapons
// in our inventory with ammo. Falls back to blaster (always owned) if
// nothing matches.
static item_id_t PickBestWeapon(edict_t *self, float range) {
    gclient_t *c = self->client;
    auto have = [&](item_id_t id) -> bool {
        return c->pers.inventory[id] > 0;
    };
    // Preference order: tight-range to long-range table, filtered by ammo.
    // For each weapon, also ensure we have ammo for it.
    auto ammo_for = [&](item_id_t id) -> item_id_t {
        switch (id) {
            case IT_WEAPON_SHOTGUN:
            case IT_WEAPON_SSHOTGUN:     return IT_AMMO_SHELLS;
            case IT_WEAPON_MACHINEGUN:
            case IT_WEAPON_CHAINGUN:     return IT_AMMO_BULLETS;
            case IT_WEAPON_HYPERBLASTER:
            case IT_WEAPON_BFG:          return IT_AMMO_CELLS;
            case IT_WEAPON_RAILGUN:      return IT_AMMO_SLUGS;
            case IT_WEAPON_RLAUNCHER:    return IT_AMMO_ROCKETS;
            case IT_WEAPON_GLAUNCHER:    return IT_AMMO_GRENADES;
            case IT_WEAPON_IONRIPPER:    return IT_AMMO_CELLS;
            default:                     return IT_NULL;
        }
    };
    auto usable = [&](item_id_t id) -> bool {
        if (!have(id)) return false;
        item_id_t a = ammo_for(id);
        if (a == IT_NULL) return true;  // melee or blaster
        return c->pers.inventory[a] > 0;
    };

    // Priority lists by range band. First usable in the list wins.
    if (range < 300.0f) {
        const item_id_t order[] = { IT_WEAPON_SSHOTGUN, IT_WEAPON_CHAINGUN,
                                    IT_WEAPON_HYPERBLASTER, IT_WEAPON_RLAUNCHER,
                                    IT_WEAPON_MACHINEGUN, IT_WEAPON_SHOTGUN,
                                    IT_WEAPON_BLASTER };
        for (auto id : order) if (usable(id)) return id;
    } else if (range < 900.0f) {
        const item_id_t order[] = { IT_WEAPON_RLAUNCHER, IT_WEAPON_HYPERBLASTER,
                                    IT_WEAPON_CHAINGUN, IT_WEAPON_RAILGUN,
                                    IT_WEAPON_MACHINEGUN, IT_WEAPON_SSHOTGUN,
                                    IT_WEAPON_BLASTER };
        for (auto id : order) if (usable(id)) return id;
    } else {
        const item_id_t order[] = { IT_WEAPON_RAILGUN, IT_WEAPON_HYPERBLASTER,
                                    IT_WEAPON_RLAUNCHER, IT_WEAPON_MACHINEGUN,
                                    IT_WEAPON_CHAINGUN, IT_WEAPON_BLASTER };
        for (auto id : order) if (usable(id)) return id;
    }
    return IT_WEAPON_BLASTER;  // always owned
}

// Predict where target will be after the projectile's time-of-flight, assuming
// straight-line travel. Zero speed returns the raw origin (hitscan).
static vec3_t LeadPoint(edict_t *shooter, edict_t *target, float proj_speed) {
    vec3_t tp = EyePos(target);
    if (proj_speed <= 0.0f) return tp;
    vec3_t sp = EyePos(shooter);
    float dist = (tp - sp).length();
    float t    = dist / proj_speed;
    // Cap lead to avoid wild overshoot on high-velocity targets or lag spikes.
    t = std::clamp(t, 0.0f, 1.5f);
    return tp + target->velocity * t;
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

// Returns true if self's current view forward is within the weapon's fire
// cone of the direction to target_point. The cone is per-weapon; the cvar
// ultron_bot_fire_cone is a global override when > 0.
static bool AimWithinCone(edict_t *self, const vec3_t &target_point) {
    vec3_t eye = EyePos(self);
    vec3_t to_target = (target_point - eye).normalized();

    vec3_t view_fwd;
    AngleVectors(self->client->v_angle, view_fwd, nullptr, nullptr);

    float dot = view_fwd.dot(to_target);
    float override_deg = CvarF(ultron_bot_fire_cone, 0.0f);
    float cone_deg = (override_deg > 0.0f) ? override_deg
                                           : WeaponFireCone(CurrentWeaponId(self));
    float cos_thresh = cosf(cone_deg * PIf / 180.0f);
    return dot >= cos_thresh;
}

// ---------------------------------------------------------------------------
// Explore / wander

// Simple wander: pick a yaw, walk forward, avoid walls, rotate on stuck.
// Runs when no target is known. Exits with ucmd filled for this frame.
static void DriveWander(edict_t *self, usercmd_t *ucmd, float frametime_s) {
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

    // Aim where we're walking — smoothed so the camera doesn't snap.
    AimSmooth(self, g_wander_yaw, 0.0f, frametime_s);

    // Forward at full speed; no strafe during exploration.
    ucmd->forwardmove = speed;
    ucmd->sidemove    = 0.0f;

    // Occasional jump to break pathing on stairs.
    if (frandom() < 0.01f) {
        ucmd->buttons |= BUTTON_JUMP;
    }
}

// ---------------------------------------------------------------------------
// State machine

enum ultron_state_t {
    UST_INTERMISSION,
    UST_RESPAWN,
    UST_SPECTATOR,
    UST_COMBAT,    // visible enemy, fight
    UST_HUNT,      // enemy in memory (last-seen), chase
    UST_HEAL,      // low HP, break engagement (falls back to wander for now)
    UST_LOOT,      // pursue known item (Phase 5 will flesh this out)
    UST_WANDER     // explore, nothing known
};

static const char *StateName(ultron_state_t s) {
    switch (s) {
        case UST_INTERMISSION: return "intermission";
        case UST_RESPAWN:      return "respawn";
        case UST_SPECTATOR:    return "spectator";
        case UST_COMBAT:       return "combat";
        case UST_HUNT:         return "hunt";
        case UST_HEAL:         return "heal";
        case UST_LOOT:         return "loot";
        case UST_WANDER:       return "wander";
    }
    return "?";
}

static ultron_state_t DecideState(edict_t *self, edict_t *visible_enemy,
                                  bool have_memory, bool heard_recently) {
    if (level.intermissiontime != 0_ms)      return UST_INTERMISSION;
    if (self->deadflag || self->client->awaiting_respawn) return UST_RESPAWN;
    if (self->client->resp.spectator)        return UST_SPECTATOR;

    if (visible_enemy) {
        // Low-HP retreat is enabled when we have an enemy that can't be
        // quickly killed. Tuning: simple health threshold for now.
        if (self->health <= 30) return UST_HEAL;
        return UST_COMBAT;
    }
    if (have_memory) return UST_HUNT;
    if (heard_recently) return UST_HUNT;  // converge on the sound

    // No enemy and no memory — consider LOOT if a valuable pickup is near.
    // We no longer require LoS; pathfinding in State_Loot handles occluded
    // goals across rooms.
    edict_t *pick = FindBestPickup(self, 1600.0f, false);
    if (pick) return UST_LOOT;

    return UST_WANDER;
}

// Damage-direction aim bias. Populated when we take new damage this frame.
static int32_t g_last_health = 100;
static vec3_t  g_damage_from = {};
static gtime_t g_damage_at   = 0_ms;

static void UpdateDamageSense(edict_t *self) {
    if (self->health < g_last_health && self->health > 0) {
        g_damage_from = self->client->damage_from;
        g_damage_at   = level.time;
    }
    // Death transition: health just dropped to <= 0. Count it once.
    static bool was_dead = false;
    bool is_dead = (self->health <= 0) || self->deadflag;
    if (is_dead && !was_dead) BrainNoteDeath();
    was_dead = is_dead;
    g_last_health = self->health;
}

// Combat state: aim at target (with projectile lead), fire through the
// weapon-specific cone, strafe-dodge, and walk toward the target.
static void State_Combat(edict_t *self, usercmd_t *ucmd, edict_t *target, float frametime_s) {
    g_target_ticks++;
    g_wander_yaw_until = 0_ms;   // drop any stale wander plan

    // Weapon selection based on engagement range. Rate-limited via hysteresis
    // inside Ultron_SelectWeapon (it short-circuits when switch already
    // pending) so per-frame calls are safe.
    float range = (target->s.origin - self->s.origin).length();
    if (CvarI(ultron_bot_auto_weapon, 1)) {
        item_id_t best = PickBestWeapon(self, range);
        Ultron_SelectWeapon(self, best);
    }

    // Projectile lead if our current weapon is not hitscan.
    float proj_speed = WeaponProjectileSpeed(CurrentWeaponId(self));
    vec3_t aim_point = LeadPoint(self, target, proj_speed);

    vec3_t desired = AimAtPoint(self, aim_point, frametime_s);
    DriveMovement(self, ucmd, aim_point, desired[YAW]);

    if (AimWithinCone(self, aim_point) && !CvarI(ultron_bot_no_fire, 0)) {
        ucmd->buttons |= BUTTON_ATTACK;
        g_fire_ticks++;
    }
}

// Hunt state: no visible enemy. Prefer the trajectory-extrapolated memory
// position when we have a recent sighting; otherwise fall back to the most
// recent "heard at" hint from UpdateHearing.
static void State_Hunt(edict_t *self, usercmd_t *ucmd, float frametime_s) {
    vec3_t goal;
    if (g_mem.last_seen_at != 0_ms &&
        (level.time - g_mem.last_seen_at).seconds<float>() < 3.0f) {
        goal = ExtrapolateLastSeen();
    } else {
        goal = g_mem.heard_at;
    }
    MoveTowardGoal(self, ucmd, goal, frametime_s);
}

// Heal state: low HP. Walk toward the nearest reachable health/armor. If
// nothing in sight, run away from last damage direction and fall through
// to wander so we at least keep moving.
static void State_Heal(edict_t *self, usercmd_t *ucmd, float frametime_s) {
    edict_t *heal = FindBestHealthPickup(self, 2000.0f);
    if (heal) {
        MoveTowardGoal(self, ucmd, heal->s.origin, frametime_s);
        return;
    }
    // No heal item visible — break line with whoever hurt us last.
    if (g_damage_at != 0_ms && (level.time - g_damage_at).seconds<float>() < 3.0f) {
        vec3_t away = (self->s.origin - g_damage_from);
        away[2] = 0.0f;
        if (away.length() > 1.0f) {
            away.normalize();
            float target_yaw = vectoangles(away)[YAW];
            AimSmooth(self, target_yaw, 0.0f, frametime_s);
            float speed = CvarF(ultron_bot_move_speed, DEFAULT_MOVE_SPEED);
            ucmd->forwardmove = speed;
            ucmd->sidemove    = (frandom() < 0.5f) ? -speed : speed;
            return;
        }
    }
    DriveWander(self, ucmd, frametime_s);
}

// Loot state: walk straight at the highest-value nearby pickupable item.
// Phase 3 will replace this walk-line with real pathfinding so we can
// cross rooms rather than just grabbing whatever happens to be in LoS.
static void State_Loot(edict_t *self, usercmd_t *ucmd, float frametime_s) {
    edict_t *pick = FindBestPickup(self, 1600.0f, false);
    if (!pick) { DriveWander(self, ucmd, frametime_s); return; }
    MoveTowardGoal(self, ucmd, pick->s.origin, frametime_s);
}

// Respawn / intermission button pulsers, extracted so dispatcher stays clean.
static void State_Intermission(edict_t *self, usercmd_t *ucmd) {
    if ((level.time - level.intermissiontime) > 5_sec
        && ((level.time.milliseconds() / 200) % 30) == 0)
    {
        ucmd->buttons |= BUTTON_ATTACK;
    }
}
static void State_Respawn(edict_t *self, usercmd_t *ucmd) {
    if ((level.time.milliseconds() / 200) % 5 == 0)
        ucmd->buttons |= BUTTON_ATTACK;
}

// ---------------------------------------------------------------------------
// Top-level

void Ultron_Bot_Command(edict_t *self, usercmd_t *ucmd) {
    if (!ultron_play_self || !ultron_play_self->integer) return;
    if (!self || !self->client) return;
    // Don't drive during a single-player session. The engine can briefly
    // boot into SP on the first frame before the autostart's gamemap
    // reload flips deathmatch from latched -> active; we'd otherwise see
    // Ultron walking around in SP which looks like an attract screen.
    if (!deathmatch || !deathmatch->integer) return;

    // Actively fight the engine's cursor grab every frame. Cheap Win32
    // calls; engine re-grabs on its next frame, we un-grab on this one.
    Ultron_FreeMouseCursor();

    // Capture the engine-provided frametime BEFORE we overwrite ucmd, so the
    // smoothed-aim rate limit is correct at any client tick rate.
    float frametime_s = (float)ucmd->msec / 1000.0f;
    if (frametime_s <= 0.0f) frametime_s = 0.025f;  // 40Hz server fallback

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

    // Refresh damage-sense + hearing + brain tick before state handlers run.
    UpdateDamageSense(self);
    UpdateHearing(self);
    BrainTick(self);

    // Perception: nearest visible enemy, with configurable last-seen memory.
    const gtime_t memory_window = gtime_t::from_ms((int64_t)CvarI(ultron_bot_memory_ms, 2000));
    edict_t *vis = FindVisibleEnemy(self);
    if (vis) {
        g_mem.target       = vis;
        g_mem.last_seen    = EyePos(vis);
        g_mem.last_seen_vel= vis->velocity;
        g_mem.last_seen_at = level.time;
    } else if (g_mem.target && (level.time - g_mem.last_seen_at) > memory_window) {
        g_mem.target = nullptr;
    }
    bool have_memory = (g_mem.target != nullptr);
    bool heard_recently = (g_mem.heard_at_time != 0_ms &&
                           (level.time - g_mem.heard_at_time).seconds<float>() < 2.0f);

    ultron_state_t state = DecideState(self, vis, have_memory, heard_recently);

    switch (state) {
        case UST_INTERMISSION: State_Intermission(self, ucmd); return;
        case UST_RESPAWN:      State_Respawn(self, ucmd);      return;
        case UST_SPECTATOR:                                    return;
        case UST_COMBAT:       State_Combat(self, ucmd, vis, frametime_s); break;
        case UST_HUNT:         State_Hunt(self, ucmd, frametime_s);         break;
        case UST_HEAL:         State_Heal(self, ucmd, frametime_s);         break;
        case UST_LOOT:         State_Loot(self, ucmd, frametime_s);         break;
        case UST_WANDER:
            g_nothing_ticks++;
            if (CvarI(ultron_bot_wander, 1))
                DriveWander(self, ucmd, frametime_s);
            break;
    }

    // Verbose per-frame dev log. Rate-limit to ~4 Hz.
    if (CvarI(ultron_bot_debug, 0))
    {
        static gtime_t last_dbg = 0_ms;
        if ((level.time - last_dbg).milliseconds() >= 250)
        {
            last_dbg = level.time;
            gi.Com_PrintFmt("[ultron/dbg] state={} hp={} wpn={} fwd={:.0f} side={:.0f} fire={}\n",
                StateName(state), self->health,
                self->client->pers.weapon ? (int)self->client->pers.weapon->id : -1,
                ucmd->forwardmove, ucmd->sidemove,
                (ucmd->buttons & BUTTON_ATTACK) ? 1 : 0);
        }
    }
}
