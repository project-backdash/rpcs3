# Game-start capsule

A replay file and a late watcher use the same capsule for the current game. The recorder captures it on the owning PPU thread immediately before the native match initializer, before that initializer overwrites retained actor values. Capsule playback starts a fresh game and applies named values through the game's initializer. That path requires neither earlier games nor savestates.

## Version and admission

The existing recorder emits `pbd-game-start-capsule-draft-2`, with anchor `before-native-match-initializer`. It is a draft format, marked `qualified: false`, and no capability is advertised on it until independent playback matches every recorded sync fact on both PCs. A production revision changes the schema string and advertises a separate spectator capability; it cannot silently reinterpret draft bytes.

The capsule is UTF-8 JSON, bounded to 65,536 bytes. Encoding uses the production canonical JSON serializer: object keys sorted, array order retained, compact numbers and strings. Integers carrying signed or float bits remain unsigned JSON integers; no conversion through a float value. Unknown fields, missing required fields, duplicate keys, invalid UTF-8, truncated JSON and out-of-range values refuse before any game starts. Every field in the tables is required except the carried fields: each retained actor field after `vsMatchHistory` (`pbd_replay_carry_codec.h` `carried_fields`, `replayCapsuleFields.ts` `CARRIED_FIELDS`). A recorder may predate any carried field, so a game start reads without one; a carried field that is present must be whole and valid. Playback never supplies a default for an absent field and leaves what a fresh boot holds. A recording's first game starts from that, so it needs none of them; a later game without every carried field plays approximately (Storage below).

## Capsule fields

| Field | Exact value or shape |
|---|---|
| `schema`, `anchor`, `qualified` | Constants above, `false` for this draft |
| `initializer` | Native kind `2` or `8`; current fresh-file engine supports `2` and refuses `8` |
| `sessionSeed` | Nonzero u32 |
| `players` | Left, right array of `{id, name, fighter, costume, row}`; id/name 1–128 UTF-8 bytes without controls; fighter/costume u16; row 0–84 |
| `selection` | `{leftRow, rightRow, allowJinpachi, stage, roundsToWin, roundTimeIndex}`; rows equal their player rows; boolean rule; stage 0–39; rounds 1–5; time index 0–4 |
| `hostContext` | `{matchReseedsBefore, mobBatchesBefore, sourceSegment, segmentCounter}`, each u32 |
| `generatorContext` | Generator inventory below |
| `carryOver` | `vsMatchHistory` and the rule fields below; each carried retained field below when its recorder wrote it |
| `nativeResourceState` | `null`; no raw address, executable, cache path or snapshot |

Stage values unsupported by the current fresh-file initializer refuse; the recorder's wider range is not permission to choose a different stage. Player labels are display data and never identify resource banks or influence the simulation.

`matchReseedsBefore` and `mobBatchesBefore` are the host's reseed counters at the anchor. Playback starts its match numbering from `matchReseedsBefore` before it loads the stage. It starts with no mob batches, because `mobBatchesBefore` already counts the mob batches of this game's own stage load, which playback runs again; at the match initializer playback refuses unless it has counted the same number.

## Generator inventory

| Field | Shape |
|---|---|
| `sharedLibc140Hex` | Exactly 280 lowercase hex characters (seed plus 136 bytes) |
| `flowLcg` | Two u32 values |
| `mt` | `{index, words, stableSample, globallyCoherent}`; index 0–624, 624 u32 words, `true`, `false` |
| `network` | `{matchSeedLcg, state, count}`, each u32 |
| `mokujin` | `{stylesHex, eligibilityHex}`, exactly 36 and 16 lowercase hex characters |
| `inventoryExhaustive` | `false` in this draft |

The MT table is sampled twice on the recorder's owner thread and the two samples must match; a match shows that the table did not change between them, not that every guest writer was idle. Generator state is applied at its documented native boundary; host reseed counts, input counters and game RNG values are separate quantities.

## Retained actor fields

Each entry has the exact schema and payload from `pbd_replay_carry_codec.h` and `replayCarry.ts`. `pair` means left then right; `matrix` means two terms per actor; `triple` means XYZ. Numeric widths are unsigned bit containers.

