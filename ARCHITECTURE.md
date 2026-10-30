# Architecture

This fork adds PROJECT BACKDASH's game-specific execution and online play to RPCS3. The launcher and services live
in the product repository. They supply mode, settings, peer identity and local control requests; this process owns
the game's execution and rollback. [SOURCE.md](SOURCE.md) is the generated inventory against the pinned upstream
commit, including the remaining changes inside upstream files.

## Execution and state

`rpcs3/Emu/Cell/PPUThread.cpp` calls `pbd::rollback_session::observe()` at checked instruction sites in the game's
main update loop. The session acts on the game's main PPU thread. The observed sites mark stages of a tick, rather
than four separate game frames. `pbd_rollback_session.cpp` is the entry; the boundary steps are declared in
`pbd_rollback_session_boundary.h` and implemented in the adjacent `pbd_rollback_session_*.cpp` files.

`pbd_rollback_session_storage.h` composes the histories and mode state. The storage headers separate peer,
rollback, lobby, Practice and replay fields, but boundary operations still share that aggregate. The receive thread
queues peer datagrams under synchronization; the main PPU thread consumes them and performs guest-memory work.
The record writer receives a read-only view of published rows, retirement/rewind handshakes, terminal facts and
its output files. It has no access to mutable guest, peer transport or replay/Practice controls. Freeze diagnostics
copy the last boundary-owner publication under a nonblocking snapshot lock and use the renderer's synchronized
snapshot; they never dump live PPU registers or history from the writer thread.
Continuous peer sessions copy confirmed raw rows into a 4,096-row trace queue. Diagnostic lag drops counted copies;
it does not retain authoritative history or end play. Confirmed peer hashes still govern history retirement.
The peer event log uses a preallocated byte queue and its own disposable writer. Neither selecting local input nor
publishing health or terminal status drains this diagnostic log. Queue overflow withholds later log rows and marks
the retained log incomplete; disk failure stops the log without stopping the game. After the producer stops,
shutdown drains final rows or cancels a stalled write; the worker joins before its file and queue are destroyed.
`pbd_stall_metrics.h` supplies a 65,536-record nonblocking queue for full-session timing and process resource samples.
The record writer drains up to 8,192 rows per pass when session ordinary measurement is enabled, and drains the
remaining capacity on shutdown.
Session health reports an advancing battle as running even when sampled inside a callback. Recovery reports waiting
immediately; otherwise one second without a newly published boundary reports waiting until progress resumes.
Per-page copy and fault/range-lock observations retain spans of at least one millisecond and measure elapsed time
without querying thread CPU counters. Whole checkpoint, update and presentation scopes retain CPU attribution;
per-boundary ticks and accepted-image points remain complete. This avoids emitting millions of uncontended page
operations while preserving every long operation at these sites. A per-page row has `cpu_valid=0`.
Vulkan `image_present_accepted` timing points carry the proved live command boundary, session epoch (`bytes`) and
segment (`detail`). Joining that boundary to scene8/12 in the tick trace locates a VS image handed to presentation,
independently of the PPU's current boundary. This records host presentation acceptance, not monitor scan-out.
`image_present_unavailable` is diagnostic only: `bytes` names the caller thread and `detail` carries shown/live/proved
bits 0..2, command state at bits 8..15, accepted at bit16 and flip-head verification at bit17. It never counts as
a visible frame. Callback `tick_dispatch` spans carry hook PC/stage in `bytes`/`detail`; `peer_input_wait`,
`confirmed_publication` and `set_command_read` separate peer backpressure from owner-side publication delays.
Five-second `clock_anchor` rows bracket a system-clock epoch-nanosecond sample (`bytes`) with steady
`begin_ns`/`end_ns`; readers use the midpoint and retain the bracket uncertainty when joining launcher UTC.
Presentation, controller polling and window requests have their own host threads and must not mutate simulation
state without the owning boundary operation.
Controller polling performs SDL enumeration/open/close, configuration reads and settings/display publication outside
the guest pad mutex. Only port inspection and applying completed button/stick reports hold that mutex.

At a frame boundary the session applies delayed local input and the best available remote input, captures guest
state and records the resulting frame facts. A late input that differs from the prediction causes restoration and
re-execution. The checkpoint policy covers guest memory and CPU/SPU state; restore order and excluded writes are
part of correctness. `rpcs3/Emu/PBD/pbd_undo_restore.h` owns retained preimage selection. Checkpoint helpers and
`pbd_rollback_session_capture.cpp` connect it to emulator state. These operations are not ordinary host savestates.

