# Your own ForkMesh instance (2026-10-07)

Anyone can start their own ForkMesh, their own Git host, from
**forkmesh.com/start**. They give it a name, a server and an admin account.
ForkMesh's servers connect to that server over SSH, install a single-node
ForkMesh core on it, and stream every step to the page. When it is done the
page takes them to their new `<owner>/forkmesh` repository on their own
domain. The instance carries their project's name everywhere, and every page
shows a small **Powered by ForkMesh** badge.

This replaces the earlier attempts: the Cloudflare-token launcher in the World,
the World's "Connect your instance" panel (Cloudflare / Vultr / SSH script),
and the old `/start` page that drove the Cloudflare and Vultr APIs
(`app/src/start_setup.py`, removed).

## Where people start

- **Homepage.** The hero's "Start your own instance" button and the final call
  to action link to `/start` (`www/templates/pages/index.html`).
  `forkmesh.com/start` redirects to `app.forkmesh.com/start`
  (`www/public/_redirects`, `www/worker.js` `APP_PATHS`).
- **Blog.** "Start your own ForkMesh" (`www/templates/pages/blog/start-your-own-forkmesh/`)
  announces the page; its links carry `utm_campaign=start_instance`
  ([campaign brief](marketing/campaigns/start-instance-2026-10-07.md)). The
  page is served from assets (`!/start/` in `run_worker_first`), so those
  visits are measured in Analytics, not on `/admin/acquisition`.
