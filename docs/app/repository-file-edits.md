# Repository File Edits

The file viewer at `app.forkmesh.com/<owner>/<repo>/blob/<path>` can edit
existing text files. Saving posts to
`POST /api/repo/<owner>/<repo>/file-edit`:

| Field | Meaning |
| --- | --- |
| `mode` | `commit` (default), `propose` or `probe`; see below |
| `path` | Repository-relative file path |
| `branch` | Existing branch to update, or a new branch to create (`commit`); the base branch (`propose`) |
| `baseRef` | Branch or commit used when `branch` does not exist (`commit` only) |
| `oid` | Blob OID the browser loaded; a mismatch returns `409 conflict` |
| `content` | New UTF-8 text, capped at 2 MB |
| `message` | Optional; defaults to `Edit <path> on the web` |
| `create` | `true` to add a new file; `409 conflict` if the path exists |
| `encoding` | `base64` when `content` is a file's bytes (an upload); otherwise text |
| `op` | `edit` (default), `delete` or `rename` |
| `newPath` | The new path for `rename` (may be in another folder); `409 conflict` if taken |

`app/src/repository_file_edits.py` reads the live tree, builds a blob, trees
and a commit authored by the signed-in account.

## Who can do what

Access is checked before any read or push: an authenticated session (otherwise
`401`), repository visibility (private repositories the caller cannot read
return `404`), then write permission: namespace ownership or organization write
permission (`_org_write_allowed`).

| Caller | `probe` | `commit` | `propose` |
| --- | --- | --- | --- |
| Signed out | 401 | 401 | 401 |
| Can read, cannot write | `{"access": "propose"}` | 403 `forbidden`, `canPropose: true` | proposal |
| Writer | `{"access": "write"}` | pushed | proposal |

A direct write by a non-writer is always refused; there is no path from
`propose` to a push.

## Commit

The commit pack is pushed through `_https_mirror_push_send` with the
`web-file-edit` credential. Only regular files (`100644`, `100755`) are
editable; a protected default branch refuses the edit at the gateway
([branch-protection.md](branch-protection.md)) and push policy still applies.

## Proposals: editing without write access

Like GitHub, a reader without write access can still edit a file: the change
becomes a pull request. ForkMesh does not need a fork or a branch on the
repository for this. Its pull requests carry a signed, self-contained patch and
format-patch commit series (the same model forks and
[remote forks](remote-forks.md) use), and the node that takes it in turns that
series into commits on the base it was cut from and merges those
([../server/pull-heads.md](../server/pull-heads.md)). A website fork needs a linked managed
mirror and finishes asynchronously, so it cannot back an instant edit; pushing
a namespaced branch into someone else's repository would give readers a write
path they do not have today.

`mode: "propose"` therefore builds the same commit as `commit` but pushes
nothing. It reads the current blob at the branch tip (checking its OID), and
returns:

| Field | Meaning |
| --- | --- |
| `proposal` | `true` |
| `base`, `baseOid` | Base branch and the commit the change was cut from |
| `head`, `headOid` | Provenance name `<account>/edit-<file>-<short oid>` and the built commit |
| `patch` | `git diff --full-index` style patch for the one file |
| `commits` | One-commit format-patch mbox with the author and message |
| `subject` | The commit message, used as the pull request title |

The base branch must exist; proposals never create branches. The browser then
signs the pull with its web key (`signWebPullContent`, the signer behind the
new pull request form), posts it to `/api/repo/<owner>/<repo>/pulls`, stores
an optimistic record and opens the new pull request. The `head` name is
provenance only: it is not a branch on the repository, so the node stores the
pull as a patch rather than a branch-backed pull. Each proposal is audited as
`repository.file_edit_proposal`.

## Editor

The editor asks `mode: "probe"` when it opens. A reader gets the notice "You
don't have write access to this repository. Saving proposes your change as a
pull request.", the branch field is fixed to the current branch as the base,
and the button reads **Propose changes**. A writer keeps the current behaviour
and can tick **Propose as a pull request instead of committing**. A `403` with
`canPropose` on save (for example after a permission change) switches the open
editor to proposing. A signed-out visitor who clicks **Edit file** is sent to
`/login?next=<this page>`.

## Rename and delete

The file view's **Rename or move** and **Delete** buttons open a small form
under the toolbar (`openRepoFileMoveForm`) with the branch, a commit message
and the same propose toggle as the editor. `op: "delete"` removes the file
(a folder it leaves empty goes too, as Git keeps no empty trees);
`op: "rename"` moves it to `newPath`, keeping its text unless `content` is
sent. Both check `oid` against the file the browser loaded. Writers commit;
everyone else gets a proposal whose patch is a deletion (plus a creation for
a rename), so `git am` replays it without rename detection. A delete lands
on the folder, a rename on the file's new page.

## Add file

The Code tab's **Add file** button (both toolbars) opens a new-file form in
the file viewer (`openRepoNewFile`): a path that starts in the folder being
browsed, the text, the branch and a commit message. It probes access like the
editor: a writer commits with `create: true` and lands on the new file's
page; a reader (or a writer who ticks the box) proposes the file as a pull
request through the same signed proposal flow. **Upload a file instead** sends any
file up to 2 MB as base64 (`encoding: "base64"`), committed byte for byte; a
proposal carries it as a `GIT binary patch` (`binary_patch`) that `git am`
replays. A read-only mirror shows the
button disabled with the fork hint, and a signed-out visitor is sent to
`/login?next=<this page>`. Tests: `app/tests/test_web_add_file.py`.

Tests: `app/tests/test_repository_file_edits.py` (proposals replay with
`git am --3way`, the patch matches `git diff`, writers still commit, direct
writes by readers are refused, signed-out callers get 401),
`app/browser_tests/tests/dashboard-file-edit.spec.js` (writer save, reader
proposal opening `/pull/<n>`).
`app/tests/test_dashboard_landing_migration.py` checks that the viewer's
toolbar wires **Edit file** to this endpoint and that the retired browser-only
draft editor (**Edit local draft**, copy and download draft) stays gone.
