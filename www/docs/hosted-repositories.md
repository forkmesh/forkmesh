# Empty repositories created from the web

You do not need the desktop app, and you do not need an account on another
forge, to start a repository on ForkMesh. Create an empty one in the browser
and push to it with the `git` you already have.

This is the path issue #676 asked for. The other three sources in the
**Create & mirror a repository** modal each assume something you may not have
yet: a clone URL means the repository already exists somewhere else, a local
path means a ForkMesh node is running on that machine to read it, and the
provider import means a GitHub, GitLab, or Codeberg account.

> **ForkMesh Next:** this page describes production. On
> `app.forkmesh.com` repositories created on the web live on all three
> core nodes, each reconciling its copy against the desired set in the
> database, with no single write node; the create form also offers private
> repositories and README, `.gitignore` and license initialization. The Next
> behaviour is documented in `docs/HOSTED_REPOS.md` in the repository.

## Create it

1. Open **Repositories → New**.
2. Keep the **Empty repository** source (it is the default).
3. Enter a name and, optionally, a description.
4. Press **Create empty repository**.

The repository exists immediately, holds no commits, and is listed publicly in
the catalog. The modal then shows the commands for your first push along with
the username and password `git` will ask for.

## Push to it

```bash
git init -b main
git add .
git commit -m "first commit"
git remote add forkmesh https://app.forkmesh.com/<owner>/<name>
git push -u forkmesh main
```

`git` prompts for a username and password on the first push:

- **Username** — your ForkMesh account name.
- **Password** — the push credential shown in the modal.

The remote URL deliberately carries no credential, so nothing secret lands in
`.git/config`. Use your platform credential helper
(`git config --global credential.helper`) if you would rather not paste the
password on every push.

The first push is what creates the bare repository on the hosting node; from
then on the repository is served, cloneable, and browsable like any other
ForkMesh repository. ForkMesh records the refs attestation (the signed list of branch and tag
commits) that public reads are routed against from the hosting node's own signed repository proof at the end
of each accepted push, because a repository created in a browser has no node
of its own to publish one.

## About the push credential

- It is issued to the browser session you created the repository from, and it
  is valid for seven days.
- ForkMesh does not store it. It is a keyed MAC over your account name, the
  repository, that session, and the expiry, checked on each push.
- Signing that session out (**Settings → Sessions**) revokes it at once.
- It can do nothing your account cannot: ownership and organization write
  permission are re-checked on every push.
- Need a new one after it expires? `POST
  /api/hosted-repositories/push-credential` with `{"owner", "name"}` and your
  session token, from the signed-in browser or with
  `Authorization: Bearer <session>`.

## Limits

- **Public only.** A private ForkMesh repository is an owner-sealed encrypted
  mirror, and only a node holding your account key can seal one. Creating a
  private repository still requires the desktop or mobile node; the web
  refuses rather than publishing a "private" repository the host could read.
- **Pack size.** A push is streamed through the Worker to the hosting node,
  so a large repository (well past 100k lines of source, including its
  history) can be imported in one `git push`. The pack is bounded by the
  hosting node's `receiveMaxInputBytes` (256 MiB unless its operator lowers
  it) and by Cloudflare's per-request body ceiling on the app origin (100 MB
  on the Free plan). Pushes above that go over SSH, or in several commits'
  worth of smaller packs. Only a pack the Worker can hold (8 MB,
  `HTTPS_MIRROR_BODY_MAX_BYTES`) is bound to its exact digest in the routing
  capability; a larger one is bound to the single-use capability and the
  node's TLS session instead.
- **Availability.** A relay only offers this when its operator runs a hosted
  write node (`HOSTED_GIT_WRITE_NODE`, plus `writableRepositoryRoot` and
  `writableOwners` on that node). Where it is not configured, the create modal
  answers `hosted_git_unavailable` and falls back to the desktop handoff.

## On ForkMesh Next: hosted by the core nodes

Where the relay's own core nodes host repositories (`HOSTED_GIT_CORE_NODES`,
as on `app.forkmesh.com`), **New repository** offers more:

- **Private.** Only you, and your organization's readers, can clone or read
  it, with the same credential. The core nodes hold it unencrypted, as any
  hosted forge does. An owner-sealed private mirror still needs your own
  node.
- **A first commit.** Choose a README, a `.gitignore` and a license. The
  commands then start from `git clone`.
- **An organization owner.** You need the maintain permission in that
  organization.
- **Import.** Paste a public `https://` repository URL from GitHub, GitLab or
  Codeberg under **Remote clone URL**, and ForkMesh hosts a real, cloneable
  copy of it.
- **Every core node holds a copy.** Pushes go to one core, and the others
  catch up within seconds.

Each account can host up to 10 repositories, and each one can be up to
200 MB. To grow past that, move the repository to your own node: clone it
with `--mirror`, add it to your desktop or headless node, and publish it.
Once that signed publication takes over the listing, the core nodes release
their copies. Operator details are in `docs/HOSTED_REPOS.md` of the Next
repository.

## Operator notes

The node side is the hosted write gateway described in
[direct-https-mirror-gateway.md](operations/direct-https-mirror-gateway.md)
under "Push-created repositories": an authorized first push creates
`<writableRepositoryRoot>/<owner>/<repo>.git`, records it in the
`hosted-pushes.json` sidecar, and the ordinary refresh/registration cycle then
serves and health-registers it. A node that holds website-created repositories
cannot enumerate the accounts it will host, so it sets
`"writableOwners": ["*"]`; every request that reaches a write already carried a
verified router capability from the Worker, which applies the account's real
push permissions before signing one.

The Worker side lives in `app/src/hosted_git.py`:

| Endpoint | Purpose |
| --- | --- |
| `POST /api/hosted-repositories` | Create the empty record, return the first push credential |
| `POST /api/hosted-repositories/push-credential` | Re-mint a credential for a repository the session may push |

Because a web-only account holds no signing key, the catalog record for a
hosted repository is written by the Worker rather than published with a node
signature, and is marked `source: "hosted-web"`. A later signed publication
from the owner's own node takes the record over through the normal catalog
path.
