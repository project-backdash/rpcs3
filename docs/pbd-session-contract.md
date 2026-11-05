# Direct session execution and status

`PBD_SESSION_POLICY=direct-v1` selects the native direct-peer execution policy. Its parsed description is immutable for
the session. Omit it for the explicit diagnostic route; an unknown value refuses setup. Run paths, peer identity,
prediction/input-delay settings, match seed and checkpoint provenance remain launch inputs.

The native policy owns the 616 restore pages, the original direct-session recipe's 32 exclusions, undo-denied and I/O
admission spans, write-fault capture, nine-entry ring, hold index 100, rolling start 110, clock version 2, round lifecycle,
replay vblank pacing, exact draw withholding, replay draw skips, reseed/stage probes and lobby loading-gate probes/lag 8.
It reuses the qualified memory data without changing checkpoints, recipe bytes or the compatibility key. Replay capture
retains its two extra exclusions. Raw memory overrides conflict with this named policy and are refused.
In a re-executed frame the shadow (`0x158d6c`), stage model layer (`0x260d44`, only where the `stage_camera` gate allows
it), player model (`0x2649a8`, only where the `player_draw` gate allows it) and skinning batch (`0x268778`) draws return
at entry. The stage layer runs when camera mode is 2 or unreadable, preserving cache writes the next live frame may
read before drawing. It also runs on Ominous Mist (stage 34) to retain the layer's fog updates, or when the stage is
unreadable. Cold starts, held games and hidden preparations emit the same skips, so they find the same
compiled objects and both PCs run the same code.
Diagnostic ring-size, gate-lag and hold-time overrides are ignored in direct mode; input/prediction delay remains
a separate launch choice. Other numeric session fields consume the whole unsigned value: decimal digits, with
`0x` hex also accepted for seeds/counters and hexadecimal address ranges. Signs, whitespace, overflow and trailing
text are refused. Replay targets require exactly their documented colon-separated decimal fields.

Stage synchronization defaults on (`PBD_SESSION_STAGE_SYNC=0` disables it). A set channel, match standby channel, or
`PBD_PREPARE_FOR=lobby` enables the loading gate (`PBD_SESSION_EXIT_GATE=0` disables it). Native-owned probes follow
those choices; the launcher supplies no guest addresses. Extra diagnostic hooks and other draw-function skip sets
belong to the diagnostic route.

Beside `update-inputs.jsonl`, native publishes these complete files through a temporary file and rename:

- `session-ready.json`: configuration, actual start-state checks and worker startup succeeded.
- `session-failed.json`: setup failed; its `reasonCode` is a stable native refusal code.
- `session-ended.json`: the owner/writer drained and the session ended; `reasonCode` is the final native end code.

Each has schema `pbd-session-status-1`, `state` (`ready`, `failed`, `ended`), `nativeGeneration`, `reasonCode` (null for
ready) and `checkpointUsable`. The latter is true after actual start-state validation, false when those checks fail,
and null when no checkpoint assessment was made. A timeout or unrelated configuration error alone does not mark a
checkpoint corrupt. Startup failure takes precedence over a ready file; an ended file records termination. These
files are additive host status, separate from checkpoint and replay encodings.
The writer publishes `ended` after required final trace writes; a short write changes its code to `output_failed`.
Optional peer-input and health diagnostics can fail independently, without changing the session's result.

Lobby `PBD-MENU-1` snapshots carry `command=<positive command serial>` and kind `post_set`, `choice_rejected`, or
`set_ended`. Result/rejection snapshots are visible with `close=0`; end-set snapshots are hidden. Native acknowledges
only a matching serial/kind at a newer generation. Title and subtitle are presentation text. The one exception is an
`end_set` read while the game still waits at the session start for the other PC: nothing is drawn yet, so native
acknowledges it and ends as `lobby_set_ended` without waiting for its snapshot.

Ranked sets add three display-only inputs. The set command `rank_stakes` names what a set win and a set loss give each
player, `{"type":"rank_stakes","left":{"win","winRank","loss","lossRank"},"right":{...}}`, with the events `promote`,
`demote`, `place` or `none` and game rank indices 0-34; each replaces the last for the set its envelope names, and
`next_set` drops it. `next_set` may name each player's rank for the new set as `left.rank` and `right.rank` (0-36, null
or absent keeps the rank shown). The `PBD-MENU-1` key `rank=<up|down|attained>|<rank index>|<rating>|<change>` shows the
settled rank change on Set complete; `PBD_RANK_BADGES` names the folder of `rank_NN.png` badges the panel draws with it.
The launcher sets it to the absolute `<profile>/rank-badges` on every lobby launch; without it the panel names the rank
without a badge and the log says so once. A badge missing from the folder is looked for again, at most every 10 s,
when a later panel or a set's `vs_cards` arrives.

