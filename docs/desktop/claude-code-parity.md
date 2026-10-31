# Claude Code desktop parity

This page compares the ForkMesh desktop's Agents workspace with the Claude
desktop app's **Code** tab ("Claude Code desktop"). It lists what Claude Code
desktop has, what ForkMesh already does, and what is still missing. Source:
the Claude Code desktop documentation (code.claude.com/docs/en/desktop), read
on 2026-10-25, and a survey of `desktop/src` and `docs/` the same day.

ForkMesh runs the real Claude Code CLI (and Codex) in per-session Git
worktrees, so most of the agent itself is shared. The gaps are in the
workspace around it. Claude Code agents also show up in the Claude app itself
([../agents/claude-desktop-sessions.md](../agents/claude-desktop-sessions.md)).

Status: **Yes** = ForkMesh has it, **Partly** = something similar or part of
it, **No** = missing. Rows marked *(2026-10-25)* were added on this branch.

## Sessions and layout

| Claude Code desktop | ForkMesh | Status |
| --- | --- | --- |
| Session sidebar, new session (Ctrl+N), search, group and sort | Workspace session list with cards, search, Sort, folders, group by repository ([agents-ui.md](agents-ui.md#workspace-layout-in-the-new-interface)) | Yes |
| Ctrl+Tab / Ctrl+Shift+Tab, Ctrl+Shift+] / [ to move between sessions | The same keys move the selection in the session list *(2026-10-25)* | Yes |
| Rename by clicking the title | Rename from the context menu | Partly |
| Archive, Archived tab, unarchive, auto-archive after a PR merges | Delete with a recycle bin, "Delete merged" | Partly |
| Split view of two sessions, open a session in a new window | Only the composer can be torn off into its own window | No |
| Panes that can be dragged, popped out and docked back | One side panel with Diff, Terminal, Browser and Details tabs; the Details cards can be dragged into any order *(2026-10-27)* | Partly |
| OS notification when a session finishes in the background | `notifyAgentDone` through `notifyIfInactive` | Yes |
| Side chat (`/btw`, Ctrl+;): a question with the session's context that is kept out of the thread | | No |
| Background tasks pane (subagents, background shells, Stop) | A strip for subagents and background shells with Stop | Partly |
| Plan pane and tasks pane | The plan arrives as an approval card; the Progress card in the side panels lists the to-do steps *(2026-10-27)* | Partly |
| View modes Normal / Thinking / Verbose (Ctrl+O) | Transcript and Raw views; thinking is a model switch | Partly |
| Context usage card, `/compact` | Context bar in the Sessions details panel; `/compact` passes through to the CLI | Partly |
| Keyboard shortcut sheet (Ctrl+/) | Ctrl+/ and "Keyboard shortcuts…" in the session list menu *(2026-10-25)* | Yes |

## Composer

| Claude Code desktop | ForkMesh | Status |
| --- | --- | --- |
| Permission modes Manual / Accept edits / Plan / Auto / Bypass | Ask / Edit / Plan / Auto (Auto = bypass); a live run switches mode at once | Yes |
| Shift+Tab cycles the permission mode | Shift+Tab in the prompt picks the next mode *(2026-10-25)* | Yes |
| Mode remembered per folder | One global mode (`agents/cliPermissionMode`) | Partly |
| Ctrl+Shift+M / I / E open the mode, model and effort menus | The same keys open the composer's pickers *(2026-10-25)* | Yes |
| Model and effort pickers, changeable mid-session | Model and Speed pickers; a change applies from the next turn | Yes |
| Typing `/` lists commands and skills | The "/" menu also opens when `/` is the first key in an empty prompt *(2026-10-25)* | Yes |
| `@` file completion while typing | "Mention file from this project…" opens a file dialog; `@` completes people | Partly |
| Attach files, drag and drop into the prompt | Paperclip, paste an image; dropping files attaches images and videos and mentions anything else *(2026-10-25)* | Yes |
| Typing while the agent runs queues the message; Esc stops | Follow-ups steer the live run; Stop button | Partly |
| Prompt suggestions as ghost text (Tab accepts) | Suggestion chips on Home only | No |

## Changes, review and pull requests

| Claude Code desktop | ForkMesh | Status |
| --- | --- | --- |
| Changes card with `+N −M`, diff pane (Ctrl+Shift+D) | Changed-files card, Diff tab; Ctrl+Shift+D toggles it *(2026-10-25)* | Yes |
| Click a diff line to comment, send all comments to the agent | Line comments exist on pull request diffs, not in the session's Diff tab | Partly |
| `/code-review` card: walk through findings, Fix this one, Apply fixes | "AI review" on a pull request: findings as threads, "Apply fix & commit", "Fix all" | Partly |
| PR status and CI checks in the Changes card, Auto-fix, Merge when ready | Create PR, Merge & clean up, Vibe auto-merge; fixing a failed run is a manual send | Partly |

## Tools around the session

| Claude Code desktop | ForkMesh | Status |
| --- | --- | --- |
| Terminal pane in the session's folder (Ctrl+\`), several tabs | Terminal tab in the worktree; Ctrl+\` toggles it *(2026-10-25)*; the side rail has up to 12 terminal tabs | Yes |
| Close the focused pane (Ctrl+\\) | Ctrl+\\ closes the side panel *(2026-10-25)* | Yes |
| Browser pane (Ctrl+Shift+B) the agent can drive | Browser tab; Ctrl+Shift+B toggles it *(2026-10-25)*; agents drive their own browser windows | Yes |
| `.claude/launch.json` dev servers, Dev servers menu, auto-verify | | No |
| File pane: edit, autosave, conflict handling | Files page editor (Ctrl+S) | Partly |
| Right-click a path: attach, open in editor, show in file manager, copy path | | No |
| Open the session in VS Code / Cursor / Zed or the file manager | Session menu > **Open in**: editors found on PATH and the file manager; **Copy folder path**, **Copy branch name** *(2026-10-25)* | Yes |

## Worktrees and environments

| Claude Code desktop | ForkMesh | Status |
| --- | --- | --- |
| Worktree per session, toggle in the composer | Branch toggle, worktree under `<repo>/.worktrees/` | Yes |
| Worktree location and branch prefix settings | Fixed location and `<id>-<slug>` names | Partly |
| `.worktreeinclude` copies gitignored files (such as `.env`) into new worktrees | Same file and rule *(2026-10-25)*, see below | Yes |
| Local / Cloud / SSH / WSL environments, saved environments, env vars | Local or Cloud (account agent nodes, mirrors, runners); no SSH sessions | Partly |
| Continue a local session in the cloud | Cloud runs move to the desktop, not the other way | Partly |
| Remote Control from the phone | iOS and Android list, start, steer and stop desktop agents | Yes |
| Cross-session tools (list, read, message sessions) | `list_agents`, `send_agent_message` | Yes |
| Suggested tasks (Start, Do it here, Start all) | `suggest_session` with Start session | Partly |
| Connectors, skills and plugins manager | ForkMesh MCP config per run, skills on special agents | Partly |
| Computer use | Agents drive their own browser windows only | No |

## What was added on 2026-10-25

**Workspace shortcuts.** `src/WorkspaceShortcuts.h` holds the table; the
workspace (`NextAccess::installWorkspace`) makes a window-wide `QShortcut` for
each bound row and turns them on only while the new layout is applied and the
Agents page is visible (`Chrome::enableShortcuts`, driven by the page's
Show/Hide), so Ctrl+Tab and the others never steal keys from other pages.
macOS uses the Control key (`Qt::META`) for Ctrl+Tab and Ctrl+\`, as Claude
does, since Cmd+\` switches windows there. Ctrl+N stays with the sidebar's
New session button and Shift+Tab with the composer; both are listed on the
sheet but bound where they were. The mode, model and effort menus only open
when that picker is visible and enabled.

**Shortcut sheet.** Ctrl+/ (or the session list's **…** menu) opens
`workspaceShortcutSheet`, a dialog listing every row of the table.

**Shift+Tab in the prompt** moves `quickAddModeSelector` to its next mode,
wrapping, the same as picking it (the setting is saved and a live run gets the
new mode on its next message). The key is not taken when the mode picker is
hidden.

**`/` opens the command menu.** When the prompt is empty, no chat room is
open and the "/" button is visible, typing `/` puts the `/` in the prompt and
opens the "/" menu (`openQuickAddSlashActions`) with its filter focused, so
the next letters filter the list. Picking a row removes that `/` first.
Closing the menu without picking (Esc) puts what was typed in the filter back
after the `/`, so a prompt that starts with a path such as `/etc/hosts` can
still be typed.

**Dropping files on the prompt.** The prompt's viewport takes drags of local
files (`src/PromptDrop.h`). Images (`png jpg jpeg gif webp bmp`) and videos
are queued as attachments (`queueQuickAddImage`); any other file or folder is
inserted at the drop point as an `@` mention, relative to the first of the
selected session's worktree, the workspace's repository and the Issues
repository that holds it (absolute otherwise), folders with a trailing `/`, quoted when the path has a
space. Drops of text or web links keep the editor's default behaviour.
`MainWindow::eventFilter` returns early for event types it does not list,
so `DragEnter`, `DragMove` and `Drop` were added to that list; without them
the prompt's handler never saw a drop.

**Open in.** The session **…** menu has **Open in** with VS Code, VSCodium,
Cursor, Zed and Windsurf when their command (`code`, `codium`/`vscodium`,
`cursor`, `zed`/`zeditor`, `windsurf`) is on PATH (`src/WorkspaceOpenIn.h`),
then **File manager**, plus **Copy folder path** and **Copy branch name**. The
folder is the session's worktree (`m_streamWorktree`, else
`cachedSessionWorktreeForUi`, which never blocks), else the repository
checkout. Editors installed only as macOS app bundles are not found yet.

**`.worktreeinclude`.** When ForkMesh creates an agent's worktree and the
repository's checkout has a `.worktreeinclude` file, files that match its
patterns (gitignore syntax) **and** are ignored by Git are copied from the
checkout into the new worktree before the agent starts
(`MainWindow::copyWorktreeIncludes`, `src/WorktreeInclude.h`). Git does the
matching: `git ls-files --others --ignored --exclude-from=.worktreeinclude`
lists the matches and `git ls-files --others --ignored --exclude-standard
--directory` the ignored set; a match is copied when it is in that set or
under an ignored folder. Files already in the worktree are never overwritten,
symlinks are skipped, and the copy runs on a worker thread. The agent log gets
one `== .worktreeinclude: copied N files` line. Tracked files are never
copied, since the worktree already has them. Without the file nothing changes.

Tests: `forkmesh-workspace-parity-tests` (`tests/test_workspace_parity.cpp`)
checks the shortcut table has no duplicate keys, editor detection, the drop
plan, and `.worktreeinclude` end to end in a scratch Git repository.

## Next candidates

In rough order of value for the effort:

1. Line comments in the session's Diff tab, sent to the agent as one message
   (the pull request diff's inline composer can be reused).
2. Side chat: a one-off question run with `--resume <id> --fork-session` that
   is shown in a popover and never joins the session.
3. Archive instead of delete, with auto-archive after the pull request merges.
4. A tasks pane fed by the latest TodoWrite, and a plan pane for plan mode.
5. `.claude/launch.json` dev servers in the Browser tab.
6. Permission mode remembered per repository.
7. Right-click menu on file links in the transcript (copy path, open in
   editor, attach as context).
8. Split view of two sessions.
