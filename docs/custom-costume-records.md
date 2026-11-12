# Custom costume records

How custom looks reach a set, a replay and the costume editor. [ARCHITECTURE.md](../ARCHITECTURE.md) describes
the session they run in; [custom-costume-compatibility.md](custom-costume-compatibility.md) says which
recordings carry a custom costume record and which launchers read them.

With `PBD_CUSTOM_LOOKS=1`, fixed set fighters read their worn `{slot, bytes}` look from the set manifest.
`pbd_custom_wire.h` validates the bounded wire shape separately from the per-row item catalogue. At the shared
resource-scene boundary after the outgoing selector fills the cards, `write_custom_looks` rebuilds each fighter's
three costume blocks from that fighter's saved inventory, clears equipped items and colours, and applies the
catalogue-normalized look. These card pages already belong to rewind capture; the side-save copies are untouched.
The live recorder keeps the agreed raw look identity in capsule draft-4, including when catalogue validation
makes it draw defaults. Draft-4 can also carry the existing throw-break record. Replay and capsule-start spectators
restore those looks at the resource boundary; fixed spectator manifests bind the same worn bytes. With the switch
off the writer does nothing, live records keep their old schema, and direct playback of a custom capsule is refused.
Lobby table transport and selected-slot agreement are separate from this fixed-pick path.

The optional offline direct-entry observation in `pbd_custom_editor.cpp` additionally requires
`PBD_CUSTOMIZE_TRIAL_ROW`, an absolute new `PBD_CUSTOMIZE_TRIAL_OUTPUT` path, and PS3 user 3. It enters
the stock select's initialized Customize parent through side state 5, opens Item Shop & Equip only after
its preview loads, and intercepts the return before NAME ENTRY or GAME START. During that emulation,
save operations are refused. Exit records the normalized card, compares the global and side-save images,
restores the card and selector snapshot, and stops before restored selector bytes can execute against
new preview tasks. This bounded observation is not the launcher's editor request/result protocol.

`pbd_custom_save.h` models the local save decision separately from file persistence. It selects one of ten
slots, requires a completed photo before saving, asks once before replacing an occupied slot, and allows
returning to editing or discarding even if capture fails. Opening waits for a neutral pad so the shop's
exit press cannot accept the panel. The launcher contract and editor save renderer are not connected yet.

`pbd_custom_photo.h` transfers one requested guest frame from Vulkan to the editor as bounded RGBA bytes.
Its ticket, emulation generation and minimum flip reject stale frames. Vulkan reads the guest present source
before drawing any overlay, with an explicit transfer-to-host barrier and its normal queue synchronization.
The editor owns the request, result consumption and PNG encoding; the renderer receives no output path.
The direct-entry observation holds the stock item view with neutral controls after exit, waits for two new
guest flips before capturing, and writes `editor-photo.png` beside its result before restoring and stopping.

The Practice select routes its physical first port through `pbd_costume_select.h` when customs are enabled.
Square opens the local Preset row, Cross selects the first stock default or the highlighted stock costume,
and Triangle requests Controller Setup. The stock button table supplies every costume confirm. The published
`pbd_costume_panel.h` view is drawn by the existing selection overlay, in its own side's red or blue panel;
it disappears outside selection and on an emulation change. Current cards show costume numbers until the
slot/photo library is connected. Lobby input routing and the remote choosing notice are not connected yet.
