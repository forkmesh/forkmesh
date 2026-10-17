# Native phone apps: feature matrix, workstreams and the watch app

`native/ios` is the iPhone app, `native/android` the Android app, and
`native/ios/ForkMeshWatch` the Apple Watch app. This page lists the phone
feature set (row numbers `#n` are stable and cited by the source and by
[DESKTOP_PARITY.md](DESKTOP_PARITY.md)), the platform decisions, the iOS
workstream structure (WS1 to WS7), the watch app's design, and the bundle ids.
Companion pages: [DESKTOP_PARITY.md](DESKTOP_PARITY.md) (parity with the Qt
desktop, WS8 to WS21) and [NAVIGATION.md](NAVIGATION.md) (information
architecture).

Paths: `NI/` is `native/ios/`, `NIC/` is `native/ios/ForkMeshCore/Sources/`,
`NA/` is `native/android/`, where `A/` is
`app/src/main/kotlin/com/forkmesh/nativeapp/` and `C/` is
`core/src/main/kotlin/com/forkmesh/nativeapp/`.

## 1. Feature matrix

Both apps implement every row unless section 2 says otherwise. Rows 7, 51 and
76 are iOS only; rows 63 and 64 are platform-specific (the announcer is
desktop-only; on-device models run through MLX on iOS).

