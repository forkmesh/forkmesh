# Mobile access to owner-encrypted agent sessions

Status (2026-09-28): `IMPLEMENTED` on current `main` for owner-device read
access, mirror-queue prompts, and live control of a desktop directly on the LAN
or through the relay. The signed v1.2.5 desktop and Android release supports
relayed control after the phone has learned the desktop through LAN pairing;
automatic same-account desktop discovery and the phone Changes and Delete
surfaces landed after that release.

## Why the Agents tab used to look empty

Agent sessions are owner-only end-to-end encrypted. The desktop node seals every session snapshot and transcript tail into a `forkmesh.owner-sealed` envelope before pushing it, and the relay stores the ciphertext without a key of its own. The browser says so plainly ("Owner-device encrypted") and offers no plaintext fallback.

The mobile client read only the plaintext `agents` array from `/agents/list`. Under the default policy that array is always empty and the real rows arrive in `encryptedAgents`, so the phone rendered "No agent sessions" while the desktop was working normally. The fix is not to weaken the boundary; it is to make the phone an owner device that holds a real key.

## The owner-device model

An account may enroll several owner devices. Each holds its own hybrid recipient key and registers only the public halves at `POST /api/security/owner-keys`.

```text
                    account
                       |
        +--------------+--------------+
        |                             |
  desktop node                   mobile node
  vault key (policy key)         device key
        |                             |
        +------------- both -----------+
                       |
        one owner-sealed envelope, one wrap entry per device
                       |
                     relay
        stores ciphertext; holds no private half
```

One random AES-256-GCM content key encrypts the payload once. That key is wrapped separately for each enrolled device under a hybrid X25519 + ML-KEM-768 KEM, so adding a device adds a small wrap entry rather than a second copy of the data — and the relay gains nothing either way.

### Envelope shape

```json
{
  "kind": "forkmesh.owner-sealed",
  "v": 1,
  "alg": "x25519+mlkem768/aes256gcm",
  "nonce": "…", "tag": "…", "body": "…",
  "recipients": [
    { "kid": "…", "x25519": "…", "mlkem768": "…", "nonce": "…", "tag": "…", "key": "…" }
  ]
}
```

Rules every runtime enforces identically:

- Between 1 and `MAX_OWNER_ENVELOPE_RECIPIENTS` (6) recipients; duplicate recipient ids are refused outright.
- The **sealing device writes its own wrap entry first**, so the leading recipient identifies the producer.
- A recipient id is `base64url(SHA-256(x25519Pub || mlkemPub))`, derived from the key bytes rather than asserted, so a device cannot alias another's entry.
- The wrap key is `HKDF-SHA256(ikm: ssX25519 || ssMlKem, salt: "forkmesh-mirror-kem-v1", info: ephemeralX25519Pub || mlkemCiphertext)`. Binding the info to the exact wrap fields means mixing fields between recipients cannot yield a usable key.
- Each device opens **its own** entry, located by key id — never another device's entry by position.

### Authorization is membership, not identity

The repository privacy policy names one `ownerKeyId`: the desktop vault key that executes agent runs. The relay requires that key to be **among** an envelope's recipients rather than to be its only recipient. So:

- The desktop seals a session snapshot to its vault key **plus** every enrolled companion device. The policy key is present, so the write is accepted, and the phone can read it.
- A prompt bound for the desktop must still include the desktop's policy key, because the desktop is what has to open and run it.
- An envelope that omits the policy key is rejected with `wrong_owner_key`, whoever else it names.

## What each runtime does

