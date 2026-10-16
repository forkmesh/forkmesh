# Move an issue to a task

On the web issue page, anyone allowed to delete the issue (repository owner or
organization admin, `parsed.issueMutationAuthorized`) also sees **Move to
task** in the sidebar, above **Delete issue**. It is the web counterpart of
the desktop Tasks page's **Move to issue** ([tasks-page](../desktop/tasks-page.md)).

## What it does

`moveWebIssueToTask()` in `app/public/dashboard/js/03-issues-profile-io.js`:

1. Asks for confirmation.
2. Creates the task with `POST /api/tasks` (`webIssueTaskBody()`): the issue
   title (cut to 160 characters), its description followed by a
   "Moved from issue owner/repo#N by …" line (cut to fit the 4000-character
   details limit), the repository as `owner/repo`, unassigned, in `general`.
   The repository is left blank when its name does not fit the task API's
   `owner/name` rule, so a mirror with an odd name still moves.
3. Deletes the issue with the same signed `delete`/`self` event that
   **Delete issue** sends. The repository's node removes it everywhere once
   the event syncs; its Git history keeps the record.
4. Opens the new task at `/dashboard/tasks?task=<id>`.

## Order and failure

The task is created first, so a failure never loses the issue's text:

- **Task creation fails** (for example the account is not a member of the
  site's organization, `org_member_required`): nothing changes and the issue
  stays.
- **The issue delete fails**: the new task is moved to trash with
  `DELETE /api/tasks/<id>` so the work is not left in two places, and the
  error says so. If that trash call fails too, the error asks the user to
  delete the task from Tasks.

Comments, labels, assignees and attachments stay in the issue's Git history;
only the title and description move.