| # | Feature |
|---|---|
| 1 | Log in with password and TOTP, bound to the device key; "Forgot password?" opens `/forgot-password` |
| 2 | Sign up (hands off to the website `/signup`, then returns to the login form) |
| 3 | Guest mode and the guest gate on writes |
| 4 | Device Ed25519 identity and owner-device enrolment (X25519 + ML-KEM-768), needed to read sealed agent transcripts |
| 5 | Portable identity recovery kit (core library; no screen on iOS) |
| 6 | Account nodes, sessions and this-device cards; revoke |
| 7 | iOS UDID lookup (`forkmesh://ios-udid`), iOS only |
| 8 | User-presence gate (biometrics before a destructive action) |
| 9 | Sign out and delete account (password plus typing `DELETE`) |
| 10 | Project list, active project, probing a new instance, per-instance feature flags |
| 11 | Separate login per project |
| 12 | Full per-project isolation: node key, owner key, vault device copy, local repos, agent branch records, node-sync rows, prefs; the legacy namespace for the first project, `p/<id>/` for the others |
| 13 | Project runtime: every service is rebuilt on a switch rather than re-pointed; removing a project erases its own namespace |
| 14 | Quick project switcher (account menu, guest top bar, sign-in screens) |
| 15 | Projects card in Settings (add, test, edit, remove, switch) |
| 16 | All-networks catalog (repositories from every project side by side) |
| 17 | Per-project legal links (terms, privacy, membership) |
| 18 | Home shell: Code, Agents and Chat pages; Tools menu (Activity, Notes, Tasks, Calories, Vault, World, Desktop, Projects); bottom prompt bar; bell; avatar |
| 19 | Theme, accents, frosted surfaces, design kit |
| 20 | World (the `world-general` room plus the external World link) |
| 21 | Repository catalog (filters, sort) |
| 22 | Repository detail: code explorer, go-to-file, highlighted preview; commits; mirrors; releases; about; actions (runs and logs); agents; worktrees |
| 23 | Issues: filters, detail, timeline, sub-issues, subscribe, new issue, move to task ([issue-to-task](../app/issue-to-task.md)) |
| 24 | Pull requests: detail, files changed and commits, review, timeline, merge plan and merge (branch- and patch-backed) |
| 25 | Discussions, including federated replies |
| 26 | Branch diff and diff viewer (unified or side by side) |
| 27 | Quick edit over the API into a proposed change or PR |
| 28 | Code editor with key bar, and the highlighter |
| 29 | Local repositories: create, import zip, browse, edit, commit, publish, PR |
| 30 | Clone to device from the mesh |
| 31 | Mirror import (GitHub, GitLab, Codeberg) and import tokens |
| 32 | Signed inbox writes (issues, PRs, comments) |
| 33 | Bounties, funding and the donation wallet panels (read-only; no wallet keys) |
| 34 | Serve as a node (node-sync drain and applier) |
| 35 | Organizations: list, create, members, teams, repo links, delete |
| 36 | Chat: channels, roster, DMs (`@`), unread banner, link text |
| 37 | Room crypto and per-device seal |
| 38 | Chat persona: speak as the account or a stable "Guest ####", never as the node key |
| 39 | Local mesh room over the LAN (multicast beacon, TCP, subnet sweep) |
| 40 | Notes: list, search, editor, visibility, history, share |
| 41 | Tasks: create, check in, complete, reply |
| 42 | Calories: ring, charts, meals, goals, challenges, photo estimate |
| 43 | Password vault: fmv1 suite, unlock, reveal and copy with a 30 s clipboard clear, device-lock quick unlock |
| 44 | Activity: network stats, leaderboards, payouts, relay connection log |
| 45 | Sponsored ads (text only, can be turned off) |
| 46 | Pings inbox: filters, swipe delete, mark read, context sheet, "start agent from ping" |
| 47 | Ping settings screen: per-kind switches, pause, quiet hours, email, push card with test push, admin monitor alerts |
| 48 | Code-push pings (`codePushes` switch; a push opens the commit or branch) |
| 49 | Live account events (`/api/nodes/events` doorbell) and the agent-list doorbell (`agents`, `cloud-agents` topics) |
| 50 | APNs register, unregister, environment, test push, badge, pending tap |
| 51 | Notification service extension: `GET /api/push/alert`, portrait, badge, de-duplication; a direct message's sender portrait from `GET /api/accounts/<sender>`; chat and direct-message text decrypted on the phone from `GET …/pending?ref=` with the "Notification content" setting |
| 52 | Per-project push: one keychain item per project host; the extension picks the project named in the push; a tap switches project |
| 53 | In-app deep links from a ping (repo, tab, number, commit, branch) |
| 54 | Agents tab: one Agents list (each row marked Desktop or Cloud) and a New segment; session detail; sealed-session notice |
| 55 | Queue a run on a desktop or mirror; steer, retry, cancel (queued only); task-chain stages |
| 56 | Coding agent on the phone, using the user's own Anthropic or OpenAI key against a local repository |
| 57 | Model catalog (Opus 5.5 default; Opus 5, Sonnet 5, Haiku 4.5, Fable 5; GPT-5.6 Terra, Sol and Luna) |
| 58 | Provider-login terminal (pty streamed over the event socket) |
| 59 | Desktop on this network: pairing code, start, prompt, stop, continue, transcript, send a file |
| 60 | Agent Watch screen (field of bodies, faces, figures, dot display, sound list with the two switches) |
| 61 | Listen sonifier engine: motions, voices on the D-major pentatonic grid, marimba synth with ping-pong echo, cadences |
| 62 | Sound and haptics output (AVAudioEngine voice pool; Core Haptics pulses) |
| 63 | Announcer (sportscaster text-to-speech; desktop only, Piper) |
| 64 | On-device Gemma 4 (E2B/E4B) through MLX |
| 65 | One floating frosted composer for every agent prompt and chat: growing field, "+", model chip, send/stop, `/` commands, extra controls (dictation is the `/dictate` command); surfaces that cannot take photos hide "+" |
| 66 | Photo attachments: camera or library; downscale to 2048 px; JPEG q80 with all metadata stripped; sent as image blocks to the model |
| 67 | Dictation (the composer's `/dictate` command, and on iOS the shell prompt listening as it opens; listens only while it is on; nothing written to disk) |
| 68 | Image blocks through the providers and the session queue |
| 69 | User status and presence (away, emoji, text, expiry, dot) |
| 70 | Settings: profile, avatar, payout address, this device, serve as node, room passphrase, import tokens, orgs, membership, ads, appearance, legal |
| 71 | Telemetry screen and strip, log categories |
| 72 | Performance monitor, debug overlay, stall log |
| 73 | Bug report button and sheet (`POST /api/bug-reports`); admins' Reported tab with triage and attachments (`/api/bug-reports/inbox`) |
| 74 | Trust lists v1 resolver (core library) |
| 75 | Accessibility labels |
| 76 | Privacy manifest |

Not rows: localization (hard-coded English; iOS emits loc strings through
`SWIFT_EMIT_LOC_STRINGS=YES`), third-party analytics (none by design), and iOS
background work (no `BGTaskScheduler`; background delivery is APNs plus the
notification service extension; Android's background connection is in
[ANDROID.md](ANDROID.md), section 6). Calendar, CRM, the Market, a wallet,
widgets, the share extension and Live Activities belong to
[DESKTOP_PARITY.md](DESKTOP_PARITY.md) (WS12, WS15, WS19).

### 1.1 Theme rules

- **Contrast.** Every light-mode text colour clears WCAG AA (4.5:1) on
  `bgBase`, `bgRaised` and `bgOverlay` and inside its own 11% status-badge
  tint. `FmColors.swift` and `ForkMeshTheme.kt` carry the same values:
  `textSecondary` 72%, `textTertiary` 60%, `success` `#0F7A3B`, `warning`
  `#8F5A0A`, `offline` `#5F6672` (`offlineOnLight`, light only).
  `textDisabled` stays 25%. Accents suit buttons and larger text, not small
  captions.
- **Accents.** Twelve accents (blue, cyan, teal, green, lime, amber, orange,
  red, pink, violet, purple, graphite), stored by name in `appearance/accent`;
  unknown names fall back to blue. Light-mode accents are 600-700 shades under
  white labels; dark-mode accents are 300-400 shades under the near-black
  `onPrimary` label.
- **Background image (iOS).** Downscaled to 2800 px, saved as JPEG in
  Application Support `wallpapers/`, named in `appearance/backgroundImage`;
  `appearance/backgroundDim` (0 to 90%, default 40%) lays `bgBase` over it.
  `FmWallpaperBackdrop` draws it once behind `AppHomeView`. With a picture,
  `fm.canvas` is clear (otherwise `bgBase`); pages paint `fm.canvas`;
  `listSurface` drops to 60%; reading surfaces use `readingSurface` and
  `readingGutter`; terminals paint `terminalSurface` (terminal black at 80%).
  Bars, inputs, chips, sheets, cards and covers keep solid fills. Every pushed
  page gets the clear navigation container through `shellPageClearance()` or
  `shellDock` (iOS 18 and later). A conversation's own background wins over the
  app background. Android has the accents but not the background image.

## 2. Platform limits and decisions

### 2.1 Android features with a different iOS shape

| Item | iOS approach |
|---|---|
| Bluetooth mesh transport (RFCOMM on Android) | iOS offers no Bluetooth Classic to apps, so the iPhone uses a BLE GATT transport (`FMChat/Mesh/CoreBluetoothMeshTransport.swift`); the desktop (`BleMeshLink`) and Android (`AndroidMeshTransport`) dial it as BLE centrals. Wire format: [networking.md](../desktop/networking.md) |
| Vault autofill into other apps (Android `AutofillService`) | An AutoFill Credential Provider extension (`ForkMeshAutoFill`) reusing `NIC/FMVault` (WS21) |
| Wi-Fi multicast discovery | The subnet sweep works without the multicast entitlement, which Apple grants by request |
| Photo picking, re-encoding, dictation | First-party frameworks only: `PhotosPicker`, `UIImagePickerController(.camera)`, ImageIO (no metadata, 2048 px max), Speech and `AVAudioEngine` |

### 2.2 Limits the platform imposes

| Item | Decision |
|---|---|
| On-device model memory | E2B 4-bit needs about 2.6 GB resident, E4B about 4.2 GB. Without `com.apple.developer.kernel.increased-memory-limit` iOS kills the app; gate E4B on `os_proc_available_memory()` in `NIC/FMLocalAI` and keep E2B the default |
| No downloaded code | Nothing in the app executes downloaded code. MLX runs precompiled Metal kernels; model weights are data |
| Background execution | iOS suspends the app, stopping the event socket, node sync and LAN control. Behaviour is foreground-only, with APNs to wake |
| On-device agent on long tasks | `beginBackgroundTask` covers the tail of a turn; runs pause in the background |
| Agent-run approvals | Implemented for desktop agents over direct or relayed desktop control: `agentops.approvals`, one-use `prepare`/`answer`, phone authentication before approving, and a signed `agent_approval` ping. The desktop must be online; revocation and timeout fail closed |
| Stop away from the LAN | Desktop runs stop through relayed `agents.stop`; cloud runs through the durable stop route, which only the target node can execute |

## 3. iOS work breakdown

### 3.0 Workstream hooks

`NI/ForkMesh/App/AppServices.swift` is the one file every area registers
into, and no workstream edits it. Each hook forwards to an installer type that
conforms to `AppServicesInstaller` (`func install(into services: AppServices)`,
a no-op by default) and lives in its own file, owned by one workstream. Swift
cannot override a method from an extension of the same type, which is why the
hooks forward to separate types.

| Hook | Installer type | File (owner) |
|---|---|---|
| `installProjects()` | `ProjectsInstaller` | `NI/ForkMesh/App/AppServices+Projects.swift` (WS1) |
| `installPush()` | `PushInstaller` | `NI/ForkMesh/App/AppServices+Push.swift` (WS2) |
| `installComposer()` | `ComposerInstaller` | `NI/ForkMesh/App/AppServices+Composer.swift` (WS3) |
| `installAgentWatch()` | `AgentWatchInstaller` | `NI/ForkMesh/App/AppServices+AgentWatch.swift` (WS4) |
| `installWatchBridge()` | `WatchBridgeInstaller` | `NI/ForkMesh/App/AppServices+WatchBridge.swift` (watch bridge) |
| `installHosts()` | `HostsInstaller` | `NI/ForkMesh/App/AppServices+Hosts.swift` (WS7) |

`AppServices.start` calls `installProjects()` before `registerFeatures()`,
because the project scope decides the storage namespace, and the others after
`wire()`, in the order above, so they can extend or replace the base wiring.
DESKTOP_PARITY.md section 19 adds the WS8+ hooks.

### WS1: Projects and multi-instance isolation

Rows #10 to #17. `ProjectScope` (id, namespace, secure view, directory) and
`ProjectStorage` in `NIC/FMAccount/`; switching rebuilds the whole service
graph (`ForkMeshApp.swift`). The first project keeps the unprefixed keys, the
others use `p/<id>/`; key names stay byte-compatible with the Flutter app
(section 5). Each project has its own node key. Only a few per-project
preference keys go to the active project's namespace; everything else is
shared. Relay switching lives in `NIC/FMChat/Relay/`; the switcher is
`NI/ForkMesh/Kit/ProjectSwitcher.swift`.

### WS2: Push, pings and code-push

Rows #46 to #53. `NI/Shared/PushCredentialStore.swift` keeps one keychain item
per project host (row #52): one APNs token serves the whole phone, but each
project's instance holds its own registration, and the notification service
extension (`NI/NotificationService/`) picks the project by host. Phone side:
`ApplePushBridge.swift`, `NIC/FMApi/Services/PushNotificationService.swift`,
`NotificationDeepLink.swift`, the ping settings model and screen. Server:
`app/src/apns.py` and `register_apns_device_token`/`_apns_send_tokens` in
`app/src/entry.py` store and send a per-token topic.

### WS3: Composer, photos, dictation and model catalog

Rows #57 and #65 to #68. `NI/ForkMesh/Kit/PromptComposer.swift`,
`NI/ForkMesh/Platform/PromptPhotoPicker.swift`, `PromptDictation.swift`,
`NIC/FMAgents/AgentModelCatalog.swift`, `NIC/FMAgents/PromptAttachments.swift`
(size and strip rules, Linux-tested). The composer card lays `sheetSurface`
over the system blur (78% light, 72% dark); the shell backdrop behind it is 12%
black in light mode and 28% in dark (`ShellPromptBar.backdropOpacity`).

Dictation on iOS:

- The shell prompt starts dictation as it opens unless
  `voice/dictateOnOpen` (`SettingsService.dictatesOnOpen`, default on) is off.
  Other composers listen only on `/dictate`.
- Listening stops after 8 s with no words, 4 s of quiet after words, or two
  minutes; typing, sending, closing or tapping the "Listening" chip stops it at
  once. A late result never overwrites typed text or restores a sent prompt.
- The 4 s quiet stop sends the prompt (`PromptDictation` reports
  `onDone(true)` only then, and `PromptComposer` sends only if words were
  added this time).
- Refused microphone or speech permission is not re-asked on every open.
- The audio session is `playAndRecord` with `duckOthers`. Each start builds a
  fresh `AVAudioEngine` after the session switches, so the tap uses the
  current hardware format (a reused engine aborts on a format mismatch).
- iOS 26 uses on-device `SpeechAnalyzer` with `SpeechTranscriber`
  (`PromptSpeechAnalysis`); its model is installed through `AssetInventory`
  after launch and shown in Voice settings. Otherwise `SFSpeechRecognizer`.
  Buffers are converted in memory (`PromptAnalyzerFeed`); nothing is written
  to disk.

### WS4: Agent Watch and Listen on the iPhone

Rows #60 to #62. Module `NIC/FMAgentWatch/` (`AgentMotion`, `AgentVoice`,
`AgentSynth`, `AgentFeel`, `AgentWatchController`, `AgentWatchBeats`), FMBase
only, so it builds on watchOS and Linux; golden PCM and note tests in
`FMAgentWatchTests`. Screens in `NI/ForkMesh/Features/AgentWatch/`; output
(AVAudioEngine plus Core Haptics) in `NI/ForkMesh/Platform/AgentWatchOutput.swift`.

### WS5: Serve as a node, and the Git engine

Row #34. `NIC/FMChat/NodeSync/*`, `NIC/FMCode/Local/*` (per-project roots
from `ProjectScope`), `NIC/FMGit/*`, `NI/ForkMesh/Features/LocalRepos/*`.

