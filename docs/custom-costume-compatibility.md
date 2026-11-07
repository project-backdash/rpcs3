# Custom costume compatibility

Games that carry a custom costume record `pbd-game-start-capsule-draft-4`. The envelope's
`build.capsuleSchema` must match. Older launchers refuse these files with
`unsupported_capsule_version`; they cannot reproduce the recorded costumes. Earlier recordings
continue to use their default costumes. Games without customs retain draft-2, or draft-3 when
they carry the throw-break rule.

Practice and Customize require `PBD_CUSTOM_LOOKS=1`. Online and spectator costumes are enabled
by the agreed manifest fields, after the launcher and service have negotiated customs. They do
not depend on the local Practice switch. Ordinary input messages retain version 12 and their
existing bytes. A selected lobby custom adds the sender's current and previous slot as two
little-endian u32 words after the version-12 tail, under input version 13. Slots are resolved
against the corresponding side's immutable table after the shared scene barrier.

For a table-bound spectator stream, byte 2 of each eight-byte owned and paired pad record carries
the side's agreed slot (0..10). Bytes 3..7 remain zero; executed and applied button words retain
their existing 16 bits and applied-pad padding remains zero. A watcher reads these slots before
the resource scene's card write. The launcher must permit and preserve this byte only for a
binding with tables, and check it against the paired record from both players. The
launcher checks in `nativeSpectatorProtocol.ts` and `spectatorStreamReader.ts`, and the service's
owned/paired checks in `lobbyRoom/streamInput.js`, preserve this byte only for table-bound sides
and validate both peers' records; applied-pad padding remains zero. Fixed
looks and capsule starts need no slot extension in the spectator stream.

Customize requires a separate offline VS-select boot under PS3 user `00000003`, with
`PBD_OFFLINE_PLAY=1` and `PBD_SESSION=0`. Practice's user `00000002`, session mode and checkpoint
cannot enter this isolated shop. An incompatible Customize boot returns `unavailable` and
changes no saved look. EXIT or a fresh Start press captures the equipped card into the request's slot,
restores the isolated card and select state, writes the result and closes. The loaded save and side-save
copies must still match their entry snapshots before a saved result can be returned. No save panel or
shop picture is part of EXIT. The launcher can close an unfinished edit without saving.

An offline Customize request uses one controller from boot and bypasses local Versus seating,
side selection and its overlays. The shop receives the controller's menu buttons before fight
action remapping. The stock select's name and record paint are suppressed; its rank sprite is
hidden through its stock opacity without skipping the rank resource lookup. Editing has no deadline.

Practice and offline Versus both create the shared select layer without an online set directory.
Offline Versus reads `practice-looks.json` from its menu channel, resolves each player's own Square
picker, and writes the chosen card before the existing direct resource load. Rematches retain the
look when its row is retained. The cards never change the persistent PS3 save.

Visible games receive `PBD_CUSTOM_PICTURES`, the absolute path to `customs/picture-looks.json`:
`{"schema":"pbd-custom-pictures-1","looks":[{"slot":3,"bytes":"0d0004ffffffffffff02ffff"}],"captureDefaults":true}`.
At a neutral fight opening the native reads this local list. A loaded actor's costume row and equipped
card must match the exact saved bytes before a picture is requested. The renderer supplies one fresh
frame before host overlays; a 320 by 480 crop of that side is written to
`customs/photos/<characterId>-<NN>.jpg`, at most 30 KiB. Existing files are never replaced by wearing the look again.
Editing removes the old picture and refreshes the identity list, so the next use captures again.
Non-Practice captures occur during the round countdown. Local Practice observes its first neutral opening through
the fade without owning input. Applied input from either fighter abandons the capture until another load; an adopted
mid-fight game also waits for another load. The renderer reads after its ordinary frame fence, without a queue flush;
cropping, JPEG encoding and atomic file publication run on a separate host thread.
`captureDefaults` adds each base costume, matching the exact default equipped card, under `costume-<row, two digits>.jpg`.
Existing base pictures are never replaced or invalidated by custom edits. `practice-looks.json` can name their absolute
directory in `costumePictures`; both native pickers then show those pictures for default cards. Practice reloads the
files whenever its picker reopens. Legacy PNGs remain readable while the launcher converts them at startup, publishing
and verifying the bounded JPEG before removing the old image.
Capturing never closes play. Picture failures leave the ordinary portrait until another run supplies its image.

Every native costume panel decorates cards from the same local `PBD_CUSTOM_PICTURES` root, including either side of
online selection and repicks. A custom photo is attached only if that slot's packed bytes match the local index;
photos and names never enter the online manifest. Panels reload when opened or when their own process atomically
publishes a photo, so an already open panel can show the new file.

First-use observation survives loading before actor cards are available. Only unsuppressed fight input cancels a
Practice opening; stock select confirms and input consumed by the launcher or start flow do not. The index reloads
on loading and visibility transitions, including a game kept ready before its player's saved slots changed.
Contract, index-count and opening-state log entries distinguish missing paths, empty indexes and unavailable neutral
openings without capturing diagnostics into the picture or delaying play.

New portrait crops start one quarter down the guest frame and extend to the bottom, below the fight HUD and Practice
damage text. Existing photos retain their pixels. Each JPEG records capture side in an APP15 segment immediately after
SOI: `FF EF 00 0C`, ASCII `PBDPHOTO`, version `01`, then side `00` (P1) or `01` (P2). The segment's 14 bytes count toward
the 30 KiB limit. Native cards reverse horizontal texture coordinates only when capture side and display side differ.
Untagged legacy photos use P1 as a fallback because their actual capture side is unknown.
