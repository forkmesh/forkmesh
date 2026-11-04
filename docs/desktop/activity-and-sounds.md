# Desktop: Watch, Listen, announcer and interface sounds

Part of [Desktop](README.md). This page covers the ways the desktop shows and
plays agent activity outside the [Agents page](agents-ui.md): the Watch
package's field of agents, hosts and connections, Listen's music from a run's
events, the spoken announcer, and the app-wide interface sounds.

## Watch (Market package `agent-watch`)

Watch is a bundled, removable Market package with its own rail page; the core
works without it. Package code:
[agent-watch/README.md](../../marketplace/packages/agent-watch/README.md).

The field draws each agent as a body ringed by activity icons, this desktop
at the centre, mirrors, hosts drawn by type, connections and the relay room.
It paints in software on the rail page; **Full screen** uses its own GPU
window, because a `QOpenGLWidget` in the main window would switch the whole
window to OpenGL composition. **Speak** reads replies aloud.

- **Core interface.** The neutral feed `src/AgentActivityFeed.*` (hosted by
  `src/MainWindowAgentFeed.cpp`) publishes agent events, sonifier notes, log
  lines, tasks and relay traffic, and answers `Host` calls. The core names no
  package; packages subscribe as `agentfeed::Listener`.
  `agentfeed::setWatching` turns on external transcript tailing and a faster
  cloud refresh: while Watch is open with a run in flight, the account list
  is read every 5 s (`kCloudAgentWatchRefreshMs`) instead of 2 min. The
  package owns kind mutes (`agents/activityMutedKinds`); **Hide output**
  (`agents/activityLogsHideOutput`) hides forwarded node and mirror journal
  lines.
- **Machine-wide agents.** While Watch shows or local control has a client,
  `tickMachineAgents()` scans `/proc` every 2 s (`src/MachineAgentScan.*`)
  for agent CLIs. Ones not started by ForkMesh get external-band ids and are
  never stored; another login's agents are listed but not read. Phones see
  them as `outside` rows and can steer a live Claude Code session through its
  cross-session inbox ([DESKTOP_AGENTS.md](../native/DESKTOP_AGENTS.md)).
- **Connection traffic.** Clicking a host or line opens `AgentTrafficWindow`:
  per-connection rows keyed by session, pid, address and port, and a live
  feed from `Scene::traffic`.
- **Ids.** The desktop is -2 and seated mirrors use -100 to -899
  (`Scene::kMirrorFirst`). The view looks up mirrors before bodies, so the
  mirror band must never overlap an agent band
  ([session ids](agents-ui.md#session-sources-and-ids)). A body's title is
  `AgentSession::name` (it seeds colour and voice); the task goes in
  `Body::task`.

## Listen

**Listen** (`src/AgentSonifier.*`) plays a run's stream events as music and
rings a cadence at the end; voice parameters are hashed from session id and
title. `agents/listenToNewRuns` listens to new runs automatically.

- **Backends.** Qt Multimedia (`QAudioSink`) when built with it, else raw PCM
  to `pw-cat --raw` or `aplay`. `FORKMESH_SONIFY_BACKEND=silent|pipe|none`
  forces one (tests use `silent`). Rendering runs on the player's thread.
- **Pipe player lookup.** `Player::availablePipePrograms()` looks the pipe
  players up once per `PATH` value and keeps the answer, holding its lock
  across the lookup so a `start()` during a warm-up waits for it. `UiSounds`
  starts the warm-up (`Player::warmPipePrograms()`, on the global thread pool)
  when cues are audible. The lookup stats every `PATH` entry; on WSL, whose
  `PATH` carries ~40 `/mnt/c` directories, doing it on the GUI thread froze
  the first cue for ~1.5 s.

## Announcer

The announcer (`src/AgentAnnouncer.*`, `src/AgentAnnouncerVoice.*`) gives
sportscaster commentary on running agents, spoken by Piper neural TTS,
downloaded on demand with a pinned SHA-256 into `announcer/` under the data
root. Commentary is pulled only when the speaker is idle, so stale lines are
dropped rather than read late.

## Interface sounds

`src/UiSounds.*` (cue table `src/UiSoundCues.cpp`) is an app-wide event
filter installed while `ui/interfaceSounds` is on (default on, except on
`offscreen`/`minimal` and in Qt test mode).

- Widgets with the `forkmeshQuietSounds` property on themselves or a parent
  are silent; the Chat section sets it.
- **Merge & clean up** plays `Cue::Merge`.
- The phones share the cue table ([NAVIGATION.md](../native/NAVIGATION.md));
  change a shared cue here, in `InterfaceCues.swift` and in `SoundCues.kt`
  together.

## Voice Changer

Voice Changer is a free Market package hosted in the desktop, not part of the
core app: [voice-changer/README.md](../../marketplace/packages/voice-changer/README.md).

## Tests

`forkmesh-sonifier-tests`, `forkmesh-ui-sounds-tests`,
`forkmesh-announcer-tests`, `forkmesh-machine-agent-scan-tests`,
`forkmesh-agent-watch-tests`.
