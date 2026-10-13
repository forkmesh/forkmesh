# Git error messages

What a person running stock `git` over HTTPS, or the desktop app, sees when a
clone, pull or push fails, and why it looks that way. The public page is
[`www/docs/git-errors.md`](../www/docs/git-errors.md)
(`https://forkmesh.com/docs/git-errors/`).

## Why plain text

Git shows a server's explanation only for a failed `info/refs` GET, and only
when the body is `text/plain`. It prints each line as `remote: …` under its own
`fatal:` line. A failed `git-upload-pack` or `git-receive-pack` POST shows
nothing from the body (`RPC failed; HTTP 503`). Before 2026-10-09 every git
error from the App and from the node gateway was JSON, so a user saw only "The
requested URL returned error: 503".

## Server side (2026-10-09)

- App: `_git_error_as_text` (`app/src/entry.py`) wraps the three `info/refs`
  routes in `app_runtime.py` (`git-upload-pack` public and private, and
  `git-receive-pack`). A JSON error of status 400 or more becomes one sentence
  plus `Help: https://forkmesh.com/docs/git-errors/`. The status and
  `cache-control`, `retry-after`, `www-authenticate`,
  `x-forkmesh-expected-degraded` and `x-forkmesh-served-by` are kept;
  `WWW-Authenticate` must survive or git never prompts for the push
  credential. A `message` or `hint` field in the JSON wins over the table in
  `GIT_ERROR_MESSAGES`. Other routes, the POSTs and the internal monitoring
  reads (`_routed_repository_read`) still answer JSON.
- Node gateway: `dispatch_error_response` (`tools/mirror_gateway.py`) does the
  same for the public `/<owner>/<repo>.git/info/refs` route that the App's
  307 sends clients to. The capability route `/v1/repositories/…/git-info-refs`
  the App proxies through stays JSON. A restricted repository and a missing one
  get the same 404 text, so the text reveals nothing the JSON did not.

## What the server cannot explain

A transport failure (`Error in the HTTP2 framing layer`, `curl 18`, `early
EOF`, DNS, connect refused) means no answer reached the client, so there is no
body to print. On 2026-10-09 a user on a Mac reported `fatal: unable to access
'https://app.forkmesh.com/forkmesh/forkmesh/': Error in the HTTP2 framing
layer`. The cores' Caddy and gateway logs showed every git request in the
window answered 200 or 307, no Caddy reload and no aborted git response, and
fetches over IPv4 and IPv6, through the 307 and directly to n1–n3, all worked.
The public page tells users to retry and, if it repeats, to set
`http.version HTTP/1.1`, and says what trace to send.

## Our own tools

- Desktop: `forkmesh::gitrefusal::explainFailure` (`desktop/src/GitRefusal.cpp`)
  returns ForkMesh's `remote:` sentence (the line before the `remote: Help:
  …/git-errors/` line) or, failing that, a plain sentence for an HTTP/2 break,
  a dropped download, a DNS failure or a refused connection. Fetch and pull
  (`MainWindowRepoDetail.cpp`), mirror sync (`MainWindowRepoFetch.cpp`), import
  clone (`MainWindowRepos.cpp`) and push (`MainWindowNavigation.cpp`) show it
  instead of the raw stderr tail; the log still gets the raw text.
  `remoteUnavailable` and `isTransientSyncError` now count HTTP/2 breaks as
  transient.
- `tools/forkmesh_transports.py` `CONNECTION_FAILURE` counts HTTP/2 breaks as
  route failures, so the next route is tried.
- `www/public/install.sh` `classify_clone_failure` names HTTP/2 breaks and
  dropped downloads.
