# Repository File Edits

The file viewer at `app.forkmesh.com/<owner>/<repo>/blob/<path>` can edit
existing text files. Saving posts to
`POST /api/repo/<owner>/<repo>/file-edit`:

| Field | Meaning |
| --- | --- |
| `path` | Repository-relative file path |
| `branch` | Existing branch to update, or a new branch to create |
| `baseRef` | Branch or commit used when `branch` does not exist |
| `oid` | Blob OID the browser loaded; a mismatch returns `409 conflict` |
| `content` | New UTF-8 text, capped at 2 MB |
| `message` | Optional; defaults to `Edit <path> on the web` |

`app/src/repository_file_edits.py` reads the live tree, builds a blob, trees
and a commit pack authored by the signed-in account, and pushes it through
`_https_mirror_push_send` with the `web-file-edit` credential.

Access is checked before any read or push: an authenticated session, repository
visibility (private repositories return 404), and namespace ownership or
organization write permission (`_org_write_allowed`). Only existing regular
files (`100644`, `100755`) are editable; branch protection and push policy
still apply.

Tests: `app/tests/test_repository_file_edits.py`,
`app/browser_tests/tests/dashboard-file-edit.spec.js`.
