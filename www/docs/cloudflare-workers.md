# Mirror and deploy a Cloudflare Worker with ForkMesh

You already have a Cloudflare Worker and a GitHub repository for it. This guide
makes ForkMesh a signed mirror of that repository and the place your Worker is
deployed from. Each push to the default branch deploys automatically. You can
also deploy, watch the log, approve and roll back from your phone.

Your Cloudflare token stays on your own ForkMesh desktop. It never goes to
GitHub, the ForkMesh relay or a shared runner.

## How it fits together

```text
GitHub push ──► GitHub Actions bridge ──► ForkMesh (ssh.forkmesh.com)
                                              │
                                              ▼
                     your ForkMesh desktop  (label: cloudflare-deploy)
                     .forkmesh/deploy-cloudflare.yml
                                              │  wrangler deploy
                                              ▼
                                     Cloudflare Workers

ForkMesh phone app ──► desktop (LAN or relay): Run · Re-run · Approve · Roll back
```

| Piece | What it does |
| --- | --- |
| `.github/workflows/forkmesh-github-sync.yml` | Copies branches and tags between GitHub and ForkMesh on every push and every 15 minutes, fast-forward only ([Connecting to GitHub](github-sync.md)) |
| `.forkmesh/deploy-cloudflare.yml` | Runs on your desktop when the default branch moves, or when you start it by hand |
| `.forkmesh/rollback-cloudflare.yml` | Started by hand: rolls the Worker back to its previous version |
| `.forkmesh/cloudflare-deploy.py` | Checks the setup, runs Wrangler and reports the deployed URL and version |
| `.forkmesh/cloudflare-deploy.json` | Worker folder, Wrangler environment, branch and variable names |

## Before you start

On the computer that will deploy (your ForkMesh desktop):

- the ForkMesh desktop, signed in to your account. Desktop Actions run on
  **Linux** only. They need `bwrap` (bubblewrap), a systemd user session, and
  a desktop that is not running as root.