- **World plots.** Clicking a "Start your world here" sign on a desert plot
  opens `/start?plot=<n>` in a new tab (`openStartInstance()` in
  `world/public/world/world.js`). The page notes which plot the visitor came
  from; the plot is not reserved. A new instance gets its own hexagon in the
  World when **Give my instance its own world** is on (see
  [In the World and on the homepage](#in-the-world-and-on-the-homepage-2026-10-13)).

## The form (`app/public/start.html`, `start.js`)

Six short sections, then **Start my instance**:

1. **Project name.** Up to 40 letters, numbers, spaces and `._&+-`. It
   becomes `INSTANCE_NAME`. An optional **logo** (PNG, JPEG, WebP or SVG, up
   to 200 KB; upload or drag and drop) is read in the browser as a `data:`
   URL into the hidden `logo` field. A live preview shows the logo, the name,
   the address and "Powered by ForkMesh". See [Logo](#logo-2026-10-08).
   A **colour theme** choice lists ForkMesh's default colours and every
   preset in `instance_theme.PRESETS` (Crimson today); the choice is sent as
   `theme`, checked by `instance_launch.validate` against `PRESETS`, and
   passed to the installer, which seeds the theme row (see Theme below).
2. **Server.** IP (or host name), SSH user (default `root`), port (default
   22), and a password or a private key (upload, drag and drop, or paste; plus
   an optional passphrase). An optional host-key fingerprint
   (`SHA256:…`); without it the first key the server shows is trusted and shown
   in the log.
3. **Domain** (optional). With a domain, the page shows the A record to add
   and setup waits for it. Without one the instance starts on
   `<ip-with-dashes>.sslip.io`, which resolves to the server at once, so HTTPS
   works without any DNS work.
4. **Admin account.** Username, email and password (10+ characters), and who
   owns the `forkmesh` repository: an **organization** (its name, prefilled
   from the project name) or **my user account** (the admin). The name
   `forkmesh` is kept for the instance's git node.
5. **Email** (optional). Mailtrap, SendGrid or Mailgun: the API key, an
   optional sender (default `no-reply@<domain>`), and for Mailgun the sending
   domain and region (US/EU).

Secret fields are cleared after the request is sent and again on `pagehide`.
The page then shows the twelve steps, a progress bar, an elapsed clock, the
server's host key, and the live log. It polls
`GET /api/start/launches/<id>` every second; the launch id is kept in the URL
(`?launch=<id>`), so a reload picks the run up again. On success it redirects
to the repository after five seconds; on failure it shows the reason and a
**Change the details and try again** button.

That button brings the form back with the details still in it and focuses the
SSH password (or key), since secrets are cleared after every send. After a
reload the form is empty, so it refills what the launch record holds: project
name, server address and port, domain (not an `sslip.io` one), admin username
and owner (`refillFrom`, from `GET /api/start/launches/<id>`). Nothing is kept
in browser storage (`test_start_instance_page.py` checks this). The failure box
says that starting again on the same server skips finished steps and follows a
setup still running there (see "Runs in the background, resumes and streams"
below).

### Set up from a file (2026-10-08)

Above the five sections, **Set up from a file** takes one JSON file (upload or
drop, 400 KB at most, so it can carry a logo) whose keys are exactly the `POST /api/start/launch` body
(`CONFIG_FIELDS` in `start.js`), so one file can fill the whole form and the
setup is one click. The file is read in the browser only; nothing is sent until
the launch, as when the form is typed.

- An empty value or a missing key keeps what the form has, so a key uploaded
  in the form is not wiped by the example's empty `sshPrivateKey`. Keys
  starting with `_` or `$` are notes; any other unknown key, an object value,
  or a choice the selects do not offer is skipped and named in the status.
- A file with `sshPrivateKey` switches to key sign-in, otherwise
  `sshPassword` switches to password; a fingerprint opens its section.
- After loading, `localCheck` runs: if something is missing it focuses that
  field with the usual error; if not, **Start my instance now** appears in the
  card and submits the form (`requestSubmit`).
- Links: the user guide `www/docs/start-config.md` (served at
  `forkmesh.com/docs/start-config/`), the example zip
  `app/public/start-config.example.zip` (`forkmesh-instance.json` and
  `logo.svg`, downloaded as `forkmesh-instance.zip`), the example settings
  alone `app/public/start-config.example.json` (downloaded as
  `forkmesh-instance.json`) and **Copy example settings**, which copies them to
  the clipboard. The zip's `forkmesh-instance.json` is byte for byte the
  example JSON (`test_start_instance_page.py` checks it); after changing the
  JSON, rebuild the zip with Python's `zipfile` (deflate, both files at the
  top). The example is not an `install-instance.sh --config` file: that one
  also needs `publicAddress` and a resolved `domain`, which the API adds.

### One zip with the settings and the logo (2026-10-13)

A JSON file can only carry the logo as a base64 `data:` URL, which nobody
writes by hand, so the settings file became a zip: the JSON and the logo image
side by side. `.json` uploads still work as before.

- `start.js` reads the zip itself (`zipEntries`, `zipRead`): the central
  directory, stored or deflated entries (deflate through the browser's
  `DecompressionStream("deflate-raw")`), no encryption, no ZIP64. The zip is
  1 MB at most; the JSON is read up to 400 KB and the logo up to 200 KB, and
  inflating stops past those limits, so a zip bomb never fills memory.
- Folders, `__MACOSX/` and dot files are ignored, so a zip made by Finder or
  of a folder works. It needs exactly one `.json`.
- The logo: when `logo` in the JSON is a file name (not a `data:` URL), that
  image is used, matched by path or by name; otherwise the zip's only PNG,
  JPEG, WebP or SVG image. Several images and no name is an error that asks
  for the name. The image becomes the same `data:` URL the logo picker makes,
  so the API and installer are unchanged. A `.json` upload naming a logo file
  loads everything else and says the logo needs the zip.

### Other organization owners (2026-10-13)

An organization can start with several owners, not only the admin. Section 4
has **Other owners** (shown for an organization only): rows of username,
email and password, up to 20, added with **Add an owner**. The settings file
carries them as `owners`, a list of `{"username", "email", "password"}`.

- Each one gets an ordinary user account (verified email, not an instance
  admin) and an `owner` row in `org_members`, the same role as the admin.
- A password is required: an instance may have no email service, so a reset
  link cannot be the way in. Owner passwords are secrets like the admin's:
  cleared from the form on `pagehide` and after the launch, never stored in
  `instance_launches`, and scrubbed from the streamed log
  (`ssh_helper.config_secrets`, the launch's `secrets`).
- Checks (page, API `parse_owners`, installer, bootstrap `read_owners`):
  usernames follow the admin rules and differ from the admin, the
  organization, `forkmesh` and each other; each has an email of its own;
  owners need `ownerType: org`.
- The installer keeps the owners' passwords off the command line: it passes
  `owners` as the first stdin line of the `instance-bootstrap` call, the admin
  password after it, and only the usernames (`CFG_owners`) reach the shell.

### In the World and on the homepage (2026-10-13)

Section 6, **On ForkMesh**, has two checkboxes, both on by default. The
settings file carries them as `ownWorld` and `listOnHomepage` (`true` or
`false`; a missing or empty value means on, and `false`, `no`, `off` and `0`
mean off, `instance_directory.flag`).

- **Give my instance its own world.** The instance gets a hexagon of its own
  in the World, joined to the ForkMesh one: a meadow the size of the city with
  its own road, a small landmark (a stone plinth, an obelisk and a board with
  the logo, facing the city), the logo laid on the ground and a label with the
  name and address. Clicking the landmark or the label opens the instance in
  a new tab. See [world/README.md](world/README.md), "Instance hexagons".
- **List my instance on forkmesh.com.** The homepage's **ForkMesh instances**
  section, under the Ecosystem section, lists the name, the logo (or the
  name's first letter) and the address, linking to `https://<domain>/`. The
  section stays hidden while no instance is listed.

Nothing is published until the launch succeeds. `run_launch` keeps the name,
the logo and the two choices in memory (`instance_directory.listing`) and,
after the row turns `succeeded`, calls `instance_directory.publish`, which
writes table `instance_directory` (migration `0257_instance_directory`,
created by the module itself too, as `marketplace_reviews` does), keyed by
domain: name, `https://<domain>/`, the logo `data:` URL, the two choices, the
World slot and times. The row's `id` is the first launch's id and is kept. A
publish failure never fails the launch.

- Launching again on the same domain updates the row and keeps its slot.
  Turning the world off frees the slot; turning both off deletes the row.
- The World has 12 slots (`WORLD_SLOTS`), the second ring of hexagons. The
  lowest free slot is taken; with all 12 taken, the instance is listed on the
  homepage but has no hexagon (`slot` -1) until one frees up.
- `GET /api/start/instances`: the listed instances and those with a hexagon,
  oldest first, at most 200, `public, max-age=60`. Each has `id`, `name`,
  `url`, `logo` (a `/api/start/instances/<id>/logo?v=<updated_at>` path or
  empty), `listed`, `world`, `slot` and `createdAt`.
- `GET /api/start/instances/<id>/logo`: the image bytes with their own type,
  `public, max-age=3600`, `nosniff` and `Content-Security-Policy: default-src
  'none'; style-src 'unsafe-inline'; sandbox`, so an SVG opened on its own
  cannot run anything even if it got past `LOGO_UNSAFE_RE`. Both the homepage
  (`forkmesh.com/api/*`) and the World (`world.forkmesh.com/api/*`) reach the
  app on their own origin.

#### Offline instances, hiding and deleting (2026-10-13)

- **Health check.** Cron phase `instance-health` (every 15 minutes at
  residue 11, `app/src/cron_activity.py`) runs
  `instance_directory.check_health` through
  `instance_directory_admin.health_cron`: the 20 entries checked longest ago
  (`HEALTH_BATCH`) get `GET https://<domain>/readyz` (8 s,
  `js_fetch_with_timeout`, no redirects, through the egress guard), which the
  instance's ASGI host answers itself. Only a 200 counts as online; a refusal,
  a timeout or any other status is a miss. `health_url` refuses anything that
  is not a plain host name.
- **Offline is not shown.** Each miss adds one to `failures` and sets
  `offline_since` on the first. From `OFFLINE_FAILURES` (2) misses in a row,
  about half an hour, the entry is left out of `GET /api/start/instances` and
  its logo (`VISIBLE` in `instance_directory.py`), so the homepage drops the
  card and the World drops the hexagon on its next refresh. The slot is kept.
  One answer resets `failures`, `offline_since` and `pinged_at`, and a new
  successful launch on the domain does the same.
- **The ping.** When an entry has been offline for a day (`PING_AFTER_MS`)
  and no ping was sent for this outage, the admins (`_operator_alert_admins`)
  get one `operational_alert` ping: "Instance offline: <name>", asking whether
  it should be deleted, linking to the Instances page. `pinged_at` and the
  dedupe key `instance-offline:<id>:<offline_since>` keep it to one per
  outage; an entry that comes back and goes offline again pings again after
  another day. Hidden entries are still checked but never ping.
- **Admin page.** Admin console, Growth, **Instances** (`?view=instances`,
  `app/src/instance_directory_admin.py`): every entry with where it shows
  (homepage, World hexagon number, or "no free hexagon"), its status (Online,
  Missed one check, Offline since …, Hidden by an admin, Not checked yet),
  when it was last checked and started, and three actions:
  - **Hide** keeps it off the homepage and the World even while it answers;
    a later launch on the domain keeps it hidden. **Show** undoes it.
  - **Delete** (with a "sure" box) removes the row and frees its World slot.
    The instance itself is not touched; launching again lists it again.
  Each action needs the console's CSRF token and is written to the audit log
  as `admin.console_hide_instance`, `admin.console_show_instance` or
  `admin.console_delete_instance`.

## The API (`app/src/instance_launch.py`)

| endpoint | purpose |
|---|---|
| `POST /api/start/launch` | validate, resolve, read the host keys, start the run; `202 {id, domain, address, hostKey, steps, url}` |
| `GET /api/start/launches/<id>` | `{launch: {state, steps, message, log, url, hostKey, …}}`; the 128-bit id is the capability |
| `GET /api/start/instances` | the public list of instances on the homepage and in the World |
| `GET /api/start/instances/<id>/logo` | an instance's logo |

- No account is needed. Launches are limited to 4 an hour per client (the
  client address is only kept as a salted SHA-256, `client_key`) and 5 running
  at once overall.
- Validation errors name the form field (`field`), so the page can focus it.
- The host must resolve to public addresses only (the WireGuard range and
  private networks are refused), exactly as for runner SSH setup.
- Table `instance_launches` (migration `0240_instance_launches`): project,
  domain, owner, admin name, address, port, the pinned host key's type and
  fingerprint, state, steps (JSON), message, the last 400 log lines, the final
  URL. No password, key or API key is ever written; rows are deleted after
  seven days. A running launch writes its row at least every 5 seconds
  (`HEARTBEAT_MS`), so one that has not reported for 2 minutes
  (`RUN_STALE_MS`) has lost its SSH session (a core restart, a dropped
  connection) and turns `failed` with `LOST_MESSAGE`: the setup keeps running
  on the server, and starting again with the same details follows it.
- Progress: the installer prints `FMI {"step":"<id>","state":"running|done|failed"}`
  lines. `run_launch` turns them into step states (a new running step marks
  the previous one done) and keeps every other line in the log, with each
  secret replaced by `[hidden]`. Only an exit status of 0 **and** a final
  `FMI {"done":true,"url":…}` line count as success; the URL must stay on the
  instance's own domain (`safe_redirect`).
- Live log (2026-10-08). A task (`_pump`) moves the helper's lines into a
  queue, and `run_launch` waits on it for at most `FLUSH_MS` (700 ms). New
  lines are written within that time even when the server then goes quiet;
  before, they waited in memory for the next line, so the page could sit on an
  old log through a whole build. The page polls every second.

## Where SSH runs (`app/host/forkmesh_host/ssh_helper.py`)

The same sandboxed `fmnext-ssh-helper` as runner SSH setup
([RUNNERS.md](RUNNERS.md), "SSH details"): socket-activated, `DynamicUser`,
no credentials, private networks blocked. It gained one operation,
`instance`:

- It checks the address, port, pinned host key, user, a `https://…/*.sh`
  installer URL and a settings object under 384 KB (`MAX_CONFIG`; a request
  line may be 512 KB, `MAX_REQUEST`, which is also the socket's read limit).
  Both were 32 KB and 64 KB before the logo, which alone may be 200 KB
  (about 270 KB as base64).
- The remote command downloads the installer to a temporary file and runs
  `bash "$t" --config-stdin` (through `sudo -n` for a non-root user, after
  `sudo -S -v` with the password when signing in by password).
- The settings, including the admin password and the email key, travel as one
  base64 JSON line on SSH standard input, never in the command line. Both
  secrets are added to the output scrubber.
- Output is read in chunks and split on `\n`, `\r\n` and a bare `\r`, so
  progress counters become separate lines; a line longer than 8 KB is sent in
  pieces (`stream`, `MAX_PENDING`). Before 2026-10-08 it used `readline`,
  which waited for a newline and failed on a 64 KB line.
- The run may take up to 50 minutes (`INSTANCE_TIMEOUT`; the app side waits
  55).

App settings: `INSTANCE_INSTALL_URL` (default
`https://forkmesh.com/install-instance.sh`), `INSTANCE_SOURCE_URL` (default
`https://app.forkmesh.com/forkmesh/forkmesh`) and `INSTANCE_SOURCE_REF`
(default `main`).

## The installer (`www/public/install-instance.sh`)

Runs on the user's server as root. Steps (the ids the page shows):

| step | what happens |
|---|---|
| `system` | Debian 13+ or Ubuntu 24.04+, x86_64 or arm64, at least 1 GB memory and 6 GB free disk, ports 80/443 free |
| `packages` | a 2 GB swap file on small machines, then `git python3 python3-venv nodejs npm caddy sqlite3 age` when any is missing (apt waits up to 15 minutes for another apt to finish, `DPkg::Lock::Timeout`); opens 80/443 in ufw when ufw is active; system users `forkmesh-app` and `forkmesh-node` |
| `source` | shallow clone of the ForkMesh source into `/opt/forkmesh-instance/src` |
| `build` | venv from `app/host/requirements.txt`, `python -m forkmesh_host build` with the ForkMesh-only marketplace packages left out (`FORKMESH_EXCLUDED_PLUGINS`), then `app/tools/apply_instance_brand.py` |
| `configure` | `/etc/forkmesh-instance/app.env` (0640 root:forkmesh-app): instance name, origins, plugins, mail, and fresh `DATA_KEY`, `ADMIN_PASS` and router keys (kept on re-runs), plus `instance.json` without secrets |
| `database` | `python -m forkmesh_host migrate` into `/var/lib/forkmesh-instance/app.db` (SQLite, single node, no rqlite) |
| `services` | `forkmesh-instance.service` (uvicorn on 127.0.0.1:8080, `ProtectSystem=strict`) and a wait for `/readyz` |
| `https` | the Caddyfile (the app, plus the git gateway paths `/v1/*`, `/health` and `/forkmesh-mirror.json` to 127.0.0.1:8790, and `Strict-Transport-Security: max-age=31536000`); with a domain, waits up to 20 minutes for its A record (AAAA for an IPv6 address) or for Cloudflare's proxy; see [DNS check](#dns-check-2026-10-08) and [Behind the Cloudflare proxy](#behind-the-cloudflare-proxy-2026-10-08); then up to 5 minutes (`FORKMESH_CERT_WAIT_SECONDS`) for a certificate that `curl` accepts with the system's CAs for that name, showing Caddy's certificate messages as it goes; then prints the issuer and expiry and checks that `http://` redirects to `https://` and, behind Cloudflare, that Cloudflare reaches the server |
| `admin` | the git node: the signed `forkmesh-mirror-node` release (through `install.sh` with `FORKMESH_NO_LAUNCH=1`, as `install-runner.sh` does, plus `FORKMESH_NO_INSTALL_DEPS=1` and `FORKMESH_NO_SOURCE_FALLBACK=1` so a server gets no desktop GUI libraries and never compiles the desktop app), `--init` as node `forkmesh` mirroring the ForkMesh source, the core overrides (`role: core`, hosted repositories on), then `python -m forkmesh_host instance-bootstrap` |
| `repo` | starts the git node and waits up to 15 minutes for `<owner>/forkmesh` to answer `info/refs` |
| `done` | prints `FMI {"done":true,"url":"https://<domain>/<owner>/forkmesh"}` |

`--update` re-runs everything from `instance.json` (pull, rebuild, migrate,
restart) and keeps the secrets, the database and the admin. The build
rewrites a tracked file (`app/public/dashboard-page-docs.js`), so before
moving to the new revision the installer saves any changes in
`/opt/forkmesh-instance/src` to `/opt/forkmesh-instance/src-changes-<time>.patch`
and then checks out with `-f` (allow-listed in
`test_no_unguarded_destructive_deletes.py`). Running the setup
again with another domain moves the instance: the git node is set up again
with its existing key (`--init` reuses `identity/ed25519.pem`), because its
signed `forkmesh-mirror.json` names the old origin and the gateway refuses to
start with it. The old daemon config is kept as
`/etc/forkmesh/mirror-node.json.before-<time>`.
`--config FILE` takes the same JSON as the page sends, for a manual install.

### Runs in the background, resumes and streams (2026-10-08)

The setup used to run inside the SSH session, so a dropped connection or a
core restart killed it halfway (possibly inside `apt`), and every run started
over, rebuilding the app. Now:

- **The run is a systemd service on the server.** After the settings parse,
  the installer (the "front") copies itself to
  `/run/forkmesh-instance-setup/`, writes the settings there as
  `settings.json` (0600, tmpfs) and starts itself with `systemd-run --unit
  forkmesh-instance-setup --service-type=exec ... --detached --config
  settings.json`, output appended to `/var/log/forkmesh-instance-setup.log`
  (the previous run's log is kept as `.log.1`). The detached run deletes
  `settings.json` as soon as it has read it and writes its exit status to
  `/run/forkmesh-instance-setup/status`. The test hooks are passed on with
  `--setenv`.
- **The front follows the log** from its first line (`tail -n +1 -F
  --pid=<MainPID>`) and exits with the run's status, so the page sees the
  same `FMI` lines and exit code as before. If the connection drops, only the
  `tail` dies.
- **Starting again while it runs** follows the running setup from its first
  line instead of starting a second one. The new details are not used (the
  log says so); start again after it ends to change them. A lock
  (`/run/forkmesh-instance-setup/start.lock`, `flock`) makes the check and
  the start one step, so two launches at the same moment cannot both start a
  run; it is released before following.
- Without systemd (`/run/systemd/system`), when `systemd-run` fails, or with
  `FORKMESH_INSTANCE_FOREGROUND=1`, it runs in the session as before.
- **Every step checks what is already done**, so a new run continues where the
  last one stopped:

  | step | skipped when |
  |---|---|
  | `packages` | every package is installed (`dpkg-query`) |
  | `source` | the fetched ref is the checked-out commit (no checkout, no patch file) |
  | `build` | `/opt/forkmesh-instance/build.stamp` matches the commit, installer version, left-out packages, project name and requirements; the stamp is removed before a build and written after it, so an interrupted build runs again |
  | `services` | the build, `app.env`, the unit and the migrations are unchanged and `/readyz` answers (no restart) |
  | `https` | the Caddyfile is unchanged, Caddy is active and HTTPS answers (no restart, no DNS wait); if only the Caddyfile changed, Caddy restarts but HTTPS already answering skips the DNS wait |
  | `admin` | the git node binary is installed (downloaded again on `--update`); the bootstrap itself is idempotent |
  | `repo` | the binary, daemon config, unit and gateway config are unchanged and the node is active (no restart) |

  `system`, `configure` and `database` always run; they are quick and
  idempotent.
- **Output streams.** `PYTHONUNBUFFERED=1`; `apt` runs under `stdbuf -oL` and
  is summed up as it goes ("Set up 25 of 412 packages", `apt_lines`); `pip`
  shows each `Collecting` line (`pip_lines`); the build streams every line
  (`lines`) instead of the last 8 at the end; the migration and clone steps say
  what they are doing before they start.

Test hooks (not used by the page): `FORKMESH_INSTANCE_SOURCE_DIR` (clone a
local checkout), `FORKMESH_INSTANCE_DAEMON` (a local daemon binary),
`FORKMESH_INSTANCE_TLS=internal` (Caddy's own CA instead of Let's Encrypt),
`FORKMESH_DNS_WAIT_SECONDS`, `FORKMESH_PROXY_WAIT_SECONDS`, `FORKMESH_REPO_WAIT_SECONDS`.

### DNS check (2026-10-08)

A server whose record was already in place sat on "Waiting for DNS". The
check asked only Cloudflare's DNS-over-HTTPS, which keeps a "no such name"
answer cached for up to the zone's negative TTL when the name was looked up
before the record existed, and it accepted only the exact address typed into
the form, so a record pointing at another address of the same server (its
outbound address behind NAT, or another interface) never matched.
`resolves_here` now takes the A/AAAA answers from Cloudflare and Google
DNS-over-HTTPS and from `getent` (loopback answers from `/etc/hosts` are
ignored) and counts DNS as ready when any of them is the form's address, an
address on a global interface (`ip -o addr`) or the server's outbound address
(`cloudflare.com/cdn-cgi/trace`). When HTTPS already answers for the domain
before the Caddyfile is rewritten, the DNS wait is skipped. When the name
resolves somewhere else, the setup says where it points (for example a
Cloudflare proxy) instead of asking to add a record.

### Behind the Cloudflare proxy (2026-10-08)

A domain proxied by Cloudflare (orange cloud) resolves to Cloudflare's
addresses, never to the server, so the DNS check waited 20 minutes and
failed. Caddy was never restarted with the instance's Caddyfile, nothing
listened on 443, and visitors got Cloudflare's 521 page ("Host: Error").

- `dns_ready` counts DNS as ready when `resolves_here` does, or when
  `http://<domain>/cdn-cgi/trace` answers with `h=<domain>`: Cloudflare's edge
  answers that path for every proxied name (`behind_cloudflare`, sets
  `PROXIED=1`).
- Caddy then gets its Let's Encrypt certificate through the proxy: Cloudflare
  passes `/.well-known/acme-challenge/` on plain HTTP to the server, even with
  "Always Use HTTPS" on. The local `https_live` check is unchanged.
- `check_cloudflare` (run from `check_https`, so also when HTTPS was already
  live) waits up to `FORKMESH_PROXY_WAIT_SECONDS` (60) for
  `https://<domain>/api/version` to answer 200 with `x-forkmesh-worker: app`
  through Cloudflare. A redirect back to the same `https://` address means the
  zone's SSL/TLS mode is Flexible (Cloudflare talks plain HTTP to Caddy, which
  redirects); the setup stops with exit 32 and asks for **Full (strict)**. A
  52x answer, or a page that is not this app, stops with the record and
  firewall to check.
- The Caddyfile trusts `Cf-Connecting-Ip` and `Cf-Ipcountry` only from
  Cloudflare's published ranges (`@cloudflare remote_ip $CLOUDFLARE_RANGES`,
  from cloudflare.com/ips, fixed in the script so the Caddyfile digest stays
  stable). Without it every visitor behind the proxy had a Cloudflare edge
  address, so per-address limits counted them all as one. Anyone else still
  gets `Cf-Connecting-Ip` set to their own address.
- The start page's domain hint says the proxy can stay on with SSL/TLS set to
  Full (strict).

## First admin, owner and the forkmesh repository (`instance_bootstrap.py`)

`python -m forkmesh_host instance-bootstrap` reads JSON on stdin and, through
the app's own record helpers:

- creates (or on a re-run updates) the admin: an active user with a verified
  email (`email_verification_method: instance_setup`) and `users.is_admin=1`.
  Admin status also needs two-factor sign-in, so the first login asks the
  admin to set it up (`_account_admin_totp_enrollment`). The authenticator
  entry carries the instance name (`_totp_otpauth_uri(..., issuer)`).
- creates (or updates) each of the other `owners` as an active user with a
  verified email, not an admin.
- for an organization owner, creates the org with the admin and the other
  owners as `owner`.
- registers the git node account `forkmesh` (kind `node`) with the daemon's
  public key, and links `<owner>/forkmesh` to that node's `forkmesh`
  repository in `org_repos`. This is the same alias that serves
  `forkmesh/forkmesh` on forkmesh.com; for a user owner the alias is keyed by
  the user's name.

`INSTANCE_FLAGSHIP_NODE` (set to `forkmesh`) tells the mirror health check
which node's `forkmesh` repository is this instance's trusted source
(`_instance_flagship_node`, used by `_https_mirror_expected_forkmesh_refs` and
`_https_mirror_accepted_forkmesh_refs`). `HOSTED_GIT_CORE_NODES=forkmesh`
makes the same node the core for repositories users create later.

Three things make that one-node layout serve Git (all found on the test VM):

- **Its own proof is enough.** Routing for the flagship accepts a refs digest
  once two healthy mirrors sign the same one (`_https_mirror_flagship_quorum`).
  An instance has one. When `INSTANCE_FLAGSHIP_NODE` is set (only instances
  set it; forkmesh.com does not), that node's own fresh signed digest counts
  (`_instance_solo_flagship_node`). Without it every clone answered
  `503 mirror_unavailable` (`refs_mismatch`: the catalog's `stateHash` and the
  gateway's refs digest are different digests).
- **No redirect to itself.** A mirror with a `mirrorUrl` normally gets Git
  clients by `307` to its direct origin. On an instance that origin is the
  app's own, so the client looped. `_mirror_origin_is_this_app` skips the
  redirect when the mirror's host is the request's host or `APP_ORIGIN`, and
  the app proxies instead.
- **The owner's name on the gateway.** The app challenges the gateway for
  `<owner>/forkmesh`, the name in the address bar, and nothing teaches the
  gateway organization aliases. The installer adds a `<owner>/forkmesh`
  entry on the same `gitDir` as `forkmesh/forkmesh` in the gateway config;
  the daemon keeps it across restarts.
- **A user owner's page.** The repository page learns an alias from
  `GET /api/orgs/<owner>/repos`, which answered 404 for anything but an
  organization, so `<user>/forkmesh` cloned but its page said "Repository not
  found". `org_repos_handler` now also answers that GET for a user account
  that has alias rows (only `instance-bootstrap` writes those). A user without
  alias rows still gets 404, and linking or unlinking still needs an
  organization.

## White label

- `INSTANCE_NAME` names the instance. When it is set:
  - `instance_name(env)` replaces "ForkMesh" as the default email sender name;
    `_instance_brand_email` renames the email card's wordmark and button,
    points its links and logo at the instance, and adds "Powered by
    ForkMesh".
  - `app/tools/apply_instance_brand.py` (run by the installer after every
    build) renames page titles and `application-name` meta tags in `app/dist`
    and adds `/instance-brand.js` to every page.
  - `app/public/instance-brand.js` replaces "ForkMesh" in visible text and
    in `title`/`aria-label`/`placeholder`/`alt`, also in content added later
    (a `MutationObserver`). It renames the product, never people's content:
    not code blocks or inputs, not "ForkMesh desktop" (the app people
    download), not READMEs, markdown, commit messages, descriptions or
    anything inside the repository view (`[data-repo-detail]`), and on
    repository, profile, chat, notes and docs pages only the header,
    navigation, sidebars, footers and dialogs. The test VM showed why: the
    ForkMesh source's own README and commit messages were being renamed. It shows the name beside the header logo,
    adds the fixed "Powered by ForkMesh" badge at the bottom left, hides links
    to switched-off plugins and to forkmesh.com's own pages, and hides
    elements of switched-off plugins (`data-instance-plugin`), the Genie
    spider included (see "Blog card, Genie spider and status page").
- `GET /api/instance` answers `{name, branded, poweredBy, plugins, offsiteHosts}`.

### Logo (2026-10-08)

The start page takes an optional logo, and the instance shows it wherever it
showed the ForkMesh logo.

- **Form and API.** `logo` is a `data:image/<png|jpeg|webp|svg+xml>;base64,…`
  URL. `instance_launch.parse_logo` decodes it, finds the type from the bytes
  (not the declared type, so a misnamed file still works), caps it at 200 KB
  (`MAX_LOGO_BYTES`) and refuses an SVG with scripts, event handlers,
  `foreignObject`, frames, `javascript:` or entities (`LOGO_UNSAFE_RE`). The
  launch body may now be 512 KB (`instance_launch.MAX_BODY_BYTES`; the other
  runner endpoints keep 64 KB). The logo goes into the installer settings
  as `logo`.
- **Installer.** `parse_config` checks it the same way and yields
  `CFG_logoType` (`png`, `jpg`, `webp` or `svg`) and `CFG_logoData` (left out
  of the parse-only output). The `build` step saves it as
  `/etc/forkmesh-instance/logo.<type>` (`save_logo`), adds its digest to the
  build stamp so a new logo rebuilds, and passes `--logo` to
  `apply_instance_brand.py`. `app.env` gets `INSTANCE_LOGO=/instance-logo.<type>`.
- **Brand tool.** `--logo FILE` copies the logo to `dist/instance-logo.<type>`
  and writes it over `dist/assets/logo.svg` and `dist/favicon/favicon.svg`
  (an SVG as it is; a raster image wrapped in an SVG `<image>` with a `data:`
  URL). Every page, the server-rendered shells and the scripts load the mark
  from `/assets/logo.svg`, so one file replaces it everywhere without
  rewriting markup. Each page's icon links are replaced by an icon and an
  `apple-touch-icon` link to `/instance-logo.<type>`, and
  `favicon/site.webmanifest` gets the instance name and the logo as its icon.
- **Email.** `_instance_brand_email` points the card's logo at
  `<site>/instance-logo.<type>` when `INSTANCE_LOGO` is set
  (`instance_logo_path`), otherwise at the ForkMesh `assets/logo-96.png` as
  before. Gmail does not show SVG images, so a PNG or JPEG logo shows in more
  inboxes.
- A launch without a logo, and `--update`, keep the saved one: the page
  cannot refill a logo after a reload (it is not kept anywhere), so starting
  again must not drop it. A new logo replaces it. To go back to the ForkMesh
  logo, delete `/etc/forkmesh-instance/logo.*` on the server and run
  `install-instance.sh --update`.
- The logo is not a secret and is not stored in `instance_launches`.

## Theme (`app/src/instance_theme.py`, 2026-10-13)

An instance can carry its own colours. The layout, the sidebar and the header
stay the same on every instance; a theme only changes colour tokens.

- **Where it lives.** One row in the `instance_settings` table (migration
  0253): `k = 'theme'`, `doc` = a JSON document. The founders' decision was
  that instance configuration belongs in the database, not in env variables
  or files on the server, so a change needs no rebuild and no SSH. The table
  is keyed by `k` so other instance settings (the plugin list is the next
  candidate) can join it. Colours are sent to every browser, so the row is
  plain, not sealed in `host_secrets`.
- **The document.** `{"dark": {...}, "light": {...}}`. Each mode maps a token
  name to a `#rrggbb` colour. The names are the dashboard's own tokens
  (`RGB_TOKENS` become `--dashboard-<name>-rgb`, `HEX_TOKENS` become
  `--dashboard-<name>`), and seven of them also set the older aliases
  (`--background`, `--primary`, …) that pages outside the dashboard read.
  A mode or token that is left out keeps the ForkMesh value.
  `test_instance_theme.py` checks every token name against
  `dashboard/shell.html`, so a renamed token fails the test.
- **Validation.** `instance_theme.clean` accepts known modes, known tokens and
  six-digit hex colours only, and the document is capped at 8 KB. No raw CSS
  is ever stored, so a theme cannot inject rules or load a URL. A stored row
  that no longer validates reads as no theme.
- **Presets.** `PRESETS` holds named themes; `clean("crimson")` returns the
  Crimson one (zinc neutrals, red primary). Every preset is offered on the
  `/start` form (the **Colour theme** choice in section 1), so anyone setting
  up an instance can pick it. A preset is copied into the row when it is
  applied, so later edits to the row do not depend on it.
- **How it reaches the page.** `apply_instance_brand.py` adds
  `<link rel="stylesheet" href="/instance-theme.css">` at the end of every
  page's `<head>`, after the page's own token block, so it wins at equal
  specificity and blocks rendering (no flash of ForkMesh green).
  `GET /instance-theme.css` renders the row as two rules, one for
  `:root, [data-dashboard-theme="dark"]` and one for
  `[data-dashboard-theme="light"]`, cached for 60 seconds. Without a row it
  answers an empty stylesheet. `GET /api/instance` also returns `theme`.
- **Setting it.** At setup, the installer setting `theme` (a preset name)
  goes to `instance-bootstrap`, which writes the row (`ensure_theme`); a run
  without `theme` leaves the row alone. Later, an administrator sends
  `PUT /api/instance/theme` with `{"theme": "<preset>" | {document}}`; a
  cookie session must come from an allowed origin. There is no Settings page
  for it yet.
- **forkmesh.com is untouched.** Both routes and the reader return nothing
  unless `INSTANCE_NAME` is set, and unbranded pages never link the
  stylesheet.
- **Known gap.** A theme only reaches markup that uses the tokens. About
  3,100 hardcoded hex colours remain in `app/public` (the login and sign-up
  pages, some header buttons, the contribution graph), and those keep their
  ForkMesh colours until they are moved to tokens.

## Plugins (`app/src/instance_features.py`)

ForkMesh-company features are named plugins, each with its routes. On
forkmesh.com (no `INSTANCE_NAME`) all are on. On a white-label instance all
are **off** unless listed in `INSTANCE_PLUGINS` (comma list, or `all`). A
switched-off plugin's routes answer 404 (`plugin_disabled` for `/api/…`, a
short "Not available here" page otherwise) before any other route runs
(`Default._route`).

| plugin | routes |
|---|---|
| `world` | `/world`, `/api/world`, `/office`, `/api/office` |
| `hackathon` | `/hackathon`, `/api/hackathon`, `/admin/hackathon`, `/api/admin/hackathon` |
| `challenges` | `/api/challenges`, `/dashboard/challenges` |
| `pitch-deck` | `/pitch-deck`, `/api/pitch-deck` |
| `ads` | `/api/ads`, `/dashboard/ads`, `/api/world/community-ads` |
| `patreon` | `/api/integrations/patreon` |
| `discord` | `/api/integrations/discord`, `/api/orgs/<org>/discord` |
| `microjobs` | `/microjobs`, `/api/microjobs` |
| `rewards` | `/ledger`, `/api/ledger`, `/api/rewards`, `/api/relay-mesh`, `/api/leaderboards/wallets`, `/api/world/wallet`, `/api/world/store`, `/mirror-payouts` |
| `referrals` | `/referral`, `/referrals`, `/api/referrals`, `/dashboard/links`, `/dashboard/bounties` |
| `forkbot` | `/api/forkbot`, `/api/genie` |
| `guestbook` | `/api/chat/agent-guestbook` |
| `ecosystem` | `/api/ecosystem` |
| `blog` | `/api/blog`, `/rss.xml`, `/feed.xml`, `/blog/rss.xml`, `/blog/feed.xml`; the dashboard's "Latest from the blog" card |
| `genie` | none; the Genie spider on every page and its Settings switch |
| `status` | `/status`, `/api/status` (with `/api/status/world` and `/api/status/ws`) |
| `developer-docs` | `/dashboard/docs`, `/api/docs-book` |
| `ios-testers` | `/api/ios-udid` |
| `website` | `/about`, `/careers`, `/press`, `/pricing`, `/features`, `/ecosystem`, `/leaderboards`, `/blog`, `/changelog`, `/market` |

The ForkMesh-only marketplace packages (`marketing`, `crm`, `competitor`,
`ledger`, `x-banner-board`, `reddit-banner-board`, `forkmesh-sky-office`,
`discord`, `sentry`) are left out of an instance's build:
`app/tools/build_app_plugins.py` skips any package named in
`FORKMESH_EXCLUDED_PLUGINS`.

To turn a plugin on, add it to `INSTANCE_PLUGINS` in
`/etc/forkmesh-instance/app.env` and `systemctl restart forkmesh-instance`.

### Blog card, Genie spider and status page (2026-10-10)

The user asked for "Latest from the blog", the Genie spider and the status
page to be plugins that a new instance starts without. All three are now in
`FEATURES`, so they are off on an instance until `INSTANCE_PLUGINS` names
them, like every other plugin.

- **The gate runs first.** `fetch()` checks `instance_features.blocked` right
  after the maintenance gate, before the status snapshot fast path
  (`_stateless_status_snapshot_response`) and the public snapshot path, which
  answered `/api/status` before `_route` ever ran. It does nothing unless
  `INSTANCE_NAME` is set. `_route` keeps its own check for the other entry
  points.
- **Plugin UI in the page.** An element that belongs to a plugin carries
  `data-instance-plugin="<id>"`: the home rail's blog card (`home.html`) and
  the "Show the Genie spider" row in Settings (`settings.html`).
  `instance-brand.js` hides every such element as soon as it runs
  (`hidePlugins(null)`), then, once `/api/instance` answers, keeps only the
  switched-off ones hidden with one CSS rule per plugin, so content drawn
  later is covered too. If `/api/instance` fails they stay hidden.
  `/api/blog`, `/rss.xml` and the other feed paths answer 404 as well, so the
  card's feed read finds nothing even without the script.
- **The spider.** `genie-spider.js` looks for the brand script tag
  (`script[data-instance-brand]`). On an instance it asks `/api/instance`
  first and builds only when the `genie` plugin is on (`instanceAllows`); on
  forkmesh.com it builds as before. The brand script also hides `.fm-genie`
  while `genie` is off. The spider no longer follows the `forkbot` plugin.
- **Links.** Sidebar and profile links to `/status` disappear with the
  plugin, through the existing path hiding in `instance-brand.js`.
- An instance set up before this change loses its status page on its next
  `install-instance.sh --update` unless `status` is added to
  `INSTANCE_PLUGINS`.

### Developer docs (2026-10-12)

The user asked for new instances to start without the dashboard's Developer
docs page ([app/docs-book.md](app/docs-book.md)): it shows and edits the
`docs/` folder of `forkmesh/forkmesh`, which is ForkMesh's own engineering
notes, not the instance's. It is now the `developer-docs` plugin, off on an
instance until `INSTANCE_PLUGINS` names it. `/dashboard/docs` and
`/api/docs-book` answer 404, and `instance-brand.js` hides the sidebar's
"Developer docs" link through the plugin's paths. The sidebar's "Docs" link
(`/docs`, the instance's own guide below) stays. An instance set up before
this change loses the page on its next `install-instance.sh --update` unless
`developer-docs` is added to `INSTANCE_PLUGINS`.

## Docs and desktop pages (`app/src/instance_pages.py`, 2026-10-10)

On an instance, `/desktop` and `/docs` (the dashboard sidebar links) used to
fall through to the website routes. `WWW_ORIGIN` is the instance itself, so
the app looked for a local copy of forkmesh.com's page, found none and
answered 404. A branded app now renders its own pages in `_route`, right after
`/api/instance` (`_instance_page`):

- `/desktop`, `/download`, `/downloads`: how to get the ForkMesh desktop app
  (the `install.sh` command and the installers on forkmesh.com/desktop), then
  how to add the instance with **Add relay…** in the desktop's instance
  picker and sign in.
- `/docs`: a generic guide to the instance's Git hosting: accounts, creating
  and importing repositories, cloning, pushing with the push password, the
  gateway's push rules, branches, forks, pull requests, forks on other
  instances ([app/remote-forks.md](app/remote-forks.md)), issues and the
  desktop app. It names the instance and its origin (`APP_ORIGIN`) and says
  nothing about ForkMesh the company. Any `/docs/<page>` 308s to `/docs`.
- `/@desktop`: no account is named `desktop`, so the profile route 302s to
  `/desktop` instead of the 404 page (`_serve_profile_page`). This applies on
  forkmesh.com as well, where it lands on the website's download page.

These pages are plain server-rendered HTML with the instance logo
(`/assets/logo.svg`) and a "Powered by ForkMesh" footer; they do not load the
brand script, so their "ForkMesh desktop" wording and the forkmesh.com
download links stay as written.

## Email (`app/src/mail_providers.py`)

`_send_email` now picks its transport from `mail_providers.provider(env)`:

1. `MAIL_PROVIDER` = `mailtrap`, `sendgrid` or `mailgun`, when that
   provider's key is set (`MAILTRAP_API_TOKEN`, `SENDGRID_API_KEY`,
   `MAILGUN_API_KEY` plus `MAILGUN_DOMAIN`); `none` turns mail off.
2. Otherwise the old order: `MAIL_TRANSPORT=smtp` (Postfix on the cores),
   then Mailtrap when its token is set.

| provider | request |
|---|---|
| Mailtrap | unchanged: `POST https://send.api.mailtrap.io/api/send`, Bearer token, `category`, `custom_variables` |
| SendGrid | `POST https://api.sendgrid.com/v3/mail/send`, Bearer key, `personalizations[0].custom_args`, `categories`; the id comes from `X-Message-Id` |
| Mailgun | `POST https://api.mailgun.net/v3/<domain>/messages` (`api.eu.mailgun.net` with `MAILGUN_REGION=eu`), Basic `api:<key>`, form fields, `o:tag`, `v:<variable>`; the id comes from the JSON `id` |

`MAIL_SENDER` and `MAIL_SENDER_NAME` are read before the older
`MAILTRAP_SENDER*` names. The daily budget, suppression list, tracking and
mail log apply to every provider. Delivery webhooks are still Mailtrap's only
(`/api/integrations/mailtrap/webhook`).

## Known limits

- **Sign-ups by other people** stay closed until the admin adds
  `TURNSTILE_SITE_KEY` and `TURNSTILE_SECRET_KEY` to `app.env`: on a VPS,
  Turnstile fails closed without a secret (`turnstile.py`). The admin account
  works from the start.
- **Trust on first use.** Without a fingerprint in the form, the first host
  key the server shows is trusted. It is shown on the page and in the log.
- **The SSH password or key transits ForkMesh's servers** once, in the launch
  request, and lives in the helper's memory for the run (as for runner SSH
  setup).
- **Public git reads of `<owner>/forkmesh`** need the git node's endpoint to
  pass the app's mirror registration and health check, which requires the
  domain to resolve publicly to public addresses
  (`_https_mirror_public_dns_ok`, `public_https_dns_required`). The app reaches
  its own git gateway through its own public name (`https://<domain>/v1/…`),
  which the egress guard allows because that name resolves to the server's
  public IP.
- **The instance's app may not fetch `forkmesh.com`** (the egress guard's
  production rule, `app/host/forkmesh_host/egress.py`). Nothing in the
  instance needs it: the git node, not the app, copies the ForkMesh source.
- **A core restart during a launch** (a deploy) ends the page's view of that
  run: the launch task lives in the app process that started it, and the SSH
  session closes with it. The setup itself keeps going on the server; the
  page shows `LOST_MESSAGE` within 2 minutes, and starting again with the
  same details (SSH secret and admin password typed again) follows it. The
  launch cannot reconnect on its own, because the SSH secret is never stored.
- **The brand script** renames text in the browser; strings the server puts in
  JSON or plain-text answers (for example push error messages) still say
  ForkMesh.

## Verified on a Debian 13 VM (2026-10-07)

`install-instance.sh` ran end to end in a local KVM Debian 13 cloud image
(2 GB, 4 cores), driven by the real `ssh_helper.install` with password SSH
and the settings on standard input. Outbound traffic went through a proxy
(`/etc/environment`, apt and systemd `DefaultEnvironment`), and Caddy used
`FORKMESH_INSTANCE_TLS=internal`. Timings: packages 66 s, build about 50 s,
the app up at 67 s, HTTPS at 72 s, the signed git node about 2 min. Checked:
the admin is active, email-verified and asked for two-factor setup at login;
the org and the `forkmesh` node account exist; `/api/instance` reports the
name and plugins; switched-off plugins answer 404; the git node copies the
ForkMesh source and its gateway serves it; `/acme/forkmesh` renders as "Acme
Code" with "Powered by ForkMesh". Not reproducible there: Let's Encrypt and
public DNS (the VM has no public address), so endpoint registration was tested
with an sslip.io name mapped to the VM and a stand-in DNS-over-HTTPS answer.

## Tests

- `app/tests/test_instance_launch.py`: validation per field, sslip names,
  host-key pinning and fingerprint checks, private addresses, rate and
  concurrency limits, no secret in rows/audits/answers, progress parsing,
  failure states, redirect safety, stale runs, the log written while the
  server is quiet, a dropped connection, the fields the retry form refills,
  the logo's type, size and SVG checks and its way to the installer.
- `app/host/tests_unit/test_ssh_helper.py`: the `instance` operation's
  validation (a settings object with a 200 KB logo fits), command, and
  settings on stdin after the sudo password; output split on `\r` and long
  lines.
- `app/host/tests_unit/test_instance_bootstrap.py`: admin, org, other
  owners, node account and alias, re-runs, conflicts.
- `app/tests/test_instance_white_label.py`: mail provider choice and request
  shapes, plugin gating on and off, the router gate, the brand tool and script,
  the brand tool's logo (raster and SVG, icon links, manifest, refusals) and
  the email logo; the blog, genie, status and developer-docs plugins, the gate ahead of the
  status fast path, the plugin-tagged dashboard elements and the spider's
  instance check.
- `app/tests/test_instance_pages.py`: the desktop and docs pages, escaping,
  the routing ahead of the website routes and the `/@desktop` redirect.
- `www/tests/genie_spider.test.mjs`: the spider builds on an instance only
  when `/api/instance` reports `genie` on.
- `app/tests/test_install_instance_script.py`: the installer's settings
  parser, step markers and secret handling; the apt/pip/build output filters
  and the detached start and follow (run in bash with stand-in `systemd-run`
  and `systemctl`); the DNS check against stand-in `curl`, `getent` and `ip`
  (resolvers, NAT, loopback, wrong address); the skip checks; the HTTPS checks;
  the logo's parsing, saving and build use.
- `app/tests/test_start_instance_page.py`: the page's fields (the logo and
  other owners too), the settings zip and its example, API calls, and the
  homepage and World links.
- `app/tests/test_instance_directory.py`: the two choices and their default,
  publishing only after success, slots kept and freed, the 12-slot limit,
  turning both off, the logo route and the migration; the health check,
  hiding after two misses and coming back, one ping a day into an outage,
  hide/show/delete, relaunching, and the admin page's rendering, CSRF,
  confirmation and audit.
- `app/tests/test_instance_git_routing.py`: the one-node flagship proof, no
  redirect to the app's own origin, the installer's gateway alias and its
  re-init on a new domain.
