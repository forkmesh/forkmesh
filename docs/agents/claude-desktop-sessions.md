# Claude Code agents as Claude desktop sessions

Since 2026-10-24 every Claude Code agent the ForkMesh desktop runs is also a
session in the Claude desktop app's **Code** tab, where it can be read and
resumed. Codex agents stay ForkMesh-only. Code:
`desktop/src/ClaudeDesktopSessions.{h,cpp}` (Qt Core only, test
`forkmesh-claude-desktop-sessions-tests`) and
`desktop/src/MainWindowClaudeDesktop.cpp`.

## What was already true

- ForkMesh runs Claude Code as the real CLI: `claude --output-format
  stream-json --input-format stream-json --verbose --include-partial-messages`
  (`ClaudeStreamSession::launch`), with `--resume <id>` on later turns, cwd =
  the agent's worktree (`<repo>/.worktrees/<slug>`). The CLI itself writes
  the transcript `<config>/projects/<encoded-cwd>/<session-id>.jsonl` and
  picks the session id; ForkMesh reads it back from the stream's
  `session_id` (`claudeResumeSessionId`, `AgentResumeIdentity.h`) and keeps
  its own copy of the stream events under `data/agents/sessions/`.
- So each Claude Code agent already *was* a Claude Code session. Two things
  kept it out of the Claude app.

## Why the Claude app did not list them (checked on this machine, Claude app 3.6556.2, CLI 2.1.309)

1. **Code-tab sessions are an index, not a folder scan.** The Code tab lists
   `~/.config/Claude/claude-code-sessions/<account>/<org>/local_<uuid>.json`
   records, each naming a `cliSessionId`. A CLI run gets a record only when it
   is imported. The automatic CLI listing and the resume picker skip
   transcripts whose `entrypoint` is `sdk-cli`/`sdk-ts`/`sdk-py`, which is
   what every stream-json run writes, and skip hard-linked files.
2. **Most transcripts are not under `~/.claude`.** ForkMesh runs Claude under
   per-account config dirs (`CLAUDE_CONFIG_DIR=data/agents/accounts/claude/<id>`,
   Settings > Agents accounts). On 2026-10-24 those held ~1,200 transcripts;
   the Claude app only looks in `~/.claude/projects`.

## The supported way in: `claude://resume?session=<uuid>`

The Claude app registers a `claude://` URL handler. `claude://resume?session=<uuid>`
runs its `importCliSession`, which adopts the CLI session **in place** (same
id, record `local_<uuid>`, `adoptedFromOtherSurface: true`), trusts the
folder without a dialog, and switches the window to the session. Tested
results:

| Transcript placed as | Result |
| --- | --- |
| only under a ForkMesh account dir | `transcript_missing` |
| hard link in `~/.claude/projects/<dir>/` | refused (`onMultiLink: refuse`) |
| file symlink | refused ("not a plain regular file") |
| `~/.claude/projects/<dir>` = symlink to the account's project folder | imported |
| symlinked folder under another name (`<dir>--forkmesh-<acct8>`) | imported |
| worktree folder already deleted | imported (readable; resuming needs the folder) |

The Claude app's own Settings > Import > "Import Claude Code CLI sessions"
copies transcripts under fresh ids instead, so ForkMesh does not use it.
Writing the index files directly would be faking it: they are the app's
private, unversioned state and it holds them in memory.

## What ForkMesh does

- **Transcript placement.** `exposeSession` finds `<id>.jsonl`. If it is
  already a plain file under `~/.claude/projects`, nothing changes. Otherwise
  it symlinks the account's whole project folder into `~/.claude/projects`
  under the same name, or `<name>--forkmesh-<first 8 of account id>` when a
  real folder of that name exists. Nothing is moved or deleted; the account
  folder stays the CLI's write target.
- **Registration.** When a Claude Code agent reaches a finished state
  (`updateAgentStatusCell` -> `noteClaudeDesktopCandidate`, once per
  `finishedAtMs`), ForkMesh reads the latest session id off-thread, places
  the transcript and opens the deep link. Links go out one every 2 s, and an
  agent that is running again by then is skipped. Once the link is launched
  the id is saved as `AgentSession::claudeAppSessionId` (`meta.json` key
  `claudeAppSessionId`); a session already registered under the same id is
  not linked again, so archiving it in the Claude app sticks.
- **Launching the link.** On Linux ForkMesh runs `xdg-open` itself (macOS:
  `open`). `QDesktopServices::openUrl` passed only the first 5 of 24 links
  on 2026-10-24 and dropped the rest without an error; Windows still uses it.
  The first build saved ids under `claudeDesktopSessionId` before the link
  went out, so that key is ignored.
- **Existing agents.** 90 s after start (and when the setting is switched
  on), `backfillClaudeDesktopSessions` links every account project folder
  into `~/.claude/projects` and registers every stored, finished Claude Code
  agent that has no `claudeAppSessionId` yet.
- **By hand.** "Open in Claude app" in the Agents list and Sessions context
  menus registers the agent if needed and switches the Claude app to it.
  External (terminal) Claude rows work too.
- **Setting.** Settings > Agents > "Add Claude Code agents to the Claude
  app" (`agents/claudeDesktopSessions`, on by default) stops automatic
  registration; the menu action still works.

## Limits

- The Claude app switches to every session the deep link adds; there is no
  quiet import. A burst of finishing agents moves the Claude window through
  them, two seconds apart. Turn the setting off to avoid that.
- ForkMesh keeps a Claude Code process alive between turns. If you send a
  message to the same agent in the Claude app while ForkMesh also continues
  it, both append to one transcript. Continue an agent in one place at a
  time.
- Deleting the session in the Claude app may delete the transcript through
  the symlink. ForkMesh's own copy of the stream in `data/agents/sessions/`
  is untouched.
- **Codex** has no Claude app counterpart; Codex agents stay in the ForkMesh
  Agents panel and Sessions tab, which list, open, continue and show the
  transcript of both kinds as before.
- **Cloud and runner agents** (mirrors, cores, r1; [README.md](README.md),
  [../server/agent-jobs.md](../server/agent-jobs.md)) run on another machine,
  so their Claude session is not on this disk and "Open in Claude app" is
  disabled for them. A queued run taken over with "Continue on this desktop"
  runs locally and then registers like any other; a run another node already
  started stays there.
- **Phone and web** list agents through the relay and are unchanged; the
  saved `claudeAppSessionId` is local metadata.
