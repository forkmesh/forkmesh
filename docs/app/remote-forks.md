# Forks on other instances and cross-instance pull requests

Part of [the App docs](README.md). A repository can list copies of itself on
other ForkMesh instances ([../INSTANCES.md](../INSTANCES.md)) and send a
branch to one of them as a pull request: the same signed, self-contained patch
a fork sends its parent. Code: `app/src/remote_forks.py`, table
`repo_remote_forks`, `_open_pull_cors_headers` in `app/src/entry.py`. Tests:
`app/tests/test_remote_forks.py`.

## Saved list

`GET|POST|DELETE /api/repo/<owner>/<repo>/remote-forks`. GET is public for
public repositories; POST `{url, label?}` and DELETE `{url}` need the
repository owner or an owner of the linked organization. `parse_url` accepts
only `https://host[:port]/<owner>/<repo>[.git]` and refuses credentials, IP
literals, dotless hosts and this instance (`same_instance`). At most 20 per
repository (`MAX_FORKS`).

## Sending

The new pull request form offers **Send to** a saved fork. The browser computes
the patch here, signs it with the target's owner and repository name (checked
by the target's `verify_pull_event`), sets `head` to
`<this host>/<owner>/<repo>:<branch>`, and posts directly to the target's
`/api/repo/<owner>/<repo>/pulls` with `credentials: "omit"`.

The target allows cross-origin requests only on that route:
`Access-Control-Allow-Origin: *`, no credentials, `POST` only. This is safe
because pull submissions are authorized by their Ed25519 signature, not
sessions. Unreachable targets report `remote_unreachable`. Review happens on
the target instance; both copies must share history.
