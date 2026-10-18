# Runners: trusted core and untrusted agent compute

The Go daemon (`server/`) has three roles:

| Role | Serves repositories | Gateway, catalog, merge executor | Agent jobs | Actions |
|---|---|---|---|---|
| `mirror` (default) | yes | yes | yes, unless `disableAgentJobs` | if `enableActions` |
| `core` | yes | yes | never | never |
| `runner` | no | none | yes | if `enableActions` (leased from the app) |

A runner starts no gateway, publishes no catalog, keeps no repositories and
talks to the app only through `/api/runners/*`. It has no node account and
opens no node event channel.

## Job flow

```
 website ── queue ──► app (org_agent_jobs)
                        │  POST /api/runners/claim  (runner-signed, long poll)
                        ▼
                     runner ──clone (read grant)──► core gateway /v1/runner/<owner>/<repo>.git
                        │  claude/codex in a per-job systemd scope
                        ├──push HEAD:refs/heads/agent/<session> (push grant)──► core gateway
                        └─ POST /api/runners/report (runner-signed) ──► app
```

1. **Claim.** `{max, providers, allowedOwners, burst, waitMs}`; the app holds
   the request up to 20 s when nothing is leasable.
2. **Lease.** Each job carries `cloneUrl`, `readGrant`, `pushGrant`, `pushRef`
   (under `refs/heads/agent/`), expiry (at most 3 h), attachment metadata, and
   for pool runners a `modelGrant` used to sign a commit attestation trailer.
3. **Check.** The runner rejects leases whose grants disagree with the job or
   expire within 5 minutes.
4. **Clone** into a fresh 0700 workdir from a per-repository cache. The grant
   travels as `X-ForkMesh-Runner-Grant` via `GIT_CONFIG_*` environment
   variables, never on a command line or in the clone's config.
5. **Run** the CLI inside the job sandbox.
6. **Publish.** Git inside the job repository runs only as the job user, with
   hooks and fsmonitor disabled, and produces a bundle of new commits. The
   daemon reads the bundle with `O_NOFOLLOW`, imports it into its own bare
   repository and pushes only the lease ref, with a minimal git environment
   (`GIT_CONFIG_NOSYSTEM`, no global config, hooks off, only HTTPS).
7. **Report**, then remove the workdir and publish repository (only paths
   strictly inside `workRoot`).

Code: `server/runner.go`, `sandbox.go`, `runnergrant.go`, `jobhome.go`,
`runnerauth.go`, `runnerlogin.go`, `runneraccounts.go`, `agentstream.go`,
`runnerpool.go`; app `app/src/runner_leases.py` (migration
`0193_runner_nodes`), `runner_logins.py` (`0195_runner_login_sessions`),
`runner_setup.py`, `runner_ssh_setup.py`, `runner_auth.py`; dashboard
`/dashboard/runners`; gateway route in `tools/mirror_gateway.py`.

An instance installs one runner next to its core with
`install-runner.sh --preregistered --name <name> --owner <owner> --binary
<daemon>`: no token handshake (the instance registers it with
`python -m forkmesh_host instance-runner`), `enableActions: true`, job user
`forkmesh-job` unless `--job-user` says otherwise, and the last output line
`FM_RUNNER_PUBLIC_KEY=<base64url raw Ed25519 key>`
([server/instance-actions.md](server/instance-actions.md)).

## Security model

A runner can read the one leased repository for the lease lifetime,
fast-forward exactly its lease ref, report on its own leases, and run
arbitrary code inside its sandbox. It cannot:

- **Write any other ref.** The gateway parses the receive-pack commands before
  git runs and refuses the whole push for another ref, more than one command,
  a delete, a push certificate or malformed input; non-fast-forward pushes are
  denied. An instance's own Actions runner may hold an `actions-push` grant
  for one `refs/heads/<branch>` instead, accepted only as a fast-forward of
  the existing tip ([server/instance-actions.md](server/instance-actions.md)).
- **Mint or extend a grant.** Grants are Ed25519-signed by the router key,
  which only the app holds, over `forkmesh-runner-grant-v1` plus node, owner,
  repo, access, ref, runner, grant id and times.
