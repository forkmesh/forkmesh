# Move an issue to a task

Every client can turn a repository issue into an organization task and delete
the issue: the web issue page, the desktop issue view, and the iOS and Android
issue screens. It is the reverse of the desktop Tasks page's **Move to issue**
([tasks-page](../desktop/tasks-page.md)).

## What every client does

1. Asks for confirmation (the phones also ask for the device passcode or
   biometrics, as their Delete does).
2. Creates the task with `POST /api/tasks`: the issue title (cut to 160
   characters), its description followed by a
   "Moved from issue owner/repo#N by …" line (cut to fit the 4000-character
   details limit), the repository as `owner/repo`, unassigned, in `general`.
   The web and phone clients leave the repository blank when its name does
   not fit the task API's `owner/name` rule, so a mirror with an odd name
   still moves.
3. Deletes the issue (see below).
4. Opens the new task (web, desktop, Android) or reports it by title (iOS).

The task is created first, so a failure never loses the issue's text:

- **Task creation fails** (for example the account is not a member of the
  site's organization, `org_member_required`): nothing changes and the issue
  stays.
- **The issue delete fails**: the new task is moved to trash with
  `DELETE /api/tasks/<id>` so the work is not left in two places, and the
  message says so. If that trash call fails too, the message asks the user to
  delete the task from Tasks.

Comments, labels, assignees and attachments stay in the issue's Git history;
only the title and description move.

## Who sees it and how the issue is deleted

| Client | Shown when | Delete |
| --- | --- | --- |
| Web | `parsed.issueMutationAuthorized` (repository owner, site admin, or organization owner/admin), the same gate as **Delete issue** | signed `delete`/`self` event to `/api/repo/<owner>/<repo>/issues` |
| Desktop | the repository is writable here (`IssueStore::canWrite()`), the same gate as **Delete issue** | `IssueStore::removeIssue()` in the local checkout |
| iOS / Android | `canManageIssues`: the account owns the repository, is a site admin (iOS), or is an owner/admin of the owning organization | signed `delete`/`self` event, as on the web |

The relay only accepts a signed issue `delete` from a repository manager
(`_authorize_issue_manager` in `app/src/entry.py`), which is why the phones
check the organization role first rather than reusing their author-only
**Delete my issue** gate.

## Where the code lives

- Web: `moveWebIssueToTask()` and `webIssueTaskBody()` in
  `app/public/dashboard/js/03-issues-profile-io.js`; the sidebar button
  `data-repo-issue-move-to-task` in `06-repo-content.js`.
- Desktop: `MainWindow::moveCurrentIssueToTask()` in
  `desktop/src/MainWindowIssues.cpp`, reached from the sidebar's **Move to
  task** and the issue list's right-click **Move to task…**.
- iOS: `IssueTaskMove` in
  `native/ios/ForkMeshCore/Sources/FMCode/Collab/IssueTaskMove.swift`,
  `ApiService.canManageIssues`, and **Move to task** in the issue's actions
  menu (`IssueToolsMenu`).
- Android: `IssueTaskMove` in
  `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/collab/IssueTaskMove.kt`,
  `CodeServices.canManageIssues`, and **Move to task** in the issue tools card
  (`IssueToolsCard`).