| Runtime | Responsibility | Source |
| --- | --- | --- |
| Worker | Validates framing and bounds only; never decrypts. `owner_envelope_key_ids()` backs the membership check. | `app/src/security_controls.py`, `app/src/entry.py` |
| Qt node | Generates the vault key, fetches companion bundles from the owner-keys registry, and seals every snapshot to the vault key plus those companions. | `desktop/src/MirrorCrypto.cpp`, `desktop/src/MainWindowAgents.cpp` |
| iOS and Android nodes | Generate the device key, enroll the public bundle on sign-in, and open the sessions and transcripts sealed to it. | `native/ios/ForkMeshCore/Sources/FMCrypto/OwnerCrypto.swift`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/security/OwnerCrypto.kt` |

The desktop refreshes companion bundles on a ten-minute cadence and re-seals when the recipient set changes, so a newly enrolled phone starts receiving snapshots without a desktop restart, and a device revoked on the website stops appearing in new envelopes. Revocation is not retroactive: envelopes already stored remain readable by the devices they were sealed to, which is why revoking a device should be followed by rotating anything still considered sensitive.

## Honest empty states

The phone distinguishes three cases that used to look identical:

| Situation | What the app shows |
| --- | --- |
| The repository genuinely has no agent sessions | "No agent sessions" |
| Sessions exist but are sealed to other owner devices | "N sessions are encrypted for another device", with how to enroll |
| A transcript exists but is not sealed to this device | The same explanation in place of the transcript body |

Counting unreadable rows instead of dropping them is deliberate: silently discarding them is what made a working desktop look idle.

## Verification

The cross-runtime contract is executable, not asserted in prose:

- `native/ios/ForkMeshCore/Tests/FMCryptoTests/OwnerCryptoTests.swift` and `native/android/core/src/test/kotlin/com/forkmesh/nativeapp/security/OwnerCryptoTest.kt` drive the **real Qt crypto binary** (`desktop/build/forkmesh-tests --seal-owner-envelope-to`), then open the resulting envelope in Swift and Kotlin. They skip, rather than fail, when the Qt binary is not built.
- `desktop/tests/test_crypto.cpp` covers ordering, per-device opening, swapped-id rejection, duplicate rejection, and recipient bounds.
- `app/tests/test_security_controls.py` and `app/tests/test_repo_agents.py` cover validator bounds and the membership-based authorization, including that a companion recipient never substitutes for the policy key.
- `desktop/tests/validate_owner_envelope_contract.py` still checks that a Qt-emitted envelope passes the Worker validator verbatim.

## Sending a prompt to a mirror

The owner-sealed sessions above are the desktop's own work. A prompt typed on the phone takes the other route, the same one the web's Agents page uses: the relay-readable agent queue.

- The prompt form leads the Agents tab in every state, including with no sessions at all. A guest sees it too and is asked to sign in on send.
- `GET /api/repo/{o}/{r}/agent-sessions` lists the nodes that can take the job. An account that does not own the repository's node is refused there and the client falls back to `/api/orgs/{o}/repos/{r}/agent-bots`, as the web does; follow-ups and deletes go back to whichever route the session came from.
- The suggested target is an online mirror (a `headless` node, or any non-source node on the organization route), then any online node; the person can pick another. Only when no node can take the job does the run happen on the phone.
- A run that failed or stopped is offered a retry, from the list row and from the session screen. What that does depends on how far it got: a session a node claimed goes back as a follow-up carrying its original task, so the agent continues on the branch it already cut (the desktop's Continue, from the phone); a session no node ever started has nothing to resume, so the same task is queued again with the same node, provider, model, issue and pull-request choice, and the screen follows the new session the relay answers with. A run its owner stopped is shown as stopped rather than failed, and its retry sends the task it was given, not the stop request.
- The Cloud section of the Agents tab is every run a mirror is working, across all the account's repositories, newest first. A run counts as being in the cloud when the relay's own `targets` list calls its `targetNode` a mirror (`runtimeMode: headless`, or a non-source node on the organization route); a target the relay does not list is not claimed for the cloud, because the run is in the flat Agents list either way. The header says how many are running there, and each row wears a cloud rather than a bolt. The relay rings the account event socket's `cloud-agents` topic when a mirror claims, starts or finishes one, and that is what reloads the tab — the phone still has no timer.
- A queued session opens onto its chain: Prompt, Mirror, Branch, Pull request, Merge, read from `targetNode`, `localAgentId` and `agentInfo.{branchName, baseBranch, prNumber}`. A headless mirror always reports `merged: false`, so the merge stage is read from the repository's pull ledger, matching the pull by number or by the task's branch. The pull request stage opens the pull, where it can be merged from the app.

- The prompt is typed in the shared `PromptComposer` (`native/ios/ForkMesh/Kit/PromptComposer.swift`, `native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/composer/PromptComposer.kt`). Queued runs and their follow-ups can carry photos as attachments: each is downscaled to 2048 px and stripped of metadata, and a prompt's photos stay within the relay's 10 MB total (`PromptAttachmentLimits.maxTotalBytes` in `native/ios/ForkMeshCore/Sources/FMApi/Models/PromptAttachment.swift`, `MAX_TOTAL_BYTES` in `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/agents/PromptAttachment.kt`). The model chip picks the run's model, Claude Opus 5.5 (`claude-opus-5-5`) by default (2026-09-24).
- A failed run shows its error once: the relay builds the diagnostic from `agentInfo.lastError` and skips it when it only repeats that text, and a failed run with no transcript no longer reads "Prompt dispatched." (fe5a131cb).

## Controlling a desktop on the LAN or through the relay

The stored owner-sealed snapshots above and live desktop control are separate
paths. Stored snapshots let an enrolled owner device read retained sessions.
Live control opens the desktop's local-control protocol directly over the mesh
TCP port on the same network or inside `{"type":"control"}` chunks forwarded
by the account event relay when away. The relay sees account, desktop, timing
and frame-size routing metadata, but the Ed25519-authenticated handshake derives
per-direction AES-256-GCM keys, so it cannot read or forge the control payload.

On current `main`, the authenticated `GET /api/accounts/desktops` route returns
only the caller's enabled `desktop_node` keys, machine labels and last-seen
times, plus the account's primary desktop key. The phone caches that list for
ten minutes, tries at most four candidates, and checks that the answering
desktop presents the expected public key. The last desktop that answered is
tried first on the next connection.

The desktop checks the phone key against enabled `owner_sign` devices in its
own account. A same-account phone is accepted without a pairing code. A phone
outside that account cannot pair through the relay; it must meet the desktop on
the LAN and enter the six-digit code there. The account approval is remembered
in the desktop's local `data/local_control/paired-devices.json` as an
account-held, desktop-local pairing (`"approval": "account"`).

Since 2026-10-01 an account-held pairing follows the account. The desktop asks
the account again on every connection and every ten minutes while a channel is
open. When the account no longer lists the phone, the desktop closes the
channel (`revoked`) and drops the phone from `paired-devices.json`. If the
account cannot be asked, a phone on the LAN is still let in. Through the relay,
a phone is let in only if the account confirmed it in the last 24 hours;
otherwise the desktop refuses with `account_check_failed`. A phone approved in
person with the six-digit code stays approved until it is removed in Desktop
**Settings → Phone**. Revocation does not make owner-sealed envelopes that are
already stored unreadable; see
[The owner-device model](#the-owner-device-model).

Away from home the channel recovers by itself:

- The phone pings an idle relayed channel every 30 seconds. Before it reuses a
  channel that has been quiet for 45 seconds, it pings that channel first. A
  channel that does not answer is closed, and the next request dials a fresh
  one, so a request is never sent on a channel that is already dead.
- A desktop that gets a frame for a channel it no longer has answers
  `channel_gone`. This happens after a restart or after its relay socket
  dropped. The phone then redials at once instead of waiting for a timeout.
- When the desktop's relay socket returns, the desktop tells each phone whose
  channel ended while the socket was down (`relay_reconnected`). It reconnects
  at once when the operating system reports the network is back, and it waits
  at most two minutes between attempts while the relay is down.
- The desktop closes a relayed channel that has been silent for 15 minutes.
  When all eight channel slots are taken, it makes room by closing the channel
  that has been idle longest (idle for at least a minute). At most 16 relayed
  hellos may be half-received at once, and each expires after 30 seconds.

Code: `app/src/entry.py` (`_account_desktops_handler`),
`native/ios/ForkMeshCore/Sources/FMControl/`,
`native/android/core/src/main/kotlin/com/forkmesh/nativeapp/control/`,
`desktop/src/LocalControl.{h,cpp}`, and
`desktop/src/MainWindowLocalControl.cpp`.

## Live desktop-agent actions

| Action | Desktop-control methods | Confirmation boundary |
| --- | --- | --- |
| List and start; read a transcript; prompt, stop or continue | `agents.list`, `agents.start`, `agents.transcript`, `agents.prompt`, `agents.stop`, `agents.continue` | Authenticated control channel |
| Attach photos to a start or a prompt | `agents.image.begin`, `agents.image.chunk`, then `images` on `agents.start` or `agents.prompt` | Authenticated control channel; the desktop checks each photo's size, type and SHA-256 |
| Read and answer pending tool requests | `agentops.approvals`, then `agentops.prepare` and `agentops.answer` | A fresh one-use desktop confirmation; approval also requires phone device authentication |
| Review committed, uncommitted and untracked changes | `agentops.diff` | Read-only; the desktop redacts configured provider credential values and may truncate a large diff |
| See changed images and SVGs in that review | `agentops.file` | Read-only; image files of the agent's branch or worktree only, at most 2 MiB each |
| Merge and clean up | `agentops.prepare`, then `agentops.merge` | One tap; the agent must be idle |
| Inspect worktrees | `agentops.worktrees` | Read-only |
| Delete the agent, worktree and unmerged branch | `agentops.prepare`, then `agentops.delete` | Explicit confirmation; the agent must be idle |

The desktop writes control changes to its log. An older desktop answers
`unsupported` for methods it does not have; the phone asks the user to update
instead of pretending the action succeeded.

## Current limitations

- The desktop must be online, signed in to the same account and connected to
  the relay for away-from-home control. The relay forwards a channel to the
  desktop; it does not execute the action while the desktop is offline.
- A relay-socket reconnect closes the live channel. The next action opens and
  authenticates a new one.
- File transfer remains a LAN feature. Relayed text prompts work, but picture
  replies are not carried over this control channel.
- The signed v1.2.5 desktop and Android release predates automatic account
  desktop discovery and the phone Changes and Delete UI. Those parts require a
  build containing the 2026-09-28 current-main change.