- **Claim ineligible jobs.** Claims are signed with the runner's node key over
  method, path, body hash, single-use request id and time. Only `active`
  runners claim; pinned jobs go only to their runner; a `personal` runner
  serves only the account that registered it (see [Private runners](#private-runners));
  a `pool` runner serves only the cores in
  `RUNNER_POOL_TARGET_NODES` and only owners in its non-empty
  `allowed_owners` (the owners' opt-in). Steer jobs stay on their session's
  ref and hand over to another runner when the original is gone.
- **Reach core secrets, other jobs or the node key.** Jobs run as a dedicated
  unprivileged user (`fmjob-<account>` on a personal runner, `forkmesh-job`
  on the fleet) that cannot read the node key, daemon config or connector
  token.

### Private runners

Since 2026-10-14 a personal runner belongs to the account that added it and
to nobody else. Someone else's runner showed its provider account (the
Claude sign-in email and usage) to every admin on `/dashboard/runners`,
and an organization member could steer a session that ran on a teammate's
runner, spending the teammate's Claude plan.

- **Seen only by its registrant.** `GET /api/runners`, `GET
  /api/runners/attention` and every `/api/runners/<name>/…` route answer
  only for `runner_nodes.registered_by`; admins get no wider view. Anyone
  else gets `404`, the same as for a runner that does not exist. Pool
  runners follow the same rule (the admin who registered one sees it); only
  revoking a pool runner stays open to any admin.
- **Used only by its registrant.** Registration refuses a personal runner
  whose `allowedOwners` is not exactly the registrant, admins included. A
  personal runner leases a job only when its owner is the registrant and its
  `requestedBy` is the registrant (`runner_leases._owner_allowed`,
  `_requester_allowed`), so a row edited to list other owners serves nobody
  extra.
- **Hidden from teammates.** A session started on a personal runner records
  `runnerPrivate`; organization members other than the runner's owner see the
  queue node instead of the runner name and no runner status
  (`_org_agent_hide_private_runner`). A follow-up from anyone else is
  refused with `403 runner_private` (`_org_agent_private_runner_block`)
  instead of queueing a job only that runner could take.
- **Alerts go to the registrant only.** An expired or expiring sign-in on a
  personal runner pings its registrant; admins are pinged (and nobody is
  emailed) only for pool runners.
- **One job user per account.** The installer creates `fmjob-<account>`
  (`fmjob-<first 19 characters>-<6 hex of SHA-256>` when the account name is
  longer than 26 characters, to fit the 32-character user name limit), with
  its own home for the provider sign-in and its own work root for stored
  accounts and session homes. See [Several accounts on one
  machine](#several-accounts-on-one-machine).

Residual risks:

- All jobs of one runner share its job user's uid; keep a pool runner at
  `maxConcurrentRuns: 1`. `/tmp` is shared with the daemon's namespace and
  swept between jobs.
- The provider login is readable by agent code. Use a dedicated provider
  account for a pool that serves other people.
- **Runner HOME isolation gap (open).** `systemd-run --scope --uid` resets
  `HOME` from the job user's passwd entry, so a job runs with the job user's
  persistent home rather than its per-session home. Credential copy-back is
  guarded (only when the job's copy changed and never over a newer
  credential), but per-session HOME isolation is not in effect until the
  sandbox sets `HOME` after the uid switch.
- The read grant exposes the whole repository; loopback is blocked for jobs,
  so agents cannot reach dev servers on `127.0.0.1`.
- A non-root daemon runs jobs as its own user; a root daemon refuses to start
  without `runner.jobUid` or `runner.jobUser`.

### Job sandbox

```
systemd-run --scope --quiet --collect [--user] -p MemoryMax=<jobMemoryMax> -p MemorySwapMax=<jobMemorySwapMax> \
  -p CPUQuota=<jobCPUQuota> -p TasksMax=512 [--uid=<jobUid> --gid=<jobUid>] -- claude ...
```

- Root daemon: system manager with the job uid and an `IPAddressDeny` filter
  for loopback, link-local and private ranges. Non-root daemon: user manager,
  no IP filter. No manager: `ulimit -v` plus the Go-side timeout.
- Each session gets `<workRoot>/.homes/<id>` and an empty `TMPDIR`. Only the
  provider credential files are copied in (regular file, owned by the job
  user, at most 256 KiB, valid JSON, no symlinks) and copied back atomically.
- The environment is an allowlist: locale and `PATH`, proxy variables,
  provider keys the operator set on the daemon, the job's own `HOME`,
  `TMPDIR`, `USER`, and `GIT_TERMINAL_PROMPT=0`.
- On small runners the daemon unit binds a disk-backed `/tmp`
  (`/var/lib/forkmesh-runner/tmp`) instead of `PrivateTmp`, and
  `jobMemorySwapMax` lets cold pages reach swap rather than OOM-killing jobs.

## App endpoints

| Endpoint | Auth | Purpose |
|---|---|---|
| `POST /api/runners/register` | session plus key proof | register name and public key |
| `POST /api/runners/revoke` | registrant (pool runners: also any admin) | revoke |
| `POST /api/runners/claim` | runner-signed | lease up to N jobs |
| `POST /api/runners/report` | runner-signed, lease holder | report result |
| `POST /api/runners/attachment` | runner-signed, lease holder | bytes of one prompt image |
| `POST /api/runners/pool-status` | core-signed | queue depth for the autoscaler |
| `POST /api/runners/poll` | runner-signed | availability and sign-in hand-off |
| `POST /api/runners/stream` | runner-signed, lease holder | live output while watched |
| `POST /api/runners/login/output`, `/login/input-poll` | runner-signed | sign-in terminal relay |
| `GET /api/runners` | registrant | the caller's own runners, availability, open sign-ins |
| `GET /api/runners/attention` | registrant | the caller's runners whose provider login is expired or expiring |
| `POST /api/runners/<name>/login` and `/login/<id>/{output,input,cancel}` | registrant | web sign-in |
| `POST /api/runners/setup-tokens`, `GET …/<id>`, `POST …/<id>/revoke` | session | one-time install tokens |
| `POST /api/runners/register-token` | setup token plus key proof | register a personal runner |
| `POST /api/runners/ssh/probe`, `/ssh/install`, `GET /api/runners/ssh/<id>` | session | server-side SSH setup |

Registration proof: the runner key signs
`forkmesh-runner-register-v1\n<name>\n<publicKey>\n<account>\n<ts>`. Only an
admin may register a `pool` runner, and a `personal` runner always serves
only its registrant. `forkmesh-mirror-node --print-public-key`
prints the key. Prompt images are fetched per lease, checked for size, digest
and type, and written under `.forkmesh/attachments/` without following
symlinks.

## Runner config

```json
{
  "schemaVersion": 1,
  "role": "runner",
  "gatewayConfig": "/var/lib/forkmesh-mirror/mirror-gateway/config.json",
  "identityKey": "/var/lib/forkmesh-mirror/identity/ed25519.pem",
  "runner": {
    "appUrl": "https://app.forkmesh.com",
    "maxConcurrentRuns": 1,
    "workRoot": "/var/lib/forkmesh-mirror/runner-work",
    "jobMemoryMax": "700M",
    "jobCPUQuota": "90%",
    "allowedOwners": ["<owner>"],
    "jobUser": "fmjob-<owner>",
    "burst": false
  }
}
```

Defaults: `maxConcurrentRuns` 1 (max 8), `jobMemoryMax` `700M`, `jobCPUQuota`
`90%`, `jobMemorySwapMax` `0`. `appUrl` must be HTTPS except on loopback.
The fleet installer (`tools/next_fleet.py`, `deploy/core/render.py`) creates
the job user (home 0700, no shell, no groups), runs the daemon as root with
`runner.jobUid`, keeps keys owner-only, and adds nftables output rules that
deny the job user metadata, private, CGNAT, loopback (except a local DNS
stub), mesh and private IPv6 ranges.

To opt owners into a pool runner, re-register it with the new
`allowedOwners`, or update `runner_nodes.allowed_owners` as an admin. An
empty list serves nobody.

## Setting up a personal runner

Any account can add a runner from `/dashboard/runners`:

- **One command.** The page mints a setup token and shows
  `curl -fsSL https://forkmesh.com/install-runner.sh | sudo bash -s -- --token fmrt_… --name <runner> --app https://app.forkmesh.com`.
  Tokens are 256-bit, stored as SHA-256 only, single use, valid 30 minutes,
  bound to one name and owner, and rate-limited. The runner proves its key
  with `forkmesh-runner-register-token-v1\n<name>\n<publicKey>\n<sha256(token)>\n<ts>`.
  A token always registers `scope: personal` for its owner.
- **Installer** (`www/public/install-runner.sh`): validates arguments, installs
  prerequisites and folders, gets the signed daemon release, generates a key,
  registers, learns the owning account from the answer (`registeredBy`),
  creates that account's job user `fmjob-<account>` and work root, writes the
  config, installs the service and its own nftables table (never touching the
  machine's firewall), installs the CLIs, runs `--check` and starts. It
  deletes nothing and has no uninstall mode.
- **Re-running it** updates the instance that already holds the runner
  name, keeping its key, job user, work root and port. Another runner name of
  the same account replaces that account's instance: the old key moves aside
  to `ed25519.pem.<old runner>` and the new name gets a fresh key, so two
  runner rows never share a key, while the job user and sign-ins stay.
- **macOS** (macOS 12 or newer, Xcode Command Line Tools): the same command
  works. See [macOS runners](#macos-runners).
- **From the desktop** (preferred for SSH): a `forkmesh://` hand-off fills the
  Hosts form; the desktop verifies the host key and runs the install line over
  its own SSH session, so the password never leaves the desktop.
- **From the servers:** `fmnext-ssh-helper` (`app/host/forkmesh_host/ssh_helper.py`)
  runs SSH in a socket-activated `DynamicUser` sandbox with no credentials and
  private networks blocked. Addresses must be public; the host key is pinned
  after the user confirms the fingerprint; a password reaches `ssh` only via
  `SSH_ASKPASS` from the child environment and a key only via a private
  `ssh-agent`; output is scrubbed. Without the helper socket the endpoints
  answer `503 ssh_setup_unavailable`.

### Several accounts on one machine

Each account that installs a runner on a machine gets its own instance
next to the others; installing one never stops or rewrites another.

| | Per account (`<account>` = the registrant) |
|---|---|
| Config | `/etc/forkmesh-runner/accounts/<account>/runner.json` (0700 folder) |
| State | `/var/lib/forkmesh-runner/accounts/<account>/{identity,gateway,work,tmp,home}` |
| Service | `forkmesh-runner@<account>.service` from the template `forkmesh-runner@.service`; `ReadWritePaths` is only its own state folder and its `/tmp` is its own `tmp` |
| Job user | `fmjob-<account>`, home `…/accounts/<account>/home` (0700), GECOS `ForkMesh runner jobs for <account>` |
| Loopback ports | `listen`/`gatewayProbe` pair from 8795 upward, skipping pairs another instance's config names or that are taken; kept on re-install |

- **Finding instances.** The installer reads `/etc/forkmesh-runner/runner.json`
  (an install from before 2026-10-14) and every
  `/etc/forkmesh-runner/accounts/*/runner.json`. Before registering it looks
  for the instance holding the runner name; a new name signs with a staged
  key (`/var/lib/forkmesh-runner/pending-<name>.pem`) and moves into the
  owner's instance once ForkMesh names the owner.
- **Older installs.** The single-runner layout (`forkmesh-runner.service`,
  `/var/lib/forkmesh-runner`, user `forkmesh-job`, ports 8795/8796) stays
  that account's instance. Re-installing it rewrites its unit with
  `InaccessiblePaths` over the `accounts` folders, so it cannot read the
  other instances.
- **Job users are never shared.** A job user that another instance's config
  names is refused, and an existing system user is taken over only when its
  GECOS (`RealName` on macOS) names the same account, so an account whose
  name collides with another's derived user name, or with an unrelated user,
  cannot inherit it.
- **One firewall.** `jobs.nft` (Linux) and `jobs.pf` (macOS) are rebuilt on
  every install with the rules for every instance's job user; a job user
  that no longer exists is left out with a warning. A loaded nft table is
  reloaded with `nft -f` in place.
- The daemon needs no change: its idle sweep kills and deletes only what its
  own job uid owns.

## macOS runners

The installer printed `Runners need Linux with systemd.` on a Mac until
2026-10-14. On Darwin it now differs from Linux in these ways:

| | Linux | macOS |
|---|---|---|
| Daemon | `forkmesh-runner@<account>.service` | launch daemon `com.forkmesh.runner.<account>` (`/Library/LaunchDaemons`), log `/private/var/log/forkmesh-runner-<account>.log` |
| Paths | `/etc/forkmesh-runner/accounts/<account>`, `/var/lib/forkmesh-runner/accounts/<account>` | `/private/etc/forkmesh-runner/accounts/<account>`, `/private/var/db/forkmesh-runner/accounts/<account>` (real paths, so no `/etc` or `/var` symlink sits inside `workRoot`) |
| Job user | `useradd --system fmjob-<account>` | `dscl` hidden user `fmjob-<account>`, first free uid from 400 down to 300, shell `/usr/bin/false`, home `…/accounts/<account>/home` |
| Job firewall | nftables table `inet forkmesh_runner` | pf anchor `com.apple/forkmesh-runner` (`jobs.pf`, rules `user <uid>`), loaded by `com.forkmesh.runner-firewall` at boot and every 5 minutes; it enables pf if pf is off and never loads `/etc/pf.conf` |
| Daemon binary | signed release via `install.sh` | built from `https://forkmesh.com/forkmesh/forkmesh` (`server/`, sparse clone) with Go, because releases publish only Linux `forkmesh-mirror-node` assets; `FORKMESH_RUNNER_SOURCE_URL` overrides the clone URL |
| OpenSSL | system `openssl` | OpenSSL 3 (Homebrew `openssl@3`): the LibreSSL in macOS has no Ed25519 `genpkey` |
| Agent CLIs | `npm -g` as root | `npm -g` as the `sudo` user, so the Homebrew prefix stays theirs |

Homebrew runs as the user who ran `sudo` (`SUDO_USER`), never as root, to
install a missing Go, `openssl@3` or Node.js. The Go build folder stays in
`/private/tmp`, which macOS clears when it restarts; the installer deletes
nothing.

### Job sandbox on macOS

macOS has no systemd, so `detectJobSandbox` (`server/sandbox_darwin.go` sets
`directJobUID`) runs a root daemon's jobs directly: the child gets the job
uid and gid via `SysProcAttr.Credential` with no supplementary groups, in its
own process group. The sign-in pty uses the same credential. When no job is
running, the daemon runs `/bin/kill -KILL -1` as the job user, which ends
everything that user left behind, then sweeps the shared temp folders
(`/private/tmp`, `/private/var/tmp`).

What macOS lacks next to Linux:

- No memory, CPU or task limits; only the job timeout. `ulimit -v` is not
  applied because macOS refuses `RLIMIT_AS`.
- No per-scope `IPAddressDeny`; the pf anchor is the only network control and
  covers TCP and UDP only.
- HOME is set after the uid switch, so the per-session HOME does apply here
  (the Linux HOME gap above does not occur).

Untested on a live Mac: the launch daemon, `dscl` user and pf anchor have
not been loaded on a real machine as root. `pfctl -n` parses the generated
`jobs.pf` and `plutil -lint` accepts both plists (office Mac, macOS 15).
Claude Code on macOS normally stores its login in the Keychain, while the
runner copies credential files; check that a web sign-in as the hidden job
user writes `~/.claude/.credentials.json`.

The daemon builds for `darwin`: `pty_darwin.go` opens a pty with
`TIOCPTYGRANT`/`TIOCPTYUNLK`/`TIOCPTYGNAME`, and `memory_darwin.go` reports
no memory figures (`/proc` and `sysinfo` are Linux only).

## Provider credentials

- **Mode A (implemented):** the runner uses its own signed-in `claude` or
  `codex` CLI, signed in from the website or on the host as the job user.
- **Web sign-in:** a `runner_login_sessions` row (10-minute TTL, one open per
  runner and provider) is handed to the runner on its next claim or poll. The
  runner runs the provider's fixed login command on a pty as the job user
  inside the sandbox and relays the screen in numbered chunks; token-shaped
  text is scrubbed before it leaves the runner. Only the account that
  registered the runner may drive it.
- **Several accounts per provider:** up to 12, stored under
  `<workRoot>/.accounts/<provider>/`; one is active, and refreshed credentials
  go back to the account the job used.
- **Expired logins:** the runner classifies authentication failures, stores a
  `needs_login` mark keyed by the credential's tokens, stops leasing that
  provider, and reports `auth` state (`ok`, `expiring`, `expired`) on every
  claim and poll from the credential file without a network call. The app
  pings the registrant (and, for a pool runner, the admins), turns a pool
  runner's `/status` row red, and
  refuses new jobs with `409 runner_login_expired` when no runner can take
  them. `GET /api/runners/attention` lets the web, desktop and phones show a
  sign-in prompt; its shape is stable.
- **Mode B (not built):** burst runners would receive the owner's provider
  token sealed to a one-time in-memory key, passed only through the job
  environment, for owners who opted in to cloud runs.

## The runners page

`/dashboard/runners` (`partials/views/runners.html`, `js/09-runners.js`)
follows GitHub's settings layout: a short header with the long sign-in
explanation folded under "How sign-in works", then one bordered list. Each
runner is a tinted header row (name, scope label, online status, last
check-in) followed by flat provider rows and hairline-separated account rows,
so nothing nests a box in a box. Colour follows the dashboard theme: links,
focus and the active account use the accent blue; green is kept for the
"Set up a runner" button, the online dot and the "Signed in" label; usage
bars are blue, amber from 80 % and red from 95 %. A runner counts as online
when it checked in within 10 minutes, the same window as `ONLINE_MS` in
`runner_logins.py`. The sign-in panel is a single box: the authorize URL as a
truncated field with Copy and Open, the terminal as one flat code block, then
the code form. The settings page renders the same rows into its own list
container ([app/settings-agents-and-runners.md](app/settings-agents-and-runners.md)).

## Live agent output

While someone watches a run, the runner streams validated stream-json lines
to `POST /api/runners/stream` (mirrors use the per-repository stream route).
A heartbeat every 5 s learns whether anyone is watching; chunks are numbered
per job and deduplicated; the terminal report ends the stream.

## Burst pool (`server/runnerpool.go`, `server/vultr.go`)

Off by default; configured as `runnerPool` in a core's daemon config
(`maxBurstRunners`, `monthlySpendCeilingCents`, `hourlyPriceCents`,
`scaleUpAfter`, `idleTimeout`, region, plan, snapshot, `apiKeyFile`). Every
30 s it reads `/api/runners/pool-status`, lists instances and applies the pure
`decidePool`:

- scale up one instance per round when the queue has waited `scaleUpAfter`,
  no always-on runner has a free slot, and the count and spend ceiling allow;
- destroy instances idle for `idleTimeout` (5-minute boot grace);
- at the spend ceiling destroy every managed instance;
- touch only instances labelled `fmnext-burst-<n>` and tagged
  `forkmesh-next`, re-checked before every destroy.

The runner snapshot and its one-time registration are not built yet, so keep
`enabled: false`. Policy defaults are in [fleet/AUTOSCALING.md](fleet/AUTOSCALING.md).

## Failed runs

A failed run reports one error in `lastError`, chosen by
`server/agent_errors.go` from the CLI's result errors, an API-error message,
the stderr tail, or the exit status, capped at 600 runes. An outdated CLI gets
an explicit update message and is not retried with `--resume`. A run publishes
its branch only when it committed something.

## Node logs

Burst runners must be discoverable and carry the log reader for their logs to
reach the desktop ([NODE_LOGS.md](NODE_LOGS.md)).
