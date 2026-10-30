# Actor values a game leaves for the next one

The game's match initializer does not clear four groups of actor scratch words. A later game of a set therefore
starts with what the game before it left in them, and a replay of that later game must start with the same values.
They are four named carry fields of the game-start capsule. None of them is a random seed.

| Capsule field | Left actor address | Right actor address | Encoding |
|---|---|---|---|
| `actorSegmentDeltas` | own `0x100906c8`, copied `0x100906d4` | own `0x10090f98`, copied `0x10090fa4` | two XYZ vectors, uint32 float bits per side |
| `actorPriorLife` | `0x100906f4` | `0x10090fc4` | uint32 16.16 life bits per side |
| `actorReactionScalar` | `0x1009032c` | `0x10090bfc` | uint32 float bits per side |
| `actorPreviousBlendWeight` | `0x10090844` | `0x10091114` | uint32 float bits per side |

Actor bases are `0x10090040` and `0x10090910`, stride `0x8d0`. The field offsets are `+0x688/+0x694`,
`+0x6b4`, `+0x2ec`, and `+0x804`. Each vector spans 12 bytes; each scalar spans four.

The game's own writers give the values their meaning:

- `0x31670..0x316a0` subtracts segment start XYZ from segment end XYZ and writes `+0x688..0x690`.
  `0x316a4..0x316bc` copies that vector from another actor into `+0x694..0x69c`. Both vectors are recorded: the own
  vector also differs between a fresh boot and a later game at the start boundary.
- `0x12724/0x12728` copies current life at `+0x6ac` to previous life at `+0x6b4` before damage processing.
  This is prior damage state, not the newly initialized current health.
- `0x4b2bc..0x4b2f8` reads and sign-extends a reaction record halfword, converts it to single precision and
  stores it at `+0x2ec`. It is a retained reaction term.
- When blend mode at `+0x7d8` equals 3 (`0x28ce3c..0x28ce44`), `0x28dbd4/0x28dbd8` copies current blend
  weight `+0x800` to previous weight `+0x804`. `0x28c6dc` later reads that previous weight.

The writer body hashes and address constants are in `rpcs3/Emu/PBD/pbd_t5dr_memory.h`.

The recorder reads all four fields immediately before the native game initializer, including the zeros of game 1, and
stores exact bit patterns, never decimal float values. Playback checks the title, executable, owner thread,
generation, native initializer and writer bodies, writes the recorded words and reads them back. It never fills a
missing field from later frames, writes a clock, copies an allocator pointer or changes a sync mask.