### WS6: watchOS app and build pipeline

Section 4. `NI/ForkMeshWatch/**`, `NI/ForkMeshWatchWidgets/**`,
`NIC/FMWatchWire/`, `NI/ForkMesh/Platform/WatchBridge.swift`, the watch
plists and entitlements, `generate_project.rb`, `asc_bundle_ids.py` and the
native section of `codemagic.yaml`.

### WS7: Hosts, live host logs, and agents over the relay

- **One channel, two transports.** The LAN local control channel
  (`desktop/src/LocalControl.h`) also rides the relay: the phone holds a
  browser-role account event socket, the desktop its node-role socket, and the
  per-owner `ForkMeshNodes` object forwards `{"type":"control"}` chunks
  (`app/src/node_control.py`, advertised as `relays: ["terminal", "control"]`,
  metered on its own window). The same hello, welcome, signed transcripts and
  AES-256-GCM frames run inside, so the relay sees only ciphertext. A phone
  signed in to the desktop's account is approved on either transport without a
  code; any other phone is approved only on the LAN (`pair_on_lan` on the
  relay), and the phone accepts only the desktop key it paired with.
- **Hosts** (`hosts.list|detail|options|prepare|run|logs.start|logs.ack|logs.stop`,
  `MainWindowLocalControlHosts.cpp`): owner-only. Every change needs a one-use,
  one-minute token from `hosts.prepare` bound to device, action and host
  (destroy also needs the typed host name) and is logged on the desktop. The
  operations reuse the Hosts page's own paths; keys never leave the desktop.
  Logs tap the desktop's host log feed in acknowledged batches (window 4,
  2000-line buffer, drops counted).
