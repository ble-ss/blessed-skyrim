// halfrate_switch - the in-game switch for the fork's half-rate far field (a hotkey and an ini line).
#pragma once

// reads [HalfRate] from skybench.ini next to the plugin dll:
//   bEnabled=1      the state at game start (default 1, on)
//   sToggleKey=F8   the toggle key: a key name (F1-F24, A-Z, 0-9) or a
//                   virtual-key code (0x77 or 119). default F8.
// SKYBENCH_HALFRATE=0/1 overrides bEnabled (for the harness).
// publishes the state in the shared memory the fork reads
// (Local\BlessedHalfRate), and starts a small thread that watches the key
// while the game window has focus. every toggle is logged as an event.
// call once from SKSEPlugin_Load. works on any runtime: it hooks nothing.
void InstallHalfRateSwitch();
