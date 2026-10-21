# Issue and forum scale limits

Part of [the App docs](README.md). A repository can be used as a forum: the
net4people/bbs mirror (net4people.forkmesh.com) carries 609 issues and about
5,900 comments in a dozen languages, with single threads over 600 KB of JSON.
Every cap on the issue path was sized for small project trackers and cut that
forum short (the list stopped at 500 open issues). They were raised together
on 2026-10-21; keep paired values in step.

| Limit | Where | Was | Now | Bounds |
| --- | --- | --- | --- | --- |
| `MAX_COLLABORATION_TREE_ENTRIES` | `tools/mirror_gateway.py` | 500 (shared `MAX_TREE_ENTRIES`) | 50,000 | entries listed in `.forkmesh/issues/open` and the other collaboration folders |
| `MAX_JSON_BLOB` | `tools/mirror_gateway.py` | 2 MiB | 16 MiB | one issue record read through `/blobs` (a larger one is cut and fails to parse) |
| `maxIssueRecordBytes` | `server/issue_reconcile.go` | 2 MiB | 16 MiB | one issue record the git node still merges across mirrors (pair with `MAX_JSON_BLOB`) |
| `maxIssueGitOutput` | `server/issue_reconcile.go` | 256 MiB | 1 GiB | all issue records read by one reconcile pass |
| `MAX_JSON_OUTPUT` | `tools/mirror_gateway.py` | 8 MiB | 32 MiB | one `/blobs` batch (50 issue files) |
| `MAX_SEARCH_RESULTS` | `tools/mirror_gateway.py` | 60 | 200 | code search hits |
| `MAX_BUNDLE_BYTES` | `app/host/forkmesh_host/repo_views.py` | 8 MiB | 32 MiB | the host-built issues bundle behind the first view |
| `MAX_BODY_CHARS` | `repo_views.py`, `repo_ssr_issue.py` | 60,000 | 262,144 | markdown rendered per body or comment on the server (GitHub allows 65,536 characters) |
| `MAX_EVENTS` | `app/host/forkmesh_host/repo_ssr_issue.py` | 400 | 5,000 | events in the server-rendered issue page |
| labels per list row | `repo_views.py` | 6 | 20 | labels in the server-rendered list |
| `IMMUTABLE_CACHE_ENTRY_MAX` | `app/host/forkmesh_host/repo_fastpath.py` | 4 MiB | 16 MiB | the largest commit-pinned response kept in the host cache |
| `MAX_ISSUE_BYTES` | `app/src/entry.py` | 64 KiB | 256 KiB | a web-posted issue or comment body in UTF-8 bytes (65,536 CJK or Cyrillic characters need up to about 190 KB) |
| `MAX_PENDING_ISSUES`, `MAX_PENDING_DISCUSSIONS` | `app/src/entry.py` | 500 | 2,000 | web posts waiting for the git node |
| `MAX_PENDING_PER_AUTHOR` | `app/src/entry.py` | 50 | 100 | the same, per author |
| `MAX_NOTIFICATIONS_PER_RECIPIENT` | `app/src/entry.py` | 500 | 2,000 | notifications kept per account |
| title, body, labels, per-run counts | `app/src/repository_provider_mirror.py` | 240 chars, 16,000 chars, 8 labels, 10 issues and 20 comments per run, 20 replies per discussion | 256, 262,144, 100, 100, 500, all replies | the in-app provider mirror; `tools/forge_sync.py` (see [forge-sync](../../www/docs/forge-sync.md)) never had caps |

## The issue index

The issues list used to download every open issue record (and the whole
closed history once Closed was chosen) to show 25 titles, and every issue page
downloaded the open records again for mentions and sub-issues. Since
2026-10-21 they read commit-pinned summaries instead. Measured on
net4people/bbs, the first index (with each opening description) cut the
issues bundle from 13 MB (3.6 MB gzip) to 2.7 MB (836 KB gzip); the slim
index below drops the descriptions and the mention lists, and a synthetic
forum of the same shape (609 issues, 6,000 comments, 400 people) comes to
about 490 KB, 56 KB gzip.

