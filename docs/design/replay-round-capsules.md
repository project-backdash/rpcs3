# Round-start capsules

A round jump must call DR's own round initializer with the state that initializer originally read. It must reproduce the round from a recorded boundary, without loading a savestate or executing earlier rounds. Exact independent-game playback comes first; recording a round capsule alone does not make a round selectable.

## Current recording

The live recorder observes initializer arguments 4 (first round), 6 (later-round cleanup) and 5 (later-round ready). `pbd_live_replay::capture_round` records the native ordinal, game frame counter, input offset, source segment, spawn selector, score/result pairs, generator context and the typed actor carry available immediately before each initializer. A later-round entry pairs its cleanup and ready observations in that order.

When ready carry is available, and cleanup carry is available for a later round, the recorder emits `pbd-round-start-capsule-draft-2`. Otherwise it retains coordinate-only `pbd-round-start-capsule-draft-1`. Both schemas remain `qualified: false`. Unreadable coordinates omit the entry while ordinary game recording continues; missing carry never becomes an invented complete point.

## Draft-2 recording

The enclosing per-game file and its game-start capsule retain their formats. Draft-2 uses the same `round`, `anchor`, `cleanup`, `ready` and `qualified: false` members as draft-1. Each point adds `carryOver` through the production game-carry codec, with the same named fields, widths, ranges and typed reference representations as game-start carry. Guest pointers, native resource objects and savestates are not persisted.

Carry and generators are captured on the same owner thread before each native initializer. First-round kind 4 has no cleanup point. Later rounds require kind 6 followed by kind 5 at distinct, ordered input offsets. Both points use the same native ordinal and source game. The next game's journal cannot inherit an unfinished point.

Selection, game rules, session seed and build identities remain in the parent game-start capsule. A round entry belongs to that file and game. Per-frame sync facts remain the authority for the reconstructed result. The initializer's saved frame counter is distinct from the first resumed input's recorded key-read counter: after the stock initializer returns, reconstruction restores the latter from the sync fact at `inputOffset`, rather than substituting the pre-initializer value.

A complete round entry is bounded to 64 KiB of UTF-8 JSON, including both points. The file admits at most 32 rounds and 100,000 inputs within 16 MiB. The replay parser has a separate bounded node allowance for one input segment per frame plus the game and round capsules; it does not inherit the smaller set-channel allowance.

## Reading older files

The readers accept draft-1 coordinates and require complete typed carry in draft-2 points. A coordinate-only point cannot independently reconstruct a round: a request without its retained values returns `native_round_retained_values_not_recorded`. Unknown round schemas and malformed draft-2 entries refuse file admission. A valid game with absent round entries can still play from its game start; playback can capture missing complete points in memory, without replacing an original point.

The recorded emulator hash is provenance rather than an equality gate for native game-start or round-request admission. Another emulator build may attempt exact playback only with complete per-frame sync facts and, for game 2+, the required carried start. Any sync or terminal-score difference refuses playback. The decrypted **game executable** identity is a separate binding and must match the supported game program.

A round request adds requirements beyond game-start admission: the active runtime generation and game, an available captured slot, complete ready/cleanup carry and generators, and a recorded entry counter at the saved input offset. Stale, repeated or competing requests are rejected. Scheduler/owner checks, resource and spawn-selector availability, generator style readback and Mokujin rebinding can leave a round unavailable even when its game plays normally. A reconstruction failure after guest mutation refuses playback instead of continuing from a partial restore.

## Reconstruction order

1. Bind the request to the current runtime generation, file identity, game and captured round. Refuse missing points before mutating the game.
2. At the existing owner boundary, verify stock initializer bodies, scheduler state and resource ownership. Rebind the recorded round's styles, including Mokujin, through the stock resource path.
3. Resolve every typed carry reference against those owned resources. Validate the complete ready point and, for later rounds, the complete cleanup point before applying either.
4. For a later round, restore cleanup carry, scores/results, spawn selector and generators, then call kind 6. Restore ready carry and the remaining ready values, then call kind 5. For round 1, restore its ready point and call kind 4.
5. Read back the native score and re-anchor the source input offset after the stock initializer returns. Physical host boundaries stay monotone. Validate the first resumed boundary against its recorded sync fact before executing recorded or controlled input.
6. Publish `reconstructed` only after that resumed sync check succeeds. A missing or different sync fact refuses playback before Take Control executes a frame. This native result alone does not qualify picture, sound or round-jump latency.

Recording supplies owner observations; the versioned readers reuse the carry codec, and direct playback performs resource validation and stock calls. Formats and round availability govern reconstruction.

## Validation and remaining qualification

Codec/parser tests cover first-round and cleanup/ready pairs, retained fields, bounds, older coordinate-only entries and strict admission. Runtime qualification must also exercise wrong game/generation, unavailable resources, captures of missing rounds and exact continuation after reconstruction. Draft-2 capture and parser acceptance alone do not establish these runtime results.

Captured-round runtime checks currently reproduce a first-boundary sync-hash difference and refuse before acknowledging reconstruction or executing controlled input. Exact round reconstruction remains unfinished. The remaining qualification goals are recorded-set jumps 1 → 3 → 1 with every resumed sync field compared, including inactive-region hashes and gate holds, and a Mokujin round. Sound, score and native game end need separate checks. A usable warm-jump picture within 100 ms remains a target, not a guarantee from the native reconstruction acknowledgement.