- **Host SSH terminal and binary install.** A host row's `actions` lists
  `installBinary` and, except on Windows, `terminal`. `hosts.run` with
  `action: "terminal"` (plus `cols`/`rows`) starts `ssh -tt user@host` in a pty
  on the desktop with the saved key or password, ForkMesh known_hosts in
  accept-new mode and `BatchMode`. The phone attaches with the
  `terminal.attach|input|resize|detach|close` methods and
  `terminal.output`/`terminal.exit` events; `terminal.start` refuses `ssh`, so
  a shell only opens through a fresh confirmation. Mode `"ssh"` keeps the
  session out of the agents list; `terminal.list` filtered by `host` lets a
  phone rejoin. The desktop caps terminals at six.
- **Agents.** `agents.*` works over the relay unchanged; `agents.transcript`
  takes `since` for live deltas. Node runs come from the relay queue
  (`GET /api/account/agent-sessions`); `POST .../agent-sessions/<sid>
  {"action":"stop"}` queues a `kind:"stop"` job.
- Code: `NIC/FMControl/` (`DesktopControlLink`, `RelayControlConnection`,
  `DesktopControlHub`, `AgentControl`), `NIC/FMHosts/`,
  `ApiService+RemoteControl.swift`, `NI/ForkMesh/Features/Hosts/**`,
  `NI/ForkMesh/Features/Agents/AllAgents/**`.