- **`issue-index?ref=<commit>`.** One entry per issue folder under
  `.forkmesh/issues/open`, `.forkmesh/issues/closed` and the legacy
  `.forkmesh/issues/<n>` location (an open copy wins over a closed one,
  either over a legacy one, as the list did), newest number first. Each
  entry is the dashboard's `parseIssueJson` result without the event list
  and without the description: number, title, status, deleted flags, author
  and author key, labels, milestone, priority, progress, assignees, dates,
  votes, comment count, the open event id and `location`. Instead of the
  events and the description it carries what the list derives from them:
  `mentionHandles` (every `@name` in the description and comments, for
  `mentions:@me`), `participants` (every event author, for `involves:@me`
  and the assignee picker), `dependsOn` and `childRefs` (the description's
  `Depends on:`/`Parent issue:` numbers and `(sub-issue #N)` links),
  `pullLinks` (`[pull, linked]`, for Development) and the status events
  alone (for the browser's pending close/reopen). Also `missing` (folders
  whose record could not be read), `openFolders`/`closedFolders` (the folder
  counts the tab badge uses) and `truncated`. List rows show no description,
  so none is sent.
- **`issue-index?ref=&related=<n>`.** What one issue page needs: only the
  issues that are its parents, dependencies or `(sub-issue #N)` children,
  that name it on a `Depends on:` line, or that mention it, each cut to the
  fields those panels draw plus `mentionRefs` (`[issue, ts, author key,
  name]`, only the references to `<n>`). The full index keeps
  `mentionRefs` internally but does not send it.
- **`issue-search?ref=&q=`.** Free-text search. Typing in the search box
  filters the loaded rows on number, title, author and the
  status/labels/milestone line at once, as before, and after 250 ms asks
  this for the rest; the answer is the numbers whose number, title,
  description, author, status line **or live comments** contain the text
  (case-insensitive substring, the client's rule), across open and closed
  issues. Differences from the old client-side search: comments are now
  searched too (a deleted comment is not, an edited one by its latest
  text); lower-casing is Python's, which differs from JavaScript's only for
  a handful of characters. The searchable text of each record is kept per
  blob id up to `ISSUE_SEARCH_CACHE_CHARS`, and each answer is commit-keyed
  like the index. One search reads at most `ISSUE_INDEX_READ_BYTES` of
  uncached records, the index's budget, so a public search never holds every
  record of a huge repository in memory; records past it are skipped and the
  answer says `truncated`. If the search cannot be reached the list says so and
  matches the summary fields only.
- **Folding.** `_issue_index_fold` in `tools/mirror_gateway.py` is a port of
  `parseIssueJson` and of `issueDependencyNumbers`, down to JavaScript's
  `Number()`, `String()`, truthiness, whitespace and line-break rules, so a
  row from the index renders, filters and sorts exactly like one from the
  record. A record that is not JSON (or over `MAX_JSON_BLOB`) folds to the
  same defaults the dashboard shows.
  `app/tests/test_mirror_gateway_issue_index.py` runs the dashboard's own
  functions under node against both and compares them, and checks that the
  search finds everything the client-side search found.
- **Cost.** Each record is folded once per blob id (`_issue_index_records`,
  up to `ISSUE_INDEX_RECORD_CACHE` entries) and the whole answer is kept per
  `.forkmesh/issues` tree id, so after a push only the changed records are
  read again. Answers are commit-keyed in the gateway's memory cache and,
  pinned to a commit id, immutable in the host's (`IMMUTABLE`). They are
  sent as UTF-8 rather than `\u` escapes, which matters for a forum in
  Persian, Russian and Chinese and compresses better. At most
  `ISSUE_INDEX_MAX_ISSUES` (50,000) entries and `MAX_JSON_OUTPUT` bytes:
  past either the answer says `truncated: true` and the dashboard uses the
  old reads.
- **Who reads it.** `loadRepoIssues` (after the `.forkmesh/issues` listing
  that names the commit), `loadRepoIssueRelationIndex` (with the issue's
  number on an issue page, the full index on a pull page) and the list
  search in `06-repo-content.js`; `plan_issues` and `plan_issue` (the
  `related` answer) in `app/host/forkmesh_host/repo_views.py`. Full records
  are fetched only for the issue being opened. Because the index has the
  closed issues too, the Closed view needs no second load, and mentions from
  closed issues show on an issue page.
- **Thread first.** An issue page draws its thread as soon as the record is
  read. If the relations (parents, sub-issues, mentions) are not ready
  within `ISSUE_RELATIONS_WAIT_MS` (150 ms) it draws without them and
  redraws once they arrive (`showRepoIssueRelationsLater`), waiting while
  the reader is typing, editing or has a picker open and keeping a comment
  draft.
- **Older gateways.** The operations ride on the `blobs` permission (like
  `pulse` on `stats`), so no signed configuration changes. A gateway or host
  without one answers 404 and the reader falls back: `related` to the full
  index, the index to the per-record `blobs` reads.
