# Watch

The agent fleet as a live picture, on its own page in the desktop's left rail.
Free, bundled with the ForkMesh desktop and installed by default; remove it
from the Market like any other package.

- `content/desktop/AgentActivityModel.{h,cpp}`: the field (`agentvis::Scene`).
  Every agent is a body seated where its Listen voice sounds, every event a
  figure of that kind of work, and the relay's clients, the web visitors from
  the Worker tail, background tasks, log lines, processes and network flows
  are drawn round it.
- `content/desktop/AgentActivityView.{h,cpp}`: paints the field. On the rail
  page it paints in software; Full screen moves it into a window of its own
  that renders on the GPU, so the main window never switches to OpenGL
  composition.
- `content/desktop/AgentNetworkSampler.{h,cpp}`: per-agent sockets via
  `sock_diag` and `/proc/<pid>/fd`, diffed into bytes per host.
- `content/desktop/AgentTrafficWindow.{h,cpp}`: the list opened by clicking
  a host or a connection line: its connections by process, address and port,
  and its traffic flowing in live, newest first.
- `content/desktop/AgentWatchFeature.{h,cpp}`: the rail page and its bar
  (Hear everything, Speak, Sounds, Working only, Network, Processes, Logs,
  Hide output, Visuals & sounds, backdrop, Full screen, Visitors).

The package never reaches into the main window. Everything it draws comes
through the desktop's neutral agent activity feed
(`desktop/src/AgentActivityFeed.h`): the desktop publishes run events,
scheduled notes, log lines, background tasks, relay roster changes, relay
frames and Worker tail events, and answers a small table of host calls
(agents, process roots, Listen, log styles, Cloudflare calls). See
`docs/agents/README.md`.

Settings keep their old `agents/activity*` and `agents/visitorBlocks` keys,
so a desktop that had the built-in Watch window keeps its choices.

Tests: `forkmesh-agent-watch-tests` (the field and the view, offscreen).
