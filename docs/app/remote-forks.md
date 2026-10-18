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

## Sending many branches at once

`/<owner>/<repo>/pulls/send` (the **Send pull requests** button beside New pull
request, shown only to the repository's owner or an owner of its
organization when at least one fork on another instance is saved) sends
several branches as separate pull requests in one action. Code:
`app/public/dashboard/js/07-repo-pull-batch.js`; tests:
`app/tests/test_pull_batch_frontend.py`.

- **Rows.** Branches whose names start with the prefix box (`pr/` when any
  branch does). Titles, descriptions, bases and dependencies come from an
  optional `.forkmesh/pull-batch.json` on the default branch, a list of
  `{branch, base, title, body, dependsOn}` (`dependsOn` is a branch name, a
  list of them, or `#N` for a pull request already open on the target). Rows
  from the file come first in file order; other matching branches follow with a
  title made from the branch name. Every field is cleaned before use: control
  and bidirectional-override characters are removed, titles are one line of at
  most 240 characters, branch names must be plain git ref names, and a
  description over 60 KB holds its row back.
- **Sending.** Each row goes through `submitRemotePullOpen`, the same path as a
  single **Send to**: compared on this instance's mirror, signed in this
  browser, posted to the target. Rows are sent one at a time, dependencies
  first.
- **Dependencies.** A row is sent only once every branch it depends on has a
  pull request number on that target (or turned out to have nothing new). Its
  description then starts with `Depends on: #N` and one sentence asking the
  reviewer to merge #N first. A row whose dependency failed, is not selected
  or is not in the list waits and says why. A `#N` dependency is target-side
  review metadata, so it is not used as a source branch when computing the
  patch.
- **Resuming.** Sent numbers are kept in `localStorage` per repository and
  target (`forkmesh.pullBatch.v1:<host>/<owner>/<repo>><target url>`), written
  after every successful send, so pressing **Send all** again, even after a
  reload, sends only what is left. **Stop** ends the run after the pull
  request in flight. An unreachable target, a full inbox, a quota, a rate
  limit or a bad signature stops the run; other failures mark that row and
  the run continues with rows that do not depend on it. The page cannot ask
  the target which pull requests are already open: the target allows
  cross-origin `POST` on its pulls route only, so its reads are not
  available to this page. Sending from a second browser therefore does not
  know what the first one sent.
- The page asks for a screen wake lock while it sends, so a phone does not
  sleep between pull requests.

### Why a stacked branch is sent against the default branch

The target has none of this repository's intermediate branches, so a stacked
branch can only be proposed against a branch the target has (normally
`main`), and its patch and commit series include the commits of the branches
below it. That merges cleanly in order. The target replays a received pull
request's `commits.mbox` with `git am --3way` onto the base branch's current
tip (`applyPatchSeries` in `server/merge.go`; the flat `changes.patch` with
`git apply --3way` only when there is no mbox). Once the parent pull request
has merged, the parent's commits in the child's series find their changes
already present: the three-way fallback reports "No changes -- Patch already
applied", `git am` skips that commit, and only the child's own commits are
added. Merging a child before its parent would bring the parent's commits in
with it, which is why the description names the parent.

Pull requests have no dependency field. The `Depends on: #N` line is the same
convention issues use (`issueDependencyNumbers`), but the pull request page
shows it as part of the description, not as a relationship list.

### Comparing with the other instance's branch, not this one's

A fork whose own `main` has moved away from the other instance's `main` (an
instance's platform repository carrying local changes) can't compare its
branches with its own `main`: the diff is the whole divergence and the mirror
times out (`mirror_unavailable`). The batch file may then be an object
`{"compareBase": "<branch>", "pulls": [...]}`, or a row may carry its own
`compareBase`. Rows are compared with that branch (a copy of the other
instance's `main`) while the signed pull request still targets `base`. A row's
own `compareBase` wins over the shared one, and either wins over a single
`dependsOn`.

### Each stacked pull request carries only its own commits

A row with exactly one `dependsOn` branch is compared with that branch, not
with its `base`: the patch and commit series hold only the row's own commits,
while the signed pull request still targets `base` and names its parent with
"Depends on: #N". Comparing every row with `main` made each stacked pull
request repeat all of its parents, so the reviewer saw the whole stack again
and the upper rows outgrew the 4 MiB submission cap (`pull_too_large`). After
the parent merges, `git am --3way` on the target applies the row cleanly. A row
with several dependencies is still compared with `base`.
