# Recovering an authenticated match connection

A live authenticated set detects missing current-peer input after 500 ms. It keeps the same game, input boundary, rollback history and key while attempting recovery. The existing wait panel remains responsive. Unauthenticated diagnostics retain their original timeout policy. A recovery episode has a 90-second lease; exhaustion reports `peer_recovery_timeout`. A confirmed peer-end message, invalid input, state mismatch or explicit player cancellation retains its normal handling.

Every protected input, exit-gate and segment wait has an absolute 90-second lease from the start of that blocked wait. A full 60-second outage plus transit therefore survives on both peers, including the peer still receiving authenticated frozen resends over the surviving direction. Neither those resends nor repeated recovery pulses renew a blocked wait. A resumed game starts a fresh lease only when it encounters a new wait. Unauthenticated waits retain their legacy budgets. Renderer UI may refresh at the completed frame handshake while the recovery panel is shown; it neither satisfies the guest semaphore nor advances a guest update.

## Status

The ordinary health JSONL and fixed-size health snapshot include `network` for an admitted protected set. A change is published immediately, independently of the normal one-second pulse. Ordinary health remains `waiting` during repair.

Numeric fields are JSON integers. `sessionId`, `setId`, `commandResult` and `reason` are strings; `recovering`, `sent` and `peerHeard` are booleans. Counters/epochs/serials are in `0..9007199254740991`; active epochs and command serials begin at 1. `peerSession` is in `1..4294967295`. `family` is 4 or 6, and `localPort` is the actual admitted socket's port in `1..65535`. The status carries:

```
sessionId, setId, peerSession, keyEpoch, localPort, relayLocalPort, family,
requestId, recovering, sent, sentAgoMs, peerHeard, lastValidReceiveAgoMs,
commandSerial, commandResult, proposalSerial,
verifiedProposalSerial, activeProposalSerial, reason, socket
```

`sent` and `peerHeard` are booleans: this game has sent under the current key (at the session start, its start digest went out), and the peer's first valid input under the key has arrived. `sentAgoMs` is the time since that first send, 0 before it. Until `peerHeard`, `lastValidReceiveAgoMs` is the time waited for the other game, not the age of a received input. Native requests recovery only once the peer has been heard, since before that the other game may still be loading. A route that carries nothing either way after the connection test therefore shows on both sides only as `sent` with `peerHeard` false and a growing `sentAgoMs`; the launcher, once it knows the other game is sending as well, stages the replacement path with the same commands. `commandResult` is `none`, `applied` or `refused`. Zero proposal values mean none. A new key or set clears candidates and proofs; the independent path-command cursor remains monotonic for that resident channel. The launcher's writer fences every observation against the admitted session, set and epoch, and treats missing metadata as unavailable.

`socket` contains saturating unsigned 32-bit counters: `sendOk`, `sendFailed`, `lastSendError`, `datagrams`, `sourceMatch`, `sourceMismatch`, `rejectTag`, `rejectRole`, `rejectSession`, `rejectEpoch`, `rejectFormat`, `rejectReplay`, and `rejectSource`. `firstDatagramUs` is the first datagram's monotonic session-clock time in microseconds, or zero before any datagram. Reception counts and source equality are recorded before authentication, including empty and oversized datagrams. Send counts describe actual socket results, including candidate probes; `sent` continues to describe the first sealed attempt. The last socket error survives subsequent successful sends. There is no per-packet output or stored source address or payload. Ordinary one-second health publication carries the counters; receiving invalid traffic does not cause extra health pulses or change liveness.

The wire carries no key epoch. A packet under an earlier key therefore normally fails its tag. `rejectEpoch` counts a retired receive key or an authenticated queued packet whose key retired before admission; it does not claim to identify an old wire key. Format, role and session labels inspect game fields only after successful authentication. All paths retain the same directional keys and replay window.

