# Repository access on the web

Settings > **Access** (owner only, `renderRepoAccessSettings`) covers who can
read a repository without the desktop app.

## Collaborators (private repositories)

`/api/repo/<owner>/<repo>/shares` (`shares_handler`) lists, adds and removes
collaborators. Besides the owner node's signature (`forkmesh-share-v1`, used
by the desktop), it now accepts the owner's own account session: the account
that is, or owns, the repository's node (`_shares_session_owner`). A
collaborator reads the repository and takes part in its issues, pull requests
and discussions ([web-discussions.md](web-discussions.md#who-may-do-what)).

- A repository the core nodes host is read through them, so its
  collaborators need no encryption key; `_hosted_read_allowed` counts a
  collaborator as a reader.
- A repository held by a personal node is sealed to each reader's key by
  that node, so a collaborator there still needs an encryption key
  (`grantee_encryption_key_required`), and the node re-seals on its next sync.

## Visibility (core-hosted repositories)

`POST /api/hosted-repositories/visibility {owner, name, visibility}`
(`_change_visibility` in `app/src/hosted_git.py`): the owner, or an admin of
the organization the repository belongs to, makes it public or private. The
catalog record, its plaintext privacy flag and the hosted row change
together; caches are cleared, the public catalog is republished and the cores
serve the new visibility from their next replica sync. Copies other people
already cloned or mirrored are not recalled. A repository held by a personal
node answers `409 node_held`: that node signs its own catalog record.

## Still on the desktop

Renaming and transferring a repository: see
[web-desktop-parity.md](web-desktop-parity.md).

Tests: `app/tests/test_web_repo_access.py`.
