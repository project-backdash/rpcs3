# Battle clock after a restore

Code: `resume_correction_clock()` in `rpcs3/Emu/PBD/pbd_correction_execution.cpp`.

## Why

A correction at boundary T restores the entry of tick m = T - depth and skips m's entry stage. If `battle_clock.tick` stayed
T, the first re-executed tick would run under the battle-clock time of tick T:

- every `mftb` read would be `depth` ticks late, and the restore-site adapter would move the frame start to T to keep the
  differences coherent;
- absolute time stored by tick m would then differ from a straight run (guest pages 0x1074f000, 0x10742000, 0x10749000
  and 0x1074b000, from the first re-executed tick on). On screen this shows as lost spark effects at depth 1.

## How

The restore puts `battle_clock.tick` back to m and keeps the restored frame start, so the re-executed ticks read the same
clock as the straight run did.
