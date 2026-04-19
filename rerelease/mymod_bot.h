// Claude-driven player bot. MVP: takes over the human slot and plays 1v1 DM
// against the Kex engine's stock bots.
#pragma once

struct edict_t;
struct usercmd_t;

// Called from ClientConnect / ClientDisconnect to track which edict is the
// human. g_edicts[1] isn't reliable when bots race the human for slot 1.
void MyMod_OnClientConnect(edict_t *ent, bool isBot);
void MyMod_OnClientDisconnect(edict_t *ent);
bool MyMod_IsHuman(edict_t *ent);

// The intercept. Mutates *ucmd in place to drive the human's character from
// bot logic. Safe to call unconditionally; the function checks mymod_play_self
// and MyMod_IsHuman() internally (but the caller already gates on those for
// the intermission/respawn carve-outs, so this is defense in depth).
void MyMod_Bot_Command(edict_t *self, usercmd_t *ucmd);

// Called from InitGame to register mymod_play_self cvar and reset per-game state.
void MyMod_Bot_Init();
