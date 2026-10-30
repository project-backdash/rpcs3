# Phase delay in lockstep phases

Switch: `PBD_PEER_PHASE_DELAY`, on by default. It needs the entry sample (`PBD_PEER_ENTRY_SAMPLE`) and an input
delay of 1 or more. Code: the `peer.phase_delay` fields of the session state in
`rpcs3/Emu/Cell/pbd_rollback_session_storage_peer.h`, and `rpcs3/Emu/PBD/pbd_phase_input_steps.h`, called by
`assign_phase_delay()` in `pbd_rollback_session_correction.cpp` (the push model that fixes each sample's tick).

## Why

Every datagram names the tick of each input value, so how far ahead a peer schedules its own samples is the sender's
business. The game's round intro, round transitions and ending are lockstep phases: every tick waits for the other
peer's input of that tick. With an input delay D below the one-way time, a lockstep phase runs at one tick per
(one-way time / D). On a 100 ms path at D 2 that is a hold of 13-24 ms on nearly every boundary until "Fight!": about
45 fps, with stuttering audio at the start and end of a round.

## How

- In a lockstep phase, the sample of boundary b goes to tick b + max(D, L), where L = ceil((one-way time + 8 ms) / tick)
  and 1 <= L <= 6 in round intros and transitions. At character select (scene 3, subscene 44) L follows the
  measured one-way time up to 30 ticks, which only bounds a nonsensical measurement; leaving character select starts
  a new phase. With the ceiling of 6 everywhere, a 220 ms round trip ran character select at about 45 fps.
- L is re-evaluated at every lockstep boundary and never decreases within a phase.
- Ticks skipped when the delay grows repeat the newest fixed value ("fill" rows).
- Back in the fight the delay shrinks to D without losing input. A sample whose tick is already taken is dropped only
  if it equals the newest fixed value; otherwise it goes to the next free tick ("deferred").
- After 60 boundaries still above D, one sample per boundary is dropped regardless ("forced"). These count as lost
  input.
