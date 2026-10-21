# Empty repositories created from the web

Create an empty repository from **Repositories → New** and push with plain
`git` to `https://app.forkmesh.com/<owner>/<name>`. When git asks, enter your
account name or email and your account password, or the push credential shown.
The credential is a
keyed MAC over account,
repository, session and a seven-day expiry; it is not stored, dies with the
session, and every push re-checks permissions. After each push ForkMesh records
the attestation (the signed list of branch and tag commits) that reads route
against.

Packs are bounded by the node's `receiveMaxInputBytes`. Where core nodes host
repositories (`HOSTED_GIT_CORE_NODES`) the form also offers private
repositories, initial files and imports, up to 10 repositories of 200 MB per
account.

## Contributing without write access

The usual way: open the repository, choose **Forks → Fork this repository**,
create a push credential for your fork (Settings → SSH keys → Push code over
HTTPS, `<you>/<name>`), push any branch to it, and choose **Contribute: Open
pull request** on your fork. A fork shares the original's history, so it is
created at once whatever the repository's size.

On a repository the operator opened to contributors, anyone signed in with a
verified email address can also push a branch and open a pull request without
a fork:

1. **Settings → SSH keys → Push code over HTTPS**: enter `<owner>/<name>` and
   choose **Create push credential**.
2. Push a branch named `pr/<your-account>/<topic>`:
   `git push https://<host>/<owner>/<name>.git pr/<your-account>/<topic>`.
   When Git asks, use your account name or email with your account password,
   or the credential.
3. Open the pull request from that branch on the repository's **Pull
   requests** tab.

Every other ref is refused (Git prints `[remote rejected]` with the reason), pushes are fast-forward only (add commits instead
of force-pushing), and a contributor keeps at most 32 branches there.
Contributors cannot delete branches, even their own: when the limit is
reached, a maintainer deletes merged ones. Only new objects are sent, so a
large repository costs no more than the change.

## Operator notes

Requires `HOSTED_GIT_WRITE_NODE` and a node with `writableRepositoryRoot` and
`"writableOwners": ["*"]`; otherwise the API answers `hosted_git_unavailable`.
See [direct-https-mirror-gateway.md](operations/direct-https-mirror-gateway.md).
Endpoints are `POST /api/hosted-repositories` and
`POST /api/hosted-repositories/push-credential`; records are marked
`source: "hosted-web"` until a signed publication from the owner's node takes
over.

Contributor pushes are off unless `HOSTED_GIT_CONTRIBUTOR_PUSH` lists
repositories: `owner/name`, `owner/*` or `*`, separated by spaces or commas
(an organization alias or its backing node both match). Only public
repositories qualify, and only active user accounts with a verified email.
The routing Worker signs `pushPrefix=refs/heads/pr/<account>/` into the
receive-pack capability; the gateway refuses refs outside it, so both must be
updated together (an older gateway refuses the field, failing closed).
Contributor pushes never queue Actions runs.

There is no per-account byte quota or push rate limit on this path: each push
may be up to the gateway's `receive.maxInputSize`, and fast-forwards are
unlimited. Listing `*` lets any verified account store data in every public
repository, so prefer naming repositories or owners.
