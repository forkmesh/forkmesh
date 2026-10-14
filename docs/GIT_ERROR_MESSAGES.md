# Git error messages

What someone running stock `git` over HTTPS, or the desktop app, sees when a
clone, pull or push fails. The public page is `www/docs/git-errors.md`
(`https://forkmesh.com/docs/git-errors/`).

## Why plain text

Git shows a server's explanation only for a failed `info/refs` GET and only
when the body is `text/plain`, printing each line as `remote: …`. A failed
`git-upload-pack` or `git-receive-pack` POST shows nothing from the body.

## Server side

- App: `_git_error_as_text` (`app/src/entry.py`) wraps the `info/refs` routes
  (upload-pack public and private, receive-pack). A JSON error of status 400 or
  more becomes one sentence plus `Help: https://forkmesh.com/docs/git-errors/`.
  The status and `cache-control`, `retry-after`, `www-authenticate`,
  `x-forkmesh-expected-degraded` and `x-forkmesh-served-by` are kept
  (`WWW-Authenticate` must survive or git never prompts for credentials). A
  `message` or `hint` in the JSON wins over the table in `GIT_ERROR_MESSAGES`.
  Other routes and POSTs still answer JSON.
- Node gateway: `dispatch_error_response` (`tools/mirror_gateway.py`) does the
  same for the public `/<owner>/<repo>.git/info/refs` route. Restricted and
  missing repositories get the same 404 text.

## What the server cannot explain

Transport failures (HTTP/2 framing errors, `curl 18`, `early EOF`, DNS,
connection refused) carry no body. The public page tells users to retry, then
to set `http.version HTTP/1.1`, and what trace to send.

## Our own tools

- Desktop: `forkmesh::gitrefusal::explainFailure` (`desktop/src/GitRefusal.cpp`)
  shows the server's `remote:` sentence or a plain sentence for transport
  failures instead of raw stderr (the log keeps the raw text); HTTP/2 breaks
  count as transient.
- `tools/forkmesh_transports.py` treats HTTP/2 breaks as route failures and
  tries the next route.
- `www/public/install.sh` names HTTP/2 breaks and dropped downloads.