## 4. watchOS app: control agents, see and hear them

### 4.1 Agent data

| Data or action | Source | Watch use |
|---|---|---|
| Sessions (status, provider, model, branch, turns, tokens, cost, files, ahead/behind, PR) | `NIC/FMApi/Models/Agents.swift` (`AgentSession`); relay `POST /api/repo/<o>/<r>/agents/list` (owner-sealed) | List rows, complications, Smart Stack |
| Transcripts (sealed per owner device: X25519 + ML-KEM-768 wrap, AES-256-GCM body) | `ApiService+Agents.swift`, `NIC/FMCrypto/OwnerCrypto.swift` | Beats for the sonifier |
| Change doorbells | Account event topics `agents`, `cloud-agents`; LAN `agents.changed` | Refresh; nothing polls |
| Relay controls | Launch, steer, retry, cancel (queued only) | Prompt and retry |
| Desktop controls | `agents.list`, `start`, `prompt`, `stop`, `continue`, `transcript`, `handoff`, `takeover` | Stop and continue |
| Listen sonifier | `FMAgentWatch` (Android `C/agentwatch`) | Plays on the watch |

There is no Pause; the watch shows Stop and Continue.

### 4.2 Architecture

**Option A, WatchConnectivity bridge (built).** The iPhone app is the only
ForkMesh client. It unseals transcripts, turns them into compact beat lists
(`FMWatchWire.AgentFrame`: id, title, repo, status, model face, the latest 64
beats, turn and token counts) and sends them with `WCSession.sendMessage`
(while reachable), `updateApplicationContext` (latest list) and
`transferCurrentComplicationUserInfo`. Watch commands travel back as messages:
`stop`, `continue`, `prompt(text)`, `retry`, `listen(on/off)`, `diff`,
`start(text, repo)`. iOS wakes the phone app to handle them.