- **Node.js 22 or newer** and **npm**, installed system-wide; current Wrangler
  needs Node.js 22. Many distributions still ship an older `nodejs`, so
  check `node --version`; [NodeSource](https://github.com/nodesource/distributions)
  packages install into `/usr`. Runs see `/usr` but not your home folder, so a
  Node.js installed with `nvm` is not found.
- `python3` and `git`.

You also need:

- a **Cloudflare API token**. In the Cloudflare dashboard, open *My Profile →
  API Tokens → Create Token* and use the **Edit Cloudflare Workers** template.
  Limit it to the account and zones the Worker uses.
- your **Cloudflare account ID**: 32 hexadecimal characters, shown in the
  dashboard's Workers overview.
- admin rights on the GitHub repository, and the `gh` command-line tool
  (optional, but it makes setting secrets easier).
- a ForkMesh repository with the same name. Create an empty one under
  **Repositories → New** on [app.forkmesh.com](https://app.forkmesh.com).
- the ForkMesh tools:

```bash
git clone --depth 1 https://forkmesh.com/forkmesh/forkmesh ~/forkmesh-tools
```

## Step 1: connect the repository

Run `connect` in a checkout of your Worker's GitHub repository. It writes the
deploy files above and the GitHub sync files, and adds `github` and `forkmesh`
remotes:

```bash
git clone https://github.com/OWNER/NAME && cd NAME
python3 ~/forkmesh-tools/tools/cloudflare_worker_deploy.py connect \
  --github OWNER/NAME \
  --bridge-key-out ~/forkmesh-bridge.pem
```

Add these options as needed:

| Option | Use it when |
| --- | --- |
| `--worker-dir worker` | `wrangler.toml` / `wrangler.json(c)` is not at the repository root |
| `--env production` | you deploy a Wrangler environment (`[env.production]`) |
| `--branch release` | you deploy from a branch other than `main` |
| `--manual-only` | you only want deploys you start yourself, never on push |
| `--needs .forkmesh/ci.yml` | a ForkMesh test workflow must pass first |
| `--forkmesh OWNER/NAME` | the ForkMesh repository has a different owner or name |
| `--runs-on LABEL` | you want a desktop label other than `cloudflare-deploy` |
| `--token-variable NAME`, `--account-variable NAME` | different Workers deploy to different Cloudflare accounts |
| `--replace-vars` | Wrangler may delete dashboard variables that are missing from your config (by default they are kept, with `--keep-vars`) |
| `--no-rollback` | you do not want the roll-back workflow |

`connect` prints a JSON summary. Its `nextSteps` list is a checklist of the
remaining steps, and `existingGitHubDeploys` lists GitHub workflows that
already deploy this Worker. Running `connect` again is safe: it only rewrites
files whose content changed.

## Step 2: let GitHub push to ForkMesh

The bridge workflow pushes to ForkMesh over SSH. Give it its own key:

```bash
ssh-keygen -t ed25519 -N '' -C forkmesh-sync -f ~/forkmesh-sync
```

1. Add `~/forkmesh-sync.pub` to your ForkMesh account under
   **Settings → SSH keys** (`/dashboard/settings/ssh-keys`). The key acts with
   your account's write access.
2. Store the secrets in GitHub:

```bash
gh secret set FORKMESH_BRIDGE_SIGNING_KEY --repo OWNER/NAME < ~/forkmesh-bridge.pem
gh secret set FORKMESH_SYNC_SSH_KEY --repo OWNER/NAME < ~/forkmesh-sync
gh variable set FORKMESH_SYNC_REMOTE --repo OWNER/NAME \
  --body ssh://git@ssh.forkmesh.com/OWNER/NAME.git
```

To pin the server's identity, also set the `FORKMESH_SYNC_KNOWN_HOSTS`
variable to this line:

```text
@cert-authority ssh.forkmesh.com,*.forkmesh.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAII5OvM7x8w4o1auHqYiSObIWHaVdAy4tT8dxLOai76Vs
```

## Step 3: prepare your desktop

1. **Add the repository.** In the desktop, open **Repositories** and clone
   `OWNER/NAME` from ForkMesh. The desktop now holds a mirror of the
   repository and receives each commit the bridge pushes.
2. **Label the desktop.** Under **Settings → Profile → Node labels**, add
   `cloudflare-deploy`. Only a node with this label runs the deploy. Mirrors
   and shared runners hold no secrets and skip it.
3. **Add the secrets.** Under **Settings → Secrets**, add:

| Name | Value |
| --- | --- |
| `CLOUDFLARE_API_TOKEN` | the API token from *Before you start* |
| `CLOUDFLARE_ACCOUNT_ID` | your 32-character account ID |
| `FORKMESH_ACTIONS_ALLOW_NETWORK` | `1` |

   A run receives only the variables its workflow names, and their values are
   masked in logs. `FORKMESH_ACTIONS_ALLOW_NETWORK` gives every Actions run on
   this desktop network access, because Wrangler needs to reach Cloudflare.
4. **Turn on Actions for the repository.** In the repository's **Actions** tab,
   tick **Run actions on push**. Then read [Keep the token safe](#keep-the-token-safe)
   before you decide on **Automatically approve runs**.

## Step 4: turn off the old deploy

If GitHub or Cloudflare also deploys this Worker, each push deploys twice and
the last deploy wins. Before your first ForkMesh deploy:

- disable or delete the GitHub workflows listed in `existingGitHubDeploys`
  (`cloudflare/wrangler-action` or `wrangler deploy` steps);
- if the Worker uses **Workers Builds** (Cloudflare's own Git integration),
  disconnect the repository under *Worker → Settings → Build* in the Cloudflare
  dashboard.

Keep your GitHub test workflows: they still run on GitHub.

## Step 5: commit and deploy

```bash
git add .forkmesh .github/workflows/forkmesh-github-sync.yml
git commit -m "Mirror to ForkMesh and deploy the Worker from it"
git push origin HEAD
```

The bridge copies the commit to ForkMesh within a minute or two, and your
desktop starts **Deploy Cloudflare Worker**. If you turned off
**Automatically approve runs**, the run waits until you approve it in the
desktop's Actions tab or from your phone. A successful run ends with a line
like:

```text
::cloudflare-deploy:: ok worker=shop version=1b2c3d4e-… url=https://shop.example.workers.dev
```

The first step, `check --online`, stops the run before anything is deployed if
the token, account ID, Node.js or network is wrong. See
[Troubleshooting](#troubleshooting).

## Deploy from your phone

Install the ForkMesh app for iPhone or Android and sign in to the same account
as your desktop. The app finds your desktop by itself, with no pairing code:
on the same Wi-Fi it connects directly, and away from home it goes through the
end-to-end encrypted relay. Open the app once on the same network as your
desktop; if the connection away from home is refused, open it on that network
again. See [Mobile access](mobile/mobile-agent-access.md).

Open the repository, then **Actions**:

| You want to | In the app |
| --- | --- |
| Deploy now | **Deploy Cloudflare Worker → Run**, branch `main` |
| Undo a bad deploy | **Roll back Cloudflare Worker → Run** |
| Let a held run start | open the run, **Approve** (or **Reject**) |
| Retry a failed deploy | open the run, **Re-run** |
| Stop a deploy | open the run, **Stop** |
| See what happened | open the run: steps and live log, ending with the URL and version |

Things to know:

- Your desktop must be switched on, signed in and online. A re-run you ask
  for while it is offline is queued and starts when the desktop reconnects.
- You can also ship from GitHub's mobile app: merge a pull request into the
  default branch, and the bridge and your desktop deploy it.
- The website on a phone lists the workflows and the runs that mirrors report.
  It cannot start or approve a run on your desktop; use the app for that.
- This guide is in the app too, under **Docs**.

## Keep the token safe

Anyone who can change `.forkmesh/` workflows on the default branch can make a
run read `CLOUDFLARE_API_TOKEN`. With the bridge, that includes everyone with
write access on GitHub. The desktop approves a workflow's exact content, so:

- untick **Automatically approve runs** for this repository if other people can
  push to it. Every changed workflow then waits until you approve it, which you
  can do from your phone;
- give the token only the **Edit Cloudflare Workers** permissions, for the one
  account and zone it needs;
- revoke and replace the token in Cloudflare if a run you did not expect ever
  reads it, then update **Settings → Secrets**.

Pull requests never run this workflow: it has no `pull_request` trigger, and the
bridge fetches pull request code only as data and never runs it.

## Several Workers or environments

- **Staging and production:** connect with `--env production` and keep the
  default deploy for `main`. To deploy staging from another branch, use a
  second checkout or repository; one repository holds one deploy
  configuration.
- **Monorepo:** use `--worker-dir` for the Worker you deploy from this
  repository.
- **Two Cloudflare accounts:** save the tokens under different names
  (`SHOP_CF_TOKEN`, `SHOP_CF_ACCOUNT`) and pass `--token-variable` and
  `--account-variable`.

## Moving off GitHub

To drop GitHub completely, import the repository (**Repositories → New →
GitHub**, see [Repository imports](repository-imports.md)) or push your
history to an empty ForkMesh repository. Then run `connect` without
`--github`. Pushes to ForkMesh's default branch deploy the same way.

## Troubleshooting

| The log says | Fix |
| --- | --- |
| `bubblewrap (bwrap) is required` / `no configured ForkMesh Actions sandbox` | Run the deploy on a Linux desktop with `bubblewrap` installed |
| `Node.js is not installed on this node` / `Node.js 20 is too old` | Install system-wide Node.js 22+ (for example from NodeSource); a Node.js from `nvm` is not visible to runs |
| `… GiB of virtual address space` | Update the ForkMesh desktop; Wrangler reserves a large sparse address range |
| `Cloudflare's API is unreachable` | Add `FORKMESH_ACTIONS_ALLOW_NETWORK` = `1` under **Settings → Secrets** |
| `CLOUDFLARE_API_TOKEN is empty` / `not a 32-character account ID` | Add or correct the secret under **Settings → Secrets** (names are case-sensitive) |
| `Cloudflare rejected the API token` | Create a new token from the **Edit Cloudflare Workers** template |
| `pins account_id … different account` | Your `wrangler.toml` names another account: fix `account_id` or the secret |
| `no wrangler.jsonc, wrangler.json, wrangler.toml` | Run `connect` again with `--worker-dir` |
| `skipped refs/heads/… is not refs/heads/main` | Expected: only the configured branch deploys |
| The run never starts | Check the `cloudflare-deploy` node label, **Run actions on push**, and whether the run is waiting for approval |
| GitHub commits never reach ForkMesh | Open the **ForkMesh GitHub sync** run on GitHub; check `FORKMESH_SYNC_REMOTE`, the SSH key, and that the ForkMesh repository exists |
| Two deploys per push | Turn off the GitHub or Workers Builds deploy (Step 4) |

## Command reference

`cloudflare_worker_deploy.py` is also copied into your repository as
`.forkmesh/cloudflare-deploy.py`:

```bash
python3 .forkmesh/cloudflare-deploy.py check            # config, Wrangler file, Node.js
python3 .forkmesh/cloudflare-deploy.py check --online   # plus token, account and Worker
python3 .forkmesh/cloudflare-deploy.py deploy --dry-run # build without uploading
python3 .forkmesh/cloudflare-deploy.py deploy
python3 .forkmesh/cloudflare-deploy.py rollback [--version VERSION_ID]
python3 .forkmesh/cloudflare-deploy.py render-workflow
```

`deploy` and `rollback` read `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID`
from the environment, so you can also run them by hand from a terminal. Exit
codes: `0` success or skipped, `1` the deploy failed, `2` setup error, `75`
Cloudflare unreachable. Wrangler output is masked so the token never appears.
