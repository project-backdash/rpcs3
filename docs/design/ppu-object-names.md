# Compiled game object names

Code: `rpcs3/Emu/Cell/pbd_ppu_object_name.h`, the object name in `ppu_initialize` and the checks in `ppu_initialize2`
(`rpcs3/Emu/Cell/PPUThread.cpp`), the hook tests in `rpcs3/Emu/Cell/PPUTranslator.cpp`, and `--pbd-ppu-codegen` in
`rpcs3/rpcs3.cpp`.

## Why

RPCS3 takes a compiled object from the cache purely by its name. A name that stays the same while the emitted code changes
makes one PC run code another PC does not: a crash, or two PCs that drift apart.

Each launch mode emits its own function hooks (a lobby set, a watcher, Practice, a match). The game executable is split into
objects of at least 100 KiB and 1000 functions, independently of any hook. When one hash of the complete hook lists named
every object, each mode compiled every object again, although a hook changes only the code of the object that holds it and
most objects hold no hook of any mode.

## How

An object's name carries:

- the prefix: the name revision, the ABI revision and the `ppu_thread` layout the generated code bakes in;
- the run's switches that change every object: input hooks (`input1-`), compiled write capture (`undo1-`), vector-leaf
  inlining (`leaf3-`/`leaf4-`) and the scalar passes (`scalarN-`);
- the object's own hooks: a hash of the probe and skippable sites the translator can test while it builds the object's
  functions (`fn<hash>-`, none when there are none), and `cp16a-` when the object holds a copy loop JIT copy16 rewrites;
- the guest code hash, the settings and the CPU, as upstream.

The sites an object can test are the instructions of each function it builds, the seven-word window of a rewritten copy
loop, and the callees vector-leaf inlining decodes into it (the leaf window in mode 1, any callee in mode 2). The translator
and the namer read the loop table and the leaf window from the same header.

A firmware library is relocatable: no PBD code reaches it, and its name is the prefix, the guest code hash, the settings and
the CPU alone, as it was before objects named their own hooks.

Before an object is written, `ppu_initialize2` makes two checks; an object that fails either is not written and the boot
stops, as for an object that fails LLVM verification:

- every hook the translator tested for and found while it built the object (an input hook slot, a function probe or
  skippable entry, at an instruction, in a copy loop's window or in an inlined leaf), and every copy loop it rewrote, is
  listed by the object's name (`first_unnamed`). A translator that tests for a hook where the namer does not look is caught
  on its first compile, before two objects with different code can share a name;
- every `__dr2026_fn_probe` call in the finished module is at a site the name lists.

## Revisions

The ABI revision (`abiN` in the prefix) must change whenever emitted code changes without changing a field offset, a setting
or a tag. It names firmware objects too, so a new revision makes every player compile all of `/dev_flash/sys/external` again
on the next boot: minutes on a 4-core PC. A change that reaches only game code gets a new tag in `part_tags` instead, which
relocatable parts never carry.

Naming each object after its own hooks needs no new revision: the emitted code is the same, and each name still stands for
one code:

- an object no hook of its run reaches (none at its instructions, in the window of a copy loop it holds, or in the leaf
  window it may inline from) is built exactly as in a run without hooks, which has always named it without a hook tag;
- an object whose own hooks are a set S is built with probes exactly at S, as in a run whose complete lists were S;
- `cp16a-` now marks only the objects that hold a copy loop head. JIT copy16 changes no other object's code, so the others
  share the name of a run without it.

What instrumented objects emit today:

- MFTB calls `__dr2026_get_tb(thread, pc)` instead of `__get_tb`;
- the input hook materializes every live vector register before its call, so the restore inside it is followed by a true
  reload;
- capture-namespaced stores test the capture flag and the static|marked page bitmap inline and call `__dr2026_undo` only on
  a clear bit or a page-crossing span; the two addresses are fetched once per function through `__dr2026_undo_on_ptr` and
  `__dr2026_undo_bits_ptr`, because external data symbols resolve to null-function stubs in this JIT;
- a function hook calls `__dr2026_fn_probe(thread, pc)` and branches on its tri-state answer before the instruction.

The split revision (`split_revision` in `pbd_ppu_object_name.h`) counts changes to how the game executable is analysed into
functions (`PPUAnalyser.cpp`) and grouped into objects (the split in `ppu_initialize`). Object names follow such a change
through the guest code hash already; the identity below does not see the code, so the revision must change with either.

## The launcher's question

`rpcs3 --pbd-ppu-codegen` prints, without booting, the identity of a run with the process's environment: the prefix, the
split revision, the run's switches and the hash of the complete hook lists. Two runs with the same identity name every object
of the same game, with the same settings and CPU, alike. The launcher prepares a mode again only when its identity (or its
settings or cache folder) changed, or one of the objects its last preparation used is gone. The identity never decides which
code runs; the emulator still looks every object up by its own name.

The launcher asks once per emulator file and environment while it runs, and remembers a failed question as well, so it never
starts the emulator over and over for an answer it does not get. A question is cancelled with the preparation that asked it:
a starting game never waits for one. While an emulator that knows the question has not answered it, the launcher prepares
nothing for that environment; only an emulator without the question is counted by its file's hash.
