# Reference links in the native apps

Parity row 4.34. Added 2026-10-02 because the user asked for every issue, task and
pull request mention to be a link in all of the mobile apps' messages.

## What is a link

`ReferenceLinks` (iOS `FMCode/Collab/ReferenceLinks.swift`, Android
`core/.../collab/RepoTools.kt`) follows the desktop's `ReferenceLinks.h`:

| Written | Opens |
|---|---|
| `#12`, `issue #12` | `forkmesh://<repo>/issues/12` |
| `PR #4`, `pull #4`, `pull request #4` | `forkmesh://<repo>/pulls/4` |
| `owner/repo#3` | `forkmesh://owner/repo/issues/3`, whatever the current repo |
| `abc1234` (7 to 40 hex digits) | `forkmesh://<repo>/commit/abc1234` |
| `https://…`, `forkmesh://…` | that address |

Code spans and fenced blocks are never linked. `ReferenceLinks.runs(text, repo:, commits:)`
returns the text cut into plain and linked runs. When `repo` is nil, only the forms that
name their own repo (`owner/repo#N` and addresses) become links.

Tasks have no number. They are 32-hex ids, so the commit rule would misread them. A task is
linked through its address (`forkmesh://tasks/<id>` or `https://app.forkmesh.com/tasks/<id>`),
which the route parser opens as the task.

## Which repo a bare `#N` means

| Surface | Repo |
|---|---|
| Issue, pull and discussion bodies, timelines, review threads, federated replies | The record's repo |
| Commit messages (repo history, branch diff) | The repo |
| Agent session messages, agent run transcripts | The session's repo (transcripts link only qualified forms when the run names no owner) |
| Chat channels, threads, DMs, chat search, Discord bridge, team board | `forkmesh/forkmesh` (`ReferenceLinks.chatRepo` / `CHAT_REPO`), the repo chat already files issues to |
| Task details, check-ins, reminders, completion notes, replies and history | The task's `repository`, else `forkmesh/forkmesh` |
| Inbox item, ping detail | The item's repo slug, else the repo in its link (`/owner/repo/...` included), else `forkmesh/forkmesh` |
| On-device agent (Android `AgentScreen`) | None: qualified forms and addresses only |

Commit ids are not linked in chat, pings, inbox items, the team board or task history. In
those places a hex word is more often a task id, a key or a hash than a commit.

List rows (the notification list and the inbox list) stay plain, because tapping the row
already opens the record.

## Opening a link

A tap stays inside the app.

- **iOS**: `referenceOpener`, set as the `openURL` action on `LinkedText`, `ChatLinkText`,
  Messages bubbles, review threads and transcripts. It hands `forkmesh://` and `https://`
  addresses to `AppNavigator.handle(url:)`. Anything the route parser does not know, and
  every `mailto:`, goes to the system.
- **Android**: `LinkAnnotation.Clickable` calls `NavigationFeature.openUri`. ForkMesh
  addresses open in the app, other web addresses open in a browser, and anything else goes
  to the platform `UriHandler`.

## `#N` that is a pull request

Issues and pull requests are numbered separately, and a bare `#N` opens the issue route. If
the repo has no issue with that number, the issue screen asks for pull request N. When the
pull request exists, the screen shows it in place (iOS `IssueDetailModel.pull`, Android
`IssueDetailScreen.pullScreen`). "Issue #N has not been published yet." appears only when
neither exists.
