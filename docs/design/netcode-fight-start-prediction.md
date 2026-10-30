# Predicted fight start

Switches: `PBD_PEER_FIGHT_START` (the first actionable tick) and `PBD_PEER_FIGHT_PREDICT` (the mode, 2 by
default). Code: the `peer.fight_start`, `peer.live.suppress` and `peer.live.edge_carry` fields of the session state in
`rpcs3/Emu/Cell/pbd_rollback_session_storage_peer.h`, used by
`rpcs3/Emu/PBD/pbd_phase_input_steps.h` through `assign_phase_delay()` in
`pbd_rollback_session_correction.cpp`. This builds on [phase delay](netcode-phase-delay.md).

## Why

Detecting the first fight boundary is retrospective. Shrinking the delay only after it would leave the first
actionable ticks of the round at the lockstep delay L. For example, at D 2 and L 4, ticks 226 to 231 would be fed from
samples four boundaries old, and 232 from three.

In the start state the launcher uses, a button that goes down at tick 226 or later starts an attack. One that goes down
at 225 and stays held does nothing: the game buffers nothing before the round, so a press needs its edge inside
actionable time.

## How

`PBD_PEER_FIGHT_START = X` names the start state's first actionable tick (226; the launcher reads it from the start
state's declaration).

- The samples of boundaries X-L .. X-D-1 only have ticks below X to go to, and those are already fixed. They are
  dropped ("predrop"), and tick X is fed by boundary X-D at delay D.
- A button that went down inside the dropped window would otherwise first appear at X as a fresh press, which the game
  would turn into an attack the offline game never starts. Such buttons stay masked until they are physically released
  (`live_suppress`). Directions are never masked.

Modes (`PBD_PEER_FIGHT_PREDICT`):

| Mode | Behaviour |
|---|---|
| 0 | Shrink the delay after the first fight boundary is detected. |
| 1 | Shrink before X and run ticks X up to the detection in lockstep: one hold of about (one-way time - D ticks) at X. |
| 2 (default) | Shrink before X and run the ticks from X to the detection as ordinary rollback ticks, with no hold. |

A prediction that does not come true within 8 boundaries falls back to mode 0.

## Edges of released buttons

The opposite case: a button that tick X-1 still carries (its sample is L boundaries old), that was released inside the
dropped window and is down again in the first sample fixed afterwards, would reach the game without a rising edge (down
at X-1, down at X), and the press would be swallowed.

- Such a bit is left out of that one tick (`live_released`, "edge_deferred" rows).
- If the button is already up again at the next fixed sample (a one-frame tap), the bit is added to that sample
  (`live_edge_carry`, "edge_carried" rows).

The press then arrives one tick late instead of never.
