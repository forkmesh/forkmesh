# Desktop agents on the phone

Both phone apps list the owner's desktop agents next to node runs and manage
them over the local control channel: on the same network directly, away from
home through the relay, encrypted end to end either way
([desktop networking](../desktop/networking.md); [PARITY.md](PARITY.md) WS7).
The website's Agents tab uses the same channel
([desktop-agents-web.md](../app/desktop-agents-web.md)).

## Reaching a desktop without meeting it on the LAN

`DesktopControlHub` (iOS `FMControl/DesktopControlHub.swift`, Android
`control/DesktopControlHub.kt`) reads the account's desktops from
`GET /api/accounts/desktops` (`_account_desktops_handler` in
`app/src/entry.py`): `{account, desktops: [{key, node, lastSeen}]}`, one row
per enabled `desktop_node` device key plus the account's primary key, cached
ten minutes.

- Candidates, in order: the desktop connected on the LAN, LAN-paired desktops
  (newest first), then the account's desktops; the last one that answered is
  tried first. `link()` dials up to four at once and keeps the first that
  completes the handshake with the expected key.
- **Several desktops running (iOS, 2026-10-25).** The user can pick which
  desktop the phone uses: on the Desktops page (Other desktops, tap one) or in
  the composer's "Where it runs" sheet, which lists one row per desktop.
  `choose(key:)` stores the key in `desktop/preferred-key`. From then on
  `link()` dials only that desktop, `currentLink` ignores a LAN or relay
  channel to any other one, and a relay channel to another desktop is closed.
  A preferred key that is no longer one of the account's desktops is ignored.
  `presence` records whether each desktop answered: every dial in the race
  counts, and `checkDesktops()` dials the desktops it has no channel to and
  closes each channel again. It runs when the Desktops page loads or is
  pulled to refresh, and after `warm()` connects when the account has more
  than one desktop, at most every five minutes. `choices` turns that into
  `ShellDesktopLight` rows. Two desktops with the same node name get the first
  six characters of their key added to the name. The icon above the clock
  draws the desktop in use, with the other desktops stacked behind it, each
  with its own dot. The core nodes are stacked the same way. Before this the
  phone took whichever desktop answered first and showed only that one.
- **Agents from every desktop (iOS, 2026-10-27).** A phone paired with two
  desktops showed "No agent sessions yet" because its chosen desktop had none,
  while the desktop it was connected to on the LAN had dozens. The Agents list
  now lists the other desktops' agents too, in an "Other desktops" section
  under the list (`AgentControlCenter.otherDesktops`, up to four desktops, each
  `OtherDesktopAgents` with its rows or "Not reachable"). They load on a full
  `refresh()`, after the desktop in use. The hub reaches a desktop that is not
  the one in use with `link(to:)` / `request(_:_:to:)`: the LAN channel when
  it is to that desktop, else a relay channel kept per desktop (`sideLinks`).
  `choose(key:)` keeps the old desktop's relay channel as a side link and
  takes over the chosen desktop's side link, so switching back and forth
  dials nothing new. Every desktop row carries `desktopKey`, and the center
  sends stop, prompt, questions, answer, continue and transcript to the row's
  own desktop. Rows from other desktops stay out of `rows` and have ids
  prefixed with the desktop key. Agent ops, terminals, bulk stop, voice
  intents and the transcript stream act by local session id on the desktop
  in use, and the same id means a different agent on another desktop.
  Tapping a desktop or one of its agents therefore calls `useDesktop(key)`:
  it switches the desktop in use, clears the old desktop's rows at once,
  reloads, and then opens the agent.
- The desktop approves the phone without a code because its key is an enabled
  `owner_sign` device of the same account (`checkLocalControlAccountDevice`),
  records it in `data/local_control/paired-devices.json` as
  `"approval": "account"`, and re-checks the account on every connect and
  every ten minutes, dropping a revoked phone.
- A relayed channel is pinged after 30 s of silence and before reuse after
  45 s idle; a dead one is redialled; a desktop that no longer knows the
  channel answers `channel_gone`.
- `keepWarm` (set while the app is in the foreground) opens the relay link at
  once and reopens it after a drop (2 s doubling to 60 s).
- `DesktopReach.notPaired` means the account has no desktop at all.
- **Back on the LAN.** Every load of the account's desktops is handed to
  `LocalControlService.setAccountDesktops`. A mesh control host whose `node`
  key is on that list is dialled over the LAN like a LAN-paired desktop, and
  retried the same way after a failed dial or a drop. Once that channel is
  `connected`, the hub's `localChanged` lets the relay go. Before this, a phone
  that had only ever reached its desktop through the relay dialled the LAN only
  when the mesh `hello`'s `ownerUser` matched its account name. It tried that
  once per launch and never retried, so a phone at home often stayed on the
  relay.
- **The last LAN address.** `LocalControlService` stores each desktop's LAN
  address once its channel is paired (`localControl/lanAddresses`, desktop key
  to address) and hands the addresses of known desktops it is not connected to
  (and has not heard) to the mesh (`rememberedBeaconTargets`). The mesh sends
  them a unicast beacon first, at start, on resume, on a network change and on
  every beat; the desktop answers at once, so the phone dials within one LAN
  round trip. Before this an iPhone (no multicast entitlement) found its desktop
  only through the subnet sweep, 16 hosts every 500 ms from start, which took
  several seconds on a /24.
- **LAN before relay.** `DesktopControlHub.link()` waits up to `lanGrace`
  (1.5 s) while `LocalControlService.lanPending` is true (a dial queued or
  running, or a remembered address probed in the last 1.5 s) and returns the LAN
  channel when it opens. Before this a request made while the LAN dial was still
  under way, such as the Agents tab's first load or `keepWarm`, dialled the
  relay, and the list came through the internet.
- **Desktop rows first.** `AgentControlCenter` reads the desktop and the
  account's node runs at the same time and publishes the desktop's rows as
  soon as they arrive, merged with the node runs it already had; the node runs
  follow when the API answers. Before this the list waited for both, so the
  account API held back a desktop that had answered over the LAN.
- `onMeshChanged` sets `dialQueued` when it queues an automatic dial and
  ignores further calls until the dial starts. Before that, the status read
  `connecting` only once the queued dial ran, so a second trigger in between (a
  beacon, a retry, the account list) queued another dial, and that dial closed
  the first one. Android also drops a retry scheduled after `dispose()`.

## What is running on the desktop, on the Agents tab

- `agents.list` (`localControlAgentsPayload` in `MainWindowLocalControl.cpp`)
  returns the desktop's sessions (every repository it has open, idle and
  finished ones included) and `outside`: one row per agent CLI running on the
  machine that ForkMesh did not start, `{id, kind, label, provider, model,
  title, cwd, branchName, repositoryOwner, repositoryName, lastActivityMs}`
  plus `steer`, `typeable`, `stoppable`, `terminal`, `otherUser` when they
  apply. Outside ids are negative.
- Session rows carry `diffStats` (`files`, `ahead`, `behind`, `added`,
  `removed`, `dirty`, `conflicted`), `worktree`, `attachments` (the first
  image), `priority` (1..99), `mode`, `unread`, `held: true` for a paused
  session, and `backgroundTasks` (open backgrounded tasks) when above zero. The reply also carries `queue`, `folders` and `defaultMode`.
- The `/proc` scan (`tickMachineAgents`, every 2 s) runs while a phone or
  browser is connected; a change rings `agents.changed`. The bell rings as soon
  as something changes, then at most every 300 ms.
- `status.capabilities` advertises what a desktop supports (`agents.outside`,
  `agents.outside.transcript`, `agents.outside.prompt`, `agents.outside.type`,
  `agents.outside.stop`, `agents.stream`, `agents.queue`, `agents.handoff`,
  `agents.images`, `agentops.bin`, `agentops.accounts`, `terminal.agents`,
  `terminal.resume`, `terminal.accounts`). Phones hide what a desktop lacks or
  read `unsupported` as "Update ForkMesh on the desktop".
