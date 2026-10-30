# Loaded move-list reader

`pbd_move_catalog.h` provides catalog and timing readers for NPUB30009's loaded character banks;
`pbd_move_readiness.h` provides committed and queued move readiness.
Call it on the main game thread at a qualified Practice boundary, after the existing title,
executable and actor-pair checks. Pass the actor's bank (`actor + 0x50`) and base character id.
`Word(address)` and `Half(address)` return unsigned big-endian values, or `~0u` on a failed read.
No function calls guest code, writes guest memory or draws a random number.

- `read_catalog(bank, character, Word, Half)` returns an optional `catalog`. Its `entries` carry
  the stock descriptor ordinal `id`, English `name`, original game-glyph `notation`,
  `command_id`, the number of additional `variants`, and the stock `conditions` / `execution`
  halfwords from descriptor offsets `0x36` / `0x38`. The command ids for a row are
  `command_id` through `command_id + variants`, inclusive; every variant is validated through
  the current command map, record table and sample table. Titled rows without commands become
  `catalog.headers` category records, each with a name and the position of the following entry.
  `list_rows()` displays those titles separately from selectable moves; untitled header rows are omitted.
- `read_command(bank, command_id, Word, Half)` returns an optional `command`: mapped record
  index, three original header words, kind and the original six-byte demonstration samples
  as three halfwords each. These are stock records; interpreting their timing/flags and
  executing the move belongs to the dummy move controller.
- `read_timing(bank, allocation, Word, Half)` returns the separate calculated timing result.
  Startup/active/recovery and standing block/hit stun are optional independently. Conditional
  scripts, unqualified reaction properties, aliases and floor terminals stay unavailable.
  Advantage assumes first-active contact. Observed advantage continues to use actual action timing.
- `readiness()` and `queued_readiness()` follow the loaded commands from a committed or queued cursor.
  Their readings describe primary readiness and earlier escape routes as offsets from the move's origin.

Persist `(character, entry.id)` for a selection, never a bank address or command pointer.
Resolve it again from the currently loaded catalog after a character/costume change or restore.
The descriptor and command readers reject changed table identities and bounded-read failures;
the caller must also recheck actor/bank identity before publishing or executing a selection.
Names and commands come from the player's loaded game; no game text or command table is bundled.

Bounds: 4,096 descriptors, 65,536 command mappings, 8,192 demonstration records,
4,096 six-byte samples per command, 256 UTF-16 units per label. The timing reader retains
its own command/property bounds. Missing data returns an empty optional or unavailable fields.