| Field | Schema | Payload |
|---|---|---|
| `vsMatchHistory` | `dr-vs-match-history-draft-1` | completedGames u32, wins u16 pair |
| `actorPlanarTerms` | `dr-actor-planar-terms-draft-1` | s32Bits u32 matrix |
| `descriptorDerivedFlag` | `dr-descriptor-derived-flag-draft-1` | value u8 |
| `actorTransformSum` | `dr-actor-transform-sum-draft-1` | f32Bits u32 pair |
| `actorDescriptorCode` | `dr-actor-descriptor-code-draft-1` | codes u16 pair |
| `actorDescriptorDeltas` | `dr-actor-descriptor-deltas-draft-1` | s16Bits u16 matrix |
| `actorReactionCursor` | `dr-actor-reaction-cursor-draft-1` | ports pair: `none`; records80/samples16 index, byteOffset, bankStyle; or bankOffset base, byteOffset, bankStyle |
| `actorMotionStep` | `dr-actor-motion-step-draft-1` | f32Bits u32 matrix |
| `actorLastReactionCode` | `pbd-actor-last-reaction-code-1` | codes u16 pair |
| `actorReactionConditions` | `pbd-actor-reaction-conditions-1` | command11e and command22 boolean-bit pairs (0 or 1) |
| `actorBoneSegments` | `pbd-actor-bone-segments-1` | ports pair of four startF32Bits/endF32Bits u32 triples |
| `actorReactionResult` | `pbd-actor-reaction-result-1` | ports pair: origin none, commandGroup/recordEntry index and bankStyle, dataPrefix byteOffset and bankStyle, or bankOffset base, byteOffset and bankStyle |
| `actorHitContacts` | `pbd-actor-hit-contacts-1` | ports pair of two contacts: endF32Bits/deltaF32Bits u32 triples, sphereIndex 0–13 |
| `actorSegmentDeltas` | `pbd-actor-segment-deltas-1` | ports pair: ownF32Bits/copiedF32Bits u32 triples |
| `actorPriorLife` | `pbd-actor-prior-life-1` | fixed16Bits u32 pair |
| `actorReactionScalar` | `pbd-actor-reaction-scalar-1` | f32Bits u32 pair |
| `actorPreviousBlendWeight` | `pbd-actor-previous-blend-weight-1` | f32Bits u32 pair |
| `previousGameFlow` | `pbd-previous-game-flow-1` | words: five u32 flow and side words the game before it left |
| `actorRecords` | `pbd-actor-records-1` | bytesHex: both fighters' records (`pbd_t5dr_memory.h` `actor_records_bytes`) |

Resource references resolve against the loaded, relocated COM bank of the recorded style, with member and alignment bounds checked. They never carry an address from the recording PC. Result-reference origins preserve their distinction, including the relocated payload prefix.

