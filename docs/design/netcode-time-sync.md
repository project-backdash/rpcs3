# Time sync between the two peers

Switch: `PBD_PEER_SYNC`, on by default (`0` turns it off). Code: the `peer.sync` fields of the session state in
`rpcs3/Emu/Cell/pbd_rollback_session_storage_peer.h`, the timing adjustment in
`rpcs3/Emu/PBD/pbd_peer_timing.cpp` and the opening alignment in `rpcs3/Emu/PBD/pbd_peer_boundary_steps.h`.

## Why

The opening lockstep phase lets the faster peer run input-delay ticks ahead, and without time sync nothing pulls it
back. That peer then sits several ticks ahead for the whole match. It predicts every remote input and rolls back
hundreds of times, even on a LAN, while the other peer never rolls back, so the input delay buys it nothing.

## How

1. A receive thread stamps every datagram on arrival. A boundary poll would stamp it up to a frame late, which puts a
   systematic error of up to one tick into the advantage estimate and a frame into every round-trip sample.
2. Every message (peer message version 5 and later) carries the sender's own smoothed advantage. Once eight round-trip
   samples and the remote estimate are available, each peer acts on half the difference between its filtered local
   estimate and the remote one. A bias common to both estimates, such as jitter or asymmetric socket waits, cancels.
3. The scheduled vblank grid moves by at most 0.3 tick per adjustment, evaluated at least 45 boundaries apart outside
   the 0.12-tick dead band. `pbd::replay_pacing::slew_us` spreads that shift across frames at a bounded 1.5 % rate change.
   During active play, an error of -0.8 tick or less grants a catch-up frame when vblank pacing is enabled.
4. Before `roll_begin`, opening lockstep alignment uses the same symmetric error. After the first eight boundaries,
   an error greater than 0.6 tick holds the ahead peer for two vblank periods, at least 16 boundaries between holds.

## The estimator

- The estimator takes one step per boundary, not a second one on the pass after a correction.
- A catch-up frame does not add 1.0 to the filter state. Recurring catch-ups would otherwise leave a standing offset that
  stops further catch-ups while the peer is still behind.
- Catch-up begins after the first 16 boundaries and is spaced at least 16 boundaries after the preceding stall or
  catch-up. Slew waits 16 boundaries after a catch-up so the filter can absorb the changed timing.
