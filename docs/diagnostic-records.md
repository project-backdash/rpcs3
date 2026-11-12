# Diagnostic records

What the session writes about itself while a game runs. None of it is authoritative: play never waits for a
diagnostic record, and losing one never ends a game. [ARCHITECTURE.md](../ARCHITECTURE.md) describes the session.

## Confirmed-row trace

Continuous peer sessions copy confirmed raw rows into a 4,096-row trace queue. Diagnostic lag drops counted copies;
it does not retain authoritative history or end play. Confirmed peer hashes still govern history retirement.

## Peer event log

The peer event log uses a preallocated byte queue and its own disposable writer. Neither selecting local input nor
publishing health or terminal status drains this diagnostic log. Queue overflow withholds later log rows and marks
the retained log incomplete; disk failure stops the log without stopping the game. After the producer stops,
shutdown drains final rows or cancels a stalled write; the worker joins before its file and queue are destroyed.

## Timing and resource samples

`pbd_stall_metrics.h` supplies a 65,536-record nonblocking queue for full-session timing and process resource samples.
The record writer drains up to 8,192 rows per pass when session ordinary measurement is enabled, and drains the
remaining capacity on shutdown.

Per-page copy and fault/range-lock observations retain spans of at least one millisecond and measure elapsed time
without querying thread CPU counters. Whole checkpoint, update and presentation scopes retain CPU attribution;
per-boundary ticks and accepted-image points remain complete. This avoids emitting millions of uncontended page
operations while preserving every long operation at these sites. A per-page row has `cpu_valid=0`.

## Session health

Session health reports an advancing battle as running even when sampled inside a callback. Recovery reports waiting
immediately; otherwise one second without a newly published boundary reports waiting until progress resumes.

## Presentation timing points

Vulkan `image_present_accepted` timing points carry the proved live command boundary, session epoch (`bytes`) and
segment (`detail`). Joining that boundary to scene8/12 in the tick trace locates a VS image handed to presentation,
independently of the PPU's current boundary. This records host presentation acceptance, not monitor scan-out.
`image_present_unavailable` is diagnostic only: `bytes` names the caller thread and `detail` carries shown/live/proved
bits 0..2, command state at bits 8..15, accepted at bit16 and flip-head verification at bit17. It never counts as
a visible frame. Callback `tick_dispatch` spans carry hook PC/stage in `bytes`/`detail`; `peer_input_wait`,
`confirmed_publication` and `set_command_read` separate peer backpressure from owner-side publication delays.

## Clock anchors

Five-second `clock_anchor` rows bracket a system-clock epoch-nanosecond sample (`bytes`) with steady
`begin_ns`/`end_ns`; readers use the midpoint and retain the bracket uncertainty when joining launcher UTC.
