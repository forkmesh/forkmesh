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

The issues list still downloads every open issue record to show one page of
titles; a commit-pinned issue index on the gateway is the next step for a forum
of this size.
