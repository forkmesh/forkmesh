# Releases and tags on the web

Writers draft, edit and delete releases on the Releases tab, and a release
creates its tag, without the desktop app.

## What a web release is

A Git tag plus notes kept in the repository, where every client already
reads releases from: `.forkmesh/releases/<tag>/release.json` on the default
branch.

```json
{"tag": "v1.0.0", "name": "Plugins 1.0", "body": "## Highlights …",
 "prerelease": false, "tag_commit": "<commit>", "assets": [],
 "created_at": 1792550000000, "author": "alice", "source": "web"}
```

`source: "web"` marks it as editable here. Signed release channels (a folder
with `release.json.sig`, and the `latest` / `candidate-*` channels the
installers read, see [docs/release](../release/README.md)) are never written
from the web: changing them would break the publisher's signature.

## Route

`POST /api/repo/<owner>/<repo>/release-edit` (`app/src/repository_releases.py`),
for accounts with write access (the `/file-edit` permission: the owner, or an
organization member with write):

| `op` | Effect |
|---|---|
| `tag` | create `tag` at `target` (a branch or commit); `409 tag_exists` if it exists |
| `save` | create the tag if missing (at `target`), then commit `release.json` with `title`, `notes` (Markdown, 128 KB), `prerelease`; editing keeps `created_at` and the tag |
| `delete` | remove the release's `release.json` and its web-attached files; the tag stays |
| `asset` | attach a file (`name`, `contentType`, base64 `content`, up to 2 MB, 20 per release): committed at `.forkmesh/releases/<tag>/assets/<name>` and listed in `release.json` with its size, type, SHA-256 and `path` |
| `asset-delete` | remove an attached file |

Commits and the tag go through the same push path as web file edits, so
branch protection and the mirrors' push policy apply. Mirrors never delete or
move a tag (`enforce_push_ref_policy` in `tools/mirror_gateway.py`) without an
owner's tag-move grant, so a web release never rewrites what a tag points to.
Publishing a release with a new tag sends the usual `release_published` ping.
Read-only mirrors refuse the route like `/file-edit`.

## Interface

The Releases tab shows **Draft a new release** to writers: tag, target
branch for a new tag, title, notes with Write/Preview and a pre-release box.
Each web release card has **Edit**, **Delete** and **Attach files** (with a remove button per attached file). Notes render as Markdown
on every release.

## Not on the web

- Large binaries in the mirrors' content-addressed store (the installers'
  channels): they are seeded by a release publisher
  (`tools/forkmesh-release-publish.sh`). Files attached on the web live in the
  repository instead and are downloaded through its raw route.
- Deleting or moving a tag: mirrors keep tags immutable unless the owner
  grants a move through `/api/git/tag-moves` on a core-held repository.

Tests: `app/tests/test_repository_releases.py` (real Git: tag creation, notes,
edits, delete, signed and reserved channels, permissions),
`app/tests/test_web_releases_frontend.py`.
