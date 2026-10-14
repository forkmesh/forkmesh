# Desktop agents on the phone

Part of [the native docs](README.md). Both phone apps list the owner's
desktop agents next to node runs and manage them over the local control
channel: on the same network directly, away from home through the relay,
encrypted end to end either way ([desktop networking](../desktop/networking.md),
"Phone control over the LAN"; [PARITY.md](PARITY.md) WS7). The website's
Agents tab uses the same channel ([desktop-agents-web.md](../app/desktop-agents-web.md)).

**Release boundary.** Relayed control of a desktop learned through LAN pairing,
including agent approvals, start, prompt, stop, continue and merge, is in the
signed v1.2.5 desktop and Android release. Automatic discovery of same-account
desktops, plus the phone Changes and Delete surfaces, is current `main` from
2026-09-28 and is not in that signed release.

## Reaching a desktop without meeting it on the LAN

Until 2026-09-28 a phone could use the relay only to reach a desktop it had
paired with on the same network, because the pairing record was the only place
it learned the desktop's key and machine name. The Agents list said "Pair a
desktop on your network first" otherwise.

Now `DesktopControlHub` (iOS `FMControl/DesktopControlHub.swift`, Android
`control/DesktopControlHub.kt`) also reads the account's desktops from
`GET /api/accounts/desktops` (`_account_desktops_handler` in
`app/src/entry.py`): `{account, desktops: [{key, node, lastSeen}]}`, one row
per enabled `desktop_node` device key (the node is the device label, which is
the desktop's `machineNodeName()`), plus the account's primary key. The list
is cached for ten minutes.

- Candidates, in order: the desktop connected on the LAN, LAN-paired desktops
  (newest first), then the account's desktops. The one that answered last is
  tried first next time.
- `link()` tries up to four candidates through the relay and keeps the first
  that completes the handshake; the welcome must carry the expected key.
- The desktop approves the phone without a code because the phone's key is an
  enabled `owner_sign` device of the same account (`checkLocalControlAccountDevice`).
- That first approval remembers the phone in the desktop's
  `data/local_control/paired-devices.json` as `"approval": "account"`. Since
  2026-10-01 the desktop asks the account again on every connect and every ten
  minutes, and drops the phone once it is revoked
  ([networking](../desktop/networking.md), "Relayed control: reliability and
  revocation").
- A relayed channel is pinged after 30 seconds of silence and before it is
  reused after 45 seconds idle; a dead one is closed and redialled, and a
  desktop that no longer knows the channel answers `channel_gone`.
- `DesktopReach.notPaired` now means the account has no desktop at all:
  "Sign in to ForkMesh on your desktop with this account".

## What is running on the desktop, on the Agents tab

Since 2026-09-28 the phone's main Agents tab shows what the desktop is running
right now. Before, the tab listed only sessions the desktop had pushed to the
relay and sealed to this phone, and agent CLIs started outside ForkMesh (a
Claude Code or Codex in a terminal) were visible only in the desktop's Watch.
The phone showed "Nothing running on your desktop" while the desktop was busy.

- **Desktop.** `agents.list` (`localControlAgentsPayload` in
  `MainWindowLocalControl.cpp`) now also answers `outside`: one row per agent
  CLI running on the machine that ForkMesh did not start, as `{id, kind,
  label, provider, model, title, cwd, branchName, repositoryOwner,
  repositoryName, lastActivityMs}`. It holds the live external Claude Code
  rows the Agents tab surfaces and the `/proc` scan's machine agents
  (`m_machineAgents`, [agents-ui.md](../desktop/agents-ui.md), Watch). A
  machine agent whose folder is inside an opened repository carries that
  repository. `status.capabilities` gains `agents.outside`.
- The `/proc` scan used to run only while Watch showed or someone listened;
  `tickMachineAgents()` now also runs it while a phone or browser is
  connected to local control, and a change in the set rings `agents.changed`,
  so the phone's list follows agents as they start and exit.
- **Phones.** `LocalAgentsSnapshot.outside` (`LocalOutsideAgent`) becomes a
  `UnifiedAgentRow` with `outside = true` (`fromOutside`, id
  `outside:<desktop id>`): running, with no stop, prompt, continue or desktop
  actions. Opening it showed a note in place of the transcript ("Codex was
  started outside ForkMesh on studio, in /home/…"); since 2026-09-29 it shows
  the agent's own transcript, live
  ([below](#following-and-watching-an-agent-started-outside-forkmesh)). Stop,
  prompt and continue on such a row throw `unsupported` without asking the
  desktop, except that since 2026-09-30 a Claude Code row the desktop marks
  `steer` takes a prompt
  ([below](#steering-a-claude-code-started-outside-forkmesh)).
- **Agents tab (Android).** `desktopAgents(rows, shown, idle: 20)` picks the
  control center's desktop rows that the pushed list does not already show
  (matched by repository name and desktop id) plus every outside row: all the
  running ones, then the 20 most recently updated idle or finished ones. They
  sit under "On <desktop>" at the top of the Agents and Desktop tabs and open
  the All agents run screen. Only the running ones count in the header's
  "running" and the Desktop tab. When the desktop could not be reached and
  nothing is listed, the section says why ("Your desktop's agents could not
  be loaded: …", the control center's `desktopNote`). iOS lists every control
  center row in its merged list since 2026-09-29 (below), idle ones included,
  and shows `desktopNote` under it. The tab refreshes the control center when
  it opens and on pull to refresh.

### Idle desktop agents and organization repositories (2026-09-29)

Until 2026-09-29 both phones kept only running rows (`runningOnDesktop`), so
an agent that had finished its turn vanished from the phone. The pushed list
did not cover the gap for organization repositories: the desktop seals and
pushes session snapshots only for repositories whose owner it holds a signing
key for (`ensureAgentE2EEControlPlane`, `hasOwnerSigningCapability`), and the
phone reads snapshots only for `GET /api/repositories`, the account's own
repositories. An owner working in `forkmesh/forkmesh` therefore saw no desktop
agents at all between turns, and a failed desktop link left the tab
silently empty. The local control list covers every repository the desktop has
open, so both phones now show its idle rows too and report a failed link.

## Following and watching an agent started outside ForkMesh

Added 2026-09-29. A Claude Code or Codex started in a terminal on the desktop
was listed on the phone but could not be followed: its run screen held only a
note, and the phone Watch had no body for it. Now the phone reads its
transcript over the same control channel (LAN or relay) and follows it as it
works.

- **Desktop.** `agents.transcript` with a negative `id` (an outside agent's
  id from `agents.list`) is answered by `answerLocalControlOutsideTranscript`
  (`MainWindowLocalControl.cpp`) from the agent's own transcript file: a live
  surfaced external Claude Code row's `.jsonl`, or the machine agent's
  `MachineAgent::transcript`. The answer has the same shape as a ForkMesh
  run's (`transcript`, `from`, `next`, `reset`, `active: true`, `status:
  "running"`, `attachments: []`) plus `outside: true`, so `since` works the
  same way.
- **Rendering.** `AgentLogText` (`src/AgentLogText.*`) turns each JSONL entry
  into the lines the desktop writes to its own `transcript.txt`: a typed
  prompt as `> `, a reply as prose, a tool call as `[Tool arg]`, a compaction
  as `==> Conversation compacted`, a background task's completion as a
  `==> Task <status>: <summary>` line with its result indented (2026-09-29,
  below). Tool results, subagent (sidechain) and meta
  entries, and the CLIs' injected messages (slash-command echoes, system
  reminders, Codex's environment context) are left out. For Codex it reads
  the rollout's `event_msg` `user_message` for prompts and `response_item`
  messages and function, shell and patch calls for the rest, and the older
  unwrapped rollout lines. `applyTranscriptEvent` now uses the same
  `AgentLogText::assistantContent` for ForkMesh's own Claude runs, so both
  logs read alike. The log starts with `==> <CLI> started outside ForkMesh, in
  <folder>`.
- **Codex transcripts.** The `/proc` scan now finds a Codex process's rollout
  (`~/.codex/sessions/…/rollout-*.jsonl`) as the file it or a child holds open
  (`MachineAgentScan::openCodexRollout`). Other CLIs still have no transcript;
  the desktop answers `no_transcript`.
- **Tail.** The desktop keeps one tail per outside agent a phone reads
  (`m_localControlOutsideTails`): the file, the byte offset read so far and
  the rendered log. The first read starts at most 2 MB from the end of the
  file on a line boundary; later reads parse only the new lines, off the UI
  thread. The log keeps the newest 128k characters (`base` keeps offsets
  stable). A different file for the same id (Claude Code's `/clear` starts a
  new one) or a shorter file starts a new log, answered with `reset`.
- **Bell.** While a phone or browser is connected, the 2 s machine-agent tick
  (`ringLocalControlOutsideTranscripts`) checks the size of each followed
  transcript and rings `agents.changed` when one grew, so the phone reads the
  new lines as it does for a ForkMesh run. Tails of agents that exited are
  dropped, and all of them when the last phone disconnects.
  `status.capabilities` gains `agents.outside.transcript`.
- **Phones.** `UnifiedAgentRow.fromOutside` now carries the desktop's id in
  `localId` (negative; the diff, merge and delete actions still need a
  positive one). `transcript` asks `agents.transcript` for it; `unknown_session`,
  `no_transcript` or `unsupported` (an older desktop, a CLI without a
  transcript) fall back to the note, which now says the desktop cannot show
  its transcript. An agent that exits while followed (`unknown_session` with
  `since` > 0) ends the run as done without clearing what was shown. The All
  agents run screen (iOS `AgentRunScreen`, Android `UnifiedAgentScreen`)
  follows it like any desktop run: on each `agents.changed` on iOS, every 3 s
  while active on Android. Since 2026-09-30 both stream it instead
  (`agents.follow`, below).
- **Watch.** Android's "On <desktop>" header has **Watch** (the running rows,
  or the last three to replay when none runs), and the
  run screen on Android has a waveform button: both open the Watch field
  on those rows. The iOS run screen dropped its waveform button on 2026-10-01
  to make room for Merge & clean up and Delete; the tab bar's waveform still
  watches the run. On iOS, whose Agents tab became one merged list the same day
  (below), the tab bar's waveform (`AgentWatchScreen.forAgentsTab`) now also
  watches the running control rows the list shows, next to the running relay
  sessions, rung by both the account's `agents` topic and the hub's
  `agents.changed` (`AgentWatchTrigger.all`); with no such row it is
  `forRelay` as before. iOS gained `controlWatchTarget` and
  `AgentWatchScreen.forControl`, which Android already had
  (`controlWatchTarget`, `AgentWatchEntry.openControl`). An outside agent's
  beats come from the same transcript, so its body moves, plays and speaks
  like a ForkMesh run.

### A prompt sent to a terminal session froze the run screen

Fixed 2026-10-14. A prompt typed from the phone into a terminal Claude Code
reached it, but the run screen stopped moving until the user went back to the
list and opened the row again. The row changed id under the open screen. An
idle terminal Claude Code is a machine agent (id from `pid:<pid>`). The prompt
made it write its `.jsonl`, the desktop's external scan of the open repository
(sessions written within 90 s) surfaced it under a new id from its uuid, and
`findSessionPids` fell back to the folder and hid every Claude Code in that
folder. The follow on the old id answered `unknown_session` and the phone
ended it. Ninety seconds after the last write the surfaced row stopped
counting as live and the machine agents came back, under new ids again.

- **One id per session.** `registerExternalSession` gives a newly surfaced
  session the id of the machine agent already showing it (Claude's
  `sessions/<pid>.json` names its uuid, or the agent's transcript is the
  same file). `applyMachineAgents` keeps that pid's id while the session is
  surfaced, so the machine agent gets the same id back if it reappears, and
  `agents.list` lists no machine agent whose id is a live surfaced session.
- **Live while its process runs.** `externalIsLive` also counts a session
  whose Claude record (`m_claudePeerSessions`) names its uuid, so a surfaced
  terminal session stays one running row while it waits for the next prompt.
- **Exact processes and files.** A surfaced session's processes are the ones
  whose Claude record names it; the folder fallback only runs for a session
  with no record and skips processes whose record names another session.
  `MachineAgentScan` reads a Claude Code's transcript from its record
  (`recordedClaudeSession`) before falling back to `--resume` or the newest
  unclaimed `.jsonl` in the folder. That guess gave several Claude Codes in
  the same folder each other's transcripts, and they swapped whenever another
  one wrote.
- **Phone.** iOS `AgentRunScreen` follows the run again after a prompt it
  sent when its follow has ended, so the reply streams in without reopening.

## Agents of other users on the machine

Fixed 2026-09-30. The phone showed "Claude Code started outside ForkMesh on
forkmesh. You can watch it here; steer it in the terminal that started it."
with no folder and "The desktop cannot show its transcript", while every
terminal Claude Code of the desktop's user could be watched and steered. The
row was the Claude desktop app's Claude Code running in another login on the
same machine (user `b`, home `0700`), while ForkMesh runs as `f`. The `/proc`
scan listed every user's processes, but another user's `cwd`, `environ`,
config folder, session record and `cc-socks` inbox cannot be read, so the row
could never show a folder, a transcript or take a steer, and the note blamed
the terminal.

Such agents stay listed, with a note. `MachineAgentScan::processUid` reads the
real uid (`Uid:` in `/proc/<pid>/status`); a process of another uid gets
`MachineAgent::otherUser` (its login name from `getpwuid_r`, else
`user <uid>`) and nothing else is read from it. In `agents.list` its
`outside` row carries `"otherUser": "<login>"`, the title `Claude Code (b)`,
and no `steer`, `typeable` or `stoppable`; `agents.prompt` and `agents.stop` on
it answer `outside_other_user`. Watch on the desktop shows "Claude Code,
running in b's login", and clicking it explains that ForkMesh cannot read or
steer another login's agents.

Phones read `otherUser` into `LocalOutsideAgent.otherUser` and
`UnifiedAgentRow.outsideUser`. The run screen's summary reads "Claude Code is
running in b's login on <desktop>. ForkMesh cannot read or steer another
login's agents; open ForkMesh in that login to follow it.", the transcript
note says the same at more length, the Android list line reads "Claude Code ·
in b's login", Siri answers with the summary, and `ControlErrorText` has a
sentence for `outside_other_user`. An older phone shows the title and the old
read-only note. Tests: desktop `test_machine_agent_scan.cpp` (`processUid`,
`userName`, pid 1 scanned as root's agent), iOS
`testAnAgentInAnotherLoginIsListedWithANote`, Android `LocalControlTest`
`testAnAgentInAnotherLoginIsListedWithANote`.

## Steering a Claude Code started outside ForkMesh

Added 2026-09-30. Following an outside agent (above) was read-only: the phone
said "follow and steer it in the terminal that started it". A Claude Code
started in a terminal on the desktop can now be steered from the phone, on
the run screen's composer or by Siri ("Tell an agent"), over LAN or relay.

### How Claude Code sessions talk to each other

Found by reading the 2.1.285 CLI bundle and confirmed on two throwaway
sessions (a stream-json pair and an interactive one in a pty), with `strace`
on the receiver's socket reads. Nothing was sent to a real session.

- **Discovery.** Every running Claude Code (interactive `cli` or `sdk-cli`)
  writes `<config>/sessions/<pid>.json`, where `<config>` is its
  `CLAUDE_CONFIG_DIR` or `~/.claude`: `pid`, `sessionId`, `cwd`, `name`,
  `status` (`idle`/`busy`), `procStart` (field 22 of `/proc/<pid>/stat`, so a
  recycled pid is caught), `peerProtocol: 1`, `peerFeatures` and
  `messagingSocketPath` (`$XDG_RUNTIME_DIR/cc-socks/<pid>.sock`). A record
  outlives a crashed process, so the pid, its start time and the socket file
  are all checked.
- **Auth.** Beside it, `<pid>.<sha256 of the socket path>.key` (mode 0600)
  holds `{"peerToken": <32 hex>}`. A client sends `{"type":"auth","token":…}`
  as its first line. Linux 2.1.285 logs "auth line optional here", but the
  token is honoured and later versions or other platforms may require it, so
  ForkMesh always sends it when the key is readable.
- **Frame.** Newline-delimited JSON on one connection, written at once, then
  the write side is closed; there is no answer.
  `{"msgV":1,"msg_id":<uuid v4>,"type":"user","message":{"role":"user","content":<envelope>},"priority":"next"}`,
  plus `"from":"uds:<own socket>"` from a session that has an inbox of its
  own. The envelope is
  `<cross-session-message from="…" from-name="…" from-mode="bypass|prompting">\n<body>\n</cross-session-message>`;
  a `<` that could start a closing tag in the body is written `<\`, as the
  CLI's own sender does, or the receiver's round-trip check drops the
  attributes. A line may be 1 MiB; the connection is closed if no full line
  arrives in 30 s.
- **Delivery.** The receiver queues it as a prompt (`isMeta`, `origin.kind:
  "peer"`): a new turn when idle, folded in at the next tool round when busy.
  The model sees "Another Claude session sent a message: <envelope> … not
  typed by your user, but very likely working on their behalf …", and the
  terminal shows `ForkMesh-phone: <text>`. There is no way to make it the
  user's own prompt, so a model may weigh a steer below the user's last typed
  instruction; in the tests it usually followed it, and once declined.
- **Hold policy** (`w()`/`k()` in the bundle). A peer message is queued,
  held for approval in the terminal, or refused:
  1. `crossSessionInbound` (`accept`, `hold` or `refuse`) decides when it is
     set. The first value found in managed settings
     (`/etc/claude-code/managed-settings.json`, on macOS
     `/Library/Application Support/ClaudeCode/managed-settings.json`), then
     `--settings` (JSON or a file), then the user settings
     `<config dir>/settings.json` wins. The config dir is the session's
     `CLAUDE_CONFIG_DIR`, else `~/.claude`. The repository files
     (`<cwd>/.claude/settings.json`, `.claude/settings.local.json`, the local
     one also at the git root) can only tighten it, to `hold` or `refuse`,
     never loosen it. An invalid value holds (in managed settings, refuses).
  2. Unset, it compares the envelope's `from-mode` with the receiver's
     permission-mode class (`bypass` or `prompting`). A mismatch is held, and
     a sender that claims no class is held when the receiver bypasses
     permission prompts and accepted when it prompts.
  3. A held message waits in the terminal for Deny or Deliver ("Held message
     from another session … The sender did not attest its permission mode,
     and this session bypasses permission prompts"); a headless session
     (`sdk-cli`) has no approval surface and drops it.
  The setting is per config dir (every session using that dir), not per
  session, and it is read live: the CLI watches its settings files and a
  change applies to the next message, with no restart ("Detected change to
  …/.claude/settings.local.json", then `cause=repo-setting` on the next
  message, in a test).
- **ForkMesh claims no mode.** `from-mode` exists so the receiver can refuse
  laundering permissions from a less trusted session; a claim ForkMesh does
  not hold would defeat it. The envelope carries only
  `from-name="ForkMesh (phone)"`, which the protocol offers (its values are
  only `bypass` and `prompting`, with nothing for a sender that is not a model
  session). A prompting session takes the steer at once. A bypass session
  holds it unless its user opts in with `"crossSessionInbound": "accept"` in
  `<config dir>/settings.json`. The desktop never writes that setting; the
  user sets it. (A first version on 2026-09-30 claimed the receiver's own
  class to get past the hold; that was reverted before release.)
- **No interrupt.** The only control actions are `rename`, message receipts,
  idle notices and artifact hand-offs. `priority: "now"` did not abort a
  running tool call in either mode; it was folded in at the next tool round
  like `next`. SIGINT to the process can end the CLI. So Stop stays
  unsupported over the peer path; a steer such as "stop and wait" is the
  closest thing, and the model may decline it. A session started under
  `forkmesh term` can be stopped: see "Typing into a terminal from the
  phone" below.

### Desktop

- `ClaudePeerMessaging` (`src/ClaudePeerMessaging.*`): the record scan
  (`readSessions`, `liveSessions` over `~/.claude`, every
  `agent-accounts/claude/<account>` and each scanned process's own config
  folder), `keyPath`/`peerToken`, `permissionClass`, the envelope and a byte
  exact frame builder (Qt's JSON sorts keys, so the lines are written by
  hand), and `send`, which refuses a symlink, checks the connected
  endpoint's pid and uid (`SO_PEERCRED`, `getpeereid` on macOS), writes,
  half-closes and drains.
- The 2 s machine-agent tick (while a phone is connected) also reads the
  records into `m_claudePeerSessions`; `agents.list` marks an outside row
  `steer: true` when a live record matches it (a surfaced external row by
  the pids `findSessionPids` found for its transcript, else by session id; a
  scanned process by pid), and rings `agents.changed` when
  that changes. `status.capabilities` gains `agents.outside.prompt` on Unix
  builds.
- `agents.prompt` with a negative id (`answerLocalControlOutsidePrompt`):
  photos answer `outside_images_unsupported`, an empty or over-long text
  `bad_task`, a CLI other than Claude Code `outside_cli_unsupported`.
  Off the UI thread it finds the process again (pids by
  `findSessionPids` for a surfaced row), re-reads the records and predicts
  the receiver's decision with `ClaudePeerMessaging::predictInbound` (the
  settings files above, `--settings` from `/proc/<pid>/cmdline`, and the
  class from the latest `permissionMode` in the session's own transcript
  `projects/<cwd key>/<sessionId>.jsonl`, or the command line's
  `--dangerously-skip-permissions`/`--permission-mode bypassPermissions`).
  - `refuse`: nothing is sent; `outside_refuses_messages`.
  - `hold` on a session without a terminal (`entrypoint` other than `cli`):
    nothing is sent, since the CLI would drop it; `outside_held_no_terminal`.
  - `hold` in a terminal: sent, answered `{"status": "held_for_approval",
    "cause": …}` and logged; the phone does not call it a live steer.
  - `accept`: sent, answered `{"status": "delivered", "cause": …}`.
  - `not_steerable` when no live inbox matches or the write fails (the
    reason goes to the desktop log).
  The prediction reads the same files the CLI reads, but a mode switched
  after the last prompt is not in the transcript yet, so it can be wrong in
  that one case; the CLI's own hold still applies. Every message is logged with the phone
  that sent it. `agents.stop` with a negative id answers
  `outside_stop_unsupported`, unless the row runs under `forkmesh term`
  (below).
- `AgentLogText` shows a peer entry as a prompt line, `> ⟵ <name>: <text>`
  (`ForkMesh (phone)` for a steer; until 2026-09-29 a steer was a plain
  `> <text>` and another session's message `> (from <name>) <text>`), so
  the phone's transcript shows what was sent. See "Task notifications and
  rich transcripts on the run screen" below.

### Phones

- `LocalOutsideAgent.steer`; `UnifiedAgentRow.fromOutside` sets `canPrompt`
  only for a `claude` row the desktop marked `steer`, so an older desktop
  (no `steer`) keeps the read-only screen. `outsideSummary` reads "Claude
  Code started outside ForkMesh on <desktop> in <folder>. You can watch and
  steer it here." (or "…steer it in the terminal that started it." when it
  cannot be steered), shown under the run screen's header; the transcript
  fallback note says the same.
- Fixed 2026-10-02: a Claude Code started in a terminal showed "You can watch
  it here; steer it in the terminal that started it." with no composer,
  although a message to it got through. The desktop matched a surfaced
  row's live record by session id only, and the record's `sessionId` need
  not be the transcript's (a resumed or cleared session keeps writing under
  another id), so `steer` was false, while `agents.prompt` finds the process
  by pid and delivers. `outsideAgentSteerable` now passes the surfaced row's
  pids to `ClaudePeerMessaging::find` too. The iOS run screen also read
  `canPrompt`, the summary and the hint from the row it was opened with, so
  a row that became steerable later never showed the composer; it now reads
  the live row from `AgentControlCenter.rows`, as Android already did.
- `AgentControlCenter.prompt` sends `agents.prompt {id, text}` for such a
  row, with no photos (the composer hides the photo button there). Stop is
  hidden on outside rows that are not `stoppable` (below). The new codes have sentences in
  `ControlErrorText`. A `held_for_approval` answer sets
  `AgentControlCenter.promptNotice` ("Sent. Claude Code is holding it for
  approval: approve it in the terminal that started it, or turn on Accept
  messages from ForkMesh ("crossSessionInbound": "accept" in that Claude
  Code's settings.json) to steer without approving."), which the iOS run
  screen shows under the composer, the Android run screen shows in the
  status line above its composer instead of "Sent.", and Siri says instead of "Sent to …".
- iOS `TellAgentIntent` steers a steerable outside row; the "Steer it in the
  terminal that started it" answer is left for rows without it.
  `StopAgentIntent` explains that an outside agent cannot be stopped here,
  unless it is `stoppable`.

### A steer says it is the user's own reply (2026-10-08)

A "Yes" sent from the phone to a Claude Code started in a terminal arrived,
and the agent answered: "Another session, "ForkMesh (phone)", sent a message
that just says "Yes". I never asked that session anything … It may have been
meant for a different session." The message reached the right session, but
the CLI frames every peer message as "Another Claude session sent a message
… not typed by your user", so a short reply to the agent's own question read
as a stray message from a stranger, and the agent left it unanswered.

The protocol has no way to send a user prompt (above), so the desktop now
opens the body of every phone steer with a fixed lead,
`ClaudePeerMessaging::steerLead()` (`STEER_LEAD` in the Python reference,
`steerLead` in the fixture): the user typed this in the ForkMesh app, on this
session's own page, as their next message in this conversation; it arrives
as cross-session messaging only because that is how ForkMesh reaches a
terminal session, and no other session sent it. `deliver` sends
`steerBody(text)` (lead, blank line, text). The lead claims nothing about
permissions: `from-mode` is still left out and the CLI's peer rules (no
escalation, no approval of a pending prompt) still apply. The phone is a
device the user paired, so the claim is true.

`AgentLogText::claudeEntry` (and `peerPrompt` in `desktop_go_new`) drops the
lead with `steerText`, so the run screen still shows `⟵ ForkMesh (phone):
Yes`. The terminal that started the session shows the lead before the text.
A `forkmesh term` session is unchanged: the phone types into its terminal,
which already is the user's own prompt.

### Shared fixture

`conformance/claude-peer/v1/fixture.json` (written by `generate_fixture.py`
from `tools/claude_peer_message.py`, the Python reference and a small CLI:
`list`, `send <pid> <text>`, which prints `delivered` or
`held_for_approval (<cause>)`) holds the frame recorded from a real
SendMessage with its token blanked, which both implementations must rebuild
byte for byte, plus frame, escape, key-name and mode cases, and the
`crossSessionInbound` decision table (`inbound`, `settingsFiles`).

## Typing into a terminal from the phone (`forkmesh term`)

Added 2026-09-30. The peer path above is honest by design: Claude Code tells
the model that a peer message came from another session, "not typed by your
user", so the model rightly does not treat it as the user's approval for
anything sensitive, and a bypass session holds it for approval. That must
not be spoofed, and ForkMesh does not. Instead the user can start the CLI
inside a terminal the desktop can type into, the same thing SSH plus tmux
from the phone would give them. What arrives is keyboard input, so the CLI
records it as the user's own turn.

### Using it

- `forkmesh term claude` (or `forkmesh term codex`, or any other CLI, with
  its own arguments after it: `forkmesh term claude --model opus`). The
  desktop app answers `forkmesh term …` before Qt starts, so every install
  has it. The build also produces a small standalone `forkmesh-term` binary
  (`cmake --install` puts it beside `forkmesh`, in the bundle's
  `Contents/MacOS` on macOS), and the app answers to that name too, so a
  `forkmesh-term` symlink to `forkmesh` works the same.
- Optional alias, offered in Settings → Phone ("Type into a terminal from
  your phone", with Copy buttons) and never written by ForkMesh:
  `alias claude='forkmesh term claude'` and
  `alias codex='forkmesh term codex'`. Inside the wrapper `claude` resolves
  through `PATH`, so the alias does not recurse.
- `forkmesh term --no-remote claude` starts with phone typing off.
  **Ctrl-^** (Ctrl-Shift-6 on US keyboards, byte 0x1e) toggles it for that
  session; the wrapper swallows that byte, so the CLI never sees it. Ctrl-]
  and Ctrl-_ were ruled out because Claude Code binds them.
- Without a terminal on stdin (a pipe, a script) the wrapper just runs the
  CLI directly with nothing registered.

### The wrapper (`desktop/src/ForkMeshTerm.*`)

A pty proxy with no Qt: `forkpty` with the user's own termios and window
size, the real terminal in raw mode, every byte passed through both ways.
SIGWINCH copies the new size to the pty; SIGTERM, SIGHUP and SIGQUIT go to
the CLI; when the CLI stops itself (Ctrl-Z) the wrapper restores the
terminal, stops itself so the shell sees a stopped job, and on `fg` goes
raw again, resends the size and SIGCONTs the CLI's process group. The exit
code is the CLI's (128 + signal when it was killed, 127 when it could not
be started). Writes to the pty are queued non-blocking, so a large paste
never stalls the user's keystrokes.

It also listens on `<dir>/<pid>.sock` and writes `<dir>/<pid>.json`, where
`<dir>` is `$XDG_RUNTIME_DIR/forkmesh-term` on Linux (when that folder is
the user's own), `$TMPDIR`'s per-user `confstr(_CS_DARWIN_USER_TEMP_DIR)`
folder plus `forkmesh-term` on macOS, and `/tmp/forkmesh-term-<uid>`
otherwise; `FORKMESH_TERM_DIR` overrides it for tests. The folder is 0700
(tightened if needed, refused if it is a symlink or someone else's), the
socket is bound under umask 0177 and chmodded 0600, and the registration is
written 0600 through a temp file and rename. Both are removed on exit.

Registration: `{"v":1, "pid", "start", "child", "cli", "cwd", "socket",
"remote", "startedMs", "tty"}`. `start` is field 22 of `/proc/<pid>/stat`
(empty on macOS), so a recycled pid is caught; `cli` is the program's base
name, folded to `claude` or `codex` for their variants; `remote` follows
the toggle.

Socket protocol, one request per connection, no JSON so the proxy stays
small: a header line `FMTERM1 <verb> <fromBytes> <payloadBytes>\n`, then
the device label and the payload; the answer is one line, `ok …` or
`err <code>`. The wrapper accepts only a peer of its own uid
(`SO_PEERCRED`, `getpeereid` on macOS), at most four at a time, each for
5 s.

- `hello`: `ok remote=<0|1> paste=<0|1> cli=<name> child=<pid>`.
- `type`: the text is cleaned (CR and CRLF become LF, every other C0
  control but tab, DEL and the C1 range are dropped, invalid UTF-8 is
  dropped, trailing blank lines trimmed), so a phone can only ever produce
  printable text and newlines, never a key sequence. If the CLI has turned
  on bracketed paste (the wrapper watches its output for `CSI ? 2004 h/l`,
  including combined modes and a split across reads) the text is written as
  `ESC[200~ … ESC[201~`, so a multi-line prompt stays one prompt; the ESC
  byte is already gone from the text, so it cannot close the paste early.
  Without bracketed paste newlines become spaces. Enter (`\r`) follows
  120 ms later as its own write, so the CLI has finished the paste before it
  sees the key. Refusals: `remote_off`, `empty`, `too_long` (64 KiB),
  `user_typing` (a key was typed at the keyboard in the last 1.5 s, so the
  phone never interleaves with the person typing; terminal replies that
  start with ESC do not count), `rate_limited` (20 a minute), `exited`.
- `key`: `esc`, `ctrl-c`, or `interrupt`, which is Esc for Claude Code and
  Codex and Ctrl-C for anything else; 30 a minute.

**Visible in the terminal.** Before the text the wrapper writes
`ESC 7, ESC[1B, CR, dim "⌨ typed from <device>", ESC 8`: the notice goes on
the line under the cursor and the cursor returns to where it was, so
nothing scrolls and the CLI's idea of the screen stays right; the CLI
repaints that line whenever it next changes it. In Claude Code 2.1.285
that is the input box's bottom border, where the notice stays until the
next repaint. Stop writes "⌨ Esc from <device>", the toggle "⌨ phone typing
on (Ctrl-^ toggles)". (A first version printed the notice on its own lines
with CRLF; it rendered, but a CRLF on the bottom row would scroll the
screen under a TUI that tracks it, so it was replaced.) A shorter notice
over a longer one can leave the old tail visible until the CLI repaints.

### Stop

Claude Code's interrupt key is Esc; a lone ESC byte is read as the Escape
key. Codex also stops on Esc: its footer reads "esc to interrupt" while a
turn runs, and in a test on 2026-09-30 (codex-cli 0.153.2) a single Ctrl-C
sent to an idle Codex quit the session ("To continue this session, run:
codex resume …"). So Codex uses Esc too, although Ctrl-C was the first
plan; Ctrl-C stays for other CLIs, where it is the usual interrupt. Esc on
an idle Claude Code or Codex does not end the session, though in Claude
Code it can clear a half-typed draft in the input box. The Codex Esc interrupt itself
was not run end to end (that account was at its usage limit); Claude
Code's was.

### Desktop

- `TerminalHandoff` (`src/TerminalHandoff.*`, Qt Core): `runtimeDir` (the
  same rule as the wrapper), `readSessions` (only 0600 regular files named
  `<pid>.json` in a 0700 folder the user owns, whose socket lies directly in
  that folder), `liveSessions` (wrapper and CLI alive, start time matches,
  socket a 0600 socket), `find` (a row's pid, or up to six of its
  ancestors, equals the wrapper's CLI or the wrapper), `exchange` (refuses a
  socket that is not the user's, checks `SO_PEERCRED` pid equals the
  registered wrapper, 2 s connect, 3 s answer), `type`, `key`, `errorCode`
  and `TypingGate`.
- The 2 s machine-agent tick reads live registrations with the peer
  records (`m_terminalHandoffSessions`) and the pids behind each surfaced
  Claude session (`m_externalSurfacedPids`). `outsideAgentTerminal(id)`
  matches an outside row to a registration with phone typing on;
  `agents.list` marks it `typeable` and `stoppable`, and `agents.changed`
  rings when either (or `steer`) changes. `status.capabilities` gains
  `agents.outside.type` and `agents.outside.stop` on Unix builds.
- `agents.prompt` with a negative id prefers the terminal: the device must
  be paired (`TerminalHandoff::admit`), then `TypingGate` allows one prompt
  a second and 10 a minute per device, 8000 characters at most
  (`typing_rate_limited`, `bad_task`). It answers
  `{"status":"typed","via":"terminal"}`. A wrapper that answers
  `remote_off`, `exited` or cannot be reached falls back to the peer path
  for a Claude Code row (logged), which answers as before plus
  `"via":"peer"`; `user_typing` and `rate_limited` are returned as
  `terminal_user_typing` and `typing_rate_limited`; any other failure is
  `not_typeable`. A typeable Codex row no longer gets
  `outside_cli_unsupported`.
- `agents.stop` with a negative id sends `interrupt` to a stoppable row and
  answers `{"status":"interrupted","key":"esc"}`; other outside rows still
  answer `outside_stop_unsupported`.
- The desktop log gets "Typing into the terminal running <CLI> in <folder>,
  requested by <phone, network>", then "Typed into the terminal in
  <folder>." or the reason it failed; Stop logs "Pressed Esc in the terminal
  running <CLI> in <folder> to stop it, requested by …".

### Phones

Both apps read `typeable` and `stoppable` (absent means false, so an older
desktop keeps the peer path and no Stop). An outside row can prompt when it
is typeable or steerable, and the prompt path is `terminal`, `peer` or
`none`. The composer reads "Types into your terminal" or "Message (arrives
as another session)"; photos stay off. A `typed` answer shows no approval
notice (Android's status line above the composer says "Typed into your terminal."). Stop shows on
stoppable outside rows and sends `agents.stop`; iOS `StopAgentIntent`
interrupts such a row ("pressed Esc in its terminal") and `TellAgentIntent`
types into a typeable one. The summary under the run screen's header says
"What you send here is typed into its terminal." `ControlErrorText` has
sentences for `typing_rate_limited`, `terminal_user_typing`,
`terminal_remote_off` and `not_typeable`.

### Security review

**Who can type.** Only a device that the desktop has paired: every request
arrives over the local-control channel, whose handshake has the phone sign
the transcript with its Ed25519 device key and then seals every frame with
AES-GCM keys derived from it (`LocalControl.h`); requests from a device that
is not paired get `not_paired` before any handler runs, and the typing
handler checks `isPaired` again. The phone never talks to the wrapper; only
the desktop does, and only after that check. The wrapper and the desktop
both clean the text, so a paired phone can produce printable text,
newlines and one Enter, or one of two named keys, and nothing else: no
escape sequences, no Ctrl-C inside a prompt.

**Relay compromise.** Away from home the same sealed channel rides the
relay; the relay sees ciphertext and chunk envelopes only, cannot forge a
device's signature, and a relayed channel is only accepted for a device
already paired at the desktop. A compromised relay can drop or delay
frames, not type.

**Local processes.** The socket is 0600 in a 0700 folder owned by the user,
and the wrapper checks the connecting uid. Any process of the same user can
connect and type; that is no more than the same user can already do with
`ptrace` or by writing to the CLI's own pty, so it adds no new power. Another user cannot reach it. The desktop, for its part, only trusts
a registration that is a 0600 file in the private folder, whose socket is
in that folder, and whose socket peer is the registered pid.

**Rate and size.** Per device on the desktop: 1 a second, 10 a minute,
8000 characters. In the wrapper, whatever the caller: 20 prompts and 30 keys
a minute, 64 KiB, and nothing while someone is typing at the keyboard.

**Auditability.** Every prompt and stop shows a notice in the terminal
naming the device, before the text, and is written to the desktop log with
the device and whether it came over the LAN or the relay. The person at the
keyboard can turn it off (Ctrl-^, `--no-remote`), which removes the
session's `typeable` mark within one tick.

**What is different from the peer path.** Peer messages are labelled by
Claude Code as another session's, and the model and the hold policy treat
them that way; that stays true. Typed input is the user's own, recorded as
`"type":"user"` with `origin: {"kind":"human"}`. That is correct here
because only the user's own paired device, through their own desktop, can
produce it, and the terminal shows it happening. It carries the user's
authority exactly as if they typed it, so a stolen, unlocked, still-paired
phone can do what the user could at that keyboard; removing a phone in
Settings → Phone revokes it.

### Platforms

Linux first (this is where it was built and tested). macOS uses the same
POSIX pty, socket and signal calls, `getpeereid` in place of
`SO_PEERCRED` and `sysctl` for parent pids; it compiles from the same
sources but was not run on a Mac. Windows is out of scope: there is no pty
or Unix socket model there that this maps onto, so neither the target nor
the capability exists on Windows builds.

### Verified end to end (2026-09-30)

On this desktop, with a throwaway Claude Code 2.1.285 (`--model haiku
--name forkmesh-test-terminal-handoff`) under `forkmesh-term` in a private
tmux, in `/var/tmp/terminal-handoff/TEST-do-not-use`, driven by a harness
that runs a real `LocalControlServer`, a phone client signing with a test
device key, and the desktop's discovery (`MachineAgentScan` +
`TerminalHandoff`):

- Discovery listed nine running Claude Code sessions; only the wrapped one
  was `typeable`. Nothing was sent to the others.
- An unpaired phone's `agents.prompt` was answered `not_paired` and nothing
  reached the terminal.
- The paired test device's prompt was answered `{"status":"typed",
  "via":"terminal"}`, the terminal showed "⌨ typed from Jett's iPhone (e2e
  test key)", and the session JSONL recorded it as `"type":"user"`,
  `origin: {"kind":"human"}`, no `isMeta`, no cross-session envelope. A
  two-line prompt arrived as one prompt.
- `agents.stop` during a long answer was answered `{"status":
  "interrupted","key":"esc"}` and the JSONL recorded `[Request interrupted
  by user]`.
- Codex 0.153.2 under the wrapper turned on bracketed paste and took a
  two-line prompt as one; the idle Ctrl-C finding above came from the same
  run.

## One compact list on the iOS Agents tab

Since 2026-09-29 the iOS Agents tab is a single dense list, laid out like the
desktop's Agents table, so a phone screen holds a dozen runs instead of three.

- **No header.** The "Agents" title, its sentence, the running/sessions/repos
  counts, "+" and Refresh are gone. The tab bar is the top of the page: a
  single "Agents" tab whose pill counts every listed agent (it counted only
  running runs until 2026-10-01; before that, relay sessions, and read 0 while
  six ran). The "New" tab was removed on 2026-10-01 as a duplicate of the
  shell's compose button, which is now the composer. Pull to refresh stays.
  The shell's Agents badge still counts running runs.
- **Usage as vertical bars.** The Usage card became `AgentUsageBars` at the
  right of the tab bar: one 4×18 pt bar per window (Claude Code 5-hour,
  weekly, Fable weekly; Codex 5-hour, weekly), filled from the bottom with
  the used share, green/amber/red by the window's state, the same geometry as
  the desktop's `TokenUsageMiniChart`. Tapping them opens the full card in a
  sheet. Watch and Saved permissions sit next to them as icons.
- **Every agent, every repository.** The list merges the relay's pushed and
  queued sessions (`AgentsListModel.entries`) with the control center's rows
  (`AgentControlCenter.rows`: every desktop session over LAN or relay, outside
  CLIs and node runs), so the hub's separate "All agents" destination is no
  longer listed (the route still opens it for deep links). `AgentLines.merge`
  (FMAgents) shows a desktop session once, keeping the pushed row's screen
  and filling its diff numbers from the live row, and a node run once;
  running first, then newest. The "Running on <desktop>" heading is gone.
- **Row.** `AgentLineRow`: the model face (`watchFaceName`) with a status
  glyph on its corner (flame pulsing while running, clock queued, hand
  waiting, check done, x failed, stop, merge purple), the title, the short age
  (`now`, `42s`, `5m`, `3h`, `2d`, `2w`, `2mo`, `1y`, as desktop), then one
  tiny line: cloud or terminal mark, repository name, branch, worktree mark,
  commits ahead, behind or conflict, files changed (`999+` cap), green/red
  churn bars on the desktop's log scale, PR number; a 26 pt thumbnail of the
  first attached image on the right opens full screen. Long-press a control
  row to stop it. Approvals stay above the list; bulk actions moved below it.
- **Desktop.** Each `agents.list` row now also carries `diffStats` (`files`,
  `ahead`, `behind`, `added`, `removed`, `dirty`, `conflicted`) and
  `worktree` from the Agents table's cached `m_agentDiffStats` (no new Git
  scan), and `attachments` with the first image the session shows
  (`agentSessionImages`), whose id the existing `agents.attachment` fetch
  already resolves. Phones read them into `AgentSession.linesAdded`,
  `linesRemoved`, `hasWorktree` and `LocalAgentRow.attachments`, then
  `UnifiedAgentRow`. Android's Agents screen is unchanged.

## Compact, Zen and Full on the phones (2026-09-29)

The desktop Agents list has three views (`docs/desktop/agents-ui.md`), and
both phone apps now have them too. A button steps through them:

- iOS: at the right of the tab bar, before Watch.
- Android: in the Agents header, before All agents.

The icon names the current view (list, moon, eye, and bird for iOS's Eagle). The choice is stored
under `agents/listView`, the desktop's setting name: iOS keeps it in
`KeyValueStore` and Android in `Prefs`.

- **Compact** is each app's row as before. On iOS that is `AgentLineRow`
  (agent dot, title, age, meta line; the dot replaced the face and status
  glyph on 2026-10-01, see NAVIGATION.md §4). On Android it is
  `AgentTwoLineRow` (icon, title, status pill, detail line).
- **Zen** shows only the title, plus the thumbnail or retry control at the
  end when the row has one.
- **Full** puts the title flush left and one line of glyph + one word under
  it: status, provider, Desktop/Cloud/Outside or the runner's name, age,
  Conflicts or Behind, files changed, `+added−removed` (e.g. `+5.2k−74`),
  Worktree or Branch. A run on a runner shows the runner's name (e.g.
  `fmnext-r1`, cut to 9 characters and "…" past 10) with a CPU glyph in the
  accent colour (`cpu` on iOS, `memory_outlined` on Android) where a desktop
  run says Desktop; the name comes from the session's `runner` field and a
  blank one falls back to "Runner" (`runnerWord` in `AgentLines` and
  `AgentLineChips`). A long name widens that row's host column instead of
  every row's.
  Every slot is as wide as its widest possible word, so the columns line up
  from row to row without measuring the list, and a missing detail leaves its
  column blank. Columns that do not fit the phone's width are dropped from
  the end. iOS uses `ViewThatFits`; Android uses a `FitRow` layout that
  stops placing children once one would overflow.
- **Running is the purple flame.** Both apps draw a running agent with a
  pulsing flame in the desktop fireball's purple `#bc8cff`:
  - iOS: `AgentStatusGlyph`. This also changes Compact, where the flame was
    the accent blue. Since 2026-10-01 the iOS rows no longer draw it: the
    agent dot in front of each row shows the status. The flame stayed on
    Eagle's status card until that display was removed the same day, and
    `AgentStatusGlyph` went with it.
  - Android: `AgentStatusGlyph` with `local_fire_department_rounded`, used in
    Full's status column.

The words come from shared logic with tests, not from the views:
- iOS: `AgentLines.chips` / `AgentListView` in FMAgents. `AgentLine` now
  also carries `provider`.
- Android: `AgentLineChips` / `AgentListView` in `core/.../agents`, fed by
  `AgentLineChips.facts` for desktop sessions, launched runs and live
  control rows.
Other screens that use `AgentTwoLineRow` without facts stay Compact.

## Eagle view on iOS (2026-10-01)

iOS has a fourth view, **Eagle** (bird icon), after Full: Compact → Zen →
Full → Eagle → Compact, stored as `eagle` under `agents/listView`. Android
does not have it yet; an Android phone that reads `eagle` is not affected
because each app keeps the setting in its own store.

### Eagle is the default on iOS (2026-10-07)

The user asked for Eagle to be the iOS Agents list's default view. A missing
or unknown `agents/listView` now reads as Eagle (`AgentListView(stored:)`;
it was Compact). A phone that already stored a view is switched to Eagle
once: the first time the Agents screen opens after the update,
`AgentsScreen.storedListView()` finds no `agents/listViewEagleDefault` mark,
stores `eagle` and writes the mark (`AgentListView(stored:eagleDefaulted:)`).
From then on the View button's choice is kept as before. Android keeps
Compact, as it has no Eagle. Tests: `AgentLinesTests`
`testListViewCyclesCompactZenFullEagleAndReadsWhatWasStored` and
`testListViewSwitchesToEagleOnceThenKeepsTheChoice`.

Eagle gives each row two lines after the agent dot:

1. The title, at the top and as far left as the row allows.
2. Tiny glyphs for where the agent runs (`desktopcomputer` for the
   desktop, `cloud.fill` for a cloud agent or runner, `terminal` for an
   outside session), for its branch (`arrow.triangle.branch`) and for its
   worktree (`folder.fill`), each shown only when the agent has one; the
   number of changed files with the green and red churn bars beside it
   (`AgentChurnBars`, the desktop's log scale); and the short age (`8m`)
   (`AgentEagleStats`), then the agent's dialog ticker in the rest of the
   line.

The user asked (2026-10-02) for the host, branch and worktree glyphs and the
churn bars on this line, and for the file and clock glyphs to go, so the file
count and the age stand as bare numbers.

The user asked (2026-10-01, later still) for the scrolling text to sit on the
same line as the icons. Until then the ticker was a third line under the file
count and age, across the whole width of the row; now it starts 10 pt after
the age and runs to the row's right edge, so a talking row is no taller than
a quiet one.

The user asked (2026-10-01, later) to remove the change count and make sure
the prose scrolls. The lines added and removed (`+5.2k −74`,
`AgentEagleChurn`) no longer sit at the left of the ticker, so the ticker
starts at the row's left edge and has the full width.

The user asked (2026-10-01) to move the title up and to the left, drop the
flapping display, and show just the file count, the updated time and the
line counts under the title, with the live dialog beside the line counts.
Before that, a small display (`AgentEagleDisplay`, 88 × 30 pt) sat between
the dot and the title and flapped through every detail the row knew
(`AgentLines.eagleFacts`) one at a time every 2.4 s; it, `AgentFlap`,
`AgentEagleGlyph`, `AgentStatusGlyph` and `eagleFacts` are gone.

While a running agent has something to say, the line under the title shows
it as a ticker after the icons:

1. The lane opens beside the file count and age (it is empty
   otherwise).
2. The line appears at the left edge of the lane at once, waits half a
   second, then scrolls left at 80 pt/s.
3. A second copy follows 5 pt behind the first (about a word space), and
   more copies follow it until the lane is full, so the lane is never empty:
   when the first copy has left, the second is exactly where the first
   started and the loop begins again with no jump. The line keeps looping
   for as long as it is the agent's latest.
4. When the agent stops running, the lane closes.

The user asked (2026-10-01) for the text to be always visible, a little
faster, and for the wrap-around to be tight enough that it reads as one
continuous line. Before that the line entered from the right edge, scrolled
off at 64 pt/s, and the chips came back between lines, so the row was blank
at the start and end of every line.

The user asked (2026-10-02) for no gaps: the blank between the end of a line
and its next copy, or the next line, was 24 pt, and a line shorter than the
lane had a blank as wide as the rest of the lane behind it. Now every seam is
5 pt and a short line repeats until the lane is full.

`AgentEagleStrip` keeps the lane as a train of lines (`train`), the first one
scrolling, padded at the end with copies of the last line until the copies
reach a lane's width past the first seam. A new line replaces every copy
that has not reached the lane yet (left edge past the right edge), so it
comes in right behind what is showing and nothing visible changes. Only the
newest waiting line is kept, so a fast agent does not build a backlog. A line
that fits the lane scrolls too. Until 2026-10-01 a line that fit the lane sat
still, and with the full width more lines fit, so the ticker would have
stopped for most of them. With Reduce Motion on, nothing scrolls: the line
sits still at the left and is replaced in place.

The ticker's position is worked out from the clock on every frame
(`TimelineView(.animation)` in `AgentEagleStrip`): the offset is the time since
the current lap began times 80 pt/s, and when a lap ends the following copy
takes over and the next lap starts. Until 2026-10-01 each lap was a
`withAnimation` whose completion started the next one. The user reported that
the tickers stopped after scrolling down the list: the rows sit in a
`LazyVStack`, a row scrolled off screen dropped its running animation, its
completion never came, and nothing started the loop again because the line had
not changed. With the clock, a row that comes back on screen picks up where it
should be, skipping the laps it missed.

The user asked (2026-10-05) for the tickers to always scroll: they started
when the app loaded and then stopped. Two things stopped them:
- A lap ends when the frame's clock passes the lap length, but `wrap()` checked
  the time again with `Date()` and gave up if that was still short of the end.
  The frame's date can be a little ahead of `Date()`, so the lap never ended,
  `due` stayed true, `onChange` never fired again and the line sat at the end
  of its lap for good. `wrap(at:)` now takes the frame's date and always starts
  the next lap.
- Once an agent's tool output filled the last 6000 characters of its
  transcript, `AgentLines.dialog(_:)` found no prose and returned an empty
  line, which closed the lane. The same happened when a stream gave way to
  another (`ControlWatchFeeds` keeps at most six). `AgentEagleFeed` now keeps
  each running agent's last non-empty line until it says something new, so the
  ticker keeps looping it; the lane still closes when the agent stops running
  or the Agents screen is left.

Where the line comes from:
- `AgentLines.dialog(_:)` reads the last 6000 characters of a transcript. It
  drops the line still being written, parses the rest with
  `AgentTranscriptFormat`, and keeps the last block of the agent's own prose:
  a paragraph, heading or bullet. Since 2026-10-01 the ticker shows only what
  the agent says to you, so tool runs (`Bash swift test`), task summaries,
  errors, your prompts and steers, status lines, code, JSON and tables are
  all skipped; while the agent runs tools the row keeps its last sentence
  instead of replaying commands. Markdown marks are stripped, the text is
  joined into one line, and it is cut to 120 characters.
- Desktop agents: `AgentEagleFeed` follows the running desktop rows of the
  control center through `ControlWatchFeeds`, the transcript streams the
  Watch screen uses. That allows at most six streams, and they run only while
  the list is in Eagle and the Agents screen is showing. A relay session
  (`AgentEntry.desktop`) finds its stream by repository + desktop session id.
- Cloud and runner runs (`AgentEntry.launched`): `liveText` when the run is
  streaming, otherwise the last message that is not the user's.
- Rows that are not running get no ticker.

## Sorting the iOS Agents list (2026-10-01)

A small button at the left of the Agents tab's tools (an up/down arrow and
the current order's word) changes the order of the list. Tapping it opens a
menu of every order, the current one checked, so any order is one pick away
(2026-10-02; until then each tap stepped to the next order and reaching one
meant cycling through the rest). The menu lists them in this order:

Recent, Running, Status, Priority, Name, Repo, Changes.

- **Recent** (the default) puts the most recently updated agent on top,
  running or not. Before this, running agents always came first; that order
  is now **Running**.
- **Status**: Waiting, Running, Queued, Failed, Stopped, Success, Merged,
  then everything else.
- **Priority**: the desktop's run-order priority, 1 (highest) first, the
  same order as the desktop's Priority sort. Runs with no priority (cloud and
  runner runs, sessions from an older desktop) count as 50, the desktop's
  default.
- **Name** and **Repo**: A–Z, ignoring case.
- **Changes**: most lines added plus removed first, then most files changed.

Rows that tie stay newest first. The choice is stored under `agents/listSort`
in `KeyValueStore`; a missing or unknown value reads as Recent. The order
comes from `AgentLineSort.sorted` in FMAgents (`AgentLines.swift`), applied
to `AgentLines.merge`'s output in `AgentsScreen`; `AgentLinesTests` covers
each order. `AgentLineSort.next` is still there and tested but the chip no
longer uses it. Android's Agents list is unchanged; its Desktop tab
keeps its own sort menu (`AgentListOrdering`). The enum is `AgentLineSort`,
not `AgentListSort`: FMAgents already has an `AgentListSort` (the desktop's
twelve orders, `Remote/AgentListOrdering.swift`), and the duplicate name
failed the 2026-10-01 ad hoc archive.

### Where the phone gets an agent's priority (2026-10-02)

The desktop keeps each session's 1..99 `priority` in `AgentStore`. Until
2026-10-02 it sent it to neither phone path, so the phones' Priority orders
(`AgentListSort.priority` on iOS, `AgentListSort.Priority` on Android) gave
every row 50 and fell back to recent activity. They also ranked the larger
number first, the reverse of the desktop, where 1 is the top.

- The desktop now puts `priority` in each local-control `agents.list` row
  (`MainWindowLocalControl.cpp`) and in the owner-sealed session snapshot it
  pushes to the relay (`MainWindowAgentSync.cpp`).
- `AgentSession` (iOS `FMApi/Models/Agents.swift`, Android `model/Agents.kt`)
  reads it, clamped to 1..99; a missing or non-positive value reads as 50.
- iOS carries it through `UnifiedAgentRow.fromDesktop` to `AgentLine`; a
  desktop session the LAN can reach takes the live row's value.
- `AgentListItem` takes the session's priority unless the caller passes one,
  and both platforms' `AgentListOrdering` sort it ascending, ties newest
  first.

## The iOS Agents tool row (2026-10-01)

The user asked for the "Agents" tab label and its count to go from the top of
the iOS Agents screen, so the row starts with the sort and view buttons. The
row is no longer an `FmSegmentTabBar` with one tab: `AgentsScreen.tabBar`
draws the same raised 46 pt strip between hairlines with only the tools in
it. Each tool sits in a 30 pt capsule chip (`AgentToolChip`: the base
background with a border), the same height as before; the sort chip shows
its arrows in the accent colour and the caption in the primary text colour.
The total that the label's pill showed moved to the bottom bar (see
NAVIGATION.md §2.0).

## Actions on one desktop agent

| Action | Method | iOS | Android |
| --- | --- | --- | --- |
| List, start, transcript, prompt, stop, continue | `agents.*` | `AgentRunScreen` | `UnifiedAgentScreen` |
| Pending tool approvals; approve or deny | `agentops.approvals`, then `agentops.prepare` + `agentops.answer` | `ApprovalScreen` | `ApprovalScreen` |
| Changes (committed diff against the base, uncommitted worktree diff, new files) | `agentops.diff` | `AgentDiffScreen` (green diff square at the right of the top bar) | `AgentDiffScreen` ("Changes") |
| Changed images and SVGs in the changes (before and after) | `agentops.file` | `AgentDiffScreen` (`DiffImagePreview`) | not yet |
| Merge & clean up (one tap) | `agentops.prepare` + `agentops.merge` | `AgentDiffScreen` (`MergeCleanupButton`, `MergeLandedScreen`) | `MergeCleanupDialog` |
| Queue: concurrent-agent limit, order of the waiting agents | `agents.queue`, `agents.queue.limit`, `agents.queue.move` | `DesktopQueueSheet` | `AgentQueueViews.kt` sheet |
| Worktrees | `agentops.worktrees` | `WorktreesScreen` | `WorktreesScreen` |
| Merge leaderboard: the merge stats without merging ([below](#merge-leaderboard-2026-10-02)) | `agentops.mergestats` | `MergeLeaderboardScreen` (trophy on All agents) | `MergeLeaderboardScreen` (leaderboard icon on All agents) |
| Delete (agent, worktree, unmerged branch) | `agentops.prepare {action:"delete"}` + `agentops.delete` | `AgentRunScreen` / `DesktopSessionOpsCard` (Delete, then Undo for 5 s: `AgentPendingDeletes`) | `AgentDeleteDialog` |
| Recycle bin: list, restore, delete for good, empty, retention ([below](#recycle-bin-from-the-phone-2026-10-02)) | `agentops.bin`, `agentops.bin.restore`, `agentops.prepare {action:"purge"}` + `agentops.bin.purge`, `agentops.bin.retention` | `RecycleBinScreen` | `RecycleBinScreen` |
| Open in its Claude Code or Codex terminal ([below](#opening-a-desktop-agent-in-its-claude-code-terminal-2026-10-01)) | `terminal.resume` | `AgentRunScreen` (Terminal) | `UnifiedAgentScreen` (Terminal) |

On iOS the actions sit in the run screen's top bar, so every route to a
desktop agent has them (previously only the notification route had merge).
Since 2026-10-01 they are buttons there, not a "Desktop actions" menu: the
branch diff, Merge & clean up, Fix conflicts with agent (only while the branch
conflicts) and Delete. Worktrees moved off the run screen; the repository's
desktop session card still opens it. On Android they are a row of buttons under the status line (previously
merge was only on the repository's session card). Merge and delete are
offered only while the agent is idle; the desktop refuses a running one with
`agent_running` anyway. A desktop too old for `agentops.diff` or
`agentops.delete` answers `unsupported`, shown as "Update ForkMesh on the
desktop".

Merge & clean up runs on the first tap, with no confirm step, Merge button,
typed branch name or device authentication (changed 2026-09-29). The desktop no
longer asks for the branch name: `agentops.prepare {action:"merge"}` answers
without `typeToConfirm` and `agentops.merge` ignores `typed`.
`MergeCleanupModel.mergeNow()` (iOS and Android) asks the desktop for the
confirmation token and merges with it at once; against an older desktop that
still sends `typeToConfirm` it sends that name back, so the user never types
it. The tap opens `MergeCleanupSheet` (iOS) or `MergeCleanupDialog` (Android)
already merging: it shows "Merging and cleaning up on the desktop…" with a
spinner, then the result, or the error with Try again, or on a conflict the
Fix with agent button. Runner agents' `RunnerMergeSheet` / `RunnerMergeDialog`
merge the same way on open; their Clean up (removing an already merged branch)
still asks first. The Android notification action merges the same way instead
of refusing because a typed name was needed. When main gained Fix with agent,
runner merges and the Changes-screen button (2026-09-30), the one-tap merge
moved into those sheets and dialogs instead of running inline from the button,
so every entry point shares one result and conflict flow.

The Changes screen (`AgentDiffScreen` on both apps) also carries **Merge &
clean up** in its top bar, so a review of the diff ends in the merge without
going back to the run screen. It is shown under the same idle rule. The run
screen passes `canMerge` and `onMerged`; other routes to the Changes screen
pass neither and get no button. Android opens the same dialog; the screen
closes and the result shows in the status line at the top of the Agents tab
and All agents (`agentsNotice`), since the user asked for no toasts
(2026-10-09).

On iOS (since 2026-10-01) the merge runs in place, with no progress sheet: the
top-bar button (`MergeCleanupButton`) shakes while the desktop merges. Since
2026-10-01 the button is the bare merge icon: no tinted tile, and on iOS 26 the
toolbar's glass circle is hidden (`sharedBackgroundVisibility(.hidden)` in
`MergeToolbar`). From the tap the icon jitters, tilts, grows and glows, and the
phone rumbles through Core Haptics (`FmRumble`: a continuous buzz plus knocks
that come faster and sharper); both start mild and ease in to full strength
over 2.8 seconds (`FmRumble.level`). The rumble stops when the merge ends, or
after 12 seconds; the shake lasts as long as the merge. With Reduce Motion the
button shows a spinner and the phone stays still, and the haptics setting
silences the rumble. On success the Changes
screen turns into the Landed numbers and merge velocity (`MergeLandedScreen`,
titled "Merged"), which come in one after another: the check mark spins and
bounces in, then the LANDED card, the MERGE VELOCITY card and each of its rows
spring up from below out of a blur (`mergeReveal`), the bars grow from the
baseline and the counts roll up from zero. A **Go back to agent list** button sits right under the
stats cards, near the middle of the screen rather than pinned to the bottom
(2026-10-01), which closes the Changes screen and then the run screen. Since
2026-10-02 the button counts down from five ("Go back to agent list (5)") and
goes back on its own when it reaches zero; a tap, the top-left arrow or the
countdown leaves only once (`MergeLandedScreen.leave`), and leaving the screen
any other way cancels the countdown. Since 2026-10-02 leaving from the
Agents tab rebuilds that tab's stack (`ShellChrome.stackReturns`, keyed into
the tab's `NavigationStack` id next to `stackResets`) instead of calling the
run screen's `dismiss`: Merge & clean up removes the run from the agent list,
which takes away the list link that pushed the run screen, and that screen's
`dismiss` then did nothing, so the button and the countdown stayed on the
Merged page. The run screen reads the tab it sits in from
`\.shellStackTab`; in another tab or a sheet it still dismisses itself. Since
2026-10-02 (second fix) the Agents tab return also covers a run opened as a
route (a push notification, Siri, a link or a restored navigation state put
`.agent(id)` on the tab's path): the first fix only rebuilt the stack when the
path was empty and otherwise called `dismiss`, which did nothing once the
merged run left the list. `ShellChrome.leave` now trims the Agents path back to
its last `.agents` list route (or the root) before rebuilding the stack, and
the run screen hands the diff screens a `leave` closure that captures its
`dismiss` and tab while it is still on screen rather than reading its
environment after the merge took its list row away. The
system back button is hidden there because the branch it showed is gone; a
back arrow at the top left does the same as Go back to agent list. An error,
or a conflict with its Fix with agent button, shows in a strip above the diff
(`MergeFailureNote`), and the top-bar button is Try again. The run screen's menu item **Merge & clean
up** opens the Changes screen with `mergeOnOpen`, so it animates and lands the
same way. Since 2026-10-01 the iOS Changes screen has no "Changes" title and
no Refresh button (pull down to reload): the middle of the top bar shows
the branch (`base...branch` until 2026-10-02) with the +added −removed counts (`DiffBranchTitle`), and Merge &
clean up is a small icon-only button on the right (`MergeToolbar`), so the
branch name has room. The branch line no longer repeats above the first file.
`MergeCleanupSheet` is kept for the Worktrees screen and the desktop session
card.

Approval, merge and delete are confirmation-token operations checked and
logged by the desktop. Approving a tool request also requires device
authentication on the phone; denying does not require it because denial grants
no capability.

### Merge stats

After a merge the phone shows the same numbers as the desktop's merge
celebration. The desktop's `agentops.merge` reply carries a `stats` object
next to `branch`:

| Field | Meaning |
| --- | --- |
| `base` | the branch it merged into |
| `commit` | the merge commit; absent when the desktop could not find it among the newest 20 first-parent merges |
| `commits` | `git rev-list --count <commit>^1..<commit>`, as on the certificate's credit line; absent with `commit` |
| `files`, `insertions`, `deletions` | the merge's first-parent `--shortstat`, the churn the desktop shows as "N files +X −Y" |
| `velocity` | the five MERGE VELOCITY cards (`1h`, `24h`, `7d`, `30d`, `52w`): `label`, `count`, `top`, `avg`, and `series`, the card's buckets oldest first (60 minutes, 24 hours, 7 days, 30 days, 52 weeks; the last bucket holds this merge) |

The desktop reads these off the GUI thread after the merge lands
(`readLandedMergeStats` in `MainWindowLocalControlAgentOps.cpp`), with the
same git queries and `mergeVelocityForTimestamps` the celebration page uses,
so the reply waits for them but not for the background worktree and branch
deletion. Parsing and the JSON shape live in `LocalControlAgentOps.cpp`
(`parseMergeShortstatLog`, `mergeStatsJson`) and are covered by
`test_local_control_agentops.cpp`.

Once the merge lands the result fits on one phone screen without scrolling
(2026-09-30); it shows results only, nothing is confirmed there. iOS
`MergeCleanupSheet` and `RunnerMergeSheet` switch from their Form to
`MergeLandedReport`: the success line, then a LANDED card (commits, files,
+X −Y, the merge commit, and a bar split green/red by insertions and
deletions) and a MERGE VELOCITY card with one row per window: the label, a
bar chart of `series` scaled to its busiest bucket (the newest bucket, where
this merge landed, drawn solid, the rest faded), and the count over "top N ·
avg M". Android `MergeCleanupDialog` and `RunnerMergeDialog` turn into a
"Merged!" dialog with the same layout (`MergeStatsBlock` in
`AgentOpsWidgets.kt`). A desktop that sends no `series` gets a bullet bar
instead: the count as a share of the best window, with a tick at the average.
Each row reads as one accessibility element ("7 days: 30 merges, best 41,
average 13"); the charts are hidden from it. The model helpers are `peak`,
`record`, `share`, `avgShare`, `spoken` on `MergeVelocityWindow` and
`insertionShare` on `MergeStats`.

### Merge leaderboard (2026-10-02)

The merge stats used to show only right after a merge. A button on the All
agents screen (a trophy on iOS, the leaderboard icon on Android) now opens the
same report at any time: `MergeLeaderboardScreen` on both apps, fed by
`MergeLeaderboardModel`. It calls `agentops.mergestats`, which is read-only:
no `agentops.prepare` and no confirmation token. The desktop lists it in the
`status` capabilities, and a desktop too old to know it answers `unsupported`,
which the screen turns into "Update ForkMesh on the desktop".

| Param | Meaning |
| --- | --- |
| `id` | optional agent session: that agent's repository, merged into its base (`agentMergeBase`) |
| `owner`, `name` | optional repository, as `agentops.worktrees` takes it |
| none | the repository open on the desktop, else the repository of the newest local agent |

The reply is `{repo, branch, stats}`. `stats` has the same shape as the
`agentops.merge` reply's, describing the newest first-parent merge into the
base (the checkout's current branch when no session names one). `branch` is
the branch that merge brought in. `readLandedMergeStats` reads it with
`newest = true` instead of matching a branch name. With no merge in the last
20, `commit` is absent and the phone shows only the velocity card and today's
activity. iOS reuses `MergeLandedReport` without `onBack`, so nothing counts
down or leaves the screen. Android puts `MergeStatsBlock` and
`TodayActivityBlock` on a pushed screen instead of the "Merged!" dialog.

Below those cards, both apps show **Today's activity** (2026-10-02), the
phone's version of the desktop's post-merge heat grid
(`docs/desktop/agents-ui.md`). It is a GitHub-style grid with 24 hour columns
and 12 rows of 5 minutes each, counting your interactions with this app on
this phone today. Each cell's level is `ceil(4 × count / busiest slot)`,
mapped onto GitHub's five greens. The current slot has a ring round it, and
empty slots later in the day are faded. A line underneath gives the active
time, the busiest hour and the busiest 5 minutes, followed by a Less/More
legend. The whole card reads as one accessibility element. Because the card
was added, the result can now scroll on a small phone. Android wraps the
"Merged!" dialog's fields in a scroll view for this; iOS already scrolls.

Since 2026-10-02 the iOS report is compacted so the success line, the stats,
Today's activity and the countdown button fit on one iPhone screen again
(`MergeLandedReport`):

- **Hero card** (`MergeHeroCard`). The success line and the LANDED numbers
  share one green-tinted card: a filled check badge (`MergeBadge`) that spins
  and bounces in, the title (two lines at most) and subtitle, then commit and
  file chips (`MergeChip`) with +X −Y, and the churn bar with the short merge
  commit beside it. A one-off confetti burst (`MergeConfetti`, 1.8 s, off with
  Reduce Motion) pops out of the badge.
- **Merge velocity** (`MergeVelocityCard`). Each row is one 20 pt line: the
  window's short id (`1h`, `24h`, `7d`, `30d`, `52w`; the label when the id is
  missing or longer than four characters), the chart, the count, and "top N"
  over "avg M" in a narrow column. A flame marks a window whose count is its
  record (`count >= top`).
- **Today's activity**. The grid keeps 24 × 12 cells but each row is 6.5 pt
  tall instead of square, so the grid is about half as high, and it sweeps in
  left to right. The summary is one row of chips (active time, busiest hour,
  busiest 5 minutes) with the five-level legend swatches at the end; when they
  do not fit, the legend drops, then the old sentence comes back
  (`ViewThatFits`).
- The page uses 12 pt side margins and 8 pt gaps, and stops bouncing when it
  fits (`scrollBounceBehavior(.basedOnSize)`). Android's "Merged!" dialog is
  unchanged.

- **What counts.** Every touch sequence counts once: a tap, a scroll, a swipe
  or a long press. iOS counts these with `FmActivityRecognizer` on each window
  (`Theme/FmUserActivity.swift`, installed from `AppRootView`), which fails
  straight after its first `touchesBegan`, so the rest of the sequence never
  reaches it. Android counts them with the `fmTapSounds()` modifier, which
  already wraps the main window and every kit dialog, sheet and menu. Nested
  uses see the same down event, so `UserActivityLog.touchDown` drops a repeat
  of the same uptime. Typing counts once per edit. The software keyboard runs
  outside the app, so iOS listens for `UITextField`/`UITextView`
  text-did-change notifications, and Android wraps the `onValueChange` of
  `FmTextField` and of the app's raw `BasicTextField`s in
  `FmUserActivity.typing`. Android also counts hardware key presses
  (`MainActivity.dispatchKeyEvent`). The text change that follows a key
  within 150 ms is not counted again.
- **Not counted.** Raw Compose `AlertDialog`/`DropdownMenu` windows that
  feature screens open without the kit wrappers. They are separate windows
  that `fmTapSounds()` does not reach.
- **Storage.** The model is `UserActivityLog`, `ActivityGrid` and
  `ActivitySummary` (Android `insights/UserActivity.kt`, iOS
  `FMBase/UserActivity.swift`). The day's 288 counters are kept in the prefs
  under `user_activity_today` as `yyyy-MM-dd|n,n,…`. They are written at most
  once a minute, and again when the app goes to the background. A new day
  starts from zero. Tests: `UserActivityTest` (core) and
  `UserActivityTests` (FMBaseTests).
- **Synced across devices** (2026-10-06). The card adds the counts from the
  member's other phones, desktops and browsers, with a line such as "On this
  device, desktop and browser". `UserActivitySync` (Android core `insights/`,
  iOS `FMBase`) sends this phone's counts to `/api/accounts/activity-today`
  every 2 minutes in the foreground when something new was counted, and once
  more on going to the background. The card asks for a fresh read when it
  appears and redraws when the answer lands. The device id is kept under
  `user_activity_device`. Signing out or switching accounts drops the other
  devices' counts. Tests: `UserActivitySyncTest` and `UserActivitySyncTests`.
  See [activity-today.md](../app/activity-today.md).

Closing the result, with Done or by going back, returns to the agent list
when the merge started from an agent's run screen or its Changes screen: the
Changes screen closes and the run screen closes after it (iOS
`AgentRunScreen.leaveAfterMerge`, Android `UnifiedAgentScreen.leave`). A
conflict hand-off ("Fix with agent") stays on the run screen. Other routes to
the sheet (worktrees, the repository's session card) stay where they were.
The session card note and the Agents tab's status line get the one-line summary
("Merged b and cleaned up. 3 commits · 5 files +40 −2.").
Siri's merge answer adds the file and line counts. Both apps parse the object
into `MergeStats` / `MergeOutcome` (`AgentOpsModels`); a desktop that
predates it sends no `stats`, and the apps show only the success line.
The relay forwards the encrypted control frames but cannot mint a valid
confirmation or read the request and answer.

Models and client calls: iOS `FMAgents/Remote/AgentOpsClient.swift`
(`diff`, `prepareDelete`, `delete`, `merge` returning `MergeOutcome`),
`AgentOpsModels.swift` (`AgentDiff`, `MergeStats`, `AgentOpsAction.delete`), `AgentOpsViewModels.swift` (`AgentDiffModel`,
`AgentDeleteModel`); Android `agents/ops/AgentOpsClient.kt`,
`AgentOpsModels.kt`, `AgentOpsStates.kt`.

### Fix conflicts with the agent (2026-09-30)

When a merge from the phone stops on a conflict, the phone offers **Fix with
agent**, like the desktop's Conflict button (`fixAgentConflictsWithAgent`):
it sends the agent that owns the branch a follow-up, "Merge `<base>` into
your branch and resolve all merge conflicts. Make sure the build and tests
still pass, then commit.", followed by the conflicting files when the refusal
listed them. The text is `ConflictResolution.sessionPrompt` (Android
`code/CodeTools.kt`, iOS `FMCode/Collab/ConflictResolution.swift`); the
confirmation line is `ConflictResolution.fixSent`.

- A desktop agent's merge refused with `conflicts` sets
  `MergeCleanupModel.conflicted`. The Android dialog becomes a "Merge
  conflicts" dialog whose button is Fix with agent; the iOS sheet adds a Fix
  with agent section. The follow-up goes through `agents.prompt`, the same
  path as the composer, so a finished agent resumes under its own provider
  (`AgentControlCenter.fixConflicts` / `fixDesktopConflicts`, iOS `prompt` /
  `promptDesktop`).
- A runner agent's merge refused with `merge_conflict` sets
  `RunnerMergeModel.conflicted`, and the follow-up steers the same relay
  session.
- An idle agent whose row is already marked conflicted (the desktop's
  `diffStats.conflicted`, a runner's `diffStats.conflicted` or `merge.state`
  `conflict`) shows **Fix conflicts** on its run screen without trying the
  merge first (Android button row, iOS actions menu). Android desktop rows
  now carry `conflicted` from the session, as iOS rows already did.
- A pull request whose merge plan conflicts shows **Fix with agent** on its
  merge card. When an agent row owns the pull (`prNumber`, or its branch is
  the pull's head: `AgentSessionLookup.forPull`, iOS
  `UnifiedAgentRow.forPull`) and is idle, that agent gets the follow-up, as
  the desktop's pull Agent fix button does. Otherwise Android opens the
  start sheet and iOS queues a new agent with `ConflictResolution.agentPrompt`,
  as the branch Conflicts screen does.

Tests: Android `AgentOpsTest.testMergeConflictIsFlaggedForAnAgentFix`,
`RunnerAgentOpsTest`, `CodeToolsTest.testConflictFixPromptNamesTheBase`,
`UnifiedAgentRowBranchTest.testConflictedRowsAndThePullAgent`; iOS
`RemoteViewModelsTests` (conflict flag, `ConflictFixModel`),
`AgentControlTests.testThePullAgentIsFoundByNumberOrBranch`,
`CollabMergeTests.testConflictFixPromptNamesTheBase`.

## Adjusting the desktop's agent queue from the phone (2026-09-30)

The desktop runs at most N agents at once (Settings, or the "3/5" queue button
on the Agents tool bar); the rest wait as `queued` and start in priority order
as slots free up. Until now only the desktop could change N or the order, so a
phone could watch a long queue but not do anything about it except stop runs.

### Desktop

- `agents.list` carries `queue: {running, limit, limitMax, queue: [ids]}`
  (`agentQueuePayload` in `MainWindowAgentLaunch.cpp`). `queue` lists the
  desktop's own queued sessions (not cloud, external or deleted) in the order
  they will start, first next (`agentQueueOrder`: the queue sorted by priority,
  ties in queue order, the same order `processAgentQueue` uses). `limitMax` is
  20, or the current limit when Settings holds a higher one.
- `agents.queue` answers that object on its own.
- `agents.queue.limit {limit}` (1 to 99, else `bad_limit`) calls
  `setAgentConcurrencyLimit`, the same path as the tool bar menu: a lower
  limit parks the newest running agents back at the head of the queue.
- `agents.queue.move {id, to}` moves a queued session to zero-based place `to`
  (past the end means last), else `not_queued` (`moveQueuedAgentSession`).
  Run order is the session's 1..99 `priority`, so a move rewrites only the
  moved session's priority: strictly between its new neighbours when there is
  room (so the order survives a restart, which rebuilds the queue oldest first
  within a priority), else equal to the neighbour it lands next to, and it is
  placed beside that neighbour in `m_agentQueue`. The priority chip in the
  desktop's list changes to match. Priorities of other sessions are left
  alone, so the per-repository drag order on the desktop is not rewritten.
- Both writes are logged with the device that asked, answer the new queue
  object, and ring `agents.changed`. `status.capabilities` gains
  `agents.queue`; the phones instead show the controls only when
  `agents.list` has a `queue` object, which older desktops never send.

### Phones

- iOS: `LocalAgentQueue` (`FMChat/LocalControl/LocalControlService.swift`);
  `AgentControlCenter.setQueueLimit`, `moveQueued`, `queuePosition`,
  `queuedRows`; `Features/Agents/AllAgents/DesktopQueueViews.swift`:
  `DesktopQueueMenu`, a numbered-list icon with "running/limit" (for example
  "2/5", running added 2026-10-02 so the count shows without opening it) in the
  Agents tab bar's tools and the All agents toolbar (it replaced the full-width
  "Desktop queue" card on 2026-10-01 to give the list its row back). Tapping it
  drops down the summary ("2 of 5 running · 3 waiting") and "Run N agents at
  once" for 1 to max(limit + 5, 10), capped at `limitMax`; a pick is sent at
  once and a refusal shows an alert. Its last item ("N waiting" or "Open
  queue") opens `DesktopQueueSheet`: a
  stepper "Run N agents at once" (sent 600 ms after the last tap, snapped back
  if refused), the waiting agents in order with Reorder (drag), swipe to top,
  swipe to stop, and a menu with top, up, down, bottom and Stop. A queued
  agent's run screen and "On the desktop" card show "Waiting — #k in queue"
  with Move to top (`DesktopQueuePositionLine`). A move shows at once and is
  undone by a refresh if the desktop refuses it.
- Android: `LocalAgentQueue` and `QueueMove` (`control/LocalControlService.kt`);
  `AgentControlCenter.setQueueLimit`, `moveQueued`, `queuePosition`;
  `feature/agents/AgentQueueViews.kt`: a header row in "On <desktop>" on the
  Agents and Desktop tabs opens a "Queue on <desktop>" sheet with a −/+
  stepper and top, up, down, bottom and Stop per waiting agent. Queued rows
  read "Waiting — #k in queue" with a move-to-top button, and
  `UnifiedAgentScreen` shows the same line.
- Tests: desktop `test_window_resize.cpp` ("a phone reorders the desktop's
  agent queue and reads its limit"); iOS `FMChatTests/LocalControlTests.swift`
  and `FMControlTests/AgentControlTests.swift`; Android `LocalControlTest`
  `testDesktopQueueParsesAndToleratesMissingFields` and
  `testDesktopQueueLimitAndOrderAreChangedFromThePhone`.

## Moving a phone agent to the desktop

Added 2026-09-28. An agent running on the phone itself (the on-device agent,
`AgentScreen` on both phones, on its own `agent/<id>-<slug>` branch of a local
clone) could only finish there or become a pull request. Now its screen has
**Continue on desktop** (the desktop icon in the top bar) once the run has
started, as long as the repository is published to ForkMesh.

1. **Phone.** After a confirmation, the phone stops the run if it is still
   going and waits for its clean-up, commits anything the agent left
   uncommitted on its branch ("Work in progress, moved to the desktop") and
   returns the clone to the base branch. It then pushes the branch to ForkMesh
   with the same push the pull-request offer uses.
2. **Desktop.** The phone asks the desktop over local control (LAN or relay)
   with `agents.handoff {owner, name, branch, base, task, title}`. The desktop
   (`MainWindowLocalControl.cpp`) checks the branch, fetches it from the
   repository's served mirror or clone URL (`fetchServedBranch`, shared with
   the cloud handoff in `MainWindowAgentSync.cpp`), and starts an agent on that
   branch with `startAgentOnExistingBranch`, so the desktop's commits land on
   top of the phone's. It answers `{id}` and logs who moved it.
3. The phone shows "Continuing on studio as agent #42, on agent/…" and hides
   the action for that branch. The desktop agent is in the Agents tab's
   "On <desktop>" list like any other.

`task` is the continuation prompt (`AgentDesktopHandoff.task`, iOS
`FMAgents/AgentDesktopHandoff.swift`, Android `agents/AgentDesktopHandoff.kt`):
it names the branch and base, repeats the original prompt (up to 3000
characters), then lists what happened on the phone: follow-up instructions,
the agent's replies, each tool call with its path or command, tool errors,
and how the run ended. Only the newest steps that fit are kept, so the whole
prompt stays under 7600 characters, below the desktop's 8000 limit.

The desktop only accepts a branch under `agent/` whose name passes
`forkmesh::localcontrol::handoffBranchValid` (`LocalControlHandoff.h`: no `..`,
no leading dash or dot, no `.lock`, no revision syntax), and a base that passes
`handoffRefNameValid`. It advertises `agents.handoff` in
`status.capabilities`.

| Desktop answer | Phone says |
| --- | --- |
| `unsupported` (older desktop) | Update ForkMesh on the desktop to move agents to it. |
| `no_checkout` | The desktop has no working copy of owner/repo. Clone it there first. |
| `branch_not_found` | The desktop could not fetch the branch from ForkMesh; try again. |
| `not_started` | The desktop could not start the agent, then the desktop's `message`. Without one: its Agents list says why. |

Since 2026-10-03 a `not_started` answer also carries `message`: the error
notice the desktop raised while trying to start the agent (for example "Branch
agent/9 is not on this node, so there is nothing for an agent to work on. Fetch
it first."). `flashMessageImpl` keeps the last `local` error notice in
`m_lastLocalOperationError`; `agents.start`, `agents.handoff` and
`agents.takeover` clear it before starting and send it with
`LocalControlServer::respondError(…, error, message)`. Both phones keep it on
`LocalControlException` (`message` on iOS, `reason` on Android) and show it
after "The desktop could not start the agent." in place of "It says why in its
Agents list", so the reason shows on the phone as well as on the desktop. A
desktop that sends no message keeps the old wording.

On any failure the run stays on the phone, and the action stays offered. A push
the server rejects stops the move before the desktop is asked.

## Pull to refresh on an agent detail

Since 2026-09-28 every agent detail on both phones reloads when pulled down,
with the platform's refresh spinner, the same gesture as the Agents list.
Previously only the iOS `AgentRunScreen` had it; the others waited for a poll
or a Refresh button.

| Screen | iOS | Android | A pull reloads |
| --- | --- | --- | --- |
| All agents run | `AgentRunScreen` (already had it) | `UnifiedAgentScreen` | the control center's rows, then the transcript; restarts the follow loop if the run had gone idle |
| LAN desktop agent | `LocalAgentScreen` | `LocalAgentScreen` | the desktop's Agents list (status line) and the transcript |
| Relay desktop session | `AgentSessionDetailScreen` | `AgentSessionDetailScreen` | the pushed transcript |
| Queued session for a desktop (cloud and runner sessions open the All agents run screen) | `LaunchedAgentSessionScreen` | `LaunchedAgentSessionScreen` | the session and its pull requests, as the Refresh button does |

The on-device agent (`AgentScreen`) runs on the phone itself, so it has
nothing to fetch and no pull.

## Live run terminal on a queued cloud session

Removed 2026-10-06 together with the step card: every run with a session id
opens the run screen instead ([runner agents](RUNNER_AGENTS.md), "Every run
opens the desktop agent's page"). What follows describes the page as it was.

Added 2026-09-28. After **Start agent** the phone lands on
`LaunchedAgentSessionScreen`, whose five-step card (Prompt, Mirror, Branch,
Pull request, Merge) said how far the run had got but not what it was doing,
and only moved when Refresh was pressed. Under the card both phones now draw a
terminal-style log of the whole request that follows the run until it ends.

- **Lines** (`AgentRunLog`, `FMApi/Models/AgentRunLog.swift` and
  `core/.../agents/AgentRunLog.kt`, built from `AgentTaskChain`): a
  `$ forkmesh agent run` line naming repo, provider, model, target and PR; each
  step the chain has reached as `[n/5] Label · detail` (green done, cyan with a
  spinner while active, red failed); the opening prompt; the relay's
  diagnostic; then the agent's output. Output is every later history turn
  (follow-up prompts as `› @author`, transcripts line by line, `▸` tool calls
  in blue) plus, while the session is `running`, the relay's `live.text`
  ([live transcript](../agents/README.md), "Live transcript on the web").
  Then the finish (`✓ agent finished in 3m 12s`, `✗ <lastError>`, `■ run
  stopped`) and the pull request and merge steps. Output is capped at the last
  400 lines, a prompt at 6. Lines with a known time carry an `HH:mm:ss` stamp.
- **Window**: a fixed 300 pt/dp dark pane with a title bar (`provider@node:
  owner/repo`), an elapsed clock, a `LIVE` / `EXIT 0` / `EXIT 1` / `MERGED` /
  `STOPPED` badge and a copy button. It scrolls on its own, keeps to the
  bottom as lines arrive and shows a blinking cursor while the run is active.
  iOS honours Reduce Motion.
- **Following**: `AgentLaunchSession` now parses `live` (`liveText`,
  `liveAtMs`). The account socket's `cloud-agents` topic (claim, branch,
  finish) and, while the session is active, `cloud-agents-live` (a node's live
  report, at most every 2.5 s) re-read the session. iOS rings
  `AgentListSignal.liveRings`, which the Agents list does not observe;
  Android has `AgentsFeature.liveSignal` beside `listSignal`. Reads are
  coalesced (one in flight, one queued), a follow read fetches the pull list
  only when status, branch, PR or merge moved, and a failed follow read shows
  no error. Nothing polls: with the socket down the log waits for its
  reconnect ring or a pull to refresh.
- Codex runs send no live report, so their output appears when the run ends.
- Since 2026-10-06 a session with an id opens the run screen instead, so this
  page is left for a launch the relay has not given an id yet
  ([RUNNER_AGENTS.md](RUNNER_AGENTS.md#every-run-opens-the-desktop-agents-page-2026-10-06)).

## Formatted transcript and branch diff on the run screen

Added 2026-09-28. The All agents run screen (iOS `AgentRunScreen`, Android
`UnifiedAgentScreen`), which the Agents tab's "On <desktop>" rows
also open, drew the desktop's `transcript.txt` as one block of small
monospaced text, and its diff sat in the "Desktop actions" menu on iOS.

- **Blocks.** `AgentTranscriptFormat.blocks` (iOS
  `FMControl/AgentTranscriptFormat.swift`, Android
  `control/AgentTranscriptFormat.kt`) splits the transcript into the kinds of
  line the desktop writes (`MainWindowAgentEvents.cpp`, `AgentRunner.cpp`):
  `==>` lines become one grey status group, `!!` a red error, `> ` (and
  the lines under `==> User steering prompt`, up to a blank line or the next
  `==>`, `!!` or tool line) a "You" bubble, `[Tool arg]` a tool chip. The
  agent's markdown becomes headings, bullet and numbered lists (indent sets
  the depth), fenced code and `|` tables as monospaced boxes that scroll
  sideways, `---` rules and paragraphs. An unclosed fence still shows while
  the agent is writing it.
- **Inline.** `AgentTranscriptFormat.inline` marks `**bold**`, `` `code` ``
  and `[label](https://…)` links; other `[..](..)` stays plain.
- **Drawing.** iOS `AgentTranscriptBlocksView`, Android `transcriptBlocks`
  and `TranscriptBlockView` (`feature/agents/AgentTranscriptBlocks.kt`), one
  lazy row per block, text selectable. Android now scrolls to the newest
  block as the transcript grows, as iOS already did; it parses off the main
  thread.
- **Blank until scrolled (2026-09-29).** iOS pinned the newest text by
  scrolling to a spacer placed after the `LazyVStack`, in the same update
  that swapped in the new blocks. The spacer's offset came from the lazy
  rows' estimated heights, so the jump could land past the real text and
  the screen stayed blank until a manual scroll re-measured the rows. Each
  block row now carries its index as its id, and `AgentRunScreen` scrolls
  to the last row (bottom anchor) on the next main-queue turn, after the
  new rows are laid out. Android's `LazyColumn` already scrolls to the last
  item and was not affected.
- **Branch diff.** `UnifiedAgentRow` carries `branchName` and `baseBranch`
  (a desktop session's `branchName` and `diffBase`, a node run's
  `branchName` and `baseBranch`). Under the status line a "View branch diff"
  row shows `branch → base`: a desktop agent opens `AgentDiffScreen`
  (`agentops.diff`, committed and uncommitted changes); a node run with a
  branch opens `BranchDiffScreen` on the mirror. Android's "Changes" button
  gave way to this row; the iOS menu entry stays.
  On iOS the row became a top-right toolbar button on 2026-10-01 (asked
  from the phone: "make the view diff section a button on the top, right"),
  first in the bar, so the transcript starts right under the status line;
  `branch → base` and the file and line counts are its VoiceOver value.
  The same day the run screen's toolbar stopped folding into a "more"
  dropdown (asked from the phone: "make these buttons expanded horizontally
  so there is no dropdown"): a `ToolbarItemGroup` with more items than fit
  collapses the extras into an overflow menu that shows "View branch diff",
  Terminal and Continue as text rows, so the buttons now sit in one
  `HStack` inside a single `ToolbarItem`, `.fixedSize()`, each label
  `.labelStyle(.iconOnly)` (set per label, not on the stack, so the Desktop
  actions and runner menus keep their text rows).

## Task notifications and rich transcripts on the run screen

Added 2026-09-29, asked from the phone: "make sure we are showing the task
notification and any type of structured data from the agent nicely
formatted on the mobile detail screen." Claude Code writes a background
agent's or command's completion into the session as a user entry holding a
`<task-notification>` block (`origin.kind: "task-notification"`). The
desktop's `AgentLogText` treated it as a typed prompt, so the phone showed
the raw XML in a "You" bubble. Markdown tables showed as pipes in a
monospaced box.

### Transcript text format

The feed stays plain text, so an older phone shows every new line as
readable text and no field was added to `agents.transcript`. A newer phone
recognises these lines (older phones read them as the notes in brackets):

| Line | Meaning | Older phone |
|---|---|---|
| `==> <text>` | status line | grey status |
| `!! <text>` | error | red error |
| `> <text>` (to a blank line) | your prompt | "You" bubble |
| `> ⟵ <name>: <text>` | a message sent into the session, `ForkMesh (phone)` for a phone steer, another session's name otherwise | "You" bubble with the prefix |
| `[<Tool> <arg>]` | tool call | tool chip |
| `▸ <Tool>: <arg>` or `▸ <Tool>` | a runner's tool call (`claudeToolSummary` in `server/agents.go`); the phone drew these as plain prose until 2026-10-08, so a long run read as an unbroken wall of commands | prose |
| `==> Task <status>: <summary>` then lines indented by four spaces (blank lines allowed between them) | task notification and its result | status line, then the result as indented text |
| anything else | the agent's markdown | markdown subset |

- **Task line.** `AgentLogText::taskNotification`: `<status>` lowercased
  (`completed`, `failed`, `killed`, …; a missing, unexpected or `cost`
  status becomes `update`, so it never reads as the run's `==> Task cost:`
  line; Monitor events carry no status), `<summary>` on one line, the body
  is `<result>`, or `<event>` for a Monitor event. `&lt;`, `&gt;`, `&amp;`,
  `&quot;` and `&#39;` are unescaped. A body over 1,500 characters is cut at
  the last line break after 1,000 characters (else at 1,500), an open code
  fence is closed, and `…more (N characters not shown)` follows. `task-id`,
  `tool-use-id`, `output-file`, `note`, `usage` and `worktree` are left out
  (paths on the desktop mean nothing on the phone). Several blocks in one
  entry each get a line.
- **Where it comes from.** Outside Claude Code sessions: `claudeEntry` for a
  user entry whose text starts with `<task-notification>`, string or text
  block. ForkMesh's own Claude runs: `applyTranscriptEvent` now passes
  stream-json `user` events to `AgentLogText::userEvent`, which keeps only a
  task notification (tool results and a subagent's events, with a
  `parent_tool_use_id`, stay out).
- **Peer messages.** A phone steer was written as a plain `> text` and
  another session's as `> (from a-13) text`; both are now `> ⟵ <name>:
  <text>`, so the phone can tell a steer from a typed prompt. A peer entry
  without a name stays `> text`. System reminders, slash-command echoes,
  meta and sidechain entries are still dropped.
- **Markdown** in replies was already passed through unchanged; a test now
  holds it to that.

### Phones

`AgentTranscriptFormat` (iOS `FMControl`, Android `control`, the same logic
line for line) gained these blocks:

- `task(status, summary, body)` with a tone: completed/done/success green,
  failed/killed/error/cancelled/stopped/timeout red, anything else
  (running, update) amber.
- `steer(from, text)` for `⟵` prompts; `from` starting with `ForkMesh` is
  the user's own.
- `tools([name, detail])`: consecutive tool calls, across blank lines,
  form one run, replacing the single `tool` block.
- `table(header, rows, align)` when the second `|` line is a separator
  (`---`, `:--`, `--:`, `:-:`); `\|` and pipes inside backticks stay in the
  cell, short rows are padded. Without a separator the lines stay a code
  box.
- `json(pretty)`: a fence tagged `json`/`jsonc` or untagged, or a
  paragraph starting with `{` or `[`, that parses as JSON, re-indented by two
  spaces with its key order kept (`TranscriptJson.pretty`, a small
  validator and printer, depth 64, 200k characters). Invalid JSON stays a
  code box.
- Inline gained `*italic*`/`_italic_`, `__bold__` and bare `http(s)://`
  links; `snake_case` and `2 * 3` stay plain.

Drawing (iOS `AgentTranscriptBlocksView.swift`, Android
`AgentTranscriptBlocks.kt`), still one lazy row per block:

- **Task card**: a rounded card tinted and outlined in the tone colour, a
  status icon (`check_circle_outline`, `error_outline`, `timelapse`),
  "Task completed" above the summary as its title, and a "Show result"
  toggle. The result is parsed only when first opened and drawn with the
  same block views.
- **Prompts and steers**: right-aligned "You" bubbles (a steer reads "You ·
  ForkMesh (phone)"); another session's message is a grey left bubble "From
  a-13".
- **Tool runs**: dimmed chips (tool name and argument in small mono on a
  faint overlay). A run of more than three folds to "12 tool calls · Bash
  ×5 · Read ×4 · Grep ×3" plus the latest chip; tapping expands it. Tapping
  a chip shows its whole argument, pretty-printed when it is JSON.
- **Tables**: a real grid in a horizontal scroll. Each column is as wide as
  its widest cell up to 260 pt/dp (longer cells wrap), the header row is
  bold on the overlay colour, cells follow the column's alignment, and
  hairlines separate cells. Android measures with a custom `Layout` from the
  cells' intrinsic sizes; iOS measures the text with `UIFont` and lays out a
  `Grid`.
- **JSON**: a "JSON" box, monospaced and scrolling sideways; over 12 lines
  it shows the first 8 and "Show all N lines".
- **Code**: the fence's language above a monospaced, non-wrapping box that
  scrolls sideways.
- **Headings** of level 3 and deeper are bold body text.

**Multi-line tool calls and a compact layout (2026-09-30).** A shell
command with line breaks (a heredoc, a `\`-continued line) is written by
`AgentLogText::toolLine` as `[Bash <command>]` across several lines, capped
at 200 characters plus `…`. The phones used to see only one-line tool calls,
so such a call came out as a paragraph of raw command text. The parser now
opens a tool call at a line `[<Tool> <text>` (a valid tool name, no closing
`]`, no `](` link, after a blank line) and closes it at a line ending in
`…]`, or at a line ending in `]` followed by a blank line or the end. If
more than 300 characters go by without a close, or the text ends first, the
lines are read again as ordinary text; if a close was seen, the call ends
there and the rest is read again.

The transcript was tightened at the same time to save room on a phone:
8 pt/dp between blocks (was 12), 12 on the sides and 8 above and below the
list (Android was 16 all round), body text at the theme's snug line height
(was 1.4), bubbles and cards with 10/8 padding and a 32 inset (was 12 and
48), code and JSON boxes with 10/8 padding. A tool run is now one rounded
card with a hairline border and a row per call: an icon for the tool's kind
(terminal for Bash/shell, a page for Read, a pencil for Edit/Write, a
magnifier for Grep/Glob, a globe for web tools, a robot for Agent/Task, a
puzzle piece for MCP tools), the name in semibold mono and the argument on
one line; a multi-line argument shows its lines joined by ` ⏎ ` with a
chevron. Tapping a row wraps the whole argument inside the margins instead
of scrolling sideways.

**Incremental parsing.** `TranscriptBlockCache.update(text)` keeps the
parser's state after the last complete line; when the new text starts with
what it already read it feeds only the new lines, then finishes a copy for
the unfinished last line. Otherwise (a `reset`, or the follower dropping
the oldest text past its 200k-character limit) it parses from scratch. The
run screens (`AgentRunScreen`, `UnifiedAgentScreen`) now use it, so each
poll or bell costs the new lines, not the whole transcript. The tests check
that it matches a full parse at every step.

**Shared fixture.** `conformance/agent-transcript/v1/`: `session.jsonl`,
short excerpts of this project's own Claude Code sessions (a runner-fix
agent's completion with a long result, a failed background command, a
Monitor event, a phone steer, another session's message, a reply with a
table and a JSON fence), with paths, IPs, ids and a teammate's name
scrubbed; `transcript.txt`, what `AgentLogText` renders from it; and
`blocks.txt`, one line per block both phones must parse from it.

## Streaming transcripts and a faster channel (2026-09-30)

Before this change the phone heard about new output only through the
`agents.changed` bell. The desktop rang it 700 ms after the first change
(trailing), and the phone then made two round trips: the whole `agents.list`
(plus the relay's cloud list) and `agents.transcript` with `since`. For each
read the desktop re-read the whole `transcript.txt` from disk. A ForkMesh run
rang no bell at all when it wrote output, so iOS showed nothing new until the
status changed. Android polled instead: every 3 s on the All agents run screen,
and every 4 s on the LAN agent screen, where each read was the full transcript.

**`agents.follow` / `agents.output`.** The desktop now streams what it writes.

- `agents.follow {id, since}` answers with the same payload as
  `agents.transcript` plus `streaming: true`, and registers the follow on that
  connection.
- From then on the desktop pushes `agents.output` events with that payload
  (`id, transcript, from, next, reset, active, status, attachments`, plus
  `outside` for an outside agent). One goes out when the log grows or when
  `status` or `active` changes.
- `{id, error}` ends the follow; `unknown_session` means the agent is gone.
- `agents.unfollow {id}` ends a follow. All follows end with their connection.
  A connection holds at most 8; a ninth evicts the oldest idle one.
- `status.capabilities` lists `agents.stream`. An older desktop answers
  `unsupported`, and the phones then read on each bell as before (iOS) or
  keep polling (Android).
- Desktop code: `MainWindowLocalControl.cpp`
  (`handleLocalControlFollowRequest`, `pumpLocalControlFollows`),
  `LocalControlStream.{h,cpp}`.

**Desktop side.**

- *ForkMesh runs:* `AgentStore::setLogObserver` fires on every `appendLog`,
  queued to the GUI thread from whichever thread appended. The follow then
  reads only the new bytes (`readLogGrowth`).
  - The cursor counts QString (UTF-16) units, the same as `agents.transcript`'s
    `next`. So a UTF-8 character split across two writes waits for its second
    half, and every `\r` is dropped, as `readLog`'s text-mode read does.
  - A shorter file is read again from the start and sent with `reset`.
  - Growth over 64,000 characters in one read is sent as its newest 64,000 with
    `reset`. `agents.transcript` instead sends 16,000 and leaves a gap.
  - `attachments` is sent with the first answer and whenever the new text
    names an attachment path (`AgentStore::attachmentPathsIn`). Otherwise the
    key is left out and the phones keep the list they have, so a busy run does
    not rescan the log's last 128 KB on every read.
- *Outside agents:* while one is followed, a 200 ms timer checks its
  transcript's size, and the same tail code as `agents.transcript` reads it.
  The 2 s machine-agent tick still rings the bell for the list.
- *Pacing:* each follow is read at most every 15 ms on the LAN and every
  80 ms through the relay. A relayed follow also waits while the desktop's
  relay socket has frames queued (below). Output written between two reads
  arrives as one event.
- *The bell:* `agents.changed` now rings as soon as something changes, then at
  most every 300 ms. Before, it rang 700 ms after the first change. Each ring
  also re-checks every follow, which is how a status change without new output
  reaches a run screen.

**Phones.**

- `DesktopTranscriptStream` (iOS `FMControl/DesktopTranscriptStream.swift`,
  Android `control/DesktopTranscriptStream.kt`) does the following:
  - applies an event only when its `from` equals what the phone has;
  - drops an older (`from` behind) event;
  - turns a gap (`from` ahead) into a reset;
  - holds events that arrive before the follow's answer is processed (the
    desktop sends them right behind it) and replays them after it;
  - follows again from `next` when the hub's link changes (LAN to relay, or a
    reconnect);
  - retries from 1 s, doubling to 15 s, while no link is up.
- iOS `AgentRunScreen`, Android `UnifiedAgentScreen` and Android
  `LocalAgentScreen` follow this way and fall back to the old path on an older
  desktop.
- `AgentControlCenter` answers a bell with `refreshDesktop()`, which re-reads
  only the desktop half and keeps the node runs it already has. The cloud list
  has its own `cloud-agents` topic; before, every desktop bell also cost an
  HTTPS call for it.
- `DesktopControlHub.link()` dials all candidates (up to four) at once. The
  first to finish the handshake wins and the rest are closed. Before, it dialled
  them in turn, so a desktop that was off held up the next one for its whole
  20 s timeout.
- The hub tells `addLinkListener` subscribers when the link underneath
  changes.
- `keepWarm`, which the app sets while it is in the foreground (iOS
  `HostsFeature`, Android `AgentsFeature`), opens the relay link at once
  instead of on the first request. It also opens it again after a drop,
  waiting 2 s and doubling up to 60 s. It does nothing when the account has no
  desktop.

**Transport.**

- The desktop sets `TCP_NODELAY` (`QAbstractSocket::LowDelayOption`) on the LAN
  control socket (`LocalControlServer::adopt`) and on its relay socket
  (`NodeEventSocket`). The phones already set it. Without it, Nagle's
  algorithm and the other side's delayed ACK could hold a small frame
  40-200 ms.
- iOS `AccountEventChannel.send` now writes through one ordered outbox.
  Before, it started one `Task` per frame, which did not guarantee order, and
  a reordered chunk of a relayed frame ends the channel with
  `relay_out_of_order`.
- The relay's control budget went from 200 to 1000 messages per socket per
  10 s (`NODE_CONTROL_MSG_MAX_PER_WINDOW`), and the hello frame now
  advertises it as `controlBudget: {windowMs, max}`.
  - The desktop paces its relayed control frames to 90% of the advertised
    budget, or of 200 when a relay does not advertise one
    (`forkmesh::localcontrol::SendBudget`, a sliding window).
  - It queues a frame instead of letting the relay drop it: one dropped chunk
    kills the channel. The queue holds up to 16 MB, and a frame past that fails
    as before.
- The relay no longer awaits its traffic-counter flush (an occasional
  database write) before it forwards a control or terminal frame; the flush
  runs after.

**QUIC.** QUIC is used where the platform and the path allow it.

- *Used:* iOS sends every HTTPS request to `forkmesh.com` and its subdomains
  with `assumesHTTP3Capable` (`FMNet` `HTTP3Hosts`). This covers the event
  ticket, the account's desktop list and the rest of the API. The first request
  therefore goes over QUIC instead of waiting for TCP and TLS. The cores' Caddy
  serves h3 on UDP 443 (`curl --http3-only https://app.forkmesh.com/api/version`
  answered 200 on 2026-09-30).
- *Not used, on the LAN:* the control channel stays on TCP. On a LAN, TCP
  with `TCP_NODELAY` costs well under a millisecond. QUIC would need a QUIC
  server in the desktop (OpenSSL 3.5's server API) plus a QUIC client on
  Android, and Android has no first-party QUIC API: Cronet would be a
  third-party dependency (see [README.md](README.md), first-party dependencies
  only).
- *Not used, through the relay:* the channel rides the account event
  WebSocket through Caddy to the app. WebSocket over HTTP/3 (RFC 9220) is not
  available in `URLSessionWebSocketTask` or in the host.
- *Future options:* an iOS-only QUIC LAN transport, or WebTransport on the
  cores.

## Watch streams what the desktop writes (2026-09-30)

The phone Watch went quiet a few seconds after it opened. It read each run
once, played the last three beats, and then read again only on a bell: the
account's `agents` topic or the desktop's `agents.changed`. Since the desktop
began streaming (`agents.follow`, above), a ForkMesh run's output reaches a
phone only on a follow, and nothing in Watch followed. So the bodies stopped
moving while the agents kept working. On Android the main Agents header's
Watch also watched only the relay's sessions, so desktop runs the relay does
not list and agents started outside ForkMesh were missing. On both phones an
agent that started after Watch opened never got a seat.

Watch now works like the desktop's field:

- **Streamed.** Every desktop row Watch shows is followed with
  `agents.follow` through the control hub (LAN or relay), as the run screen
  follows it. `ControlWatchFeeds` (iOS `FMAgents/ControlWatchFeeds.swift`,
  Android `agentwatch/ControlWatchFeeds.kt`) keeps each row's streamed text in
  a `TranscriptFollower`. Each `agents.output` event rings the controller, and
  the row's `controlWatchTarget` reads from that text instead of asking the
  desktop again. `unknown_session` ends the row as done.
- **At most six follows per Watch.** The desktop holds eight follows per
  connection and silently drops the oldest idle one, so Watch leaves room for
  a run screen. A row past the limit, a row whose follow failed and an older
  desktop without `agents.stream` are rung every 3 s instead, while any row is
  in that state (`ControlWatchFeeds.lagging`). The ring reads only live runs.
- **Node runs.** Watch also rings on the account's `cloud-agents-live` topic
  (a node's live report), through `AgentListSignal.addLiveListener` on iOS and
  `AgentsFeature.liveSignal` on Android.
- **One read per node run, shared (iOS).** A node run has no stream, so every
  ring used to read the whole session again
  (`GET /api/repo/<o>/<r>/agent-sessions/<sid>`, full history). The Agents tab
  runs up to three watchers at once (the tab's dots, the dots bar and Home's
  radar), and each rings on every desktop delta and live report. The
  2026-10-01 performance report counted 80 reads of each of two node runs in
  five minutes, one every 3.75 s. `controlWatchTarget` now reads node runs
  through `AgentControlCenter.watchTranscript`, which shares one read
  (in flight or done) across every watcher. It reads again when the control
  center's row for the run changes (status, active, `updatedAtMs`) or after
  `cloudReadReuse` (30 s). A failed read is not kept. The run screen and the
  assistant still call `transcript(_:since:)`, which always reads.
- **Newly started agents.** `AgentWatchController.add` seats targets it does
  not already watch. The seat is announced and the run is read as on start.
  `ControlWatchRoster` re-reads the control center's rows whenever they change
  (iOS observes `rows`, Android `addListener`) and adds the new ones.
- **Every agent once.** The Agents tab's Watch (iOS `forAgentsTab`, Android's
  header button, now `AgentWatchEntry.openAll`) shows every running row of the
  control center: desktop runs, agents started outside ForkMesh and node runs.
  A running relay session is added only when the control center does not list
  it, matched by repository name and desktop id. So a desktop run in reach is
  streamed rather than read from the relay's snapshots, and is not drawn
  twice. With nothing running it replays the last three, as before. A single
  session's "Watch & listen" (iOS `forRelaySession`, Android `relayScreen`)
  also uses the control row when there is one.

## Tests

- iOS: `Tests/FMAgentsTests/AgentDesktopHandoffTests.swift` (moving a phone
  agent to the desktop); `Tests/FMAgentsTests/RemoteAgentDiffDeleteTests.swift`;
  `Tests/FMAgentsTests/RemoteAgentOpsClientTests.swift` and
  `RemoteApprovalsQueueTests.swift`;
  `Tests/FMControlTests/RelayControlTests.swift`
  `testWithNothingPairedTheHubDialsTheAccountsDesktops`;
  `Tests/FMControlTests/DesktopTranscriptStreamTests.swift` (the stream, an
  older desktop, and the dial race);
  `Tests/FMAgentsTests/ControlWatchFeedsTests.swift` and
  `Tests/FMAgentWatchTests/AgentWatchTests.swift`
  `testSeatsAnAgentThatStartsWhileWatching` (Watch streaming and new seats); `Tests/FMNetTests/HTTPClientTests.swift`
  `testForkMeshHostsAreAskedForHTTP3`;
  `Tests/FMControlTests/AgentControlTests.swift`
  `testAgentsStartedOutsideForkMeshListAndStreamTheirTranscripts` and
  `testOutsideClaudeCodeIsSteeredThroughTheDesktop`;
  `Tests/FMApiTests/AgentRunLogTests.swift` (the live run terminal);
  `Tests/FMAgentsTests/AgentLinesTests.swift`
  `testListViewCyclesCompactZenFullEagleAndReadsWhatWasStored`,
  `testFullChipsNameEachDetailInOneWordInColumnOrder`,
  `testEagleDialogIsTheLatestFinishedProseTheAgentSaid`,
  `testEagleDialogOfACloudRunPrefersItsLiveOutput` and
  `testEagleFeedHandsEachRunningDesktopRowItsLatestLine` (the list views and
  Eagle's ticker).
- Android: `AgentLineChipsTest` (the three list views and Full's chips),
  `AgentDesktopHandoffTest` (moving a phone agent to the desktop),
  `AgentRunLogTest` (the live run terminal), `AgentOpsTest` (`testDiffIsReadAndSplitIntoLines`,
  `Tests/FMApiTests/AgentRunLogTests.swift` (the live run terminal);
  `Tests/FMControlTests/AgentTranscriptFormatTests.swift` and
  `AgentControlTests` `testRowsCarryTheBranchTheirDiffCompares` (the run
  screen's blocks and diff link).
- Android: `AgentTranscriptFormatTest` and `UnifiedAgentRowBranchTest` (the
  run screen's blocks and diff link; task cards, steers, tool runs, tables,
  JSON, italic and bare links, the block cache and the shared
  `blocks.txt`, which iOS `AgentTranscriptFormatTests` checks too), `AgentRunLogTest` (the live run terminal), `AgentOpsTest` (`testDiffIsReadAndSplitIntoLines`,
  `testDeletePreparesThenSpendsTheToken`), `LocalControlTest`
  (`testWithNothingPairedTheHubDialsTheAccountsDesktops`,
  `testDesktopAgentsStartedOutsideForkMeshListAndStreamTheirTranscripts`,
  `testOutsideClaudeCodeIsSteeredThroughTheDesktop`),
  `DesktopTranscriptStreamTest` (the stream, an older desktop, and the dial
  race), `ControlWatchFeedsTest` and `AgentWatchControllerTest`
  `testSeatsAnAgentThatStartsWhileWatching` (Watch streaming and new seats),
  `MobileNodeApiTest`
  (`testAccountDesktopsListsKeysAndMachineNames`).
- Desktop: `tests/test_local_control.cpp` `testStream`
  (`forkmesh-local-control-tests`: `readLogGrowth` against a whole text-mode
  read, a split UTF-8 character, a rewritten log, and `SendBudget`).
- Server: `app/tests/test_node_event_push.py` (the hello's `controlBudget`,
  the control window).
- Desktop: `tests/test_local_control_handoff.cpp`
  (`forkmesh-local-control-handoff-tests`, the branch-name checks).
- Desktop: `tests/test_agent_log_text.cpp` (`forkmesh-agent-log-text-tests`,
  Claude Code and Codex JSONL entries as transcript lines, peer steers, task
  notifications and their clipping, a run's stream `user` events, markdown
  kept as written, and `conformance/agent-transcript/v1` rendered to its
  `transcript.txt`) and
  `tests/test_machine_agent_scan.cpp` (a Codex rollout held open);
  `tests/test_claude_peer_messaging.cpp`
  (`forkmesh-claude-peer-messaging-tests`: the shared fixture, record
  discovery with a stale pid, a recycled pid, a missing socket and an old
  protocol, and delivery over a real socket with the endpoint check).
- Python: `app/tests/test_claude_peer_message.py` (the reference against the
  fixture, discovery and delivery).
- Desktop: `tests/test_forkmesh_term.cpp` (`forkmesh-term-tests`, plain
  C++ driving the built `forkmesh-term` under a pty: argument parsing, text
  and label cleaning, bracketed-paste tracking, the rate limiter, request
  framing, the private folder, passthrough, resize, exit codes and signals,
  registration and cleanup, injected bytes with and without bracketed
  paste, a paste that tries to close itself, Esc and Ctrl-C, `--no-remote`
  and the Ctrl-^ toggle, the typing-at-the-keyboard guard, the per-minute
  cap, and a non-terminal stdin) and `tests/test_terminal_handoff.cpp`
  (`forkmesh-terminal-handoff-tests`: cleaning and framing match the
  wrapper, discovery refusing loose permissions, foreign sockets, wrong
  names and recycled pids, ancestor matching, `TypingGate` and `admit`,
  typing and keys into a real `forkmesh-term`, and over a real
  local-control handshake an unpaired phone refused, a forged signature cut
  off, and a paired phone typing exactly its own text, rate limited).
- iOS: `FMControlTests/AgentControlTests.swift`
  (`testForkmeshTermAgentsAreTypedIntoAndInterrupted`,
  `testOutsideRowDecodingDefaultsToNoTypingOrStop`).
- Android: `core/src/test/.../control/LocalControlTest.kt`
  (`testOutsideTerminalsTakeTypedPromptsAndInterrupts`,
  `testTerminalTypingErrorsReadAsSentences`).
- Server: `app/tests/test_mobile_node_devices.py`
  `test_account_desktops_lists_enabled_desktop_keys_for_a_phone`.

## Paused desktop agents in `agents.list` (2026-09-30)

An agent created through the MCP connector with `paused`
([../tools/mcp-create-agent.md](../tools/mcp-create-agent.md)) is a `queued`
session the desktop will not start until the user does
([desktop/agents-ui.md](../desktop/agents-ui.md)). Its `agents.list` row
carries `held: true`; the key is absent on every other row. It is not in
`agents.queue`, and `agents.queue.move` answers `not_queued` for it. The phones
do not read `held` yet, so they show such a row as queued.

## Starting an agent in a desktop terminal from the phone (2026-10-01)

The desktop's Agents tab has a Claude Code and a Codex button that open the
provider's CLI in a terminal on the open repository
(`openAgentProviderTerminal`). The phones can now do the same on the paired
desktop, over the LAN or the relay: the desktop starts the CLI in a terminal
of its own, the phone shows that terminal live and types into it, and the
agent is listed as a job in the Agents list until its CLI ends.

### Using it

- iOS: the terminal icon in the Agents tab bar opens "Agent in a terminal":
  pick a repository the desktop has open and Claude Code or Codex, then
  Start. Android: "Terminal" in the Agents header.
- The screen is the same terminal as the provider sign-in one: the live grid,
  Esc, Tab, arrows, Enter, Backspace, Ctrl-C, Paste, and a line that sends
  text then Enter. Leaving the screen leaves the agent running; End (with a
  confirmation) closes it.
- The agent shows up in the Agents list. Its run screen has Terminal, which
  opens the live terminal again from any phone paired to that desktop; its
  composer types into the terminal and Stop presses Esc, as for any
  `forkmesh term` session.

### Protocol (`terminal.` local control family)

Owner-only, like the other families (`registerLocalControlFamilies`).
`status.capabilities` lists `terminal.agents` on builds with a pty (not
Windows); without it the phones hide the entry points.

A terminal object is `{terminal, provider, label, cli, repositoryOwner,
repositoryName, cwd, startedAtMs, device, running, cols, rows}`. `terminal`
is an opaque id (32 hex characters); `provider` is `claude-code` or `codex`;
`label` is "Claude Code" or "Codex"; `device` is the label of the phone that
started it.

| Request | Params | Answer |
| --- | --- | --- |
| `terminal.start` | `owner`, `name`, `provider`, `cols`, `rows`, optional `mode` and `account` ([accounts](#accounts-usage-and-terminals-from-the-usage-sheet-2026-10-01)) | terminal object, `replay: ""` |
| `terminal.attach` | `terminal`, `cols`, `rows` (both optional) | terminal object, `replay` |
| `terminal.input` | `terminal`, `data` (base64, at most 16 KiB decoded) | `{}` |
| `terminal.resize` | `terminal`, `cols`, `rows` | `{}` |
| `terminal.detach` | `terminal` | `{}` |
| `terminal.close` | `terminal` | `{}` |
| `terminal.list` | | `{terminals: [terminal object]}` |

- `start` and `attach` attach the connection: from then on it receives the
  terminal's events, until `detach`, `close`, the CLI's exit or the
  connection's end. Several phones may attach to one terminal; the newest
  `start`, `attach` or `resize` sets the size (cols 20–500, rows 5–200).
- `replay` is base64 of the last 512 KiB the terminal wrote, cut at a line
  start, so a phone that comes back redraws the screen and its scrollback
  from it. 512 KiB (683 KiB as base64) is the most that fits, with the JSON
  envelope and sealing, under the phones' 1 MiB relay frame; the attach
  answer and a `reset` event each carry the whole replay in one frame.
  128 KiB until 2026-10-01, which left only the last screens of a Claude Code
  session to scroll back through.
- The phone emulators keep 10,000 rows of scrollback (`maxScrollback` on
  iOS, `TERMINAL_MAX_SCROLLBACK` on Android); 1,000 until 2026-10-01.
- Errors: `unknown_repository` (not open on the desktop),
  `unsupported_provider`, `cli_missing` (the CLI is not installed),
  `guardrails_on` (the repository's agent guardrails forbid a free-form
  terminal, as on the desktop), `terminal_vm` (agents run in the KVM guest),
  `terminal_limit` (six terminals already run), `terminal_failed` (no pty),
  `unknown_terminal`, `bad_input`, and `unsupported` on Windows.

Events, to attached connections only:

- `terminal.output {terminal, data, reset}`: base64 of what the CLI wrote,
  coalesced (at most every 15 ms on the LAN, 80 ms through the relay, and not
  while the relay socket has frames queued), at most 16 KiB per event.
  `reset: true` means the phone fell more than 256 KiB behind: clear the
  screen and feed `data`, which is the replay.
- `terminal.exit {terminal, code}`: the CLI ended (its exit status, -1 when
  it was killed). The terminal is gone; `attach` answers `unknown_terminal`.

### In the Agents list

Each running terminal is an outside row in `agents.list` carrying
`terminal: <id>`:

- The desktop runs the CLI under `forkmesh term` (`forkmesh term
  /path/to/claude`, the app's own binary), so it registers like any wrapped
  terminal session. The machine-agent scan, which skips the desktop's own
  processes, treats the terminal's process as a root of its own
  (`MachineAgentScan::scan`'s `foreignRoots`), so on Linux the CLI is found
  within one 2 s tick and its row has the transcript, `typeable` and
  `stoppable` of any `forkmesh term` session. The desktop marks that row
  with `terminal` by matching the wrapper's registration pid to the
  terminal's pid.
- Until then, and on macOS where there is no process scan, the desktop
  lists the terminal itself: a negative id, `kind` `claude` or `codex`,
  `title` the repository folder, `typeable` and `stoppable` true. For that
  row `agents.prompt` writes the text into the pty (bracketed paste when the
  CLI asked for it, then Enter 120 ms later) and answers
  `{"status":"typed","via":"terminal"}`; `agents.stop` writes Esc and
  answers `{"status":"interrupted","key":"esc"}`; `agents.transcript` and
  `agents.follow` answer `no_transcript`.
- `agents.changed` rings when a terminal starts or ends.

### Desktop

- `AgentLoginPty::run` (the sign-in pty, generalised) starts a program with
  arguments in a working directory; the sign-in keeps stripping provider API
  keys, an agent terminal keeps the desktop's environment like the Agents
  tab's terminal. A pty read error (`EIO`, what Linux answers once the child
  has gone) now ends the session like end of file.
- `LocalControlTerminals.{h,cpp}` holds the policy (ids, sizes, the replay
  cut, the event queue per phone) and `MainWindowLocalControlTerminals.cpp`
  the requests. Each start, input-free attach, close and exit is written to
  the desktop log with the phone and how it reached the desktop.
- A terminal lives until its CLI exits, a phone ends it, or the desktop
  quits (the quit reaps it like every other pty, [lifecycle](../desktop/lifecycle.md)).
  Rebuild & restart therefore ends these agents.

### Phones

- The streamed sign-in terminal's emulator and views are shared: iOS
  `StreamedTerminal` (`FMAgents/AgentTerminal.swift`), which
  `AgentLoginTerminal` and the new `DesktopAgentTerminal`
  (`FMAgents/DesktopAgentTerminal.swift`) implement, so
  `TerminalStatusLine`, `TerminalGridView` and `TerminalKeyBar` draw either;
  Android `AgentTerminalSession` (`core/.../agents/AgentTerminal.kt`) with
  `DesktopAgentTerminal.kt` and the shared `AgentTerminalPane`.
- `DesktopAgentTerminal` starts or attaches, holds events that arrive before
  the answer and applies them after the replay, attaches again when the hub's
  link changes (retrying from 1 s up to 15 s), batches keys for 25 ms and
  sends `terminal.input` one at a time in order, and never retries a failed
  `start`, so a lost answer cannot start a second agent.
- Screens: iOS `Features/Agents/DesktopTerminalScreen.swift` (the start form,
  then the terminal; the tab-bar icon is hidden when `status` lacks
  `terminal.agents`, checked once per link by `DesktopTerminalSupport` in
  `FMControl/DesktopTerminals.swift`), Android
  `feature/agents/DesktopTerminalScreen.kt` (Android does not read
  capabilities; an old desktop's `unsupported` reads "Update it").
- Rows: `LocalOutsideAgent.terminal`, `UnifiedAgentRow.terminal` and
  `isPhoneTerminal`, whose summary reads "… started from your phone in a
  terminal on <desktop>. Open the terminal for full control; what you send
  here is typed into it." The run screens (iOS `AgentRunScreen`, Android
  `UnifiedAgentScreen`) show Terminal on those rows.
- Keyboard-mode sequences are not styles (2026-10-01). Claude Code turns on
  xterm's modifyOtherKeys with `CSI > 4 ; 2 m` and asks about the kitty
  keyboard protocol with `CSI > 1 u` / `CSI ? u`. The emulators (iOS
  `TerminalScreen`, Android `TerminalScreen`, web `agent-terminal.js`) read
  the first as plain SGR `4;2`, so everything after it was underlined and
  dimmed on the phone, and the `u` forms restored a saved cursor. A CSI with
  any prefix or intermediate byte is now dropped, except `?` on `h`, `l`,
  `J` and `K`. SGR colon sub-parameters are read too: `4:0` ends an
  underline, `4:3` (curly) draws a plain one, and `38:5:n` / `38:2::r:g:b`
  set colours, where before a colon made the whole group 0 (a reset) or 4.
- The iOS grid keeps to its columns (2026-10-01). Claude Code's terminal was
  cut off at the edges: `TerminalGridView` drew each row as one `Text` at its
  natural width, and Claude Code's symbols (`⏺ ⎿ ✻ ⏵`, rules and boxes) fall
  back to fonts whose advance is not SF Mono's, so a row could come out wider
  than the pane, widen the whole stack and push both edges out of view. Now
  every row is framed and clipped to the pane, a glyph that is not ASCII is
  kerned to exactly one cell (shrunk instead when it is over 1.25 cells, as
  emoji are), and the column count comes from a hidden 100-character `Text`
  ruler measured by SwiftUI itself rather than from UIKit's string size.

### Tests

- Desktop: `tests/test_local_control_terminals.cpp`
  (`forkmesh-local-control-terminals-tests`: providers, ids, sizes, the
  replay cut at a line and never inside a character, input decoding and its
  16 KiB cap, the per-phone outbox, its reset and event size, the terminal
  object and the agents-list row) and `tests/test_machine_agent_scan.cpp`
  (an agent under a phone terminal is found, under the wrapper or as the
  terminal itself). Checked by hand on 2026-10-01 with a harness around
  `AgentLoginPty::run`: the built app's `forkmesh term` wrapping a shell
  started in the requested folder, registered with the terminal's pid, took
  typed input and reported its exit status 7 when the pty closed.
- iOS: `FMAgentsTests/DesktopAgentTerminalTests.swift` and three tests in
  `FMControlTests/AgentControlTests.swift`. The SwiftUI screens were only
  parsed on Linux, not compiled.
- Android: `core/src/test/.../agents/DesktopAgentTerminalTest.kt` (against
  the real hub over the loopback relay channel) and three tests in
  `control/LocalControlTest.kt`.
- Not yet run: a phone against a real desktop.

### Security

Starting a CLI in a repository is as much power as the desktop's own
Claude Code button, and a terminal on it is as much as the person at the
keyboard: Claude Code runs `!` shell commands. So the family is owner-only
(the paired device must name the desktop's account), every frame is sealed
end to end on either transport, the command is fixed (the wrapper and the
provider's CLI, resolved by `AgentCliLocator`, never a shell or anything from
the request), the folder is a repository the desktop has open, guardrails
are honoured, and at most six run at once.

## Opening a desktop agent in its Claude Code terminal (2026-10-01)

The user asked for a button on the phone's agent detail screen that shows
the agent running in the Claude Code terminal. A ForkMesh desktop agent runs
Claude Code headless (stream-json, no screen), so there is no terminal to
mirror while it runs. The desktop's own answer is the session menu's "open in
a terminal" (`popOutAgentSessionToTerminal`): stop the headless run and
`claude --resume <conversation>` in the agent's worktree. The phone now does
the same, into a desktop terminal it watches and types into, as for
"Agent in a terminal" above.

### Using it

- The run screen of a desktop Claude Code or Codex agent (iOS
  `AgentRunScreen`, Android `UnifiedAgentScreen`) has **Terminal**. iOS shows
  it only when the desktop's `status.capabilities` lists `terminal.resume`;
  Android shows it always and reads an older desktop's refusal as "Update
  it".
- On a running agent it asks first ("Open in Claude Code's terminal?"),
  because the headless run is stopped; on an idle one it opens at once.
- The screen is the live terminal of "Agent in a terminal": Claude Code's
  own screen with the conversation so far. Type "continue" there to pick up
  stopped work. Leaving the screen keeps it running; tapping Terminal again
  (from any paired phone) reopens the same terminal; End closes it.
- While the terminal runs, the agent's composer types into it and Stop
  presses Esc there, so the conversation is never driven from two places.
  Continue answers `terminal_open`.

### Protocol

`terminal.resume {id, cols, rows}`, where `id` is the agent's desktop id
from `agents.list`, answers the terminal object of `terminal.start` with
`session: <id>`, `replay`, `stopped` (a running run was stopped) and
`resumed` (false when the agent never reached Claude Code, so the CLI opened
fresh in its folder). If a terminal for that agent already runs, it is
attached like `terminal.attach` instead of starting a second one.

- The plan is the desktop's `agentTerminalHandoff`: the provider's CLI
  resolved by `AgentCliLocator`, `--resume <session>` (Codex: `resume
  <thread>`), the agent's worktree (else the repository), and
  `CLAUDE_CONFIG_DIR`/`CODEX_HOME` of the agent account in use, which
  `AgentLoginPty::setEnvironment` passes to the pty. Its refusals now carry
  codes: `unknown_session`, `cloud_session`, `unsupported_provider`,
  `guardrails_on`, `cli_missing`, `no_worktree`; plus `terminal_vm`,
  `terminal_limit` and `terminal_failed` as for `terminal.start`.
- It runs under `forkmesh term` like every phone terminal. Unlike them it
  is not an outside row in `agents.list`: the agent's own row carries
  `terminal: <id>` instead, and the outside row the process scan would add
  for it is dropped.
- `agents.prompt`, `agents.stop` and `agents.continue` on that agent go to
  the terminal (`answerLocalControlTerminalAgent`, found by
  `localControlTerminalForRow` with a positive id).
- The desktop log reads "Opened agent <id> in Claude Code in a terminal in
  <folder> after stopping its run, requested by <phone>".
- An older desktop answers `unknown_terminal` (its terminal family takes any
  other method as a terminal id); Android shows that as `unsupported`.

### Tests

Desktop `test_local_control_terminals.cpp` (the capability, `session` in the
terminal object); Android `DesktopAgentTerminalTest.testResumeOpensTheAgentInATerminal`;
iOS `DesktopAgentTerminalTests.testResumeOpensTheAgentInATerminal` and
`testSupportReadsTheResumeCapability` (not run on Linux). Not yet run: a
phone against a real desktop.

## Stop on the run screen acts at once (2026-10-01)

Stop on an agent's detail screen no longer asks "Stop this run?" (or
"Interrupt this agent?" for an agent running outside ForkMesh) first. On iOS
`AgentRunScreen` and Android's queued-session detail in
`AgentSessionScreens.kt`, the tap stops (or interrupts) the run straight away,
the same as Android's `UnifiedAgentScreen` already did. Stopping keeps the branch
and its commits, and a stopped run can be continued, so the dialog only added a
tap. Drop, Delete and Continue on desktop still ask first because they discard
or move work.

## Model and mode on the follow-up and the main prompt (2026-10-01)

A desktop agent left in Plan could only be moved to Auto from the desktop,
and the phone never showed which mode a run used. The follow-up composer on
the run screen now carries the same two chips as the desktop's composer: the
model and the permission mode (Auto, Ask, Plan, Edit). The shell's main
prompt shows the mode chip, and a Mode section in its picker, whenever the
run goes to the desktop.

### Desktop

- `agents.list` rows carry `mode` (the session's stored label, empty for
  sessions older than per-session modes), and the reply carries
  `defaultMode`, what the desktop's own composer is set to.
- `agents.start` takes an optional `mode`, passed to `startAdHocAgentForRepo`
  as its mode override.
- `agents.prompt` takes optional `model` and `mode`.
  `applyLocalControlAgentChoice` stores them on the session before the
  prompt is sent, the way `applyComposerSelectionToAgentSession` does for
  the desktop's composer. The send path then switches the mode itself:
  `syncLivePermissionMode` changes a live Claude Code to Plan, Ask or Edit
  in place, and restarts it under Auto with the message (bypassing checks
  is only offered to a process launched that way). Codex reads the model
  and mode on every turn. A live Claude Code cannot change its model, so a
  new model drops the idle CLI between turns and the prompt relaunches the
  session on it, with the conversation carried over; mid-turn the new model
  applies from the next launch.
- Only the four labels are accepted, in any case; anything else is ignored.
  A model is only stored when `agentModelMatchesProvider` accepts it for
  the session's provider.

### Phones

- `AgentPermissionMode` (FMControl) lists the four modes with the desktop's
  descriptions and remembers the main prompt's choice under
  `agent/desktop/run-mode` (Auto until changed, as on a new desktop).
- `UnifiedAgentRow.mode`, `offersModelChoice` (desktop rows started by
  ForkMesh) and `offersModeChoice` (those whose provider is Claude Code or
  Codex). `AgentControlCenter.prompt(_:_:images:model:mode:)` adds the
  choices to `agents.prompt` for such rows only. Node rows steer through
  the relay, which takes neither, so they show no chips.
- `PromptComposer` gained a mode chip (`modeOptions`, `selectedModeId`,
  `onSelectMode`) drawn before the model chip. The run screen shows the
  session's mode, else the desktop's `defaultMode`, else Auto, and the
  session's model; both chips offer Claude models for a Claude Code session
  and Codex models for a Codex one.
- `QuickAgentLauncher.mode` and `takesMode`: the chip and the picker's Mode
  section appear only when the destination is the desktop, the one place
  the mode is honoured, and the desktop start sends it.

### Tests

- iOS: `FMControlTests/AgentPermissionModeTests.swift`, the mode and model added
  to `testControlsRouteToTheDesktopOrTheRelayQueue`, and the desktop case of
  `QuickAgentLaunchTests`.
- Desktop: `MainWindowLocalControl.cpp` was compiled with the app's flags.
  The SwiftUI screens were only parsed on Linux, not compiled.
- Android has neither chip yet.

## YOLO and No worktree on the main prompt (2026-10-01)

The desktop's composer has two small switches above the prompt: **vibe**
(merge a finished run into the default branch, push it, and delete the
agent, its branch and worktree) and the **worktree** box (untick it and the
next run works straight in the repository's folder, with no branch or
worktree). The phones' main prompt now has the same two, as tiny toggles at
the top of the composer card, above the "owner/repo · where" line: **YOLO**
and **No worktree**.

They show only when the run goes to the desktop. Cloud nodes and the phone's
own agent do not merge, push or clean up by themselves, and they always run
on a branch, so the switches would do nothing there.

### Behaviour

- **YOLO** is remembered (`agent/desktop/yolo`, off until turned on), like
  the desktop's `agents/composerVibe`.
- **No worktree** applies to the next run only and turns itself off once
  that run has started, like the desktop's worktree box re-ticking.
- No worktree is offered only for Claude Code and Codex, the providers the
  desktop can run in the checkout itself; the others always cut a worktree.
- A run with no worktree has no branch to merge, so while No worktree is on,
  YOLO is dimmed and not sent.

### Desktop

- `agents.start` takes optional `vibe` (bool, default false) and `worktree`
  (bool, default true). They go to `startAdHocAgentForRepo` as `vibe` and
  `directCheckout = !worktree`, so a phone-started run is stamped the same
  way as one started from the desktop's composer and takes the same finish
  path (`maybeAutoMergeForSession`). Older phones send neither and get the
  old behaviour.

### Phones

- iOS: `QuickLaunchRunOptions` (FMAgents) holds `yolo` and `worktree`;
  `QuickAgentLauncher.runOptions`, `takesRunOptions` (destination is the
  desktop), `takesNoWorktree` (and the model is Claude Code or Codex),
  `sendsYolo`, `toggleYolo()` and `toggleWorktree()`.
  `QuickLaunchDesktop.start` takes the options and `HubQuickLaunchDesktop`
  adds `vibe: true` and `worktree: false` to `agents.start` only when set.
  The toggles are drawn in `ShellComposerOverlay.runOptions`
  (`ShellPromptBar.swift`, identifiers `shell-prompt-yolo` and
  `shell-prompt-no-worktree`).
- Android mirrors it: `QuickLaunchRunOptions` and the same members on
  `QuickAgentLauncher` (core), `QuickLaunchBackend.startOnDesktop(…, options,
  images)`, `QuickSendState.toggleYolo`/`toggleWorktree`, and the
  `QuickRunOptions` row at the top of `QuickContextLine` (`QuickSend.kt`).

### Tests

- iOS: `QuickAgentLaunchTests.testYoloAndNoWorktreeGoToTheDesktopOnly` and
  `testNoWorktreeIsNotOfferedForAProviderThatAlwaysCutsOne` (`swift test`
  on Linux).
- Android: the same two cases in `QuickLaunchTest` (`:core:coreTest`).
- Desktop: `MainWindowLocalControl.cpp` was compiled with the app's flags.
- Android: `:app:compileDebugKotlin` builds the Compose row. The SwiftUI
  view was only parsed on Linux, not compiled.

## Accounts, usage and terminals from the usage sheet (2026-10-01)

The user called the sheet behind the usage bars on the iOS Agents tool row
"useless": it showed the meters and nothing else. They asked for everything
the desktop's account menu has (every Claude Code and Codex profile, a new
Claude Code or Codex terminal, all the usage, log in and log out) and for
the terminal to start with full permissions.

### Using it

The usage bars open "Accounts and usage" (`AgentAccountsSheet`, iOS). Per
provider:

- "New terminal" starts that provider in "Agent in a desktop terminal". The
  provider is already known, so the start form has no Claude Code / Codex
  choice (the user found asking again unnecessary); with one repository open
  on the desktop the terminal starts at once, with several the form asks
  only for the repository. The terminal button on the Agents screen still
  asks for both.
- Every account profile is a row: its name, the identity the CLI is signed
  in as (or "Not signed in"), and its usage windows (the live ones for the
  account in use, the last ones the desktop saw for the others). Tapping a
  row uses that account for new agents, as picking it in the desktop menu
  does.
- The terminal button on a row starts a terminal as that account the same
  way. Its
  menu has "Use for new agents", "New terminal as this account", "Log in…",
  "Log out…" and "Rename…". Log in and Log out open the CLI's own `auth
  login` / `auth logout` (Codex: `login` / `logout`) in a terminal on the
  desktop, streamed here, with a button for the sign-in link.
- "Add account" names a new profile, the desktop creates and selects it,
  and its sign-in opens at once.
- Runner accounts and the month's API spend stay at the bottom.

Opening a destination grows the sheet to full height; coming back reads
the usage again.

### Full permissions

A terminal started from a phone runs Claude Code with
`--dangerously-skip-permissions` and Codex with
`--dangerously-bypass-approvals-and-sandbox`, and so does
`terminal.resume`. The start form says so in the warning colour when the
desktop is new enough to do it. Repositories whose agent guardrails are on
still refuse a terminal (`guardrails_on`).

### Protocol

`status.capabilities` adds `terminal.accounts` (on builds with a pty) and
`agentops.accounts`. The phone offers per-account terminals, sign-in and
sign-out only with the first, and switch, add and rename only with the
second, so an older desktop is never asked to start a sign-in it would read
as an agent.

- `terminal.start` takes `mode` (`agent`, the default, `login` or `logout`)
  and `account` (a profile id; the account in use when absent; an unknown
  id is `unknown_account`). A sign-in needs no repository: it runs in the
  desktop user's home, without the `forkmesh term` wrapper, with provider
  API keys stripped as in the desktop's own sign-in, and is not listed in
  `agents.list`. The account's folder is passed as `CLAUDE_CONFIG_DIR` or
  `CODEX_HOME`, as the desktop's "Launch in system terminal" does; before
  this an agent terminal ignored a selected non-default account. The
  terminal object carries `mode` (sign-ins only) and `account`. When a
  sign-in ends the desktop reads that provider's usage again.
- `agentops.usage` account rows add `signedIn`, `identity` (the CLI's
  name and email, at most 120 characters) and `windows` (that account's
  stored windows, same shape as the provider's).
- `agentops.usage` also answers `activeProvider` (2026-10-02), the provider
  new agents use (`defaultAgentProvider()`); the phone's Agents usage chip
  shows only that provider's session window (`docs/native/NAVIGATION.md`
  §2.0.40).
- `agentops.accounts.select {provider, account}`,
  `agentops.accounts.rename {provider, account, label}` and
  `agentops.accounts.add {provider, label}` answer the new `agentops.usage`
  summary; `add` also answers `account`, the new profile's id, and selects
  it. Errors: `unsupported_provider`, `unknown_account`, `bad_request`
  (an empty label), `account_failed` (the profile folder could not be made).
  Each is written to the desktop log with the phone that asked.

### Code

- Desktop: `LocalControlTerminals` (`accountsCapability`, `validMode`,
  `cliArguments`, `accountEnvVariable`), `startLocalControlTerminal` and
  `endLocalControlTerminal` in `MainWindowLocalControlTerminals.cpp`, the
  `agentops.accounts.*` branch in `MainWindowLocalControlAgentOps.cpp`,
  `MainWindow::createAgentAccount` (split out of `addAgentAccount`), and
  `agentAccountSignedIn`, `agentAccountIdentity` and
  `agentAccountUsageWindows` in `MainWindowChat.cpp`.
- iOS: `UsageAccount` (`signedIn`, `identity`, `windows`),
  `AgentOpsClient.selectAccount` / `renameAccount` / `addAccount` and the
  matching `AgentUsageModel` methods; `DesktopAgentTerminal` `mode`,
  `account`, `isSignIn` and `taskLabel`; `DesktopTerminalSupport.accountsCapability`;
  `DesktopTerminalScreen` presets (provider, account) and its sign-in
  initialiser with the link bar; `AgentAccountsSheet`.
- Android does not have the sheet yet.

### Tests

- Desktop: `test_local_control_terminals.cpp` (modes, arguments, account
  variables) and `test_local_control_agentops.cpp` (account fields).
- iOS: `DesktopAgentTerminalTests` (a sign-in sends `mode` and `account`,
  an agent terminal sends neither; the capability) and
  `RemoteAgentOpsClientTests` (account fields and the three account calls),
  run with `swift test` on Linux. The SwiftUI views were only parsed on
  Linux, not compiled.

## Swipe left to delete an agent (2026-10-01)

An agent in the Agents tab list can be deleted from the list: swipe its row
left. The swipe deletes straight away, with no confirmation (changed
2026-10-01, see "Swipe delete does not ask"). The run screen's Delete button
still asks.

### Which rows can be swiped

`AgentLines.deletion(_:rows:)` (iOS) decides, and answers which delete applies:

- A desktop agent (`UnifiedAgentRow`, or a relay-pushed desktop session) goes
  to `agentops.delete` on the desktop. A relay-pushed
  session only qualifies when the desktop in reach lists the same repository
  and session id, so the delete cannot reach a different desktop.
- A runner or relay run with a session id goes to `cancelAgentSession`, the
  same as the run screen's Delete.
- Nothing while the agent is running, queued or waiting, and nothing for an
  agent started outside ForkMesh. A row also needs the client its delete uses
  (`AgentOpsFeature.client` or `RunnerAgentFeature.api`).

### iOS

The Agents tab list is a `ScrollView` and `LazyVStack`, not a `List`, so
`.swipeActions` does not apply there. `AgentSwipeToDelete`
(`Features/Agents/AgentSwipeDelete.swift`) is a horizontal drag that only
claims a drag 1.5 times wider than tall, so scrolling still works. A short
swipe reveals a red Delete button and keeps it open (one row at a time; a tap
on the row closes it) and a tap on it deletes; a long swipe, past 200 points,
deletes straight away. VoiceOver gets a Delete action on the row. After a delete the
list refreshes and a status line in the list says so (iOS: `AgentUndoDeleteLine`
at the top of the Agents tab list and of "All agents"; Android: under the
Agents tab's counts; no toast, 2026-10-09). The "All agents" screen, which is a
`List`, gets Delete next to Stop in its trailing swipe actions.

#### The swipe no longer steals scrolls (2026-10-02)

The list sometimes did not scroll when swiped. Every row carried a SwiftUI
`DragGesture` as a `simultaneousGesture`; since iOS 18 SwiftUI gestures are
UIKit recognizers, and a drag in a `ScrollView` can win the touch from the
scroll view's pan, so a vertical swipe that started on a row went nowhere.
On iOS 18 and later the row now uses `AgentSidewaysPan`, a
`UIPanGestureRecognizer` through `UIGestureRecognizerRepresentable` whose
delegate refuses to begin unless the finger moves 1.5 times faster sideways
than up or down (the distance moved when the speed reads zero). A vertical
swipe therefore fails it at once and only the scroll view sees it. The release
speed times 0.25 s stands in for `predictedEndTranslation`, which decides a
fling open. iOS 17 keeps the `DragGesture`.

### Android

`AgentSwipeToDelete` (`feature/agents/AgentSwipeRow.kt`) wraps the Agents
tab's entries and its "On <desktop>" live rows in a `SwipeToDismissBox`
styled like the Inbox swipe, end to start only. The swipe never removes the
row: it snaps back and runs the delete. Only a successful delete plays the
delete cue, shows the result in the status line under the tab's counts,
drops the agent from the list and reloads; the reload keeps older finished
runs from the previous list, so the deleted one is removed first or it would
come back. The rules are `AgentSwipeDelete` in `core/agents`; a desktop row
needs a paired desktop (`AgentsFeature.hub.desktop`). The Desktop tab's live
rows do not swipe. It uses `onDismiss` rather than the deprecated
`confirmValueChange`.

### Tests

- iOS: `AgentLinesTests.testSwipeDeleteGoesToTheTransportThatOwnsTheRunAndSkipsLiveOnes`,
  run with `swift test` on Linux. The SwiftUI views were only parsed on Linux,
  not compiled.
- Android: `AgentSwipeDeleteTest` (`./gradlew :core:coreTest
  -Pfilter=AgentSwipeDelete`); the app compiles with
  `./gradlew :app:compileDebugKotlin`. The swipe was not tried on a device.

### Swipe delete does not ask (2026-10-01)

The swipe used to open the run screen's confirmation sheet (iOS) or dialog
(Android) before deleting. It no longer does: the swipe is the deliberate act,
so it deletes at once and a status line in the list reports the result or
the error (it was a snackbar until 2026-10-09, when the user asked for no
toasts).

- The desktop's `agentops.delete` still needs a confirmation token, so the
  client asks for one (`prepareDelete`) and spends it on the same call, with
  no screen in between: `AgentLineDeleteRequest.run()` on iOS, which drives
  `AgentDeleteModel` or `RunnerDeleteModel`, and `deleteAgentTarget` in
  `AgentSwipeRow.kt` on Android.
- A row being deleted cannot be swiped again until its delete returns (iOS
  keeps the ids in flight; Android ignores a swipe while one runs).
- The "All agents" screen's swipe Delete on iOS also deletes at once and
  shows the result in an inline line at the top of its list, since there is
  no sheet left to show an error.
- The run screen's Delete button keeps its confirmation, and so does the
  desktop app's own Delete on an agent row.

### Deleting a run deletes its branch (2026-10-01)

A desktop agent's delete already removed its worktree and branch on the
desktop. A runner or relay run's delete only removed the run and left
`agent/...` on ForkMesh. Now both phone apps delete that branch first, merged
or not, and then the run: `RunnerDeleteModel.run()` (iOS) and
`RunnerAgentOps.delete` (Android), used by the swipe and by the run screen's
Delete. The server side is the `discard` clean up in
[../server/merge-executor.md](../server/merge-executor.md). The cores keep the
deleted tip under `refs/forkmesh/trash/agent-branch/`.

- `no_branch` (the run never pushed one) and `forbidden` (a member who is not
  an owner may not delete branches) still delete the run.
- Any other failure keeps the run and shows the error, so the delete can be
  tried again.
- The confirmation text now says the branch is deleted on ForkMesh.
- Tests: `RunnerDeleteModelTests` and
  `RunnerAgentOpsTests.testDiscardAsksTheRelayToDeleteAnUnmergedBranch`
  (iOS, `swift test` on Linux), `RunnerAgentOpsTest.testDeleteDiscardsTheBranchBeforeTheRun`
  (Android).

### Agents the phone could not delete or restart (2026-10-01)

A few agents on the phone had no working Delete and no way to run them again.
Four causes, each fixed:

- **A stale relay copy hid Delete and Continue.** A desktop session in the
  Agents tab list comes from the snapshot the desktop pushes to the relay.
  That snapshot can stay at "running" after the run ends, for example when
  publishing to the relay is turned off. The phone read the running state from
  it, so `AgentLines.deletion` refused the row even when the desktop in reach
  said the agent had stopped. `AgentLines.line(_:live:)` now takes the status,
  label and running flag from the desktop's own row when it lists the session.
  The relay copy is used only when the desktop is out of reach. Android's
  `AgentSwipeDelete.forEntry` now judges a pushed desktop session only by the
  matching live row.
- **The desktop session screen had no Delete.** `AgentSessionDetailScreen`,
  which the Agents tab opens for a pushed desktop session, could stop,
  continue and merge, but not delete. The pushed status also decided which of
  those buttons showed. `DesktopSessionOpsCard` now reads the desktop's live
  row (`liveDesktopRow`) for running, stoppable and merged, and offers Delete
  (`AgentDeleteSheet`) when the desktop lists the agent and it is not running.
  The screen closes after the delete.
- **A merged agent stuck at "running" could not be deleted.** The phone treats
  a merged session as finished, so it offered Delete. `agentops.delete`
  (`MainWindowLocalControlAgentOps.cpp`) still refused it with
  `agent_running`, because it checked the raw status. Stop was not offered
  either, since the desktop does not stop merged sessions. Both the prepare and
  the delete step now refuse only what `isStoppableAgentSession` calls
  running: a queued, running or waiting session that is not merged.
- **A failed runner run could not be retried from the run screen.** The Agents
  tab opens runner runs in `AgentRunScreen`. Its Continue needs a run a node
  has claimed (`localAgentId > 0`), so a run that failed or was stopped before
  a node took it had no way to run again. `AgentControlCenter.retryableRun`
  returns the cached relay session when it is failed or stopped and kept its
  task (`isRetryable`). The run screen then shows Retry ("Retry on its branch"
  when the run can resume) in place of Continue. Retry calls the same
  `retryAgentSession` as the queued-run screen's retry card.

Agents started outside ForkMesh still have no Delete. They are live CLI
processes in the user's own terminals, and the phone still stops them only
when they run under `forkmesh term`.

Tests: `AgentLinesTests.testPushedDesktopSessionTakesItsStateFromTheDesktopInReach`
and `AgentControlTests.testAFailedOrStoppedNodeRunOffersItsRunForARetry` (iOS,
`swift test` on Linux), `AgentSwipeDeleteTest.testPushedDesktopEntryDeletesThroughTheMatchingLiveRow`
(Android). The SwiftUI views were parsed on Linux but not compiled. The
desktop change was syntax-checked on its own (`-fsyntax-only`), not built into
the app.

The same change fixes a broken build. `AgentsListModel.init` had
`refusals: AgentReadRefusals = AgentReadRefusals()`, which calls a
main-actor initializer from a default argument. Swift 6.3 rejects that, so
`FMAgents` did not compile on Linux. The parameter is now optional, and the
initializer builds the default itself.

### Delete works for every agent, whatever its state (2026-10-02)

After the fix above, some agents on the phone still had no Delete. The phone
hid Delete for any agent it thought was running, for a desktop session the
desktop in reach did not list, and for every agent started outside ForkMesh.
The user asked that any agent can be deleted, whatever its status. Delete now
shows on every row, and on the run and session screens.

- **Desktop sessions.** `AgentLines.deletion` no longer looks at the running
  state or the desktop's live row. Any session with a desktop id gets
  `.desktop(sessionId:)`. `agentops.prepare`/`agentops.delete`
  (`MainWindowLocalControlAgentOps.cpp`) no longer refuse with
  `agent_running`. A queued, running or waiting session is stopped first
  (`stopAgentSessionById`). The delete then waits, polling every 250 ms for up
  to 30 s, until the session's `AgentRunner` is idle, because
  `deleteStoredAgentSession` refuses while the runner is busy. The confirmation
  summary says "Stop this agent and delete it" when the agent was running
  (`agentops::deleteSummary(branch, repo, running)`). If the desktop is out of
  reach, the swipe answers "The desktop is not in reach." instead of hiding
  Delete.
- **Agents started outside ForkMesh.** An outside row (negative desktop id)
  gets `.outside(id:)`. It uses the same `agentops.prepare`/`agentops.delete`
  calls, and the desktop recognises the id as one of:
  - a ForkMesh terminal (`localControlTerminalForRow` or
    `localControlTerminalForOutside`): the desktop closes it
    (`endLocalControlTerminal`);
  - a CLI process (`m_machineAgents`) or a surfaced Claude Code session
    (`m_externalSurfaced`): the desktop ends its processes with
    `killExternalSessionPids` (SIGTERM, then SIGKILL after 4 s) and drops the
    row.

  The process of another OS user is refused with `outside_other_user`. Ending
  a process ForkMesh did not start cannot be undone. So this is the one swipe
  delete that asks first ("Stop and delete this agent?"). The desktop's own
  Delete asks for these sessions too.
- **Runner and relay runs.** `AgentLineDeletion.runner` carries `active`.
  `RunnerDeleteModel` stops an active run first (`stopAgentSession`, now part
  of `RunnerAgentsApi`). The relay's branch discard refuses a run that is still
  running (`session_running`). On that answer the model asks the run to stop,
  then retries every 2 s, up to 10 tries. If the run still has not stopped, it
  deletes the run anyway and leaves the branch, so a stuck node cannot keep an
  agent undeletable.

Android (`AgentSwipeDelete.forRow`/`forEntry` and the runner delete) follows
the same rules.

Tests: `AgentLinesTests.testSwipeDeleteGoesToTheTransportThatOwnsTheRunWhateverItsState`,
`RunnerDeleteModelTests.testARunningRunIsStoppedBeforeItsBranchAndRunGo`,
`testARunStillStoppingIsWaitedForBeforeTheBranchGoes` and
`testARunThatNeverStopsIsStillDeleted` (iOS, `swift test` on Linux), and the
delete summaries in `desktop/tests/test_local_control_agentops.cpp`.

### Long press to delete an agent (2026-10-02, iOS)

The user asked to delete an agent by long pressing it, as well as by
swiping.

- A long press on any row of the Agents tab list opens a menu with
  **Delete** (and **Stop** first, for a run that can stop). Delete is the
  same as the swipe: `AgentLineDeleteRequest` decides whether the row can be
  deleted, it deletes at once and reports in the list's status line, and an agent
  started outside ForkMesh still asks first. A row whose delete is in flight
  shows no Delete. The menu closes any row left swiped open.
- Before, only the desktop's live rows had a long-press menu, with Stop
  alone. The menu now sits on every row (`AgentsScreen.lineMenu`); the
  preview lifts with the row's rounded corners.
- The "All agents" list gets the same menu: Stop and Delete.
- Android is unchanged: its rows still delete by swiping.
- Not checked on a device from Linux, which can only parse the Swift.

### Delete at once from the agent page, with Undo (2026-10-02, iOS)

The user asked for the Delete button on an agent's page to delete at once,
with an Undo button that shows for 5 seconds.

- **Delete no longer opens a sheet.** On `AgentRunScreen` (the trash in the
  top bar, for a desktop or a runner run; runner runs lost their actions menu
  on 2026-10-02) and on
  `AgentSessionDetailScreen` (`DesktopSessionOpsCard`), Delete closes the page
  at once and the shell shows "Agent deleted." with **Undo**. The confirm
  sheets `AgentDeleteSheet` and `RunnerDeleteSheet` are gone. A run started
  outside ForkMesh still asks first, because the delete ends that process.
- **The delete waits out the Undo window.** The desktop and the runner have no
  trash: a delete removes the worktree and the branch for good. So
  `AgentPendingDeletes` (FMAgents) holds the delete for
  `AgentPendingDeletes.undoWindow` (5 s) before it sends it, through the same
  `AgentLineDeleteRequest.run()` the swipe uses. Undo cancels it, and nothing
  reaches the desktop or the runner.
- **The agent vanishes from the lists at once.** The Agents tab and "All
  agents" hide every row whose `AgentLineDeletion.key` is waiting or being
  deleted. The key is the desktop session id or the runner session id, so the
  pushed copy and the live copy of one run hide together. A delete that fails
  brings the row back, and the shell shows why.
- **The Undo line sits in the agent list.** On iOS `AgentUndoDeleteLine`
  (`Features/Agents/AgentSwipeDelete.swift`) reads `AgentPendingDeletes.shared`
  and shows "Agent deleted." with Undo at the top of the Agents tab list and
  of "All agents", where the row was; the failure (red) or the delete report
  follows in the same place until dismissed or the list is left. It replaced
  the shell's floating Undo snackbar (`agentUndoDeleteSnack`) on 2026-10-09,
  when the user asked for no toasts anywhere. It closes when the window ends.
- **The diff button is a separate green square** at the right end of the run
  page's top bar, in the green glass (`MergeGlass`) of the Merge & clean up
  button, apart from the group of other actions.
- The list's swipe and long-press Delete are unchanged: they still delete at
  once with no Undo.
- Android is unchanged.
- Tests: `AgentPendingDeletesTests` (iOS core, `swift test` on Linux). The
  screens were only parsed on Linux, not built or run on a device.

### Delete says what went with the agent (2026-10-02)

The user asked that a delete from the phone say whether the branch, the work,
the issue, the pull request and the task went too, since all of them usually
should when they are attached.

- **The desktop answers when the delete is done.** `agentops.delete` used to
  answer `{id}` straight away and clean up afterwards. It now notes what the
  agent holds first (branch, worktree, issue, pull request, organization task)
  and answers once the clean up ends, with `settled` and a `parts` list. Each
  part has `kind` (`agent`, `branch`, `work`, `issue`, `pull`, `task`), `name`,
  `status` (`deleted`, `closed`, `absent`, `kept`, `pending`) and an optional
  `reason`. `agentops::deleteParts` builds it from `DeleteFacts`
  (`LocalControlAgentOps.cpp`), which the handler fills by checking the branch,
  the worktree folder, the issue's status and the session after the delete.
- **The delete itself is unchanged.** It is still the desktop's own
  `deleteAgentSessionAndCleanup`: it trashes the task, deletes an unmerged pull
  request, removes the worktree and branch, and closes the linked issue (an
  issue is closed, not deleted, so its history stays). An
  `agentops::DeleteTrace` now rides along that chain. It records the task's
  outcome and fires when the chain ends, on success or failure.
- **The answer waits at most 75 s** (`kDeleteAnswerWaitMs`). A task the relay
  is backing off on can take longer, so whatever has not finished by then is
  reported as `pending`. The phones wait 90 s for the call. A phone older than
  this change still gives up after 20 s, so a delete that first has to stop a
  running agent may show a timeout there even though it goes through.
- **The phone says it in one status line,** built by `AgentDeleteReport.summary`
  (iOS `FMAgents/Remote/AgentDeleteReport.swift`, Android
  `agents/ops/AgentDeleteReport.kt`), for example: "Deleted the agent on the
  desktop with branch agent/12-fix, its worktree and pull request #9. Closed
  issue #12. No task was attached." Parts the desktop kept get their own
  sentence with the reason ("Kept pull request #3: it was merged."). If the
  desktop kept the agent itself, the delete counts as failed and the row comes
  back. A desktop older than this change sends no parts, and the phone falls
  back to "Deleted on the desktop."
- **Where it shows.** iOS: the list swipe and long press, "All agents", the
  Undo path on the agent page (`AgentPendingDeletes` now takes an
  `AgentDeleteOutcome` and shows the report when the window closes), and the
  Siri delete intent. Android: the list swipe, the "All agents" screen, and
  the delete dialogs.
- **Runner and relay runs.** The relay's run delete removes only the run. The
  phone already deletes the branch first, and now reports it: deleted, already
  gone (`no_branch`), or kept (`forbidden`, or the run never stopped). A pull
  request on the run is reported as kept, because the relay has no route that
  closes one. An owner run never has a task or an issue. An organization run
  can, but the phone's run rows do not carry them, so they are not mentioned
  and the delete leaves them alone.
- Tests: `AgentDeleteReportTests`, `RunnerDeleteModelTests`,
  `AgentPendingDeletesTests` and `RemoteAgentDiffDeleteTests` (iOS, `swift
  test` on Linux), `AgentDeleteReportTest` and `RunnerAgentOpsTest` (Android
  `:core:coreTest`), and the `deleteParts` checks in
  `desktop/tests/test_local_control_agentops.cpp`. The desktop sources were
  compiled with `-fsyntax-only`, but the app was not built or run against a
  phone. The iOS app files were only parsed on Linux.

## Stop all agents from the Agents header (2026-10-02)

The phones' Agents list has a red stop button at the top, so every running
agent can be stopped in one go instead of one swipe or long press at a time.

- **Where.** iOS: the first icon in the Agents tab bar, before the sort chip
  (`agents-stop-all`). Android: the first icon in the Agents header, before
  Start an agent ("Stop all agents"). It shows only while at least one agent
  can be stopped, and shows a spinner (iOS) or stays disabled (Android) while
  the stops run.
- **What it stops.** `AgentStopAll.targets`: every row of the unified control
  list (`AgentControlCenter.rows`) that is active, can be stopped, and is not
  an agent running outside ForkMesh. Desktop agents get `agents.stop`, runner
  and mirror runs (queued ones included) get the relay stop. Outside agents are
  left alone, as the Siri "Stop all agents" intent already did: their Stop
  presses Esc in someone's terminal, which is not a bulk action. The list's
  launched sessions that have no control row cannot be stopped from the list
  one by one either, so they are not included.
- **Asks first.** "Stop all N running agents?" (or "Stop the running agent?"),
  with the count taken when the button was tapped, then Stop all or Cancel.
  Unlike Stop on the run screen, a bulk stop can reach agents the user did not
  look at, so it confirms.
- **Result.** `stopAll()` (an extension on `AgentControlling` on both apps)
  stops each target in turn, keeps going past failures, and returns
  `AgentStopAllOutcome`; the status line says "Asked N agents to stop.", or how
  many did not stop and the first error's sentence (`ControlErrorText`). The
  list then refreshes.
- **Not the bulk bar.** The existing Stop all / Continue all bar under the list
  (`BulkAgentActionsBar`) still covers the desktop sessions the Agent Ops
  client sees; the header button is the one that covers every transport.
- Tests: `testStopAllStopsEveryRunningRunAndNothingElse` and
  `testStopAllSkipsOutsideAgentsAndSaysWhatFailed` in `AgentControlTests`
  (iOS, `swift test` on Linux) and `AgentStopAllTest` (Android
  `:core:coreTest`). The iOS app file was not built on Linux.

## Recycle bin from the phone (2026-10-02)

The user asked to manage deleted agents from the phone: restore them or delete
them, with everything the desktop's recycle bin offers. A deleted desktop agent
moves to the desktop's recycle bin (`AgentStore`), where it stays for the
retention window (Settings > Agents, 1 hour to 30 days, 24 hours by default).
The desktop shows it in Settings > Agents > Recycle bin… and in the strip over
the Agents list. The phones now reach the same bin.

### Protocol

All four methods are in the owner-only agentops family
(`MainWindowLocalControlAgentOps.cpp`; the pure parts are in
`LocalControlAgentOps.cpp`). `status` lists the capability `agentops.bin`.

- `agentops.bin` answers `{agents, count, truncated, retentionHours,
  minHours, maxHours}`. Each row is `{id, title, owner, name, branch,
  provider, deletedAtMs, removeInMs}`, newest delete first. The title is the
  issue title, or else the prompt's first line, cut at 90 characters, as in
  the desktop dialog. `removeInMs` is worked out on the desktop, so the
  phone's clock does not matter; it is `-1` when the delete time is unknown.
  At most 500 rows are sent (`kMaxBinAgents`); `count` is the whole bin and
  `truncated` says some were left out.
- `agentops.bin.restore {ids}` restores each one with
  `AgentStore::restoreRecycledSession`, as the desktop's Restore does, and
  answers the bin again plus `restored` and `failed`. A restore fails when a
  live agent already has that id. It needs no confirmation: a restored agent
  can be deleted again. The desktop reloads its agents and pushes
  `agents.changed`.
- `agentops.bin.purge {ids, token}` needs a token from
  `agentops.prepare {action:"purge", ids}`, whose summary says how many
  agents go and that this cannot be undone. `purge` counts as destructive.
  It answers the bin plus `purged` and `skipped` (already gone).
- `agentops.bin.retention {hours}` sets the retention window, as the
  Settings spin box does, then sweeps the bin at once, so a shorter window
  frees space now. Values outside 1 to 720 hours are refused with
  `bad_request`.

The id lists take up to 500 ids, not the 50 of `agentops.bulk`, so "Restore
all" and "Empty bin" are one request each. Each change is written to the
desktop log with the device that asked. A desktop older than this answers
`unsupported`, shown as "Update ForkMesh on the desktop to manage its recycle
bin from here."

### Phones

- iOS: `RecycleBinScreen` (`ForkMesh/Features/Agents/Remote`), opened by the
  trash button in the Agents tab's tool row (shown while a desktop is
  reachable) and by "Recycle bin" on the Desktop agents hub. Android:
  `RecycleBinScreen` (`feature/agents`), opened by the trash icon in the
  Agents header (shown while a desktop is reachable) and by "Recycle bin"
  beside "Saved permissions".
- The screen shows the desktop's explanation, a "Keep deleted agents for"
  menu (1, 6, 12, 24 hours, 2, 3, 7, 14 or 30 days), then Restore all and
  Empty bin. Tapping rows selects them, and the buttons become "Restore N"
  and "Delete N". Each row also restores or deletes on its own: iOS swipes
  right to restore, left to delete, and has a long-press menu; Android has a
  restore and a delete icon on the row.
- Deleting for good always shows the desktop's summary in a confirm dialog,
  like the desktop's own "Delete now" and "Empty bin". Restoring does not ask.
  After a restore the phone refreshes its agent list.
- State lives in `RecycleBinModel` (iOS `FMAgents/Remote/AgentOpsViewModels.swift`,
  Android `agents/ops/AgentOpsStates.kt`); the rows are `RecycledAgent` and
  `RecycleBin` in the AgentOps models.

### Tests

`RemoteRecycleBinTests` (iOS, `swift test` on Linux), `RecycleBinTest`
(Android `:core:coreTest`) and `testBin` in
`desktop/tests/test_local_control_agentops.cpp`. The desktop's handler file
and the Android app were compiled; the iOS app files were only parsed on
Linux. Neither phone was run against a desktop.

## Unread dot on each agent (2026-10-02)

Both phone apps mark an agent whose latest turn finished after the user last
opened it, as the desktop does ([desktop/agents-ui.md](../desktop/agents-ui.md#unread-dot-on-each-agent-2026-10-02)).
Running, queued and waiting agents are never unread on their own; a new
finish makes a read agent unread again.

### Protocol

- Every `agents.list` row carries `unread`: `true` when the desktop counts
  the run unread, `false` otherwise. A row without the key comes from an
  older desktop.
- `agents.markRead {id}` marks one of the desktop's own sessions read and
  rings `agents.changed`; it answers `{}`, or `unknown_session`. The phones
  send it fire-and-forget and ignore every error, `unsupported` included.

### Rule on the phones

The phone keeps its own stamps so the dot goes as soon as an agent is opened,
before the desktop's next list arrives:

- `agents/readBaselineMs` is written the first time the list loads; a finish
  before it counts as read.
- `agents/seenFinishedAt` is a JSON object from row key to the finish time
  last seen, capped at 400 entries (lowest stamps dropped first). A cap, not
  pruning to the list on screen, because the list loads in parts and the
  desktop may be out of reach.
- Row keys: `desktop:<repo name lowercased>#<session id>` for desktop
  sessions (the key `AgentLines.merge` / `liveDesktopRow` already match on,
  so the relay-pushed row and the live row share it), `run:<session id>` for
  cloud and runner runs; iOS also keys outside agents `outside:<id>`.
- A row is read when the phone's stamp is at or after its finish. Otherwise,
  when the desktop sent `unread`, the flag decides; without it the row is
  unread when it finished after the baseline (iOS also treats merged runs as
  read).
- Opening an agent from the list records its finish time and, for a row
  matched to a live desktop row, sends `agents.markRead`. A relay-pushed
  session with no live row sends nothing, so the wrong desktop is never
  marked.

### iOS

`AgentReadState` / `AgentReadTracker` (FMAgents) hold the rule and storage in
`KeyValueStore`; `AgentLine` carries `readKey`, `finishedAtMs`,
`desktopUnread` and `unread`, and its accessibility summary starts with
"Unread". `LocalAgentRow.unread` and `UnifiedAgentRow.unread` /
`finishedAtMs` carry the desktop flag; `AgentControlCenter.markRead(id:)`
sends the request. `AgentsScreen` opens every row through `readPage(line)`,
which records the agent when its page appears and again when it closes (so a
finish while the page is open does not leave a dot), and the open row never
shows one. `AgentUnreadDot` is an 8 pt `fm.accent` circle (the Inbox unread
blue) with a background ring on the top-right of the agent dot, in Compact,
Zen, Full and Eagle; titles do not move. Watch and All agents do not show it.

### Android

`AgentReadState` (core `agents/`) holds the rule, merge, cap and JSON;
`AgentReadMarks` (app) keeps it in `Prefs` and sends the request.
`LocalAgentRow.unread`, `UnifiedAgentRow.unread` / `finishedAtMs` and
`AgentControlCenter.markRead` / `markReadAsync` mirror iOS.
`AgentTwoLineRow` takes `unread: Boolean?`; when it is not null an 8 dp slot
at the start of every row keeps titles aligned and an unread row fills it
with an accent dot like Inbox's, with "Unread" as its state description.
The Agents tab (pushed, launched and live desktop rows), the Cloud tab, the
Desktop tab's running rows and All agents pass it, in every view. A tap marks
the row read before it opens; opening from a notification does not.

### Tests

- iOS: `AgentReadStateTests` (keys, baseline, desktop flag, open row,
  storage cap, tracker), `AgentControlTests.testTheDesktopsUnreadFlagIsReadWhenSent`
  and `testOpeningADesktopAgentMarksItReadThere`.
- Android: `AgentReadStateTest`, `LocalControlTest.testDesktopListCarriesTheUnreadFlag`
  and `testOpeningADesktopAgentMarksItReadOnTheDesktop`.
- Desktop: `forkmesh-window-tests --agent-list-view-only`.

## Fresh agents and desktop folders on the iOS Agents tab (2026-10-06, #1436)

The iPhone kept showing agents that had been deleted or cleaned up on the
desktop. There were two causes.

**The relay snapshot was never emptied.** `pushAgentSessionsSnapshot` grouped
sessions by repository and skipped any repository with no sessions left. It
also returned early when no repository had any. Deleting the last agent in a
repository therefore never reached the relay, and the owner-sealed rows the
phone reads (`AgentsListModel`, `GET /api/repo/<o>/<r>/agents`) stayed as they
were. Now:

- Every repository that last had sessions on the relay is listed in the
  `agents/publishedRepos` setting. It is updated after each successful push:
  added when the push held sessions, removed when it was empty. A repository
  in that list gets one more push, with an empty `encryptedSessions`. The
  handler already replaces the stored set in full, so this clears it and
  rings `agents`.
- The snapshot leaves out what the phone's live `agents.list` leaves out:
  recycle-bin sessions (`deletedAtMs`), link-only rows (`associationOnly`)
  and external rows.

**The phone trusted the snapshot over the live list.** `AgentLines.merge`
showed every pushed desktop session even when the desktop's own list, read
seconds earlier, no longer had it. It now takes a `DesktopListCover`
(`AgentControlCenter.desktopCover`). This is set only when the desktop's
list loaded without error and the account reaches exactly one desktop
(`hub.candidates.count == 1`), so a session pushed by a second desktop is
never dropped. With a cover, a pushed session missing from the live list is
dropped. The exception is when the list hit the desktop's 200-row cap
(`DesktopListCover.listCap`). Then only sessions created on or after the
oldest listed row are dropped, because older ones may simply be beyond the
cap.

### Desktop folders

The desktop's agent folders (`docs/desktop/agents-ui.md`, "Agent folders")
now appear on the iPhone.

- **Desktop.** `agents.list` carries `folders`. Each entry has
  `repositoryOwner`, `repositoryName`, `id`, `name`, `collapsed` and
  `members`. `members` are the `agentReadKey()` keys of the sessions filed in
  the folder: the local id, or `cloud:<id>`. `claude:<uuid>` keys are left
  out because the phone cannot match them. Folders come per repository in the
  desktop's order. `saveAgentFolders()` and an agent drop ring
  `agents.changed`, so a phone in reach reloads.
- **Phone.** `LocalAgentsSnapshot.folders` (`LocalAgentFolder`) and
  `AgentControlCenter.desktopFolders` hold them.
  `AgentLines.folderSections` groups the sorted lines in the same order as
  the desktop: folders first, in order, each with its members, then
  everything else. A line's key (`AgentLines.folderKey`) is the repository
  name plus the desktop session id, or `cloud:<relay session id>` for a node
  run. Empty folders are not shown. A header row shows a chevron, the folder
  glyph, the name, the repository when folders come from more than one, and
  "N agents · N running". Tapping it collapses or expands the folder on the
  phone only. It starts in the desktop's collapsed state; since 2026-10-08
  the phone remembers a folder it toggled (`AgentFolderFolds`, see "Folder
  open or closed state is kept"). An older desktop
  sends no `folders`, so the list stays flat.

### Tests

- `AgentLinesTests.testAPushedSessionTheWholeDesktopListLacksIsDropped`,
  `testDesktopListCoverIsWholeBelowTheCap`,
  `testLinesGroupIntoTheDesktopsFolders`.
- `LocalControlPrimitiveTests.testAgentFoldersReadFromTheAgentsList`.


### Android (2026-10-06)

Android's Agents tab kept showing agents that had been merged and cleaned up
or deleted on the desktop. The desktop half of the fix above (the emptied
relay snapshot) reaches Android as it is. The phone half did not: the tab
listed every pushed session in `AgentRepoGroup.sessions`, whatever the
desktop's live list said.

- `DesktopListCover` (`control/AgentControl.kt`) and
  `AgentControlCenter.desktopCover` match iOS: there is a cover only when
  `agents.list` loaded without error and `hub.candidates` holds exactly one
  desktop, and `DesktopListCover.of` keeps the oldest listed `createdAtMs`
  when the list reaches the 200-row cap (`LIST_CAP`).
- `AgentsList.covered(groups, rows, cover)` drops each pushed session that
  the live list lacks and the cover reaches, then drops repositories left
  empty (`visible`).
- `AgentsScreen` keeps the loaded `groups` as they are and draws from
  `shown`, the covered groups. The Agents, Cloud and Desktop tabs, the header
  counts and Watch all use it.

Tests: `AgentsListTest.testAPushedSessionTheWholeDesktopListLacksIsDropped`,
`testDesktopListCoverIsWholeBelowTheCap`.

## An open agent stays open when the list changes (2026-10-06, #1458, iOS)

The user was watching an agent's transcript on the iPhone when the page
closed and the Agents list came back.

Each row on the Agents tab was a `NavigationLink` with a destination
closure, inside the `LazyVStack` of `AgentsScreen.lineList`. SwiftUI closes
a page pushed that way when its link leaves the view tree. The list rebuilds
on every desktop ring and relay read, and several changes remove or replace
a row while its agent is still running:

- `AgentLines.merge` drops a pushed desktop session when the desktop's
  current list lacks it (`DesktopListCover`, #1436).
- A line moves between sections when the desktop's folders load or go away
  (`AgentLines.folderSections`), so it is a different view.
- A line switches source between the relay entry and the live control row,
  which takes a different branch of `lineRow`.
- The list is empty for a moment, so `FmSwap` shows the empty state.

Now a row is a plain `Button` that sets `AgentsScreen.opened`
(`AgentOpenedLine`), and one `navigationDestination(item:)` on the screen
pushes the page. Which page to open (`AgentRunScreen`,
`LaunchedAgentSessionScreen` or `AgentSessionDetailScreen`) is decided when
the row is tapped, including the `AgentLines.runRow` lookup for a node run,
so a later list read cannot swap the page or close it. Back, a tab reselect
and starting an agent from the prompt bar still close it as before.

Android pushes each agent page on its `Navigator` from the row's click
handler, so a list rebuild never closed it there.

## Switching a desktop agent between Claude and Codex from the phone (2026-10-06, #1493, iOS)

The model chip on a desktop agent's run screen offered only the session's own
runtime: a Codex session listed the four GPT/Codex models and nothing else,
so moving a run to Opus meant going to the desktop. The desktop's own
composer could already hand a session to the other CLI.

### Desktop

- `handOffAgentSessionToProvider(sessionId, provider)` holds what
  `applyComposerSelectionToAgentSession` did when the composer's provider
  changed: stop the live CLI transport and any runner (otherwise the send
  path hands the prompt back to the old agent), set `handoffFromProvider`,
  the new `provider` and its `runtimeId`, clear the model, save, and log the
  "Agent handoff" line. The next launch includes the saved transcript, as
  for a handoff made on the desktop. Association-only sessions are refused.
- `agents.prompt` takes an optional `provider`, `claude-code` or `codex`
  only. `applyLocalControlAgentChoice` hands the session over before it
  checks the `model`, so a Claude model on a Codex session (or the reverse)
  now passes `agentModelMatchesProvider` instead of being dropped.

### Phones

- `AgentModelCatalog.followUpChoices(provider:)` lists the session's own
  runtime first, then the other one's models; other runtimes (Gemma) get no
  chip, as before. `runtimeLabel` names the runtime.
- The run screen notes each model from the other runtime with "Hands off to
  Claude Code" or "Hands off to Codex". On send it passes the picked model's
  provider; `AgentControlCenter.prompt(…, provider:)` adds `provider` to
  `agents.prompt` only when it differs from the row's.
- A desktop older than this change ignores `provider` and drops a model of
  the other runtime, so the prompt still reaches the agent on its old
  runtime and model.

### Tests

- iOS: `testFollowUpChoicesOfferTheOtherRuntimeAfterTheSessionsOwn` and the
  handoff case in `testControlsRouteToTheDesktopOrTheRelayQueue`.
- Desktop: `MainWindowLocalControl.cpp` and `MainWindowAgents.cpp` were
  compiled with the app's flags (syntax only). `AgentRunScreen.swift` was
  only parsed on Linux.
- Android has no model chip on the agent page yet.

## Folder open or closed state is kept (2026-10-08, iOS)

A folder opened or closed on the iPhone's Agents tab went back to the
desktop's state each time the tab was rebuilt or the app restarted, because
the choice lived only in the screen's `@State`.

- `AgentFolderFolds` (`FMAgents/AgentLines.swift`) holds the folder ids the
  phone closed and the ids it opened, stored as two string lists in the
  `KeyValueStore`: `agents/folderCollapsed` and `agents/folderExpanded`. A
  folder in neither list uses the desktop's `collapsed`. Folder ids are the
  desktop's UUIDs, so one list covers every repository.
- `AgentsScreen` loads it once and saves it on every header tap.
- The choice stays on the phone. It is not sent back to the desktop, whose
  own state is already saved in `agents/folders` (`docs/desktop/agents-ui.md`,
  "Agent folders").
- Android has no folders on its Agents tab, so there is nothing to keep.

## Answering agent questions from the iPhone (2026-10-10)

An `AskUserQuestion` used to appear in the phone transcript only as an
expanded tool payload, leaving a waiting desktop agent with no usable answer
control. The desktop now exposes its unanswered question ids and their input
through `agents.questions`, and accepts the selected response through
`agents.answer`. The endpoint confirms that the question is still pending
before delivering it through the same runtime path as the desktop card; a
secret answer remains redacted in the saved transcript. `AgentRunScreen`
reloads these cards alongside its live transcript and presents options, an
Other field, and Send answer above the transcript.

### Tests

- iOS: `AgentLinesTests.testFolderFoldsOutliveTheScreen`. Not compiled here
  (no Swift toolchain on Linux).

## Usage pace on the phone, and day and hour segments (2026-10-13, #1982)

The user sent a phone screenshot ("Weekly 59% used · resets in 5d 18h") and
said the usage showed five days when the week is seven, and asked for the
desktop's hourly and daily marks on the phone. The reset time was right: the
`/api/oauth/usage` endpoint gave `seven_day.resets_at` 2026-10-19 20:00 UTC,
5 d 18 h away. The count was what was wrong. The desktop menu bar (task 1967)
drew a faint 1 px line at each day of a weekly window; at 132 px the day lines
land between pixels and blur, the first sat against the pace line and the
fourth on the end of the 59% fill, so about five could be read. On the 5-hour
bar the hour lines vanished the same way.

### Desktop

- `TokenUsageMenuRow` cuts the bar into segments instead of drawing lines:
  each day (or hour, for windows of a day or less) is cut out of the bar as
  a 2 px gap on a whole pixel, so a weekly bar is seven pieces and a 5-hour
  bar five, whatever the fill or the pace line covers. The pace line is drawn
  above and below the bar only, so it no longer hides a gap.
- `agentops.usage` windows carry `windowMs`: 5 h and 7 d for Claude Code's
  5-hour, weekly and Fable weekly and Codex's fallback rows, and
  `windowDurationMins` for Codex's own windows (`codexWindows`), for both the
  provider rows and each account's rows.

### Phones

- `UsageWindowMeter` reads `windowMs`; when a desktop older than this does
  not send it, the length is taken from the id or label (5-hour, weekly,
  Fable). It adds `segments`, `elapsedFraction`, `periodLabel` ("Day 2 of 7",
  "Hour 3 of 5") and `paceLabel` ("41% over pace", "12% under pace",
  "on pace"), the phone's `usagePaceNote`.
- iOS `UsageMeterRow` (the desktop usage card and runner account rows) and
  the Android usage card draw the bar as the same segments with the pace
  marks above and below, and add a line under the bar: "Day 2 of 7 · 41%
  over pace". A window without a reset ahead or a known length keeps the
  plain bar.

### Tests

- Desktop: `forkmesh-local-control-agentops-tests` checks `windowMs` is
  carried for a Codex window and left out when unknown. The bar was drawn
  off-screen with the same painter calls to check the seven and five pieces.
- iOS: `RemoteAgentOpsClientTests` covers segments, period and pace labels
  and `windowMs` from JSON. Android: the same cases in `AgentOpsTest`.