Each checkpoint memory writer owns its exclusion mask, address ranges and ordered write plan. Static restores,
undo restores and readback diagnostics use that same plan; there are no process-global pointers to another session's
policy. `pbd_checkpoint_write_plan.h` performs the actual byte selection through an explicit memory-write operation.
The hook dispatcher keeps its order while voice leases and random-history handlers own separate state.

Battle clock 2 also supplies the GPU duration consumed by the game's load-monitor sampler at `0x259dfc`. Its
asynchronous GCM timer otherwise makes the second monitor bank prune different effect objects on each PC; those
callbacks consume the same stock RNG as combat. GPU work within one logical frame has zero elapsed time, matching
the CPU timebase policy. LLVM reloads only this probe's r3 result, and only objects containing it receive a new hook
cache tag. The RNG routines, peer messages and hash checks retain their ordinary paths.

## Modules to start reading

Unless qualified, session implementation files below are under `rpcs3/Emu/Cell/`; domain helpers are under
`rpcs3/Emu/PBD/`.

| Concern | Entry and supporting files |
|---|---|
| Frame execution and correction | `pbd_rollback_session_run.cpp`, `_tick.cpp`, `_correction.cpp`, `_capture.cpp` |
| Hook dispatch and guest functions | `pbd_fn_hook.cpp`, `pbd_function_hook_*`; `pbd_t5dr_memory.h` names shared guest facts |
| Peer traffic | `pbd_rollback_session_peer.cpp`; `pbd_peer_message.h`, `pbd_peer_auth.h`, `pbd_peer_auth_state.h` |
| Set commands and game transitions | `pbd_set_channel.h`, `pbd_lobby_set_driver.h`, `pbd_direct_match.cpp`, `pbd_boot_request.cpp` |
| Ranked stakes and the game's rank screens | `pbd_rank_stakes.h` (rules, guest write plans), `pbd_rank_screens.cpp` (hooks, off by default) |
| Practice | `pbd_practice_native.cpp`, `pbd_rollback_session_practice*.cpp`, `pbd_practice_*` domain modules |
| Per-game recording and playback | `pbd_replay_*`, `pbd_live_replay.cpp`, `pbd_match_recording.h`, replay boundary operations |
| Spectating | `pbd_spectator_local_channel.h`, `pbd_spectator_channel_{model,controls,input,publication}.h`, `pbd_spectator_*`, `pbd_corner_chat.h` |
| Input | `rpcs3/Input/pbd_pad_*`, `pbd_controller_ports.*`, `rpcs3/Emu/Cell/Modules/pbd_pad_*` |
| Display and window | `rpcs3/Emu/RSX/Overlays/pbd/`, `rpcs3/rpcs3qt/pbd_game_host*`, `pbd_game_window*` |
| Frame presentation | `rpcs3/Emu/RSX/pbd_frame_tags.h`, `pbd_replay_pacing.h`, `Utils/pbd_render_scale.h` |

Upstream CPU, memory, RSX and system-call changes preserve the state and scheduling the session needs. They are
listed by actual added/removed lines in SOURCE.md. Generated font atlases and the pinned HLE slot table are data;
their generators and compatibility requirements belong beside those tables.

## Peer contract

`pbd_peer_message.h` defines inner datagram versions, offsets and byte order. Inputs include recent local input
history, confirmation and frame hashes, timing echoes, segment state and exit coordination. Initial option and
setup digests must agree before play; an unreadable setup is refused.

Authenticated sessions wrap those bytes using `pbd_peer_auth.h`. The authentication covers sequence and payload,
uses different directional keys, and checks a receive sequence window before accepting a datagram. The launcher
provides per-set key material through a private pipe. `pbd_peer_auth_state.h` binds receive state to the installed
key epoch, so packets from an old set and reflected/reused sequences are refused. Endpoint learning follows accepted
session traffic; a syntactically valid unkeyed packet cannot replace an authenticated peer's endpoint.
Local terminal sealing retains authenticated retransmission while the other peer finishes its final barrier.
Only the applied service-agreed `show_post_set` command retires that epoch; a continued set installs a fresh key.
Recovery arms when the peer's first valid input under the current key arrives. Before that the other game may still
be loading, so boot, key staging and this side's own sends count as no fault, and a wait keeps its own budget (180 s
at the session start). Once armed, 500 ms without valid current-key input requests a replacement path, and so does
500 ms in which the peer's inputs echo none of this side's sends under the key (one direction never worked). Repeated
sends do not renew the protected 90-second wait, which runs from the first contact at the earliest; a new context must
arm again. The status's `peerHeard` says whether that first input has arrived; until it has, `lastValidReceiveAgoMs`
is the time waited for the other game, not the age of a received input. `sent` and `sentAgoMs` say that this game is
sending under the key and since when: a route that carries nothing either way shows only as both games sending unheard,
which the launcher, knowing both games are there, repairs with the same stage and commit.