- **Phones.** Outside rows become `UnifiedAgentRow`s with `outside = true`
  (`fromOutside`, id `outside:<desktop id>`). The list merges the relay's
  pushed and queued sessions with the control center's rows
  (`AgentLines.merge`), showing a desktop session once.
- **Freshness.** The desktop pushes an empty snapshot for a repository whose
  last session went (`agents/publishedRepos`) and leaves recycle-bin,
  association-only and external rows out of snapshots. A `DesktopListCover`
  (set only when `agents.list` loaded without error and exactly one desktop is
  reachable) drops pushed sessions the live list lacks; at the 200-row cap only
  sessions newer than the oldest listed row are dropped.
- **Folders.** `folders` entries (`repositoryOwner`, `repositoryName`, `id`,
  `name`, `collapsed`, `members` as `agentReadKey()` keys) group lines in the
  desktop's order (`AgentLines.folderSections`). The phone remembers folders it
  toggled (`AgentFolderFolds`: `agents/folderCollapsed`,
  `agents/folderExpanded`).

## Agents of other users on the machine

`MachineAgentScan::processUid` reads each process's real uid. A process of
another uid gets `MachineAgent::otherUser` (login name or `user <uid>`) and
nothing else is read from it. Its `outside` row carries `"otherUser"`, a title
like `Claude Code (<login>)`, and no `steer`, `typeable` or `stoppable`;
`agents.prompt`, `agents.stop` and deletes answer `outside_other_user`. Phones
explain that ForkMesh cannot read or steer another login's agents.

## Following and watching an agent started outside ForkMesh

- `agents.transcript` with a negative id is answered by
  `answerLocalControlOutsideTranscript` from the agent's own transcript (a
  surfaced external Claude Code `.jsonl`, or `MachineAgent::transcript`, which
  for Codex is the rollout file the process holds open). Same shape as a
  ForkMesh run plus `outside: true`. CLIs without a transcript answer
  `no_transcript`.
