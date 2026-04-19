// Claude-driven player bot. MVP: takes over the human slot and plays 1v1 DM
// against the Kex engine's stock bots.
#pragma once

struct edict_t;
struct usercmd_t;

// Called from ClientConnect / ClientDisconnect to track which edict is the
// human. g_edicts[1] isn't reliable when bots race the human for slot 1.
void Ultron_OnClientConnect(edict_t *ent, bool isBot);
void Ultron_OnClientBegin(edict_t *ent);
void Ultron_OnClientDisconnect(edict_t *ent);
bool Ultron_IsHuman(edict_t *ent);

// The intercept. Mutates *ucmd in place to drive the human's character from
// bot logic. Safe to call unconditionally; the function checks ultron_play_self
// and Ultron_IsHuman() internally (but the caller already gates on those for
// the intermission/respawn carve-outs, so this is defense in depth).
void Ultron_Bot_Command(edict_t *self, usercmd_t *ucmd);

// Called from InitGame to register ultron_play_self cvar and reset per-game state.
void Ultron_Bot_Init();
