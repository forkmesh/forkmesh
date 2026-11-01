# Claude Code desktop parity

Part of [Desktop](README.md). This page compares the desktop's
[Agents page](agents-ui.md) workspace with the Claude desktop app's **Code**
tab ("Claude Code desktop"): what Claude Code desktop has, what ForkMesh
already does, and what is still missing, with the next candidates to build.
Source: the Claude Code desktop documentation
(code.claude.com/docs/en/desktop) as of 2026-10-25. How the matching
features work is on the Agents page.

ForkMesh runs the real Claude Code CLI (and Codex) in per-session Git
worktrees, so most of the agent itself is shared. The gaps are in the
workspace around it. Claude Code agents also show up in the Claude app itself
([claude-desktop-sessions.md](../agents/claude-desktop-sessions.md)).

Status: **Yes** = ForkMesh has it, **Partly** = something similar or part of
it, **No** = missing.

## Sessions and layout

| Claude Code desktop | ForkMesh | Status |
| --- | --- | --- |
| Session sidebar, new session (Ctrl+N), search, group and sort | Workspace session list with cards, search, Sort, folders, group by repository ([agents-ui.md](agents-ui.md#workspace-layout-in-the-new-interface)) | Yes |
| Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+Shift+] / [ to move between sessions | The same keys move the selection in the session list ([shortcuts](agents-ui.md#shortcuts-and-open-in)) | Yes |
| Rename by clicking the title | Rename from the context menu | Partly |
| Archive, Archived tab, unarchive, auto-archive after a PR merges | Delete with a recycle bin, "Delete merged" | Partly |
| Split view of two sessions, open a session in a new window | Only the composer can be torn off into its own window | No |
| Panes that can be dragged, popped out and docked back | One side panel with Diff, Terminal, Browser and Details tabs; the Details cards can be dragged into any order | Partly |
| OS notification when a session finishes in the background | `notifyAgentDone` through `notifyIfInactive` | Yes |
| Side chat (`/btw`, Ctrl+;): a question with the session's context that is kept out of the thread | | No |
| Background tasks pane (subagents, background shells, Stop) | Sub-sessions and Background cards in the side panels, with Stop ([sub-sessions](agents-ui.md#sub-sessions)) | Yes |
| Plan pane and tasks pane | The plan arrives as an approval card; the live task list under the transcript and the Progress card list the to-do steps ([live task list](agents-ui.md#live-task-list)) | Partly |
| View modes Normal / Thinking / Verbose (Ctrl+O) | Transcript and Raw views; thinking is a model switch | Partly |
| Context usage card, `/compact` | Context card in the side panels ([side panels](sessions-page.md#side-panels)); `/compact` passes through to the CLI | Partly |
| Keyboard shortcut sheet (Ctrl+/) | Ctrl+/ and "Keyboard shortcuts…" in the session list menu | Yes |

## Composer

| Claude Code desktop | ForkMesh | Status |
| --- | --- | --- |
| Permission modes Manual / Accept edits / Plan / Auto / Bypass | Ask / Edit / Plan / Auto (Auto = bypass); a live run switches mode at once | Yes |
| Shift+Tab cycles the permission mode | Shift+Tab in the prompt picks the next mode | Yes |
| Mode remembered per folder | One global mode (`agents/cliPermissionMode`) | Partly |
| Ctrl+Shift+M / I / E open the mode, model and effort menus | The same keys open the composer's pickers | Yes |
| Model and effort pickers, changeable mid-session | Model and Speed pickers; a change applies from the next turn | Yes |
| Typing `/` lists commands and skills | The "/" menu also opens when `/` is the first key in an empty prompt ([typing, drops and modes](agents-ui.md#typing-drops-and-modes)) | Yes |
| `@` file completion while typing | "Mention file from this project…" opens a file dialog; `@` completes people | Partly |
| Attach files, drag and drop into the prompt | Paperclip, paste an image; dropping files attaches images and videos and mentions anything else | Yes |
| Typing while the agent runs queues the message; Esc stops | Follow-ups steer the live run; Stop button | Partly |
| Prompt suggestions as ghost text (Tab accepts) | Suggestion chips on Home only | No |

## Changes, review and pull requests

| Claude Code desktop | ForkMesh | Status |
| --- | --- | --- |
| Changes card with `+N −M`, diff pane (Ctrl+Shift+D) | Changed-files card, Diff tab; Ctrl+Shift+D toggles it | Yes |
| Click a diff line to comment, send all comments to the agent | Line comments exist on pull request diffs, not in the session's Diff tab | Partly |
| `/code-review` card: walk through findings, Fix this one, Apply fixes | "AI review" on a pull request: findings as threads, "Apply fix & commit", "Fix all" | Partly |
| PR status and CI checks in the Changes card, Auto-fix, Merge when ready | Create PR, Merge & clean up, Vibe auto-merge; fixing a failed run is a manual send | Partly |

## Tools around the session

| Claude Code desktop | ForkMesh | Status |
| --- | --- | --- |
| Terminal pane in the session's folder (Ctrl+\`), several tabs | Terminal tab in the worktree; Ctrl+\` toggles it; the side rail has up to 12 terminal tabs | Yes |
| Close the focused pane (Ctrl+\\) | Ctrl+\\ closes the side panel | Yes |
| Browser pane (Ctrl+Shift+B) the agent can drive | Browser tab; Ctrl+Shift+B toggles it; agents drive their own browser windows | Yes |
| `.claude/launch.json` dev servers, Dev servers menu, auto-verify | | No |
| File pane: edit, autosave, conflict handling | Files page editor (Ctrl+S) | Partly |
| Right-click a path: attach, open in editor, show in file manager, copy path | | No |
| Open the session in VS Code / Cursor / Zed or the file manager | Session menu > **Open in**: editors found on PATH and the file manager; **Copy folder path**, **Copy branch name** | Yes |

## Worktrees and environments

| Claude Code desktop | ForkMesh | Status |
| --- | --- | --- |
| Worktree per session, toggle in the composer | Branch toggle, worktree under `<repo>/.worktrees/` | Yes |
| Worktree location and branch prefix settings | Fixed location and `<id>-<slug>` names | Partly |
| `.worktreeinclude` copies gitignored files (such as `.env`) into new worktrees | Same file and rule ([worktree creation](agents-ui.md#worktree-creation)) | Yes |
| Local / Cloud / SSH / WSL environments, saved environments, env vars | Local or Cloud (account agent nodes, mirrors, runners); no SSH sessions | Partly |
| Continue a local session in the cloud | Cloud runs move to the desktop, not the other way | Partly |
| Remote Control from the phone | iOS and Android list, start, steer and stop desktop agents | Yes |
| Cross-session tools (list, read, message sessions) | `list_agents`, `send_agent_message` | Yes |
| Suggested tasks (Start, Do it here, Start all) | `suggest_session` with Start session | Partly |
| Connectors, skills and plugins manager | ForkMesh MCP config per run, skills on special agents | Partly |
| Computer use | Agents drive their own browser windows only | No |

## Next candidates

In rough order of value for the effort:

1. Line comments in the session's Diff tab, sent to the agent as one message
   (the pull request diff's inline composer can be reused).
2. Side chat: a one-off question run with `--resume <id> --fork-session` that
   is shown in a popover and never joins the session.
3. Archive instead of delete, with auto-archive after the pull request merges.
4. A plan pane for plan mode.
5. `.claude/launch.json` dev servers in the Browser tab.
6. Permission mode remembered per repository.
7. Right-click menu on file links in the transcript (copy path, open in
   editor, attach as context).
8. Split view of two sessions.
