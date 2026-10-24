# ForkMesh guide for coding agents

You are a coding agent (Claude Code or similar) and a person has handed you
this file. It explains how to start a ForkMesh instance for them and how to
work on ForkMesh repositories the way the ForkMesh team does. Read it all
before acting. Latest copy: https://app.forkmesh.com/forkmesh-agent.md

ForkMesh is an open-source Git hosting platform. An instance is a
white-label copy that a person runs on their own server: repositories, pull
requests, issues, chat, Actions and agents under their own name and domain.

## Ground rules

- Ask the person before anything that is hard to undo or that sends their
  data somewhere: running the installer, sending SSH credentials to a
  website, pushing, opening a pull request, deleting files.
- Never print, log or commit secrets: SSH keys and passwords, the admin
  password, mail API keys, the ForkMesh identity key (`ed25519.pem`).
- Never delete ForkMesh data folders (`~/projects/<project>/data`,
  `~/.config/ForkMesh`, `~/.local/share/ForkMesh`, `~/.forkmesh`). They hold
  the node identity key; without it the account cannot be recovered.
- If a fetch, pull or clone fails, stop and report. Do not delete and
  re-clone a checkout.

## 1. Start an instance

### What the person needs

- A fresh server running Debian 13 or Ubuntu 24.04, x86_64 or arm64, with at
  least 1 GB RAM, 6 GB free disk, ports 80 and 443 free, and SSH access as
  root or as a user with passwordless `sudo`.
- Optional: a domain with an `A` record pointing at the server. Without one,
  the instance gets a free `<ip-with-dashes>.sslip.io` address.
- Optional: a mail provider key (Mailtrap, SendGrid or Mailgun) for sign-up
  and password emails.
- About fifteen minutes.

### Path A: the start page (recommended)

1. Ask the person for the settings below. Do not guess the server address,
   domain or admin email.
2. Write a settings file `forkmesh-instance.json`. Every key is optional;
   the full list is at https://forkmesh.com/docs/start-config/ and an
   example is at https://app.forkmesh.com/start-config.example.json.

   ```json
   {
     "projectName": "Acme Code",
     "host": "203.0.113.10",
     "sshUser": "root",
     "port": 22,
     "domain": "code.acme.example",
     "adminUsername": "acme-admin",
     "adminEmail": "admin@acme.example",
     "ownerType": "org",
     "ownerName": "acme",
     "mailProvider": "none",
     "ownWorld": true,
     "listOnHomepage": true
   }
   ```

   Leave the secrets out: `sshPassword`, `sshPrivateKey`, `sshPassphrase`,
   `adminPassword` and `mailApiKey`. The person types those into the form
   themselves, so you never see them. Leave `adminPath` out too: the start
   page makes a private, random admin console address. If the person
   wants a logo, zip the JSON with one PNG, JPEG, WebP or SVG image (up to
   200 KB) and set `"logo"` to its file name.
3. Tell the person to open https://forkmesh.com/start, choose
   **Upload settings zip**, pick the file, type the SSH password or key and
   the admin password (at least 10 characters) into the form, check it and
   press **Start my instance**. After the upload the page points at each
   field still empty. The page reads the file in the browser and shows
   every step live.
4. When it finishes, the person must sign in as the admin **within five
   minutes**. An instance started from the page that nobody signs in to
   removes itself, so an abandoned launch does not leave a server running.
5. Remind them to save the admin console address the page shows next to
   their password. ForkMesh does not keep it.
6. The settings file holds no secrets, but it does hold the server
   address; offer to delete it afterwards.

### Path B: run the installer yourself over SSH

Use this when the person wants no secrets to pass through forkmesh.com, or
the server is not reachable from the internet over SSH.

1. Write the same settings file, without `host`, `sshUser`, `port` or the
   SSH secrets. The installer is stricter than the start page; it needs:
   - `domain`: there is no automatic `sslip.io` fallback here. Without a
     domain of their own, use `<ip-with-dashes>.sslip.io`, for example
     `203-0-113-10.sslip.io`.
   - `publicAddress`: the server's public IPv4 or IPv6 address.
   - `adminPassword`, at least 10 characters: ask the person to add it
     themselves, or to let you generate one they save at once.
   Add your own `adminPath` (`/` then 8 to 64 letters, digits,
   `-` or `_`, hard to guess).
2. With the person's go-ahead, copy it to the server and run:

   ```sh
   curl -fsSL https://forkmesh.com/install-instance.sh -o install-instance.sh
   sudo bash install-instance.sh --config forkmesh-instance.json
   ```

   The installer runs as a detached systemd service, so a dropped SSH
   connection does not stop it; each step resumes on a re-run. It prints
   `FMI {"step": ..., "state": ...}` lines as it goes: `system`, `packages`,
   `source`, `build`, `configure`, `database`, `services`, `https`, `admin`,
   `repo`, `done`.