`relayLocalPort` is zero when the primary socket can receive the local relay, or a companion socket's port for a pinned public IPv6 source. The companion binds only `::1`, uses nonblocking I/O, and shares the primary socket's receiver, authentication state and replay window. Local relay targets send through it; direct and previous direct targets keep the original pinned socket. It closes with the transport. If the optional companion cannot open, IPv6 startup fallback is unavailable and the ordinary activation bound still applies. Older native versions omit the field and cannot opt into startup fallback.

## Silent startup fallback

After the direct handshake connects, supporting launchers exchange the set-bound `nr1-fallback-1` offer. The new `nr1-<peerSession>-activated-1` notice identifies a game sending under its admitted key without triggering an older launcher's legacy silent-start detector. Both offers and matching game notices are required for startup fallback. A late notice for another set, session tag or opponent is ignored.

The direct winner retains its prepared relay until the game's first valid current-peer input or the set ends. If the local game remains unheard for three seconds and the peer has reported sending for three seconds, the existing recovery exchange takes ownership of that allocation. A prepared IPv4 shim can be replaced with an IPv6 loopback shim while retaining the allocation and permissions. If preparation failed, the exchange can still request a replacement allocation.

The three-second bound leaves margin over the roughly one-second successful first-contact observations; it measures both games' activation rather than either game's loading time. The normal stage, proof, commit and active messages below coordinate the switch. An initial recovery that cannot establish the path in twelve seconds ends the admitted set as `launch_failed`, closes its game, and offers Retry without identifying a Ranked fault. A working committed candidate remains open while a delayed peer acknowledgement is retried. If that candidate also falls silent before agreement, the initial failure bound still applies. An older peer that does not offer support remains on the existing route.

## Path commands

One launcher writer publishes immutable `network-path-<serial>.json` files inside the admitted set channel. It waits for each status acknowledgement before allocating another serial. This stream is independent of menu/gameplay commands. Files are exclusive and atomically published; never overwrite a serial.

Every envelope contains `schema: "pbd-network-path-1"`, `sessionId`, `setId`, `peerSession`, `keyEpoch`, `serial`, `operation` and `proposalSerial`. Unknown fields, duplicate keys, invalid ranges and documents over 4096 bytes are refused. Refusal acknowledges that path serial without changing gameplay or authentication.

- `stage`: `proposalSerial == serial`; additionally supplies a literal `address` and a host-order `port`. It must be a unicast endpoint of the socket's existing family. Loopback and Tailnet endpoints are permitted. An IPv6 socket therefore uses `::1` for a local relay proxy, and an IPv4 socket uses `127.0.0.1`.
- `commit`: references the staged proposal, with no endpoint fields. It requires current authenticated candidate input received after staging and within the previous second.
- `cancel`: references the staged proposal and discards only that candidate.

Staging retains the old target and probes both paths using separate fresh authentication sequences, so a faster old route cannot consume the candidate's replay-window entry. When a second candidate overlaps previous-path grace, retransmissions alternate those two extra endpoints: one active send and at most one extra send per poll keeps a 250 ms candidate inside the existing 64-packet replay window, including 100 Hz input holds. Only fully validated current-peer/session input from the exact candidate proves it. Candidate input can repair the same game's missing inputs before commitment. An automatic NAT rebind may adopt a returned non-loopback path; entering or changing a protected loopback route requires the explicit sideband.

The launcher waits for **both** peers' `verifiedProposalSerial`, publishes their commits, then waits for **both** `activeProposalSerial` acknowledgements before closing the old path. The native target changes before its active acknowledgement becomes visible. For 60 seconds after a commit, the previous endpoint can still deliver authenticated input and receives separate fresh-sequence retransmissions. This bounded grace allows an asymmetric peer commit or service acknowledgement to arrive late. It clears on a new set/key and never switches the active target back. Authentication keys and the local game socket stay unchanged. Stale epochs, earlier queued packets and expired proofs cannot commit a route.
