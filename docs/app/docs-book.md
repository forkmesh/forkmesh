# Developer docs page (`/dashboard/docs`)

Added 2026-09-29. Every Markdown file under `docs/` in `forkmesh/forkmesh`, read
from `main`, as one continuous page. A separator line above each file names it and
shows the date of the last commit that touched it. Admins (`users.is_admin`) can
edit any file in place, and can select text to delete blocks or whole sections.
Each save is one commit on `main`.

On a white-label instance the page is the `developer-docs` plugin and is off
unless `INSTANCE_PLUGINS` names it ([../INSTANCES.md](../INSTANCES.md)).

## Code map

| Path | Responsibility |
| --- | --- |
| `app/src/docs_book.py` | `GET /api/docs-book` (the list with contents) and `POST /api/docs-book/save` (one commit). It also builds the commit and pack |
| `app/src/app_runtime.py` | Routes `/api/docs-book*` to `docs_book.route` |
| `app/src/entry.py` `_https_mirror_push_send` | The upstream half of `_https_mirror_push_proxy`, shared by `git push` and the docs page |
| `tools/mirror_gateway.py` `GitRepository.tree` | Tree entries carry `mode` and `oid` |
| `app/public/dashboard/partials/views/docs.html` | The view and its styles |
| `app/public/dashboard/js/09-docs.js` | The page. It is a lazy chunk (`dashboard-page-docs.js`, `dashboard_bundle.LAZY_CHUNKS`) |
| `app/tests/test_docs_book.py` | API against a real bare repository: the pack is fed to `git receive-pack`, then `git fsck --strict` |
| `app/tests/test_docs_book_page.py` | Page wiring, plus the block and section maths run under node |

## Reading

`GET /api/docs-book` answers `{commit, latest, canEdit, docs: [{path, oid,
updated, author, subject, change, content, truncated}]}`. `canEdit` is true when
the session's account is an admin. The handler reads through the App's own public
`/api/repo/forkmesh/forkmesh/...` routes, which are the reads the repository page
makes:

- `tree?path=docs&ref=main` gives the commit and the top level.
- One `tree` per subdirectory, pinned to that commit.
- `blobs` in batches of 40 paths.

Each tree entry's own last-commit fields (`date`, `author`, `subject`) are the
separator's date. The result is cached in process for 30 s per commit, so a
reload costs only the first `tree` call. Files over the gateway's 2 MB blob cap
arrive `truncated` and cannot be edited here.

Files are ordered by directory, with `README.md` first in each, so `docs/*.md`
comes before `docs/agents/`.

## Editing

The pencil on a separator opens a textarea with the file's Markdown and an
optional commit message. Ctrl/Cmd+S saves and Esc cancels.

Selecting text opens a small menu. Everyone gets **Copy**, **Copy Markdown**
(the source of the selected blocks) and **Select section**. Admins also get:

- **Edit** opens the editor with the selected blocks' source selected.
- **Delete N blocks**.
- **Delete section "…"** deletes from the section's heading down to the next
  heading of the same or a higher level.

A delete marks the blocks in red and asks once, stating the block count. After
the confirmation it saves straight away as `Docs: delete … from <path>`.

The page splits each file into blocks itself: a fenced code block, a heading
line, or a run of non-blank lines. It renders each block with the dashboard's
`renderMarkdown`, and each block's element records its source lines, which is
how a selection maps back to Markdown. A consequence is that list items
separated by a blank line render as separate lists. An ordered list keeps its
number through `ol.start`.

Heading ids are `<doc id>--<slug>`. Relative links to another file on the page
jump within the page; other links go to the repository page.

## Save: a commit without a checkout

`POST /api/docs-book/save` takes `{path, oid, content, message}`:

1. Refuse unless the session is an admin (403 `not_admin`), and unless the path
   matches `docs/…/*.md` with no dot segments.
2. Read the root tree at `main`, which pins the parent commit, then each
   directory on the path at that commit.
3. If the file's current blob id is not the `oid` the page loaded, answer 409
   `conflict`. Edits to other files in the meantime do not conflict.
4. Build the new blob, the changed trees and a commit whose author and committer
   are `<account> <account@users.noreply.forkmesh.com>`. Each tree keeps the
   gateway's entry order and modes (`040000` is written `40000`).
5. Pack those objects (version 2, no deltas; everything else already exists on
   the cores) and send `old new refs/heads/main` with `report-status` through
   `_https_mirror_push_send`. That is the same router capability, core
   candidates, ref rules, fsck and replication a `git push` gets. The org alias
   is resolved with `_org_repo_node`.
6. Parse git's report: `ok refs/heads/main` is success. `fetch first`,
   `non-fast-forward` or a lock failure (`main` moved between steps 2 and 5)
   answers 409.

Only SHA-1 repositories are supported (`unsupported` otherwise). A gateway that
does not yet send `mode` and `oid` in tree entries makes saves answer 503
`gateway_outdated`, while reading still works. The gateway change and the App
change therefore both need deploying before editing works.

## Security

- The admin check is `users.is_admin`, not the organization-owner rule
  `git push` uses ([CORE_PUSH.md](../CORE_PUSH.md), "Who can push"). This is on
  purpose: the page was asked for admins. The push is made as the admin's
  account and audited twice. `git.push` has `credential: "docs-page"`, and
  `docs.edit` records the path, the commit and the parent.
- The path pattern and the tree walk only ever replace one existing regular file
  (`100644`/`100755`). A save cannot create, delete or rename files, or write
  outside `docs/`.
- Cookie POSTs are same-origin only (`_account_session_record`). An unverified
  email is refused, as on every POST.
- Repository text reaches the DOM only through `renderMarkdown`, which escapes
  first, or as `textContent`. `test_docs_book_page.py` checks every `innerHTML`
  assignment.

## Traps

- A web save moves the cores' `main`. The source publisher then finds `main`
  behind the cores and merges it back into the checkout
  ([source-publisher.md](../tools/source-publisher.md), `fetch first`). A
  checkout that edited the same lines gets a real conflict there.
- Deleting a level-1 section usually deletes the whole file's text. The
  confirmation's block count is the warning.