`PBD_RANK_SCREENS=1` turns on the game's own Ranked screens (stake banner and life-bar marks, PROMOTED TO at the knockout
that ends a set); without it no hook site is emitted and nothing is written, so preparation and play must agree on it.
A game's values are planned where its `continue_game` or `next_set` command is applied, before its `game_preparing` is
written, and kept until the next game's command. Its `game_preparing` and `game_started` both record them as
`start.rankScreens` `{"stakeLeft":0..2,"stakeRight":0..2,"promoteLeft":-1..34,"promoteRight":-1..34}` (only when one is
set, so the two starts stay equal apart from `nativeRound`), and the replay's game start as `capsule.rankScreens` with the
same four integer keys. A watcher's `begin_game` control carries them back as a root `rankScreens`, and a replay file as
`games[0].capsule.rankScreens`. The two PCs receive `rank_stakes` from their own launchers at different times, so each
announces its stakes with every scene barrier (`rank_stakes` and `prev_rank_stakes` in the version-12 input message); a
command plans from the stakes both PCs announced at the barrier before it, and from none while they differ, so both games and
all four records agree. A set's first game, and a game whose stakes reached one PC only after that barrier, has none. The
PLAYER DATA numbers under the life bars (a fighter's `record` in the manifest, `rank_stakes` or `next_set`, or an in-game
fighter's `records` table, plus the set's games) are announced the same way (`records` and `prev_records`, the last two
words): each scene barrier outside the stretch from the end of the VS screen to the end of the fight writes the numbers both
PCs announced, and zeros while they differ, so a record that reached only one PC shows on neither. The test switches `PBD_TEST_RANK_STAKE=left:<0..4>,right:<0..4>` and
`PBD_TEST_RANK_PROMOTE=left:<0..36>,right:<0..36>` force values into every VS game of offline play (`PBD_OFFLINE_PLAY=1`
without `PBD_SESSION=1`) and log the game's words at both hook points. A set, a watcher and the replay viewer ignore them,
and a replay refuses to start while either is set.

`rpcs3 --pbd-features` prints, without booting, `{"schema":"pbd-features-1","features":[...]}`. `rank-stakes-1` names
everything above: `rank_stakes`, the `next_set` ranks, `rank=`, `PBD_RANK_BADGES` and the `rankScreens` records. An older
emulator refuses each of these, so the launcher sends them only to an executable that contains the text `rank-stakes-1`.

