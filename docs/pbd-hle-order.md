# Reproducing the checkpoint HLE table

The checked-in table contains 3,875 slots and has existed since fork commit `1c6ebefc` (then under its former filename).
Its historical producer executable is not retained here. The canonical index/key dump recoverable from that table has
SHA-256 `a30d55cccc2651b886cf71b542be42e8199151abe6e0c0c829195a9706df410a` and slot digest `3677ef020c937593`.
These identify the compatible input; an arbitrary current executable is not a substitute.

On a build compatible with that checkpoint table, set `PBD_HLE_ORDER_DUMP` to an absolute output path and boot the game.
`ppu_function_manager::pbd_pin_order()` writes one decimal index, a tab, and its key per line after registration/pinning.
Then, from the native source root:

```powershell
python tools/pbd_hle_order.py C:\scratch\hle-order.tsv --check
```

The converter checks contiguous indices, unique keys, the complete input identity and the same FNV-1a slot digest as
the runtime. It accepts Windows line endings. Omit `--check` to reproduce the include after those checks pass.
The converter's output must be byte-identical apart from working-tree line endings; the existing table and checkpoint
compatibility key are unchanged. The runtime may register additional slots: a larger/different dump is refused here,
even if its leading pinned slots match. Supporting such a table requires an explicit checkpoint compatibility change.

At checkpoint creation, retain the runtime's complete `PBD HLE slot digest` in the existing checkpoint metadata.
At load, pass that value through `PBD_HLE_ORDER_EXPECT`; `pbd_pin_order()` refuses an unequal full runtime table before
the checkpoint executes. The converter's pinned-table digest does not replace that per-checkpoint runtime check.