A retained word can name no member of the image loaded now: when a character changes between games, the game loads the new COM image at the same slot base and leaves the actor's cursor and result pointing into the old image. Such a word is carried as `bankOffset`: an even (cursor) or 4-byte aligned (result) byte offset from a base, with the style loaded at that base. The base is `opposing` (the opponent's slot, tried first), `own` (the actor's own slot) or `executable` (the fixed 32-byte table at 0x10001030 in the pinned executable, with bankStyle 0). A slot offset stays below 0x8a280 and, when the other slot lies above, below that slot's base. Like the payload prefix, the offset has no catalog meaning; it rebuilds the exact 32-bit word, which the per-tick sync hash covers. The recorder keeps the records80, samples16, commandGroup, recordEntry and dataPrefix forms whenever they name the word, and uses `bankOffset` only otherwise, so earlier files keep their bytes. Playback resolves the offset against the slot loaded at the initializer, requires the recorded style, writes the word and reads it back; any mismatch refuses the replay. A word outside both slots and the table is refused at recording. An older reader refuses a `bankOffset` port instead of reinterpreting it. The four final rows capture retained values implicated by game 2 input 638; their writer origins are documented in `replay-input-638.md`.

## Additional rule and producer fields in carryOver

| Field | Shape and use |
|---|---|
| `nativeOrdinal` | u32, native game-start ordinal |
| `appliedHandicapPercent` | Two u16 values, each 1–200, observed before native maximum-life calculation |
| `nativeInputContext` | `{schema: "dr-native-keyread-clock-draft-1", counter: u32}`; boundary witness, never copied into the running input clock |
| `nativeReplayFlag` | `0` for an ordinary recording |
| `styleProducer` | `{generatorContext, nativeOrdinal, nativeReplayFlag, leftRow, rightRow}` captured before the style producer; generator shape above, u32 ordinal, flag 0, rows 0–84 |
| `styleProducerBoundary` | u32 source boundary, no later than the game-start boundary |
| `roundOptionBytesHex` | 26 lowercase hex characters |
| `roundsOptionHex` | 2 lowercase hex characters |
| `gameRuleWordsHex` | 88 lowercase hex characters |

## Storage and spectator transport

The recorder opens one `game-N.partial.jsonl` at game start, writes the capsule first, appends round capsules and inputs with all 48-byte `pbd-sync-facts-v1` records, and closes at the native game-end boundary. The finalizer creates one immutable `pbd-replay-v2` file for that game and retains the recording emulator and game-executable identities. Finalization never manufactures missing carry, timestamps or sync facts from a completed session. Playback requires the game-executable identity, per-frame sync facts, and every carried field for games after the first. Files from another emulator build follow the same exact checks. The `configured` notice retains `playback: exact` and `recordedBy`. Any coordinate or combat hash difference stops playback and names the game, boundary, input offset and recorded round; the terminal native score must also match before success is reported. Take Control explicitly switches away from recorded playback and lets the changed fight end natively. Replay starts refuse memory-pasting, dumping, probing or timing test switches (`rpcs3/Emu/PBD/pbd_replay_switches.h`) unless the run sets `PBD_TEST_REPLAY_DIAGNOSTICS=1`.

The spectator transport carries these same capsule bytes in a versioned envelope. The proposed envelope is `pbd-game-start-capsule-envelope-1`: `{schema, game, sourceBoundary, build, capsule}`. `game` is 1–65535, sourceBoundary is u32, and build contains `emulatorSha256`, `gameExecutableSha256`, `capsuleSchema`. The complete envelope also stays within 65,536 UTF-8 bytes. Its identity is SHA-256 of canonical envelope bytes, excluding any identity field. The stream's existing generation/session admission binds the envelope; no session secret is serialized into it. The spectator transport owns fragmentation, delivery, retention and the spectator capability handshake.

For a late join, the spectator channel supplies the current game's envelope and the subsequent ordered inputs and original sync facts. The replay player validates and initializes that game at input offset zero. Source boundary coordinates remain recorded coordinates even when local playback starts from zero. The channel holds the capsule until every dependent input is consumable and discards an older generation. Ordered delivery uses the `spectator-native-2` capability; older watchers remain on `spectator-native-1` and receive no capsule message. Capsule bytes travel separately from ordinary control JSON, whose smaller cap remains unchanged; the envelope identity is bound before dependent inputs.

A missing or refused capsule refuses the player's capsule initialization. The spectator channel then falls back to its full stream from the clean local start. It never substitutes another game's capsule. This fallback belongs to the spectator channel; the independent replay player always consumes its own game file and has no earlier-game fallback.

Admission returns stable reason codes: `unsupported_capsule_version`, `capsule_too_large`, `invalid_capsule`, `incomplete_capsule`, `unqualified_capsule`, `unsupported_initializer`, `unsupported_stage`, `replay_build_mismatch`, `game_build_mismatch`, `missing_sync_facts`, `capsule_game_mismatch`, `capsule_generation_mismatch`, `capsule_resource_mismatch`. A failed carry write/readback or sync comparison stops playback; it never weakens the facts or advances with guessed state. The draft stays a diagnostic path (`qualified: false`) until a production revision replaces it.

## Golden fixtures and ownership

Neutral fixtures in `product/replay-fixtures/game-start-capsule/` pin production canonical bytes, content identity, unsigned retained bits and generator shapes. Tests pass those bytes through the production carry reader and live finalizer. Native carry tests pin the same field schemas and values through the native codec. Transport fixtures and older-watcher tests belong to the subsequent stream integration.

The replay side owns the capsule format and its reader; the spectator side owns transport, the capability handshake and the watcher's fallback. Each side has its own tests.