`ranked-notices-1` names the Ranked and Quick Match notices' inputs, all display only. `PBD-MENU-1` adds
`placing=<before>|<after>|<total>` (Set complete's placement count; `post_set` with `you=`; total 1-10, at most one set
between the two), `rival=<text>` (the other player's status; needs `you=` except on `challenger`), `link=<bars 0-4>|<words>`
(`post_set` only), `countdown_row=<row>` (needs `timer=`), and on `challenger` only `card=<caption>|<name>|<plate>` (a plate
is `r<0-34>`, `p<done>/<total>` or `-`), `ping=<bars 1-4>|<ms>` and `pill=placement`; `rank=` adds the kind `same`, and
`chosen=` may leave the other rows enabled. `PBD-SEARCH-1` adds `placing=<done>|<total>` to a running Ranked search.
`vs_cards` adds `mode` (`ranked` or `quick`) and each side's `character` (upper-case printable ASCII, 1-24 bytes), and each
side of `rank_stakes` may add `placement` `{"done","total"}` (total 1-10), which is not part of the stakes word the two PCs
compare; an invalid optional field reads as absent and is logged once. `PBD_NOTICE_ART` names the absolute
`<profile>/notice-art` folder of pictures the launcher makes from the player's own game files; it is read only when its
`version.txt` names this recipe, and a picture missing when the game starts is looked for again like a badge. An older
emulator refuses the new menu and search keys, so the launcher sends every addition only to an executable that contains
the text `ranked-notices-1`.

`character-ranks-1` adds each manifest and `next_set` side's optional `ranks` table:
`[{"rows":[12,13],"rank":19}]`. Up to 64 groups cover rows 0-84 without duplicates; ranks are 0-36.
A table takes precedence over the scalar rank, resolves the actual selected row at shared scene boundaries,
and shows Beginner for a character absent from the table. All costume rows of one character share its entry.
`next_set` replaces each supplied table, including an empty table; without a table the scalar behavior is unchanged.
Malformed tables resolve to empty tables, so a previous character's plate cannot leak into an unranked pick.

A manifest with `rankProfiles:true` publishes `start.rankProfiles` with absolute `left` and `right` sides,
each either `{"ranks":[...]}` or `{"rank":19}`. The service freezes and compares that display metadata
with the start, and a capable watcher's `begin_game` carries it verbatim. Selection streams resolve their
actual rows from this frozen table. Capsule watchers and saved replays use the ranks recorded with their players.
The launcher sends the manifest additions, next-set tables and watcher fields only when the executable
advertises `character-ranks-1`; an older launcher supplies none and retains its scalar behavior.
These fields never create stakes: Quick Match and lobby plates remain display-only.

`opponent-results-1` adds `PBD-MENU-1`'s `opponent=<plate>|<MMR or ->` for Set complete. It needs `post_set`, a score and
`you=`. A rank plate `r0`-`r34` requires MMR 0-99999; a placement plate `p<done>/<total>` requires `-` (total 1-10,
done at most total). The opposite side shows this settled standing at the result's large size instead of its small HUD
plate. The launcher omits the key unless the executable contains `opponent-results-1`.

`rpcs3 --pbd-host-info` prints, without booting, `{"schema":"pbd-host-info-1","host":"windows"|"wine"|"other","seen":...,
"wine","build","system","release","probe":{"thread_times_us","cycle_time","protect_us","write_fault_us","clock_ns"}}`: the
host and what one GetThreadTimes call, one VirtualProtect call, one write fault on a protected private page and one clock read
cost there. The launcher asks only under Wine, and only an executable that contains the text `--pbd-host-info`. The emulator
runs under Wine when ntdll exports `wine_get_version` (or, with that export hidden, when `HKCU\Software\Wine` and
`system32\winecfg.exe` both exist); `PBD_HOST_FORCE=wine` makes a test run on Windows act the same, and a player's session never
inherits it. Under Wine only, timed scopes and points are counted without reading a clock (`stall-metrics.csv` keeps its
header, clock anchors and trace cost rows), the five-second process snapshot is skipped, RPCS3.log gets one `Host: Wine` line at
boot, and `tick-correction.jsonl` gets a `host_timing` row every 600 live ticks and at the session's end: the stretch's
`live_ticks`, `wall_ms`, `live_ticks_per_s`, `live_tick_ms_p50` and `live_tick_ms_p95` (0.5 ms bins), and the growth of
`replayed_ticks`, `corrections`, `hold_ms`, `faults`, `fault_handler_ns`, `arm_calls`, `arm_ns`, `ring_ns`, `scopes`,
`cpu_scopes` and `points`, with `wine`, `system`, `release` and `seen` repeated in every row.

`tick-correction.jsonl` has its own writer thread: the game thread formats each row into a reused buffer, copies it into
a 1 MiB queue and never waits for the disk. A row that does not fit is dropped whole and counted, so the file holds
complete rows only. It ends with a `correction_journal_end` row: `rows_written`, `bytes_written`, `rows_dropped`,
`bytes_dropped`, the drops per kind as `dropped_predicted_update`, `dropped_corrected_update`, `dropped_correction`,
`dropped_correction_end`, `dropped_fault`, `dropped_host_timing` and `dropped_undo_policy` (flat keys, so no row type
name appears quoted in this row), `rows_unwritten` (queued rows lost to a failed write), `queue_capacity_bytes`,
`queue_high_water_bytes`, `owner_enqueue_max_ns`, `worker_write_max_ns`, `worker_writes_over_ms` (writes above 1, 5,
16.7, 50 and 100 ms) and `write_failed`. Rows count lines: a correction's `correction_clock` and `correction_end` rows
are two. The session's end waits up to 100 ms for the summary before it reports the end. A row that arrives after a
summary (only Wine's closing `host_timing` row can, when the end came between two game callbacks) is followed by a new
one; the parked game thread waits up to 100 ms for it, and a host that ends the process inside that wait leaves that row
last. When teardown has to cancel a write stalled in the OS, the file can end mid-row and without a summary; readers
treat a last line with no newline as cut off.

Hook event capture defaults off; `PBD_FN_MAX_EVENTS` explicitly bounds capture. `PBD_PEER_TRACE_INPUTS=1` enables the
bounded input trace queue, drained by the existing writer to `peer-inputs.jsonl` (recv/fill/assign rows and a final
dropped-record count). Sync checks and live counters remain active without these diagnostic traces.
Freeze reports use the last synchronized boundary-owner and renderer snapshots plus atomic liveness facts. They
do not suspend a guest thread, read live PPU histories/registers or infer a cause from a missing boundary.

Guest fault injection is compiled only with CMake `-DPBD_DIAGNOSTIC_BUILD=ON` or MSBuild `/p:PbdDiagnosticBuild=true`.
Release builds omit the voice delay/rendezvous/drop-stop, flip-reset delay, anchor paste, forced camera/skip failures,
early flush, stage steering and synthetic pad branches. Product tape input and replay controls remain available.
