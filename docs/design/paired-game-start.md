# Paired starts for continued-game watching

`pbd-paired-game-start-1` keeps both complete original canonical envelopes, named `left` and `right`, with each PC's native generation and original envelope identity. The source is always `left`, the existing canonical input publisher. Neither envelope is edited to agree with the other. The new publication path also preserves the player display records that the older publication path omits.

Agreement compares all fields, array elements and actor bytes except this fixed list:

| Local value | Evidence and playback policy |
| --- | --- |
| `capsule.generatorContext.mt.index` and `.words`, and the same fields beneath `carryOver.styleProducer.generatorContext` | `pbd_function_hook_state.h` documents the ambient-sound manager's host-derived MT seed. Both retained recordings have 623 different words per inventory, before the match initializer, unchanged between games. The index remains bounded to 0–624 and the inventory to 624 u32 words. Stability/coherence claims must still agree. Paired playback leaves the watcher's own MT inventory in place at both generator restores. Every simulation generator is restored as before. |
| `capsule.carryOver.nativeInputContext.counter` | `pbd_tick_input.cpp` documents the guest increment at `0x120740` and asynchronous reset at `0x120764`. Both originals already differ in game 1. Playback retains the local clock, as the replay initializer already does. |
| Actor-record bytes `0x8c0..0x8c3` and `0x1190..0x1193` | Each 0x8d0-byte actor's +0x8c0 bone-root pointer belongs to that boot. `pbd_replay_actor_records.h` validates its local back-link and retains that local object during restoration. Both originals have precisely these two differing words. All other 4,504 actor bytes must agree. |

Unknown differences refuse. Missing carried fields refuse. The list is code, never supplied by a capture or derived from observed differences. Neither player capture nor player execution changes.

The service builds a paired start only from both authenticated fighter publications, the agreed native round start, and the actual paired confirmed successor input. Its `proof` contains the original 48 bytes of that first frame's sync facts, its digital frame ordinal and the existing sync encoding/definition. A full sync mask of 7 is required for the first frame. The capsule's original initializer segment and the successor's input segment remain distinct. The launcher checks the proof against the first retained input; the native channel checks the same bytes before receipt. Existing native coordinate and combat-hash checks gate every executed frame. A mismatch stops watching and the continued-set recovery waits for the next game.

Negotiation requires the existing capsule switch plus `PBD_SPECTATOR_PAIRED_STARTS_ENABLED=1`, the publisher/consumer capability `spectate-capsule-2`, and watch capability `spectator-native-3`. Older services, publishers and watchers keep the exact-agreement contract. A native paired session uses `pbd-spectator-session-2` and the explicit `PBD_SPECTATOR_NATIVE_3=1` gate as well as the capsule initializer gate. Older native readers reject that session version. A session of version 1 cannot contain a paired start.

The native binding covers both complete envelopes, the first-frame proof and the selected capsule. It distinguishes the verified local `emulatorSha256` from `capsuleStart.recordingEmulatorSha256`, the immutable source build identity. This lets the retained original recordings exercise a newer consumer without replacing their recorded build hashes. The game executable identity and complete capsule schema remain exact, and execution must reproduce the original frame facts before a live picture is accepted.

The two `rpcs3/tests/pbd/fixtures/paired-start-*.json` files contain deterministic synthetic protocol values. Their actor bytes, RNG inventories, appearance records, player identities and first-frame facts are constructed for the regression cases. Native regressions refuse version downgrade, missing capability and unlisted RNG differences, bind the first frame, and stop on a changed input proof or executed combat hash.
