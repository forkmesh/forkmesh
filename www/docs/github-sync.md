# Connecting a repository to GitHub

A ForkMesh repository can be connected to a GitHub repository so both stay in
sync:

- **Code**: branches and tags move both ways. A ref that exists on only one
  side is created on the other, and the side that is behind is
  fast-forwarded. Nothing is ever force-pushed or deleted. A branch that has
  moved on both sides is reported as *diverged* and left alone until someone
  merges one side into the other.
- **Collaboration records**: GitHub issues, discussions, and pull requests
  are converted into the repository-native `.forkmesh` records every ForkMesh
  client reads. They show up in the desktop app, on the website, and on
  mirrors exactly like records created on ForkMesh.

Everything runs as a GitHub Actions workflow in the GitHub repository. ForkMesh
Actions run without network egress, so GitHub cannot be reached from the
ForkMesh side. ForkMesh-side changes are picked up by the workflow's
15-minute schedule, and GitHub-side events trigger it immediately.

## Connect

From a checkout of the repository (either remote):

```bash
python3 tools/github_forkmesh_bridge.py connect \
  --github octo/app \
  --forkmesh octo/app \
  --key-out ~/.config/forkmesh/github-bridge-octo-app.pem
```

`connect` writes three files and adds `github` / `forkmesh` git remotes if
they are missing:

| File | Purpose |
| --- | --- |
| `.forkmesh/github-sync.json` | The connection (`github`, `forkmesh`, `forkmeshRemote`) and the GitHub → ForkMesh number mapping the workflow maintains |
| `.forkmesh/github-bridge.py` | A copy of the bridge. It is self-contained, so the connected repository does not depend on this one |
| `.github/workflows/forkmesh-github-sync.yml` | The workflow |

It also generates the bridge signing key (mode `0600`, never inside the
repository) and prints the remaining steps:

1. `gh secret set FORKMESH_BRIDGE_SIGNING_KEY < <key file>`: the Ed25519 key
   that signs imported records.
2. `gh secret set FORKMESH_SYNC_SSH_KEY < <ssh private key>`: an SSH key
   whose public half is registered on ForkMesh with write access to the
   repository.
3. `gh variable set FORKMESH_SYNC_REMOTE --body ssh://git@ssh.forkmesh.com/octo/app.git`.
   The workflow does nothing until this variable is set.
4. Optionally, set `FORKMESH_SYNC_KNOWN_HOSTS` to the gateway's
   `known_hosts` line. Without it, the first connection trusts the host key
   it is offered (`StrictHostKeyChecking=accept-new`).
5. Commit the three files and push them to both remotes.

Any branch protection on GitHub's default branch and on `forkmesh/pulls` must
let the workflow's `GITHUB_TOKEN` push, because imported records are
committed there.

## How records are converted

| GitHub | ForkMesh record |
| --- | --- |
| Issue, labels, milestone, assignees, comments, open/closed | `.forkmesh/issues/{open,closed}/<n>/issue-<n>.json` (`forkmesh-issue-v1`), plus new entries in `labels.json` / `milestones.json` |
| Discussion, comments and replies (flattened in time order), closed | `.forkmesh/discussions/<n>/discussion.md` + `NNNN-comment.md` |
| Pull request (open / closed / merged), conversation comments, reviews, inline review comments | `.forkmesh/pulls/<n>/` on the `forkmesh/pulls` ledger branch: `pull.md`, `changes.patch` and `commits.mbox` for open pulls, and `NNNN-comment.md` / `NNNN-review.md` / `NNNN-line-comment.md`. (Native clients also write `NNNN-thread-comment.md`, `NNNN-thread-reply.md`, `NNNN-thread-state.md` and `NNNN-suggestion-state.md`; inline events may carry a signed patchset anchor, see [Review threads across patchsets](design/pull-review-threads.md).) |

- **Signatures.** ForkMesh rejects unsigned events, so every imported event
  is signed by the bridge key with the same canonical strings the desktop
  uses. The signing public key is the record's `author`. The GitHub account
  is kept as display credit in `authorName` with a `github:` prefix (for
  example `github:alice`). That prefix can never match a ForkMesh account
  name, so an imported comment cannot pass as a native user's.
- **Numbers.** GitHub numbers are kept, so `#12` references still resolve.
  If a native ForkMesh record already owns a number, the GitHub item gets the
  next free number and the job summary reports the remapping. The mapping is
  stored in `.forkmesh/github-sync.json`.
- **Idempotence.** Event ids are derived from GitHub ids (`github-…`),
  timestamps come from GitHub, and Ed25519 signatures are deterministic.
  Re-running on an unchanged item therefore rewrites nothing. Issues and pull
  requests whose `updated_at` has not moved are skipped without fetching
  their comments.
- **Edits on ForkMesh are kept.** The bridge only regenerates the `github-*`
  events it owns. A comment, priority, or status change made on ForkMesh on an
  imported item stays, merged in timestamp order. GitHub is authoritative for
  an imported item's open/closed state.
- **Pull request payloads.** For an open pull, the head is fetched from
  `refs/pull/<n>/head` as data only and written as
  `git diff --binary base...head` and `git format-patch --binary --stdout base..head`,
  matching what the desktop derives. Pull request code is never checked out
  or executed, which is what makes `pull_request_target` safe here. A payload
  over 8 MiB is skipped with a warning.

## Limits

- This bridge imports collaboration records GitHub → ForkMesh only. For issues
  and comments in both directions, use [Two-way forge sync](forge-sync.md).
  Pull requests created on ForkMesh stay on ForkMesh, but their commits reach
  GitHub through the code sync.
- Branch deletions are not propagated. A branch deleted on one side is
  recreated from the other on the next run.
- Attachments and images in GitHub bodies stay links to GitHub.
- Review events from fork pull requests run without secrets, so they are
  imported by the next scheduled run instead.
- A repository whose ForkMesh owner node republishes history that GitHub does
  not contain appears as *diverged*. The bridge never picks a winner.

## Running pieces by hand

```bash
# Two-way ref sync only
python3 tools/github_forkmesh_bridge.py git-sync --forkmesh-remote ssh://git@ssh.forkmesh.com/octo/app.git

# Convert records into a checkout without committing (pulls into a ledger worktree)
GITHUB_TOKEN=… FORKMESH_BRIDGE_SIGNING_KEY="$(cat key.pem)" \
  python3 tools/github_forkmesh_bridge.py export --github octo/app --pulls-root ../pulls-ledger

# Everything the workflow does, without pushing
python3 tools/github_forkmesh_bridge.py run --github octo/app --dry-run
```

The contracts are covered by
[`app/tests/test_github_forkmesh_bridge.py`](../../app/tests/test_github_forkmesh_bridge.py).
