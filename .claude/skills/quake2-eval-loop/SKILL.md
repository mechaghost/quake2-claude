---
name: quake2-eval-loop
description: Run a bounded eval of the Claude-driven bot, collect structured telemetry, and use it to iterate on bot behavior. Use when the user asks to test, measure, evaluate, benchmark, profile, or iterate on the bot's performance. Use before and after any change to bot combat/aim/movement code. Turns "launch, play, close, tweak, repeat" into a single command.
tools: Read, Bash, Edit
---

# Eval harness for the Claude bot

Lets Claude run the game autonomously, capture structured logs, kill the process, and parse metrics — all without a human in the loop. Drives the change-and-measure workflow.

## One-shot

```
eval.bat -Duration 30
```

This:
1. Clears stdout.txt / stderr.txt / CRASHLOG.TXT.
2. Launches the game via `modlaunch.ps1` (passes `-EvalSeconds 30`, so the mod auto-`quit`s after 30s of human gameplay).
3. Waits 6s for the engine to boot, then polls `Get-Process -Name quake2ex_steam` until it exits or the deadline (Duration + 20s) passes. Force-kills if still running.
4. Parses `[ultron]` and `[eval]` lines from `stdout.txt`.
5. Prints a summary + JSON tail suitable for downstream parsing.
6. Copies the full stdout to `eval-logs/eval-<timestamp>.log` (gitignored).

## Flags

```
eval.bat -Duration 60                 # longer match
eval.bat -Map q2dm3                   # different map
eval.bat -Runs 3                      # repeat N times and aggregate mean stats
eval.bat -Scenario stationary         # enemy bot can't move
eval.bat -Scenario passive            # enemy bot can't fight back
eval.bat -Scenario noaim              # enemy bot can't aim
eval.bat -Scenario deaf               # enemy bot can't sense
eval.bat -Scenario hardmode           # enemy bot has perfect aim
eval.bat -Scenario crippled           # enemy bot disabled entirely
eval.bat -BotSkill 3                  # 0..3, Kex bot difficulty
eval.bat -DebugBot                    # enables ultron_bot_debug + bot_debugSystem
eval.bat -Cvars @{ultron_bot_fire_cone=3; ultron_bot_no_strafe=1}
eval.bat -Quiet                       # suppress the tail-of-log dump
eval.bat -KeepRunning                 # launch but don't wait/kill (manual poke)
```

## Run the baseline suite

```
eval-suite.bat                              # default: stationary, passive, baseline, hardmode, 20s each
eval-suite.bat -Duration 30 -Runs 2         # 30s per run, 2 runs per scenario
eval-suite.bat -Scenarios baseline,stationary
```

Per-scenario table + aggregate total_score. Appends a row to `eval-logs/suite-history.csv` keyed by git commit SHA so progress is trackable across commits. Emits `SUITE_JSON:` line for automation.

## Compare two runs

```
eval-compare.bat -Latest 2                     # diff the two most recent logs
eval-compare.bat -A baseline.log -B hard.log   # diff specific files (auto-resolves in eval-logs/)
```

Prints a per-metric A / B / delta / % change table.

## Tuning knobs — set at launch without rebuilding

All `ultron_bot_*` cvars are runtime-live; pass via `-Cvars` or set in console.

| Cvar | Default | Purpose |
|---|---|---|
| `ultron_bot_fire_cone` | 8.0 | Degrees; fire if view-forward within this cone of target |
| `ultron_bot_move_speed` | 400 | Units/sec; Q2 run speed |
| `ultron_bot_strafe_period` | 500 | Milliseconds between strafe flips |
| `ultron_bot_backpedal_dist` | 200 | Back away when closer than this |
| `ultron_bot_memory_ms` | 2000 | Last-seen target memory window |
| `ultron_bot_no_fire` | 0 | 1 = never fire (movement-only test) |
| `ultron_bot_no_move` | 0 | 1 = stand still (aim-only test) |
| `ultron_bot_no_strafe` | 0 | 1 = disable combat strafe dodge |
| `ultron_bot_debug` | 0 | 1 = log per-decision at ~4 Hz |
| `ultron_play_self` | 1 | 0 = hand control back to human |
| `ultron_eval_seconds` | 0 | >0 = auto-quit after N seconds |

## DM-mode validity check

`dm_valid` fields tell you whether the match actually entered DM. Two signals:
- Log contains `[ultron] autostart: ... deathmatch=1` banner or `[eval] start ... deathmatch=1`.
- At least one non-Mechaghost "`<name> connected.`" line (confirming `addbot` actually spawned an opponent — bot names can have spaces, e.g. "The Makron").

When `dm_invalid > 0` in the aggregate, treat the run's numbers as untrustworthy.