Reasons: the session and keys live in the phone's device-only keychain, which
does not reach the watch; transcripts are sealed per enrolled owner device;
watchOS restricts WebSockets and raw sockets (TN3135), so the event doorbell
and LAN control cannot run there; and the watch only renders, synthesises and
plays haptics.

**Option B, standalone networking (phase 2).** Its own login through a code
shown on the phone, its own Ed25519 node key and owner enrolment, HTTP reads
only on the WidgetKit budget. Needs FMNet (HTTP only), FMCrypto, FMAccount and
FMApi on watchOS, and flips `WKRunsIndependentlyOfCompanionApp` to true.

### 4.3 Screens and behaviour

- **Agent list**: one row per active or recent session (model face, title,
  repo, status dot, turns and tokens gauge), running first, then by recency.
- **Live view**: the agent's body from the Watch field in `Canvas` and
  `TimelineView(.animation)`, coloured by instrument, with recent marks and the
  last reply on the dot display.
- **Controls**: Stop (desktop `agents.stop` or relay cancel for queued runs),
  Continue (`agents.continue` or relay retry), Prompt (dictation or Scribble,
  limited to the relay's maximum length), Review changes (4.3.1).
- **Haptics** via `WKInterfaceDevice.current().play(_:)` (Core Haptics is not on
  watchOS): tool call `.click`, file write `.directionUp`, error `.failure`,
  run done `.success`, needs attention `.notification`; at most one per 0.6 s.
- **Sound**: the FMAgentWatch synth renders PCM on the watch; `AVAudioEngine`
  with `.playback`. Continuous Listen uses `routeSharingPolicy =
  .longFormAudio` and the `audio` background mode. Listen and per-agent mute
  are in settings.
- **Complications and Smart Stack** (watch widget extension): circular,
  rectangular, inline and corner families reading the last
  `ComplicationSummary` from the watch's keychain group; relevance rises while
  a run is active or just finished; tapping deep-links to the live view.
- **Notifications**: phone notifications mirror to the watch.

#### 4.3.1 Reviewing a diff on the watch

The watch sends `WatchCommand.diff(key:)`; the phone answers with a `diff`
envelope holding a `WatchDiff`, so one round trip carries the whole review. An
older iPhone app answers "Update ForkMesh on your iPhone".

| Frame | Read with | Notes |
|---|---|---|
| Desktop agent | `AgentOpsClient.diff` over the desktop control hub | Committed, uncommitted and untracked |
| Queued node run with a branch | `ApiService.runnerAgentDiff`, first 100 files | Committed only |

`WireControls.diff` is set only when one of those routes exists.
`AgentWatchDiff.make` (FMAgents) parses the patch with FMDiff into
`WatchDiffFile`s (path, old path, status, group, counts, lines; hunk headers as
`@` lines carrying the first new line number). FMDiff stays off the watch.

A WCSession reply has a ~64 KB ceiling, so `WatchWire.fitted(_: WatchDiff)`
cuts lines to 160 characters and files to 600 lines, then shrinks every file to
300, 160, 80, 40, 16 and 0 lines, then drops files from the end (binary
search). Cuts are counted (`hiddenLines`, `hiddenFiles`) and shown.

The watch lists branch, base, totals and "n of m reviewed", files grouped
Committed, Uncommitted, Untracked; a file shows wrapped monospaced lines.
Reviewed marks are local (`UserDefaults`, newest 600), keyed by agent, branch,
group, path and line counts, so a re-changed file comes back unreviewed. They
are separate from the phone's viewed marks ([DIFF_REVIEW.md](DIFF_REVIEW.md)).

#### 4.3.2 Starting an agent with Siri

The watch app has its own `NewAgentIntent` (`ForkMeshWatch/NewAgentIntent.swift`)
and `AppShortcutsProvider`, since Siri on the watch runs the watch app's
intents. User guide: `www/docs/mobile/siri.md`.

1. Siri asks for the prompt and reads it back for confirmation.
2. The intent opens the watch app (`openAppWhenRun`);
   `WatchStore.connected(within:)` waits up to 6 s for a reachable session.
3. The watch sends `WatchCommand.start(text:repo:)`.
4. `WatchBridge.start` runs the composer's `QuickAgentLauncher` with the
   repository, destination and model last picked on the phone; it refuses while
   the composer is sending.
5. `QuickAgentLauncher.send(prompt:repository:onDevice:)` (shared with the
   Genie) resolves a spoken repository by key, `owner/name`, public owner or
   bare name. With `onDevice: false` a run destined for this iPhone is refused.
6. The reply is `GenieFeature.outcomeLine`; the bridge refreshes so the run is
   listed before Siri answers.

### 4.4 Sharing ForkMeshCore

`Package.swift` declares iOS, macOS and `.watchOS(.v10)`, and the product
`ForkMeshWatchCore` (`FMBase`, `FMAgentWatch`, `FMWatchWire`). The watch links
only those.

| Module | On watch? |
|---|---|
| FMBase, FMAgentWatch, FMWatchWire | Yes (FMBase-only dependencies) |
| FMCrypto, FMNet, FMAccount, FMApi | Phase 2 only |
| FMChat, FMCode, FMGit, FMDiff, FMAgents, FMLocalAI, FMTools, FMVault | No: sockets, Git and MLX are phone only |

### 4.5 Project generator (`native/ios/tools/generate_project.rb`)

The project is generated by a Ruby `xcodeproj` script.

- New targets are created after `REMOTE_PACKAGES`, so the deterministic `F0…`
  object ids of existing objects do not move.
- `ForkMeshWatch` (`:application`, `SDKROOT=watchos`,
  `WATCHOS_DEPLOYMENT_TARGET=10.0`, `TARGETED_DEVICE_FAMILY=4`, bundle
  `com.forkmesh.forkmesh.watchkitapp`, `WKCompanionAppBundleIdentifier=com.forkmesh.forkmesh`,
  `UIBackgroundModes=[audio]`), linking `ForkMeshWatchCore`.
- `ForkMeshWatchWidgets` (`com.apple.widgetkit-extension`, bundle
  `com.forkmesh.forkmesh.watchkitapp.widgets`), embedded in the watch app's
  `PlugIns`.
- The watch app is embedded in the iOS app under
  `$(CONTENTS_FOLDER_PATH)/Watch`.
- Both watch targets share the keychain group
  `$(AppIdentifierPrefix)com.forkmesh.forkmesh.watchkitapp`; no `aps-environment`
  (notifications are mirrored).
- A shared `ForkMeshWatch` scheme; MLX stays linked into the app target only.

### 4.6 Codemagic, signing and App Store Connect

- Bundle ids `com.forkmesh.forkmesh.watchkitapp` and
  `com.forkmesh.forkmesh.watchkitapp.widgets` are in `BUNDLE_IDS` of
  `native/ios/tools/asc_bundle_ids.py`.
- The App Store Connect API cannot create or assign App Groups, so the watch
  shares data through its keychain group and ships in every build
  ([IOS_RELEASE.md](IOS_RELEASE.md), "Widgets, Live Activities and the watch").
- `&native_embed_entitlements` signs nested code innermost first (watch
  widgets, watch app, then the phone extensions and app) and fails if
  `Watch/ForkMeshWatch.app` is missing.
- `ios-native-build` also builds `-scheme ForkMeshWatch` for the watchOS
  simulator (downloading the platform if needed).
- The watch app ships inside the iOS IPA with the same archive, export,
  upload, app record and `Version.xcconfig`. App Store Connect needs watch
  screenshots and a 1024 px watch icon.
- Ad hoc builds need Apple Watch UDIDs registered as devices (platform IOS,
  class `APPLE_WATCH`).

## 5. Bundle ids and the move from the Flutter app

### 5.1 Bundle ids

| Platform | Ids |
|---|---|
| iOS | `com.forkmesh.forkmesh`, `.NotificationService`, `.Share`, `.AutoFill`, `.Widgets`, `.watchkitapp`, `.watchkitapp.widgets`; keychain group `742ZANCB57.com.forkmesh.forkmesh`; secure-store service `com.forkmesh.forkmesh.secure`; push items `com.forkmesh.forkmesh.push` |
| Android | `com.forkmesh.forkmesh` for the release APK (`-Pforkmesh.appId`, passed by `tools/forkmesh-release-host.py`); `com.forkmesh.nativeapp` by default (`native/android/app/build.gradle.kts`), which installs beside it |

The native iOS app is the App Store app ForkMesh from 1.4.0 on, shipped as an
update over the retired Flutter builds. Release steps:
[IOS_RELEASE.md](IOS_RELEASE.md).

### 5.2 Shipping native as an update under `com.forkmesh.forkmesh`

Reusing the id keeps the listing, ratings, TestFlight groups, installed users,
the keychain and the app container, and matches the server's APNs topic.

- **APNs topic per token** (`apns_device_tokens.bundle_id`): the app sends
  `bundleId` on registration; `apns.normalize_bundle_id` allowlists it and
  `apns_topic_for` sends under it.
- **Ids** are set in `APP_BUNDLE_ID` (`generate_project.rb`,
  `asc_bundle_ids.py`), the entitlements' keychain groups, and
  `PushCredentialStore` (the per-host item names the Flutter app wrote).
- **Version** stays above every Flutter version; `ios-native-release` takes one
  build past App Store Connect's highest.
- **One-time data migration** (`FMAccount/FlutterMigration.swift`, run by
  `ForkMesh/App/FlutterMigrationRunner.swift` before any store is built; tests
  `FlutterMigrationTests`) copies the Flutter keychain items and `flutter.*`
  preferences under the same names (`identity/ed25519Seed`,
  `ownerCrypto/hybridV1`, `auth/session/<projectId>`, `vault/<account>/…`,
  `chat/e2ee/…`, `agent/credentials-v1`, `p/<projectId>/` prefixes). Rules:
  [IOS_RELEASE.md](IOS_RELEASE.md), "Flutter-to-native migration".
- The push token survives the update; the app re-registers on launch for every
  project with a session.
- URL scheme `forkmesh://`; routes and universal links in
  [NAVIGATION.md](NAVIGATION.md).
- Android makes the same move: the release APK is `com.forkmesh.forkmesh`,
  signed with the same key as the Flutter APK, and migrates the Flutter
  preferences and secure storage ([ANDROID.md](ANDROID.md), section 4).
