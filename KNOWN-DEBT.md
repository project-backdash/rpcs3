# Open structural work

The executable checks are described in [CODE-BAR.md](CODE-BAR.md). This page records unresolved design boundaries,
without freezing file counts or promising checks that do not run. [SOURCE.md](SOURCE.md) is the generated inventory
of the current fork, including additions inside upstream files.

- **Session ownership.** Boundary steps still share the aggregate in `pbd_rollback_session_storage.h`. Separate
  histories, mode state, diagnostics and host transport by who may mutate them; preserve restore and tick order.
- **Configuration.** Native modes still read an environment matrix through several helpers. Consolidate one mode at
  a time into validated immutable input. Keep launchers and stored setup/checkpoint identities compatible.
- **Guest facts and hooks.** Shared facts have a named header, but instruction sites and guest offsets remain in
  individual subsystems. Name each with its producer. Keep required JIT/SPU/RSX changes visible in the upstream diff;
  a small wrapper does not by itself establish ownership.
- **Diagnostics and failures.** Hook/input event capture is opt-in, high-volume input records are queued, and renderer
  freeze diagnostics use a synchronized snapshot. Continue reviewing other optional diagnostic paths and shared
  observations. New failures should retain structured causes at their owning boundary.
- **Automation.** Formatting runs in the PBD workflow and unit tests run in eligible inherited pull-request jobs.
  Portable sanitizers and broader native static checks are additional work, not current guarantees.

Peer authentication and directional sequence checks are implemented. The upstream overlay, pad and window additions
have already been reduced; earlier line totals no longer describe them. Input-script compatibility is separate from
the launcher's per-game replay library and must not be used to advertise a retired session replay format.