- `AgentLogText` (`src/AgentLogText.*`) renders JSONL as the desktop's
  `transcript.txt` lines (see "Task notifications and rich transcripts on the
  run screen"), dropping tool results, sidechains, meta entries and injected
  messages. ForkMesh's own Claude runs use the same rendering.
- The desktop keeps one tail per followed outside agent
  (`m_localControlOutsideTails`): first read at most 2 MB from the end on a
  line boundary, then only new lines off the UI thread, keeping the newest 128k
  characters; a new or shorter file answers `reset`.
- **One id per session.** `registerExternalSession` reuses the id of the
  machine agent already showing a session; `applyMachineAgents` keeps it while
  surfaced; `agents.list` never lists both. A session whose Claude record
  (`m_claudePeerSessions`) names its uuid stays live while its process runs.
  Processes and transcripts are matched by the Claude record before any
  folder fallback.
- An agent that exits while followed (`unknown_session` with `since` > 0) ends
  the run as done. After a phone prompt, or on returning to the foreground,
  the run screen calls `DesktopTranscriptStream.refollow()` so the reply
  streams in.

## Steering a Claude Code started outside ForkMesh

### How Claude Code sessions talk to each other

- **Discovery.** Every running Claude Code writes
  `<config>/sessions/<pid>.json` (`<config>` is `CLAUDE_CONFIG_DIR` or
  `~/.claude`): `pid`, `sessionId`, `cwd`, `name`, `status`, `procStart`
  (field 22 of `/proc/<pid>/stat`), `peerProtocol: 1`, `peerFeatures`,
  `messagingSocketPath` (`$XDG_RUNTIME_DIR/cc-socks/<pid>.sock`). Pid, start
  time and socket are all checked.
- **Auth.** `<pid>.<sha256 of the socket path>.key` (0600) holds
  `{"peerToken": <32 hex>}`; the client sends `{"type":"auth","token":…}` first
  whenever the key is readable.
- **Frame.** One newline-delimited JSON line, then the write side is closed:
  `{"msgV":1,"msg_id":<uuid v4>,"type":"user","message":{"role":"user","content":<envelope>},"priority":"next"}`.
  The envelope is
  `<cross-session-message from="…" from-name="…">\n<body>\n</cross-session-message>`,
  with `<` that could start a closing tag escaped as `<\`. Lines up to 1 MiB.
- **Delivery.** The receiver queues it as a peer prompt: a new turn when idle,
  folded in at the next tool round when busy. The model is told it came from
  another session.
- **Hold policy.** `crossSessionInbound` (`accept`, `hold`, `refuse`) from
  managed settings, then `--settings`, then `<config dir>/settings.json`;
  repository settings can only tighten it. Unset, a sender without a claimed
  `from-mode` is held when the receiver bypasses permission prompts and
  accepted when it prompts. A held message waits for approval in the
  terminal; a headless session drops it.
- **ForkMesh claims no mode.** The envelope carries only
  `from-name="ForkMesh (phone)"`. A bypass session therefore holds phone steers
  unless its user sets `"crossSessionInbound": "accept"`; the desktop never
  writes that setting.
- **No interrupt** over the peer path; use `forkmesh term` for Stop.
- Every phone steer's body starts with `ClaudePeerMessaging::steerLead()`,
  stating that the user typed this in the ForkMesh app as their next message
  in this conversation. It claims nothing about permissions.
  `AgentLogText` strips the lead (`steerText`) for display.

### Desktop

- `ClaudePeerMessaging` (`src/ClaudePeerMessaging.*`): record scan
  (`readSessions`, `liveSessions` over `~/.claude`, every
  `agent-accounts/claude/<account>` and each process's config folder),
  `peerToken`, `permissionClass`, a byte-exact frame builder, and `send`, which
  refuses symlinks, checks the endpoint's pid and uid (`SO_PEERCRED`,
  `getpeereid`), writes, half-closes and drains.
- `agents.list` marks an outside row `steer: true` when a live record matches
  it (by the pids `findSessionPids` found, else by session id).
- `agents.prompt` with a negative id (`answerLocalControlOutsidePrompt`):
  photos answer `outside_images_unsupported`, empty or long text `bad_task`,
  other CLIs `outside_cli_unsupported`. It predicts the receiver's decision
  with `ClaudePeerMessaging::predictInbound` (settings files, `--settings` from
  the command line, the latest `permissionMode` in the transcript):
  `refuse` sends nothing (`outside_refuses_messages`); `hold` without a
  terminal sends nothing (`outside_held_no_terminal`); `hold` in a terminal
  answers `{"status": "held_for_approval", "cause"}`; `accept` answers
  `{"status": "delivered", "cause"}`; otherwise `not_steerable`. Every message
  is logged with the sending phone.
- Shared fixture: `conformance/claude-peer/v1/fixture.json` (generated from
  the Python reference `tools/claude_peer_message.py`), which both
  implementations must rebuild byte for byte, plus the inbound decision table.

### Phones

`LocalOutsideAgent.steer`; `UnifiedAgentRow.fromOutside` allows prompting only
for a `claude` row marked `steer` (no photos). A `held_for_approval` answer
sets `AgentControlCenter.promptNotice`, explaining how to approve in the
terminal or enable `crossSessionInbound`. iOS `TellAgentIntent` steers such
rows; `StopAgentIntent` explains they cannot be stopped unless `stoppable`.

## Typing into a terminal from the phone (`forkmesh term`)

Peer messages are labelled by Claude Code as another session's, and ForkMesh
does not spoof that. For full control the user runs the CLI inside a terminal
the desktop can type into; what arrives is keyboard input, recorded as the
user's own turn.

### Using it

- `forkmesh term claude` (or `codex`, or any CLI with its arguments). The app
  answers `forkmesh term …` before Qt starts; a standalone `forkmesh-term`
  binary (or symlink) works the same.
- Optional aliases offered in Settings > Phone and never written by ForkMesh:
  `alias claude='forkmesh term claude'`, `alias codex='forkmesh term codex'`.
- `--no-remote` starts with phone typing off; **Ctrl-^** (byte 0x1e) toggles it
  and is swallowed.
- Without a terminal on stdin the wrapper runs the CLI directly.

### The wrapper (`desktop/src/ForkMeshTerm.*`)

A Qt-free pty proxy: `forkpty` with the user's termios and size, raw mode,
bytes passed both ways, SIGWINCH forwarded, job control handled, exit code the
CLI's (128 + signal, 127 if it could not start), non-blocking queued writes.

It listens on `<dir>/<pid>.sock` and writes `<dir>/<pid>.json`, where `<dir>`
is `$XDG_RUNTIME_DIR/forkmesh-term` (Linux), the per-user temp folder plus
`forkmesh-term` (macOS), or `/tmp/forkmesh-term-<uid>`; `FORKMESH_TERM_DIR`
overrides it for tests. Folder 0700 (refused if a symlink or foreign), socket
0600, registration 0600 via temp file and rename; both removed on exit.

Registration: `{"v":1, "pid", "start", "child", "cli", "cwd", "socket",
"remote", "startedMs", "tty"}`.

Socket protocol, one request per connection: header
`FMTERM1 <verb> <fromBytes> <payloadBytes>\n`, then device label and payload;
answer `ok …` or `err <code>`. Only a peer of the same uid, at most four at a
time, 5 s each.

- `hello`: `ok remote=<0|1> paste=<0|1> cli=<name> child=<pid>`.
- `type`: the text is cleaned (CR/CRLF to LF; other C0 controls except tab,
  DEL, C1 and invalid UTF-8 dropped; trailing blank lines trimmed), so a phone
  can only produce printable text and newlines. With bracketed paste on (the
  wrapper tracks `CSI ? 2004 h/l`) it is wrapped in `ESC[200~ … ESC[201~`;
  otherwise newlines become spaces. Enter follows 120 ms later. Refusals:
  `remote_off`, `empty`, `too_long` (64 KiB), `user_typing` (a key typed in the
  last 1.5 s), `rate_limited` (20 a minute), `exited`.
- `key`: `esc`, `ctrl-c`, or `interrupt` (Esc for Claude Code and Codex,
  Ctrl-C otherwise); 30 a minute.

Before typed text the wrapper draws a dim "⌨ typed from <device>" notice on
the line under the cursor and restores the cursor, so nothing scrolls.

### Desktop

- `TerminalHandoff` (`src/TerminalHandoff.*`): `runtimeDir`, `readSessions`
  (only 0600 `<pid>.json` files in a private 0700 folder with the socket in
  it), `liveSessions`, `find` (a row's pid or up to six ancestors),
  `exchange` (peer pid must be the registered wrapper; 2 s connect, 3 s
  answer), `type`, `key`, `TypingGate`.
- `agents.list` marks a matching outside row `typeable` and `stoppable`.
- `agents.prompt` with a negative id prefers the terminal: paired device
  (`TerminalHandoff::admit`), `TypingGate` (1 per second, 10 per minute per
  device, 8000 characters), answering `{"status":"typed","via":"terminal"}`.
  `remote_off`, `exited` or unreachable falls back to the peer path
  (`"via":"peer"`); `terminal_user_typing`, `typing_rate_limited`,
  `not_typeable` otherwise.
- `agents.stop` with a negative id sends `interrupt` to a stoppable row:
  `{"status":"interrupted","key":"esc"}`; others `outside_stop_unsupported`.
- Every prompt and stop is logged with the device and transport.

### Security

- Only a paired device can type: requests arrive over the signed, sealed
  local-control channel; unpaired devices get `not_paired` before any handler,
  and the typing handler checks again. The phone never talks to the wrapper.
- Both the desktop and the wrapper clean the text; only printable text,
  newlines, one Enter, or two named keys are possible.
- The relay sees ciphertext only and cannot forge a device signature.
- Any process of the same user can reach the socket, which adds no power
  beyond what that user already has; other users cannot.
- Rate and size limits apply on both sides, and nothing is typed while someone
  types at the keyboard.
- Every action shows a notice in the terminal and is logged. Ctrl-^ or
  `--no-remote` turns it off within one tick. Typed input carries the user's
  authority; removing the phone in Settings > Phone revokes it.
- Linux and macOS (`getpeereid`, `sysctl`); not on Windows.

## Agents tab layout

- **One list** (iOS) with a tool row: Stop all, sort menu, view button, queue
  menu, recycle bin, terminal, usage bars. The shell's Agents badge counts
  running runs.
- **Views** (`agents/listView`): Compact, Zen (title only), Full (one-word
  columns: status, provider, host, age, conflicts, files, `+added−removed`,
  worktree or branch, columns dropped from the end when they do not fit), and
  Eagle (the default on both). Logic in
  `AgentLines.chips` / `AgentListView` (iOS FMAgents) and `AgentLineChips`
  (Android core).
- **Row** (Compact, Zen, Full): agent dot with status, title, short age, a
  meta line (host glyph, repository, branch, worktree, ahead/behind/conflict,
  files, churn bars on the desktop's log scale, PR), and a thumbnail of the
  first image. Eagle rows differ; see below.
- **Usage bars** (`AgentUsageBars`): one small bar per usage window, coloured by
  state; on iOS they push the native Usage screen. On Android the Usage card
  on Desktop agents opens the native Usage screen when tapped.

### Eagle view on iOS

Each row shows the title and a dialog ticker under it, with an image tile
(`AgentEagleTile`) on the right. Since 2026-10-22 there is no agent dot on the
left, so the text starts at the row's edge (asked for from a phone screenshot:
the dot took width and the churn bars were hard to read).

- The tile shows the first image of the session, or a blank tile when there is
  none. Since 2026-10-25 its border is the token flow (`AgentTokenRing`): two
  1 pt dashed lanes, tokens in on the outer lane and tokens out on the inner
  one, moving in opposite directions while the agent is active. Each lane's
  colour follows its rate: grey idle, blue slow, green steady, orange fast,
  red very fast (`AgentTokenPace`; output steps at 1, 15, 60 and 200
  tokens/s, input at ten times that because each request re-reads the
  context). Its speed follows the rate smoothly, 4 pt/s at 1 token/s plus
  4 pt/s per tenfold, at most 24 (`AgentTokenPace.travel`), and the dash
  position adds up frame by frame (`AgentTokenClock`), so a change of speed
  does not make the dashes jump. The phone measures the rate from how the
  totals grow: once a second from the desktop vitals feed's `agentTokens`
  while it watches the desktop (see NAVIGATION.md, agent token charts), and
  on each list update otherwise (`AgentTokenSampler`: gaps count as at most
  8 s; a second with no growth halves the rate every 1.5 s, down to idle below
  0.5 tokens/s, so bursty arrivals glide instead of stopping each second; the
  rate drops to idle after 10 s with no growth). Asked for on 2026-10-25: the
  ring should move with tokens as they are sent and received. With no token
  counts the border is a hairline. Reduce Motion keeps the lanes still, and
  they pause while the tab is hidden.
- The totals come from the desktop row's `tokensIn` and `tokensOut`, counted
  live as the run streams (`transcripttools::LiveTokenMeter`, since
  2026-10-27). For Claude sessions a `message_start` adds the request's input
  (fresh, cache written and cache read); each `content_block_delta` adds an
  output guess of one token per four characters of text, thinking or tool
  input; the closing `message_delta` usage tops the guess up to the real count,
  and the finished `assistant` message adds only what is still missing, all
  once per message id. Codex agent deltas add the same four-character guess.
  Each `result` resets the totals to the CLI's own counts, so a guess that ran
  high is corrected at the end of every turn. Before, the totals grew only when
  a whole assistant message landed, so the ring and the island charts sat
  still while a long reply streamed. Older desktops without these fields fall
  back to the session's stored `promptTokens` / `completionTokens`.
- Inside the tile, top to bottom: the file count in a small semi-transparent
  pill (`AgentTileFiles`) whose outline is the churn, a 0.75 pt stroke green
  for the share of lines added and red for the share removed (it used to be the
  turning 2 pt border); a row of tiny icons (`AgentTileBadges`, up to four,
  most urgent first: conflict or behind, worktree, branch, commits ahead, pull
  request, then runner, cloud or outside terminal, the same icons as the list's
  meta line, from `AgentLineBadge.of`); and the open background tasks.
- Background tasks are thin 1 pt lines (`AgentTileTasks`, up to ten, or seven
  and `+n` past that), from the desktop row's `backgroundTasks`: the session's
  open backgrounded tasks (`agentclitasks::Ledger::openBackground`). The
  desktop rings `agents.changed` when that count changes. The lines blink while
  the agent is active; Reduce Motion holds them still.
- When the agent has finished successfully the bottom slot shows a small green
  `done` bar instead of the task lines (`AgentTileDone`).
- The status dot (blue running, orange waiting, green done, purple merged, red
  failed, grey queued or stopped; `AgentTileStatusDot`) sits on the tile's top
  right corner and the unread dot on its top left.

- `AgentEagleStrip` keeps the lane as a train of copies of the latest line
  with 5 pt seams, scrolling left at 80 pt/s after a 0.5 s pause, so the lane is
  never empty. A new line replaces copies not yet visible; only the newest
  waiting line is kept. Position is computed from the frame clock
  (`TimelineView(.animation)`, `wrap(at:)`), so rows that leave and re-enter
  a `LazyVStack` resume correctly. Reduce Motion shows the line still.
- Every row shows the agent's last line, running or not (asked for on
  2026-10-16: stopped rows had an empty bottom line). Only running rows scroll
  (`moving`); a stopped row holds its latest line still, faded at the right
  edge, and a row that stops mid-scroll settles on its newest line.
- `AgentLines.dialog(_:)` reads the last 6000 characters of a transcript, drops
  the line being written, and keeps the agent's last block of prose (no tool
  runs, tasks, errors, prompts, code, JSON or tables), stripped to one line of
  120 characters. `AgentEagleFeed` keeps each agent's last non-empty line,
  including after it stops.
- Running desktop rows stream through `ControlWatchFeeds` (at most six, only
  while the Agents screen shows Eagle); cloud runs use `liveText` while running
  and the last non-user message after. A stopped row with no line yet asks
  `recall(_:)` when it appears, which reads its transcript once through
  `AgentControlCenter.transcript` (at most three at a time, each row once until
  the screen leaves Eagle).
- A missing or unknown `agents/listView` reads as Eagle; a phone that stored
  another view is switched once (`agents/listViewEagleDefault`).
- Android (since 2026-10-20) has the same view: `AgentEagleFeed` and
  `AgentEagle.dialog` in `core/.../agents/AgentEagleFeed.kt`, the row and
  ticker in `feature/agents/AgentEagleViews.kt`. See
  [ANDROID.md](ANDROID.md) section 26.

### Sorting

iOS orders (`agents/listSort`, `AgentLineSort.sorted`): Recent (default),
Running, Status (Waiting, Running, Queued, Failed, Stopped, Success, Merged),
Priority (1 first, missing = 50), Name, Repo, Changes. Ties are newest first.
FMAgents also has `AgentListSort` (the desktop's twelve orders), so the names
must not collide.

### Unread dot on each agent

- `agents.list` rows carry `unread`; `agents.markRead {id}` marks one read and
  rings `agents.changed` (fire-and-forget from the phones).
- Phones keep `agents/readBaselineMs` (first load) and `agents/seenFinishedAt`
  (row key to last seen finish, capped at 400). Keys:
  `desktop:<repo lowercased>#<session id>`, `run:<session id>`, iOS also
  `outside:<id>`. A row is read when the phone's stamp is at or after its
  finish; otherwise the desktop flag decides; otherwise a finish after the
  baseline is unread.
- Opening a row records its finish and, for a live desktop row, sends
  `agents.markRead`. iOS `AgentReadTracker`, `AgentUnreadDot`; Android
  `AgentReadState`, `AgentReadMarks`.

### Transcript scrolling (2026-10-21)

A run's transcript follows its newest line until the reader scrolls up; then
new output no longer pulls them back down. A round arrow button appears at the
bottom centre, just above the composer, and takes them back to the end (a dot
on it means something new arrived below). Sending a prompt also goes back to
the end.

- iOS: `TranscriptFollowScroll` (`ForkMesh/Kit`) wraps `AgentRunScreen` and
  `AgentTranscriptList`. iOS 17 has no scroll-phase API, so it reads the
  content frame from a preference: at the end (within 24 pt) it follows again;
  the top edge moving down (the reader scrolling up) stops it; content growing
  with the offset unchanged keeps following.
- Android: `TranscriptFollowBox` (`feature/agents/TranscriptFollow.kt`) wraps
  the `LazyColumn` of `UnifiedAgentScreen` and `AgentScreen`. A drag while
  `canScrollForward` stops following; reaching the end restarts it. The lists
  end in a 1 dp `transcriptEnd()` item so scrolling to the last item lands at
  the true end, even below a reply taller than the screen.

### Navigation stability

Rows are plain buttons that set `AgentsScreen.opened`; one
`navigationDestination(item:)` pushes the page, decided at tap time, so list
rebuilds never close an open agent.

### Your last prompt, pinned (iOS)

Every agent detail screen pins what you last wrote to the agent above its
transcript, so the instruction stays in view while the reply scrolls (asked for
on 2026-10-21): `AgentRunScreen` (desktop, node and runner runs),
`AgentSessionDetailScreen` (a desktop-pushed session), `LaunchedAgentSessionScreen`
(a queued run) and `AgentScreen` (the phone's own agent).

- `PromptDeck` (FMControl) holds your prompts newest first: `.prompt` blocks and
  steers whose sender is you (`TranscriptBlock.steerFromYou`), the user turns
  of a queued run's history, or `AgentEvent.userMessage`. A prompt still
  being sent counts as soon as it shows.
- `AgentPromptDeck` draws it as one compact line, a stack of cards with up to
  two edges peeking out below for earlier prompts, and `3/5` when there is more
  than one.
  - Swipe right to go back to the previous prompt and left to go forward to
    the next (2026-10-22; it used to be pull down and push up).
  - The card follows your finger with a slight tilt and resists past either
    end.
  - Tap to read the whole prompt. Long press to copy it or jump back to the
    latest.
- The edges are drawn as the card's `background`, so they take the card's
  size. They used to be shapes in the same `ZStack`, and a `Shape` fills all
  the height it is offered, so on `AgentRunScreen` the deck grew into a tall
  empty box under the line (fixed 2026-10-22).
- The desktop (`PromptDeckBar`, `docs/desktop/agents-ui.md`) and the web
  session pages (`docs/agents/README.md`) have the same deck.
- A new prompt drops in on top, and the deck returns to it even when you were
  reading back. Cards are keyed by the prompt's position from the first, so
  this animates instead of swapping text.
- VoiceOver reads it as one adjustable element (swipe up for newer, down for
  older). Reduce Motion fades between cards without the tilt.

## Actions on one desktop agent

| Action | Method | iOS | Android |
| --- | --- | --- | --- |
| List, start, transcript, prompt, stop, continue | `agents.*` | `AgentRunScreen` | `UnifiedAgentScreen` |
| Pending tool approvals | `agentops.approvals`, `agentops.prepare` + `agentops.answer` | `ApprovalScreen` | `ApprovalScreen` |
| Agent questions (`AskUserQuestion`) | `agents.questions`, `agents.answer` | `AgentRunScreen` cards | none |
| Changes (committed, uncommitted, untracked) | `agentops.diff` | `AgentDiffScreen` | `AgentDiffScreen` |
| Changed images | `agentops.file` | `DiffImagePreview` | none |
| Merge & clean up | `agentops.prepare` + `agentops.merge` | `MergeCleanupButton`, `MergeLandedScreen` | `MergeCleanupDialog` |
| Queue limit and order | `agents.queue`, `agents.queue.limit`, `agents.queue.move` | `DesktopQueueMenu`, `DesktopQueueSheet` | `AgentQueueViews.kt` |
| Write text with the default agent | `agents.write` | Genie outreach writers | none |
| Worktrees | `agentops.worktrees` | `WorktreesScreen` | `WorktreesScreen` |
| Merge leaderboard | `agentops.mergestats` | `MergeLeaderboardScreen` | `MergeLeaderboardScreen` |
| Delete | `agentops.prepare {action:"delete"}` + `agentops.delete` | run screen (with Undo), swipe, long press | swipe, `AgentDeleteDialog` |
| Recycle bin | `agentops.bin*` | `RecycleBinScreen` | `RecycleBinScreen` |
| Open in its CLI terminal | `terminal.resume` | Terminal button | Terminal button |

- On iOS the actions are top-bar buttons on the run screen: branch diff (a
  green glass square), Merge & clean up, Fix conflicts with agent (while
  conflicted), Delete. On Android they are a button row under the status line.
- Approval, merge, delete and purge are confirmation-token operations checked
  and logged by the desktop. Approving requires device authentication on the
  phone; denying does not. The relay cannot mint a token or read the request.
- Stop on the run screen acts at once; stopping keeps the branch.
- Errors carry the desktop's last notice as `message` when one exists
  (`m_lastOperationNotice`, `m_lastLocalOperationError`), so the phone shows
  why a merge or start failed (`ControlErrorText.withReason`).

### Merge & clean up

- One tap, no confirm: `MergeCleanupModel.mergeNow()` gets the token and
  merges; a `typeToConfirm` from an older desktop is sent back automatically.
- The Changes screen carries the same button when opened from the run screen
  (`canMerge`, `onMerged`).
- iOS animates the button (`MergeToolbar`, `FmRumble` haptics easing in over
  2.8 s, stopping after 12 s; a spinner with Reduce Motion). On success the
  Changes screen becomes `MergeLandedScreen` with a **Go back to agent list**
  countdown (5 s). Leaving rebuilds the Agents tab stack
  (`ShellChrome.stackReturns`, `ShellChrome.leave` trims the path to the last
  `.agents` route), because the merged run's list link is gone.
- Failures show in `MergeFailureNote` above the diff with Try again.
- A phone merge never waits on the desktop. `agentops.merge` and the YOLO and
  Vibe auto-merges (`maybeAutoMergeForSession`) run `mergeWorktreeIntoMain`
  with `unattended`, which never opens a window there: an agent still working
  on the branch is refused ("Stop it, then merge"), and a conflict found while
  updating the branch from its base is refused with `conflicts` (so the phone
  offers Fix with agent) instead of opening the modal merge editor. Before
  this, such a merge opened the editor on the desktop and held the merge lock
  until someone answered it there, so the phone's merge, and every merge after
  it, waited 180 s and failed with "The desktop did not answer in time".
- While a merge started on the desktop waits for an answer there (the "Agent
  is still working" box or the merge editor, `m_worktreeMergeAwaiting`), a
  phone merge is refused at once with a message naming that window instead of
  queueing behind it (`awaitDesktopDuringMerge` also refuses the phone merges
  already queued).

### Set aside and merge

The desktop refuses to merge into a checkout whose uncommitted changes touch
files the merge changes ("Can't merge … yet: the checkout has uncommitted
changes in files this merge touches"). On 2026-10-23 that blocked a phone
merge while another agent was mid-edit in the primary checkout, and the only
fix was at the desktop. Now the phone can do it:

- The phone always sends `offerSetAside: true` with `agentops.merge`. When the
  refusal is only that overlap (the checkout is on the base branch, nothing is
  staged, and every overlapping file is an unstaged edit, `" M"`), the desktop
  answers `checkout_dirty` with the same message instead of `merge_failed`. An
  older phone never sends the flag, so it keeps getting `merge_failed`.
- `MergeCleanupModel.canSetAside` turns on, and `MergeFailureNote` (Changes
  screen) and `MergeCleanupSheet` show **Set aside and merge**, which runs the
  merge again with `setAside: true`.
- The desktop (`desktop/src/MergeSetAside.cpp`, called from
  `prepareWorktreeMerge`) saves all uncommitted work with `git stash create`
  on a branch `wip/<base>-before-<branch>-<yyyyMMdd-HHmmss>` (the shared stash
  list is never touched), keeps the overlapping files' diff, checks only those
  files out at `HEAD`, runs the normal merge, then reapplies the diff with
  `git apply --3way` and unstages it. Other uncommitted files are never
  touched, so an agent working in that checkout keeps its edits.
- If the edits clash with the merge, those files keep conflict markers and
  the `wip/` branch holds the untouched copy. The note saying what happened
  (`putBackNote`) is posted on the desktop and answered as `setAside` next to
  `branch` and `stats`; the phone appends it to the merge message and shows it
  on the landed screen.
- Staged changes, untracked or deleted files, and a checkout on another
  branch still refuse as before: those need a person at the desktop.

### Merge stats

`agentops.merge` answers `stats` next to `branch`:

| Field | Meaning |
| --- | --- |
| `base` | the branch merged into |
| `commit` | the merge commit (absent if not among the newest 20 first-parent merges) |
| `commits` | `git rev-list --count <commit>^1..<commit>` |
| `files`, `insertions`, `deletions` | the merge's first-parent `--shortstat` |
| `velocity` | five windows (`1h`, `24h`, `7d`, `30d`, `52w`): `label`, `count`, `top`, `avg`, `series` (oldest first) |

Read off the GUI thread after the merge (`readLandedMergeStats`), shaped by
`parseMergeShortstatLog` and `mergeStatsJson` in `LocalControlAgentOps.cpp`.
The phones draw a LANDED card and a MERGE VELOCITY card (iOS
`MergeLandedReport`, Android `MergeStatsBlock`), each row one accessibility
element.

### Merge leaderboard

`agentops.mergestats` is read-only (no token) and answers `{repo, branch,
stats}` for the newest first-parent merge into the base of: the session `id`'s
repository, or `owner`/`name`, or the repository open on the desktop. The
screen also shows **Today's activity**: a 24 x 12 grid of 5-minute slots
counting the user's interactions on this device, plus counts synced from the
account's other devices (`UserActivityLog`, `UserActivitySync`,
`/api/accounts/activity-today`, [activity-today.md](../app/activity-today.md)).
Counts are stored as `user_activity_today` (`yyyy-MM-dd|n,n,…`), written at
most once a minute.

### Fix conflicts with the agent

A merge refused with conflicts offers **Fix with agent**, sending the owning
agent `ConflictResolution.sessionPrompt` (merge the base, resolve, keep tests
passing, commit, plus the conflicting files) through `agents.prompt` or the
relay steer. Rows already marked conflicted show Fix conflicts directly. A
conflicting pull request's merge card sends the follow-up to the agent that
owns the pull (`AgentSessionLookup.forPull`, `UnifiedAgentRow.forPull`) or
starts a new agent.

### Delete

- Every row can be deleted, whatever its state:
  - Desktop sessions: `agentops.delete` stops a running session first
    (`stopAgentSessionById`) and waits up to 30 s for the runner to idle.
  - Outside agents: a ForkMesh terminal is closed; a CLI process or surfaced
    session is ended (`killExternalSessionPids`: SIGTERM, then SIGKILL after
    4 s). This is the one delete that asks first.
  - Runner runs: stop if active, discard the branch (retry on
    `session_running` every 2 s, up to 10 times; delete the run anyway after
    that), then delete the run. `no_branch` and `forbidden` still delete the
    run.
- The swipe (iOS `AgentSwipeToDelete` / `AgentSidewaysPan`, which claims only
  drags 1.5 times wider than tall; Android `SwipeToDismissBox`) and long press
  delete at once. The run screen's Delete closes the page and holds the delete
  for 5 s with Undo (`AgentPendingDeletes`); rows being deleted are hidden.
- `agentops.delete` answers when cleanup ends (at most 75 s,
  `kDeleteAnswerWaitMs`; phones wait 90 s) with `settled` and `parts`: each
  `{kind: agent|branch|work|issue|pull|task, name, status:
  deleted|closed|absent|kept|pending, reason?}` (`agentops::deleteParts`).
  `AgentDeleteReport.summary` turns it into one status line.
- Results show inline in the list (`AgentUndoDeleteLine` on iOS, the status
  line on Android).

### Stop all

The red stop button stops every active, stoppable control row except outside
agents (`AgentStopAll.targets`), after a confirmation with the count, and
reports `AgentStopAllOutcome`.

## Writing text with the desktop's agent

- `agents.write {prompt}` (2026-10-19) hands the prompt to the desktop's
  default agent through `askNativePackageAgent`, the same one-shot Claude Code
  or Codex run (plan mode, 5 minutes) the CRM's **Send AI email** uses when the
  host has no model, and answers `{text, agent}` when it finishes. It writes no
  files and starts no agent row; the desktop log records which device asked.
- Errors: `bad_prompt` (empty or over 60,000 characters), `no_writer` (the
  default agent is not Claude Code or Codex), `write_failed` with the agent's
  error as the message.
- Why: production cores have no host model (`AI_API_URL` is unset,
  `docs/app/package-ai-failures.md`), so the CRM's `POST /outreach/auto`
  answers `ai_unavailable` with its prompt. A phone without an agent API key or
  an on-device model had nothing to write with and iPhone auto-send paused
  with "the server has no AI model and no writer on this phone gave a usable
  draft". The paired desktop's agent is now the phone's last writer
  (`docs/native/GENIE.md`).

## Adjusting the desktop's agent queue from the phone

- `agents.list` carries `queue: {running, limit, limitMax, queue: [ids]}`
  (`agentQueuePayload`), the queued sessions in start order (priority, then
  queue order). `limitMax` is 20 or the current higher limit.
- `agents.queue` answers that object; `agents.queue.limit {limit}` (1 to 99,
  else `bad_limit`) calls `setAgentConcurrencyLimit`; `agents.queue.move {id,
  to}` moves a queued session (`not_queued` otherwise) by rewriting only its
  priority to sit between its new neighbours. Both log the device and ring
  `agents.changed`.
- Phones show the controls only when `queue` is present: iOS `DesktopQueueMenu`
  ("running/limit") and `DesktopQueueSheet` (stepper, reorder, move to top,
  stop); Android `AgentQueueViews.kt`. Queued rows read "Waiting — #k in
  queue" with Move to top.

## Recycle bin from the phone

Owner-only agentops methods (`MainWindowLocalControlAgentOps.cpp`, pure parts
in `LocalControlAgentOps.cpp`), capability `agentops.bin`:

- `agentops.bin` answers `{agents, count, truncated, retentionHours, minHours,
  maxHours}`; rows `{id, title, owner, name, branch, provider, deletedAtMs,
  removeInMs}`, newest first, at most 500 (`kMaxBinAgents`).
- `agentops.bin.restore {ids}` (no confirmation) answers the bin plus
  `restored` and `failed`.
- `agentops.bin.purge {ids, token}` needs `agentops.prepare {action:"purge",
  ids}` and answers `purged` and `skipped`.
- `agentops.bin.retention {hours}` (1 to 720) sets the window and sweeps.

`RecycleBinScreen` on both phones: retention menu, Restore all, Empty bin,
selection, per-row restore and delete; deleting for good always confirms.
State in `RecycleBinModel`.

### Restoring brings the branch back

Deleting an agent also removes its worktree and runs `git branch -D`, keeping
the tip under `refs/forkmesh/trash/deleted/<branch>` and trashing the branch on
the cores. Moving the session out of the bin alone used to leave a row whose
branch was gone: a resume runs `git worktree add <path> <branch>` and failed.
A branch that only came back locally was pruned again on the next converge,
because the cores still list it as trashed and only a checked-out branch is
safe from that.

Every restore (bin dialog, Restore all, `agentops.bin.restore`) now goes
through `MainWindow::restoreRecycledAgentSession`. After the store moves the
session back, `restoreDeletedAgentBranch` runs off the UI thread:
`forkmesh::upstream::restoreDeletedBranch` recreates the branch at the
tombstone tip and clears the tombstone, then the worktree is added back at the
session's `worktreeName` under `.worktrees/`. A branch that still exists, or
one with no tombstone, is left alone. Uncommitted files in the deleted
worktree are gone either way; only committed work comes back.

Seen 2026-10-20 (task 2373): agent #2368 was deleted at 18:08:37 and restored
from the bin at 18:08:56. The branch and worktree had to be recreated by hand.

## Moving a phone agent to the desktop

An on-device agent (`AgentScreen`) offers **Continue on desktop** once
started, if the repository is published:

1. The phone stops the run, commits leftovers ("Work in progress, moved to the
   desktop"), returns the clone to the base branch and pushes the branch.
2. It sends `agents.handoff {owner, name, branch, base, task, title}`. The
   desktop validates the branch (`handoffBranchValid`: under `agent/`, no `..`,
   no leading dash or dot, no `.lock`, no revision syntax) and base
   (`handoffRefNameValid`), fetches it (`fetchServedBranch`) and starts an
   agent on it (`startAgentOnExistingBranch`), answering `{id}`.
3. `task` (`AgentDesktopHandoff.task`) names branch and base, repeats the
   original prompt (up to 3000 characters) and lists what happened, newest
   kept, under 7600 characters.

Errors: `unsupported`, `no_checkout`, `branch_not_found`, `not_started` (with
the desktop's `message`). On failure the run stays on the phone. Runner runs
use `agents.takeover` instead ([RUNNER_AGENTS.md](RUNNER_AGENTS.md)).

## Streaming transcripts

- `agents.follow {id, since}` answers like `agents.transcript` plus
  `streaming: true` and registers the follow; the desktop then pushes
  `agents.output` events (`id, transcript, from, next, reset, active, status,
  attachments`, `outside`) when the log grows or status changes. `{id, error}`
  ends a follow; `agents.unfollow {id}` ends it; at most 8 per connection (a
  ninth evicts the oldest idle). Code: `handleLocalControlFollowRequest`,
  `pumpLocalControlFollows`, `LocalControlStream.{h,cpp}`.
- ForkMesh runs: `AgentStore::setLogObserver` fires on every append and the
  follow reads only new bytes (`readLogGrowth`); the cursor counts UTF-16
  units, a split UTF-8 character waits, `\r` is dropped, a shorter file resets,
  growth over 64,000 characters is sent as its newest 64,000 with `reset`.
  `attachments` is sent only when new text names an attachment path.
- Outside agents: a 200 ms size check while followed.
- Pacing: at most every 15 ms on the LAN, 80 ms through the relay, and not
  while the relay socket has frames queued.
- Phones: `DesktopTranscriptStream` applies an event when its `from`
  matches, keeps only the unseen tail of one that starts before `next`, holds
  events that arrive before the follow's answer, re-follows from `next` when
  the hub's link changes, and retries 1 s to 15 s while no link is up.
  `refollow()` re-registers on the same stream (never an unfollow first, which
  could race). `AgentControlCenter.refreshDesktop()` re-reads only the desktop
  half on a bell.
- **One desktop follow, several phone streams.** The desktop keeps one follow
  per connection and session, but a phone streams one run from several places
  at once: the run page, the Agents tab's Eagle feeds (`AgentEagleFeed`) and
  dots watch (`AgentDotsWatch`), both through `ControlWatchFeeds`, and Watch.
  Fixed 2026-10-16, when a reply sent from the phone did not show on its run
  page until the app came back to the foreground or the page was reopened:
  - The Agents tab stops its feeds 500 ms after a run page opens over it
    (`AgentsScreen.leave`), and its dots watch stops when no row runs. Each
    stopped stream sent `agents.unfollow`, which also removed the run page's
    follow. The page still read as streaming, so `load()`, the
    `agents.changed` bell and pull to refresh all skipped the read. Streams of
    one session on one hub now count each other, and `stop()` unfollows only
    when it is the last one.
  - A feed that starts when the reply makes the run active follows from 0 and
    moves the desktop's cursor. The run page then saw a gap and replaced its
    transcript with the newest chunk. On a gap a stream now holds the event,
    reads the missing part with `agents.transcript {id, since: next}` (which
    leaves the follow alone), and then applies what it held. If that read
    fails, it follows again.
  - Pull to refresh on a streaming run page calls `refollow()`. Before, iOS
    did nothing, and Android closed the stream and opened a new one, whose
    follow the old stream's late `agents.unfollow` could remove.
- `TranscriptBlockCache.update(text)` parses only new lines and falls back to a
  full parse on reset.

### Transport

- `TCP_NODELAY` on the LAN control socket and the relay socket.
- iOS `AccountEventChannel.send` writes through one ordered outbox (a
  reordered chunk ends the channel with `relay_out_of_order`).
- The relay's control budget is `NODE_CONTROL_MSG_MAX_PER_WINDOW` per socket
  per 10 s, advertised in the hello as `controlBudget: {windowMs, max}`. The
  desktop paces relayed frames to 90% of it (`SendBudget`) and queues up to
  16 MB rather than let a chunk drop.
- QUIC: iOS uses HTTP/3 for every request to `forkmesh.com` hosts (`FMNet`
  `HTTP3Hosts`). The LAN control channel stays TCP, and the relay channel is a
  WebSocket, which has no HTTP/3 path on the platforms in use.

## Watch

Every desktop row Watch shows is followed with `agents.follow`
(`ControlWatchFeeds`, at most six; the rest are rung every 3 s). Node runs ring
on `cloud-agents-live` and share one read per run across watchers
(`AgentControlCenter.watchTranscript`, reused until the row changes or 30 s).
`AgentWatchController.add` seats agents that start while watching. The Agents
tab's Watch shows every running control row once, adding relay sessions the
control center does not list.

## Task notifications and rich transcripts on the run screen

### Transcript text format

The feed stays plain text, so older phones read every line:

| Line | Meaning |
|---|---|
| `==> <text>` | status line |
| `!! <text>` | error |
| `> <text>` (to a blank line) | the user's prompt |
| `> ⟵ <name>: <text>` | a message sent into the session (`ForkMesh (phone)` for a phone steer) |
| `[<Tool> <arg>]` | tool call (may span lines, closed by `…]` or `]` before a blank line; capped at 200 characters) |
| `▸ <Tool>: <arg>` | a runner's tool call (`claudeToolSummary` in `server/agents.go`) |
| `==> Task <status>: <summary>` then lines indented four spaces | task notification and its result |
| anything else | the agent's markdown |

`AgentLogText::taskNotification` lowercases the status (missing, unexpected or
`cost` becomes `update`), unescapes entities, and cuts bodies over 1,500
characters at a line break, closing an open fence and adding
`…more (N characters not shown)`. IDs, output paths and usage are left out.
ForkMesh's own runs pass stream-json `user` events through
`AgentLogText::userEvent`, which keeps only task notifications.

### Phones

`AgentTranscriptFormat` (iOS `FMControl`, Android `control`, identical logic)
produces blocks: status groups, errors, "You" bubbles, steers, tool runs
(consecutive calls folded past three with per-tool counts), task cards (green,
red or amber by status, result parsed on first open), tables (when the second
line is a separator; columns up to 260 pt/dp), JSON (pretty-printed by
`TranscriptJson.pretty`, depth 64, 200k characters; collapsed past 12 lines),
code boxes, headings, lists, rules and paragraphs. Inline: bold, italic, code,
`[label](https://…)` and bare http(s) links. Drawn one lazy row per block
(iOS `AgentTranscriptBlocksView`, Android `AgentTranscriptBlocks.kt`); iOS
scrolls to the last row by id on the next main-queue turn.

Shared fixture: `conformance/agent-transcript/v1/` (`session.jsonl`,
`transcript.txt`, `blocks.txt`), checked by the desktop and both phones.

The run screen also shows a branch diff button (`branchName`, `baseBranch`):
desktop agents open `AgentDiffScreen`, node runs `BranchDiffScreen`.

## Desktop terminals from the phone

### `terminal.` local control family

Owner-only; capability `terminal.agents` on builds with a pty (not Windows). A
terminal object is `{terminal, provider, label, cli, repositoryOwner,
repositoryName, cwd, startedAtMs, device, running, cols, rows}` (`terminal` is
32 hex; `provider` `claude-code` or `codex`).

| Request | Params | Answer |
| --- | --- | --- |
| `terminal.start` | `owner`, `name`, `provider`, `cols`, `rows`, optional `mode` and `account` | terminal object, `replay: ""` |
| `terminal.attach` | `terminal`, `cols`, `rows` | terminal object, `replay` |
| `terminal.input` | `terminal`, `data` (base64, at most 16 KiB decoded) | `{}` |
| `terminal.resize` | `terminal`, `cols`, `rows` | `{}` |
| `terminal.detach` / `terminal.close` | `terminal` | `{}` |
| `terminal.list` | | `{terminals: [...]}` |
| `terminal.resume` | `id`, `cols`, `rows` | terminal object plus `session`, `replay`, `stopped`, `resumed` |

- Several phones may attach; the newest start, attach or resize sets the size
  (cols 20 to 500, rows 5 to 200).
- `replay` is base64 of the last 512 KiB written, cut at a line start (the most
  that fits one relay frame). Phone emulators keep 10,000 rows of scrollback.
- The iPhone terminal follows new output only while it is scrolled to the
  bottom: Claude Code redraws its spinner several times a second, and pinning
  on every redraw made the scrollback impossible to read. Scrolled up, it stays
  put and shows a jump-to-newest button (`TerminalGridView`).
- The key bar (`TerminalKeyBar`) is two rows of equal-width keys sized to the
  window, so no key sits off screen behind a sideways scroll. Ctrl is a
  one-shot modifier: tap it, then type a letter (Ctrl+X then Ctrl+S sends
  Claude Code's "send now") or tap an arrow (`ESC [1;5` + letter). `^C`
  stays a key of its own.
- Events: `terminal.output {terminal, data, reset}` (coalesced like follows, at
  most 16 KiB; `reset` after falling 256 KiB behind) and
  `terminal.exit {terminal, code}`.
- Errors: `unknown_repository`, `unsupported_provider`, `cli_missing`,
  `guardrails_on`, `terminal_vm`, `terminal_limit` (six), `terminal_failed`,
  `unknown_terminal`, `bad_input`, `unknown_account`, `unsupported`.
- The CLI runs under `forkmesh term` (the app's own binary), so it appears as a
  typeable, stoppable outside row carrying `terminal: <id>`; until the process
  scan finds it (and on macOS) the desktop lists the terminal itself.
- Terminals started from a phone run Claude Code with
  `--dangerously-skip-permissions` and Codex with
  `--dangerously-bypass-approvals-and-sandbox`; repositories with guardrails
  refuse.
- `mode` `login`/`logout` runs the CLI's own auth command in the user's home
  without the wrapper, with provider API keys stripped. `account` selects a
  profile, passed as `CLAUDE_CONFIG_DIR` or `CODEX_HOME`.
- Code: `AgentLoginPty::run`, `LocalControlTerminals.{h,cpp}`,
  `MainWindowLocalControlTerminals.cpp`. Terminals end with the CLI, a phone's
  close, or the desktop quitting.
- Security: owner-only, sealed end to end, fixed command (wrapper plus the CLI
  resolved by `AgentCliLocator`), a repository the desktop has open,
  guardrails honoured, at most six.

### Opening a desktop agent in its Claude Code terminal

`terminal.resume {id}` runs the desktop's `agentTerminalHandoff`: stop a
running headless run, then the provider's CLI with `--resume <session>`
(Codex: `resume <thread>`) in the agent's worktree with the agent account's
config folder. An existing terminal for that agent is attached instead. The
agent's own row carries `terminal: <id>`, and while it runs `agents.prompt`,
`agents.stop` and `agents.continue` go to the terminal (`continue` answers
`terminal_open`). Refusals: `unknown_session`, `cloud_session`,
`unsupported_provider`, `guardrails_on`, `cli_missing`, `no_worktree`, plus the
start errors.

### Phones

`StreamedTerminal` (iOS `FMAgents/AgentTerminal.swift`) and
`AgentTerminalSession` (Android) back both sign-in and desktop terminals
(`DesktopAgentTerminal`): start or attach, buffer early events, re-attach on
link change (1 s to 15 s), batch keys for 25 ms, never retry a failed `start`.
Screens: iOS `DesktopTerminalScreen.swift`, Android `DesktopTerminalScreen.kt`.

The emulators drop CSI sequences with a prefix or intermediate byte (except
`?` on `h`, `l`, `J`, `K`) and read SGR colon sub-parameters. The iOS grid clips
each row to the pane and kerns non-ASCII glyphs to one cell.

## Accounts, usage and terminals from the Usage screen

On iOS the usage bars push `UsageScreen`, whose body is `AgentAccountsPanel`:
per provider, New terminal, every account profile (name, signed-in identity,
usage windows), tap to use for new agents, and a menu with New terminal as
this account, Log in, Log out, Rename, Delete; Add account creates and selects
a profile and opens its sign-in
([../marketplace-usage-feed-mastodon.md](../marketplace-usage-feed-mastodon.md#usage-on-ios)).
Android's `UsageScreen` offers the same actions, with Log in and Log out
started as `terminal.start {provider, mode, account}`
([Usage on Android](../marketplace-usage-feed-mastodon.md#usage-on-android)).

- Capabilities `terminal.accounts` and `agentops.accounts` gate the per-account
  terminals and the account methods.
- `agentops.usage` account rows add `signedIn`, `identity` and `windows`; the
  reply adds `activeProvider`.
- `agentops.usage {refresh: true}` re-reads every Claude Code account from
  Anthropic before answering (within 20 s, coalesced, at most once per 30 s);
  without `refresh` it answers the stored snapshot at once
  ([desktop/agents-ui.md](../desktop/agents-ui.md)).
- `agentops.accounts.select {provider, account}`,
  `agentops.accounts.rename {provider, account, label}`,
  `agentops.accounts.add {provider, label}`,
  `agentops.accounts.delete {provider, account}` answer the new usage summary
  (add also answers `account`). Errors: `unsupported_provider`,
  `unknown_account`, `bad_request`, `account_failed`, `default_account` (the
  default account cannot be deleted). Clients confirm a delete before sending
  it.
- Usage windows carry `windowMs`. `UsageWindowMeter` derives `segments`,
  `elapsedFraction`, `periodLabel` ("Day 2 of 7") and `paceLabel` ("41% over
  pace"); bars are drawn as segments (one per day or hour) with pace marks
  above and below.

## Model and mode on the follow-up and the main prompt

- `agents.list` rows carry `mode`; the reply carries `defaultMode`.
- `agents.start` takes optional `mode`, `vibe` (default false) and `worktree`
  (default true; false runs in the checkout, Claude Code and Codex only).
- `agents.prompt` takes optional `model`, `mode` and `provider`
  (`claude-code` or `codex`). `applyLocalControlAgentChoice` stores them before
  sending; a provider change calls `handOffAgentSessionToProvider` (stop the
  live CLI, switch provider and runtime, carry the transcript). Modes are Auto,
  Ask, Plan, Edit; `syncLivePermissionMode` switches a live Claude Code. A model
  must pass `agentModelMatchesProvider`.
- Phones: `AgentPermissionMode` (main prompt choice in
  `agent/desktop/run-mode`), mode and model chips on the run screen for desktop
  rows (`offersModelChoice`, `offersModeChoice`),
  `AgentModelCatalog.followUpChoices(provider:)` listing the other runtime's
  models as hand-offs.

## YOLO and No worktree on the main prompt

Two toggles on the composer card, shown only when the run goes to the
desktop:

- **YOLO** (`agent/desktop/yolo`, remembered): sends `vibe: true`, so a
  finished run is merged into the default branch, pushed and cleaned up
  (`maybeAutoMergeForSession`). A conflict leaves the branch unmerged with a
  notice; it never opens the desktop merge editor (see Merge & clean up).
- **No worktree** (next run only, Claude Code and Codex): sends
  `worktree: false`; YOLO is dimmed while it is on.
- iOS `QuickLaunchRunOptions`, `QuickAgentLauncher.runOptions`,
  `ShellComposerOverlay.runOptions`; Android `QuickLaunchRunOptions`,
  `QuickRunOptions`.

## Pull to refresh

Every agent detail reloads on pull: the run screen (rows, then transcript,
restarting the follow), the LAN agent screen, the relay session detail and the
queued session screen.

## Tests

- Desktop: `test_local_control.cpp` (`testStream`, `SendBudget`),
  `test_local_control_agentops.cpp`, `test_local_control_handoff.cpp`,
  `test_local_control_terminals.cpp`, `test_agent_log_text.cpp`,
  `test_machine_agent_scan.cpp`, `test_claude_peer_messaging.cpp`,
  `test_forkmesh_term.cpp`, `test_terminal_handoff.cpp`.
- Server: `app/tests/test_node_event_push.py`, `test_mobile_node_devices.py`,
  `test_claude_peer_message.py`.
- iOS: `FMControlTests` (`AgentControlTests`, `RelayControlTests`,
  `DesktopTranscriptStreamTests`, `AgentTranscriptFormatTests`,
  `AgentPermissionModeTests`), `FMAgentsTests` (`AgentLinesTests`,
  `ControlWatchFeedsTests`, `AgentDesktopHandoffTests`,
  `RemoteAgentOpsClientTests`, `RemoteRecycleBinTests`,
  `AgentPendingDeletesTests`, `AgentDeleteReportTests`,
  `DesktopAgentTerminalTests`, `AgentReadStateTests`).
- Android: `LocalControlTest`, `AgentOpsTest`, `AgentLineChipsTest`,
  `AgentTranscriptFormatTest`, `DesktopTranscriptStreamTest`,
  `ControlWatchFeedsTest`, `AgentSwipeDeleteTest`, `AgentStopAllTest`,
  `RecycleBinTest`, `AgentReadStateTest`, `DesktopAgentTerminalTest`,
  `AgentsListTest`.
