# Developer docs page (`/dashboard/docs`)

Every Markdown file under `docs/` in the main repository, read from `main`,
shown as one continuous page. A separator above each file names it and shows
its last commit date. Admins (`users.is_admin`) can edit a file in place or
delete selected blocks or sections; each save is one commit on `main`. On a
white-label instance the page is the `developer-docs` plugin, off unless
`INSTANCE_PLUGINS` names it ([../INSTANCES.md](../INSTANCES.md)).

## Code map

| Path | Responsibility |
| --- | --- |
| `app/src/docs_book.py` | `GET /api/docs-book` and `POST /api/docs-book/save`; builds the commit and pack |
| `app/src/entry.py` `_https_mirror_push_send` | Upstream half of the HTTPS push proxy, shared by `git push` and this page |
| `tools/mirror_gateway.py` `GitRepository.tree` | Tree entries carry `mode` and `oid` |
| `app/public/dashboard/partials/views/docs.html`, `js/09-docs.js` | The page (lazy chunk) |
| `app/tests/test_docs_book.py`, `test_docs_book_page.py` | API against a real bare repository; page wiring and block maths |

## Reading

`GET /api/docs-book` answers `{commit, latest, canEdit, docs: [{path, oid,
updated, author, subject, change, content, truncated}]}`. It reads through the
App's public repository routes (`tree` pinned to one commit, `blobs` in
batches of 40) and caches the result for 30 s per commit. Files over the
gateway's 2 MB blob cap arrive `truncated` and cannot be edited. Files are
ordered by directory with `README.md` first.

## Editing

The page splits each file into blocks (a fenced code block, a heading, or a
run of non-blank lines), renders each with `renderMarkdown`, and records each
block's source lines so a selection maps back to Markdown. The selection menu
offers Copy, Copy Markdown and Select section to everyone; admins also get
Edit, Delete N blocks, and Delete section. Deletes confirm once and save
immediately.

## Save: a commit without a checkout

`POST /api/docs-book/save` takes `{path, oid, content, message}`:

1. Refuse unless the session is an admin (403 `not_admin`) and the path is
   `docs/…/*.md` with no dot segments.
2. Read the root tree at `main` (pinning the parent), then each directory on
   the path.
3. If the file's blob id is not the loaded `oid`, answer 409 `conflict`.
4. Build the new blob, changed trees and a commit authored as
   `<account> <account@users.noreply.forkmesh.com>`.
5. Pack the new objects (version 2, no deltas) and send
   `old new refs/heads/main` through `_https_mirror_push_send`, with the same
   capability checks, ref rules, fsck and replication as `git push`.
6. `ok refs/heads/main` is success; `fetch first`, `non-fast-forward` or a
   lock failure answers 409.

Only SHA-1 repositories are supported. A gateway without `mode` and `oid` in
tree entries makes saves answer 503 `gateway_outdated`.

## Security

- The check is `users.is_admin`, not the repository push rule
  ([CORE_PUSH.md](../CORE_PUSH.md)). The push is made as the admin's account
  and audited as `git.push` (`credential: "docs-page"`) and `docs.edit`.
- A save can only replace one existing regular file under `docs/`; it cannot
  create, delete or rename files.
- Cookie POSTs are same-origin only; unverified emails are refused.
- Repository text reaches the DOM only through `renderMarkdown` (escapes
  first) or `textContent`; `test_docs_book_page.py` checks every `innerHTML`.

## Traps

- A web save moves `main` on the cores; local checkouts must merge it back
  ([source-publisher.md](../tools/source-publisher.md)).
- Deleting a level-1 section usually deletes the whole file's text.

## Notices and the selection menu

The page notice (`data-docs-notice`) is an `fm-flash` (success, danger or blue
info) with its action (**Try again**, **Reload**) as an `fm-btn-sm` on the
right. Refresh and the editor's Cancel and Save are `fm-btn-sm`; Save is green
because it commits. The floating selection toolbar is an `fm-popover` with
28px rows and muted icons (destructive rows stay red). Focused inputs show a
1px inset blue line instead of a glow.