Both peers compare confirmed-frame hashes. A mismatch ends the session. A missing hash remains unchecked; it does
not count as a matching frame. Unit codec tests establish byte-level behavior, while an online run checks that both
complete emulators compute the same facts.

## Launcher and file contracts

- `PBD_*` settings select the mode, paths and execution options. Parsing is spread across mode/configuration helpers;
  the source does not yet implement one typed session file for every launch route.
- The direct peer route selects the native-owned `direct-v1` policy and snapshots its typed description at admission.
  Standby binding consumes the validated peer fields directly. Native readiness/end files contain stable reason codes
  and an explicit checkpoint assessment; overlay acknowledgement uses command serial and panel kind. See
  [pbd-session-contract.md](docs/pbd-session-contract.md) for the launcher contract and diagnostic build option.
- The set channel exchanges bounded JSON commands and events. Set/game identity and command serials protect against
  stale requests. The overlay publishes snapshots for display and routes accepted input back to the simulation owner.
- The native writer publishes `session-health.jsonl` once per second and the same health row in the preallocated
  4096-byte `session-health.snapshot`. The latter is overwritten in place even if append fails. Its ASCII header is
  `PBD-HEALTH-1\n`, eight lowercase hex payload-length digits plus newline, then eight lowercase hex FNV-1a32
  checksum digits plus newline; the JSON payload follows at byte31 and unused bytes are spaces. Readers require the
  exact extent, valid checksum/schema and a fresh increasing sequence. A missing, torn or stale snapshot proves no
  liveness; the actual owner-idle and paused/waiting checks and stopped-writer deadline still apply.
  Matched health may also carry `progress: {gameNumber, roundWins: {left, right}}` at the top level. It comes only
  from confirmed, verified unfinished rounds, and is absent before round start, after a decided game or after a
  lifecycle refusal. It shares the network identity lock: changing the session, set, peer or key clears the score,
  and a score change advances the health sequence. Quick Match and Ranked use the same observation.
- A `pbd-replay-v2` file describes one game's initializer, confirmed inputs, sync facts and build identity. Native
  readers validate the initializer before playback. The viewer implements pause, stepping, speed and Take Control.
  A round request remains pending until its rebuilt starting state passes the recorded sync check, including when
  its continuation takes a side. A mismatch refuses the request before any controlled frame executes.
  The product launcher assembles and stores these per-game files; older input-script headers are separate contracts.
- A spectator receives confirmed inputs through local files, then executes its own copy of the game. See
  [spectator-wire.md](docs/design/spectator-wire.md).
- The game window and control pipes connect this process to its launcher host. See
  [game-hosting.md](docs/design/game-hosting.md).

## Checks

Resolution scaling maps presentation dimensions to the game's native coordinate space. Precise ZPASS pixel counts
are normalized by the squared render ratio, which is lossy for integer samples (and cannot correct differences in
GPU coverage). This arithmetic does not establish cross-resolution determinism. The optional boolean ZPASS path
preserves zero/nonzero semantics without count normalization. Render-size unit tests check coordinate mapping only;
only mixed-resolution online runs with the existing per-frame checks can establish synchronization for that route.
No mixed-resolution match result is recorded by this source snapshot.

`rpcs3/tests/pbd` contains GoogleTest tests linked into `rpcs3_test`; `pbd_tests.props` and `pbd_tests.cmake` list them.
`rpcs3_test --gtest_filter=Pbd*` runs the PBD groups. They cover production codecs, rules, input transformations,
parsers, retained history and state transitions without booting a game.

The movement suites invoke production generators, cursors and flow plans; the former duplicate dummy scheduler is
removed. Checkpoint tests drive the production byte-selection/write operation. The pinned HLE table's deterministic
converter and identified compatible input are documented in [pbd-hle-order.md](docs/pbd-hle-order.md).

`.github/workflows/pbd-bar.yml` runs C++ formatting. The inherited `rpcs3.yml` also builds and runs unit tests in its
eligible pull-request jobs; its repository/event conditions do not make every fork push a native test run.
[CODE-BAR.md](CODE-BAR.md) lists the actual local checks. Native behavior changes also need online match validation;
formatting and portable unit tests alone cannot establish synchronization of a running game.