## Brain file

Each run loads and saves `%USERPROFILE%\Saved Games\Nightdive Studios\Quake II\Ultron\brain\<map>.json`. `games_played` and other lifetime counters grow. Inspect manually to watch Ultron's knowledge accumulate.

## What the JSON line contains

```json
{
  "duration_s": 30,          // requested duration
  "crashed": false,          // whether CRASHLOG.TXT was created
  "stdout_lines": 380,       // size of engine log
  "ultron_lines": 32,        // lines matching [ultron] or [eval]
  "eval_lines": 27,          // lines matching [eval] only
  "log_file": "eval-logs/eval-20260418-224605.log",
  "last_t": 29.9,            // last telemetry timestamp (elapsed seconds)
  "last_score": 3,           // human player's frags at end
  "last_health": 45,         // health at end
  "fire_ticks": 820,         // ticks we held BUTTON_ATTACK
  "target_ticks": 924,       // ticks we saw a visible enemy
  "idle_ticks": 210          // ticks with no target and no memory
}
```

`target_ticks == fire_ticks` means the bot fires whenever it sees — could be aggressive or could mean fire-cone is too wide. `fire_ticks > 0 && score == 0` means we're shooting and missing. `health` drops over time → the enemy is hitting us.

## Telemetry producer

The mod stamps one `[eval]` line per ~1 sec from `Ultron_Bot_Command` in `rerelease/ultron_bot.cpp`. It's driven by the `ultron_eval_seconds` cvar set via `+set ultron_eval_seconds N` (launcher does this when `-EvalSeconds N` is passed).

Auto-quit fires `gi.AddCommandString("quit\n")` once elapsed ≥ deadline. A final `[eval] done …` line is emitted first.

## Iteration loop (what Claude should actually do)

1. Read the current bot behavior (e.g. `rerelease/ultron_bot.cpp` — perception, aim, fire, movement).
2. Form a hypothesis: "tighter fire cone will raise accuracy without crushing target_ticks."
3. Make one surgical change.
4. `eval.bat -Duration 30`.
5. Compare the JSON to the last run. Score, fire_ticks, target_ticks, health.
6. Commit + push if the change is a clear improvement (standing auto-commit authorization on this repo).
7. Repeat.

## Controlled difficulty via Kex bot cvars

See `docs/kex-vocab-guide.md`. Useful for isolating what the Claude bot struggles with:

- `+set bot_aim_disabled 1` — opponent can't aim. Tests our movement + map control without incoming fire.
- `+set bot_combatDisabled 1` — opponent doesn't fight. Pure target-tracking test.
- `+set bot_move_disable 1` — stationary target. Tests aim precision.
- `+set bot_senses_disabled 1` — opponent can't see us. Baseline for "how much of our damage is from stock bots' sensing".
- `+set bot_aim_instant 1` — perfect-aim opponent. Hard mode.
- `+set bot_followPlayer 1` — opponent chases us, good for duel practice.

Pass these as launch-time overrides via `modlaunch.bat -StartMap q2dm1` followed by manual cvar set, or extend `modlaunch.ps1` with a `-BotCvars` dict param if the eval loop needs them frequently.

## Known failure modes and their signature

- **target_ticks=0 for entire run** → no enemy spawned. Check the log for `addbot` success. The Kex console command is `addbot` (NOT `bot_add` or `sv_addbot`). The mod fires this from `Ultron_OnClientBegin` when `active_players().count < 2`.
- **Process force-killed** → auto-quit never fired. Usually means eval cvar wasn't set, or human never reached ClientBegin (e.g., stuck in menu). Check `stdout.txt` for `[eval] start` line.
- **`stdout_lines: 0`** → the harness read the log before the engine wrote it. The 6s startup wait should cover this; if not, raise it.
- **Engine crashed (crashed: true)** → `D:\SteamLibrary\steamapps\common\Quake 2\rerelease\CRASHLOG.TXT` has the backtrace. The harness dumps its head automatically.
- **Logs land mid-line** → Kex's log prints sometimes omit trailing newlines, so our `[ultron]` string gets concatenated with an engine log line. The eval parser uses an unanchored regex (`\[(ultron|eval)\]`) to still catch them.

## Important file locations

- `eval.ps1` / `eval.bat` — the harness itself.
- `rerelease/ultron_bot.cpp` — telemetry producer, bot logic.
- `%USERPROFILE%\Saved Games\Nightdive Studios\Quake II\stdout.txt` — engine's log. The harness copies this into `eval-logs/` each run.
- `D:\SteamLibrary\steamapps\common\Quake 2\rerelease\CRASHLOG.TXT` — post-crash backtrace.