3. A manual install never removes itself, and it does not appear in the
   forkmesh.com directory until the admin turns that on in the admin
   console (Platform → ForkMesh directory).
4. The file now holds the admin password. Once the install reports `done`,
   ask before removing it from the server and from your machine.

### After it is up

- Open the instance URL and sign in as the admin.
- The instance hosts its own copy of the platform source at
  `<owner>/forkmesh`. Updates: admin console → Platform → Update ForkMesh, or
  `sudo bash install-instance.sh --update` on the server. Data, secrets and
  the admin account are kept.
- If DNS is not ready yet, the `https` step waits. A Cloudflare-proxied
  domain needs SSL/TLS mode Full (strict).

## 2. Work on a ForkMesh repository

- Clone with stock Git: `git clone https://<instance>/<owner>/<repo>`.
  forkmesh.com's own source is `https://forkmesh.com/forkmesh/forkmesh`.
- A repository created on the web takes `git push` over HTTPS with the
  account name and the push credential shown when it was made. Pushes are
  fast-forward only.
- Branch from an up-to-date `main` with a prefix (`fix/`, `feat/`, `docs/`,
  `test/`). Never work on `main`.
- Read the repository's `AGENTS.md`, `CLAUDE.md` and `CONTRIBUTING.md` first;
  they win over this file.
- Make small commits, one scope each. Stage explicit paths, never
  `git add -A`, so you do not sweep in the person's own unfinished work.
- Run the tests for what you changed and report real results. A skipped
  suite is not a pass.

## 3. Open a pull request

A ForkMesh pull request is a signed record, not a pointer to a branch on a
server. It carries the patch and the commits, so a branch that was never
pushed can still become a pull request. Three ways to make one, all
producing the same record:

1. **Desktop app**: Pulls → New pull request, or Code → Branches →
   Create PR.
2. **MCP**: the ForkMesh MCP server (`tools/forkmesh_mcp_server.py` in the
   ForkMesh source, already listed in its `.mcp.json`) has
   `open_pr_from_branch`. Write tools need a token from Desktop → Settings →
   MCP.
3. **HTTP API**: `POST https://<instance>/api/repo/<owner>/<repo>/pulls`
   with `{"pull": {title, base, head, patch, commits, description, author,
   ts, sig}}`:
   - `patch` is `git diff origin/main..<branch>`; `commits` is
     `git format-patch --stdout origin/main..<branch>`.
   - Add `aiModel` (for example `"Claude Opus 5.5"`) or an `AI model:` line
     in the description. A repository that requires AI disclosure refuses
     the pull request without one (`422 ai_disclosure_required`).
   - `author` is the node's Ed25519 public key, base64url without padding;
     `ts` is Unix seconds. Leave `authorName` out: the server sets it.
   - `sig` is the node key's Ed25519 signature over
     `"forkmesh-pull-event-v2\n" + "<owner>/<repo>" (lowercase) + "\n" +
     author + "\n" + ts + "\n" + sha256_hex(title + "\0" + base + "\0" +
     head + "\0" + patch + "\0" + commits)`. The reference implementation
     is `verify_pull_event` in `app/src/events.py`.
   - Load the key only inside the signing script. Never print it.
   - Send a descriptive `User-Agent` such as `ForkMeshMCP/0.1`; an
     operator's edge rules may refuse generic scripting agents.
   - A `201 {"ok": true, "number": N}` means the server verified and queued
     it. The number is provisional until the owner's node takes it in.

How the ForkMesh team keeps pull requests easy to accept:

- **Small.** Keep each patch at or under about 0.5 MB. Split generated files
  (built bundles, rendered pages) into their own pull requests.
- **Independent.** Build each one on a fresh branch from the current
  `origin/main` (re-fetch first) and check it with `git apply --check`, so
  any one can be accepted without the others. When pieces really depend on
  each other, open an ordered series and name the dependency in each
  description.
- **Never broken in between.** Every merge to `main` deploys. Keep a module,
  the route that reaches it and the hooks that call it in one pull request.
- **Explained.** Say what a user will notice, how to check it, and which
  tests you ran with their results.
- **Confirmed.** Before telling the person a pull request is live, check the
  server: `git ls-remote origin 'refs/heads/forkmesh/pull/*'` lists the ones
  the owner's node has taken in (some repositories keep them under
  `refs/pr/<n>/metadata` instead). A local push or a local MCP read is not
  proof.

## 4. Keep this file to yourself (optional)

You can save this file next to the person's code so every session reads it,
without committing it:

```sh
curl -fsSL https://app.forkmesh.com/forkmesh-agent.md -o FORKMESH.md
echo FORKMESH.md >> .git/info/exclude
```

`.git/info/exclude` works like `.gitignore` but stays in that one clone and
is never committed. Use `.gitignore` instead if the whole team should ignore
it. Then point the agent at it, for example by adding `@FORKMESH.md` to a
`CLAUDE.md` that is also ignored, or by saying "read FORKMESH.md" at the
start of a session.
