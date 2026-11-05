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
binding with tables, and check it against the paired record from both players. The current
launcher checks in `nativeSpectatorProtocol.ts` and `spectatorStreamReader.ts`, and the service's
owned/paired checks in `lobbyRoom/streamInput.js`, reject this byte unconditionally. All three
must change together, gated by the table contract; applied-pad padding remains zero. Fixed
looks and capsule starts need no slot extension in the spectator stream.

Customize requires a separate offline VS-select boot under PS3 user `00000003`, with
`PBD_OFFLINE_PLAY=1` and `PBD_SESSION=0`. Practice's user `00000002`, session mode and checkpoint
cannot enter this isolated shop. An incompatible Customize boot returns `unavailable` and
changes no saved look. The save panel, request/result exchange and fixed portrait crop are
implemented; a verified preview pose reset and visual crop check remain required before
publishing its optional picture. Production results currently omit `picture`, so these saves use
the numbered outline.
