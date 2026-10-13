# Repository File Edits

The repository file viewer on `app.forkmesh.com/<owner>/<repo>/blob/<path>`
can edit regular text files in the browser. The dashboard fragments in
`app/public/dashboard/js/05-repo-list-explorer.js` and
`app/public/dashboard/js/06-repo-content.js` render the Edit button, textarea,
branch field and commit-message field. `app/tools/build_dashboard_assets.py`
concatenates those fragments into `app/public/dashboard.js`.

Saving posts JSON to
`POST /api/repo/<owner>/<repo>/file-edit`:

| Field | Meaning |
| --- | --- |
| `path` | Repository-relative file path |
| `branch` | Existing branch to update, or the new branch to create |
| `baseRef` | Existing branch or commit used when `branch` does not exist |
| `oid` | Blob OID the browser loaded; mismatches return `409 conflict` |
| `content` | New UTF-8 text content, capped at 2 MB |
| `message` | Optional commit message; defaults to `Edit <path> on the web` |

`app/src/app_runtime.py` routes only the `file-edit` action to
`app/src/repository_file_edits.py`; all other repository API reads keep the
existing gateway path. The handler reuses the repository page read APIs to
fetch the live tree/blob OIDs, builds one blob, the changed trees and a commit
pack, then sends that pack through `_https_mirror_push_send` with the
`web-file-edit` credential. The commit author and committer are the signed-in
account with its ForkMesh noreply address.

Access is checked before any tree read or push:

- `_authed_account_name` must identify the browser session.
- `_repository_access_context` must allow the actor to see the repository, or
  `_org_write_allowed` must prove write access to the canonical linked repo.
  Private repositories still fail closed as `404` for everyone else.
- `_account_owns_node` allows the owner namespace.
- `_org_write_allowed` allows organization writers with write, maintain or
  admin permission for that repository.

The endpoint edits only existing regular files (`100644` or `100755`). It does
not create new files, delete files, edit binary previews, or bypass protected
branch/push policy; a host refusal is reported back to the browser.

Tests:

| Path | Coverage |
| --- | --- |
| `app/tests/test_repository_file_edits.py` | API against a real bare repository and `git receive-pack`, including owner writes, org writes, branch creation, stale-OID conflicts and private-repo access |
| `app/browser_tests/tests/dashboard-file-edit.spec.js` | Browser flow from file view to Edit, save request payload, bearer token and success status |
