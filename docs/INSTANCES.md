# Self-hosted instances

Anyone can start their own ForkMesh instance, a white-label Git host on their
own server, from `forkmesh.com/start`. The app connects to the server over
SSH, runs `www/public/install-instance.sh`, streams progress to the page, and
finishes on the new `<owner>/forkmesh` repository. Every instance shows a
"Powered by ForkMesh" badge.

## Start page (`app/public/start.html`, `start.js`)

Sections: project name, optional logo and colour theme; server (address, SSH
user and port, password or private key, optional host-key fingerprint);
optional domain (without one the instance uses `<ip-with-dashes>.sslip.io`);
admin account and repository owner (organization or user, with optional
extra organization owners); optional mail provider; listing choices
(`ownWorld`, `listOnHomepage`, both on by default).

- **Set up from a file.** A JSON file whose keys are exactly the launch body,
  or a zip holding that JSON and a logo image. It is read in the browser only.
  The user guide is `www/docs/start-config.md`; examples are
  `app/public/start-config.example.json` and `.zip`.
- Secret fields are cleared after sending and on `pagehide`; nothing is kept
  in browser storage. The page polls the launch every second, keeps the
  launch id in the URL, and on failure refills non-secret fields from the
  launch record.
- The page follows the landing style, not the dashboard's: its checkboxes use
  `--ink-strong` (white in dark mode, near-black in light), like its white
  start button. Until October 2026 they used `--accent-bright` and showed
  green, which the landing style keeps for status only.

## API (`app/src/instance_launch.py`)

| Endpoint | Purpose |
|---|---|
| `POST /api/start/launch` | validate, resolve, read the host key, start the run; `202 {id, domain, address, hostKey, steps, url}` |
| `GET /api/start/launches/<id>` | state, steps, message, log, URL; the 128-bit id is the capability |
| `GET /api/start/instances` | public list of listed instances and World slots |
| `GET /api/start/instances/<id>/logo` | an instance's logo, served with a sandboxing CSP and `nosniff` |

- No account needed. Rate limits per client (stored only as a salted hash)
  and a global concurrency cap.
- The host must resolve to public addresses only.
- `instance_launches` stores no password, key or API key; rows expire after
  seven days. A run that stops reporting for 2 minutes is marked lost; the
  setup continues on the server and a new launch with the same details
  follows it.
- The installer prints `FMI {"step":…,"state":…}` lines; success needs exit 0
  and a final `FMI {"done":true,"url":…}` on the instance's own domain.
  Secrets are scrubbed from the log.

## SSH (`app/host/forkmesh_host/ssh_helper.py`)

The sandboxed, socket-activated `fmnext-ssh-helper` (shared with runner SSH
setup, [RUNNERS.md](RUNNERS.md)) has an `instance` operation. It pins the
host key, downloads the installer and runs `bash "$t" --config-stdin` (via
`sudo -n` for a non-root user). Settings, including passwords and mail keys,
travel as one base64 JSON line on standard input, never on the command line.
Runs may take up to 50 minutes.

App settings: `INSTANCE_INSTALL_URL`, `INSTANCE_SOURCE_URL`,
`INSTANCE_SOURCE_REF`.

## Installer (`www/public/install-instance.sh`)

| Step | What happens |
|---|---|
| `system` | supported Debian/Ubuntu, x86_64 or arm64, at least 1 GB RAM and 6 GB disk, ports 80/443 free |
| `packages` | swap on small machines; git, Python, Node, Caddy, sqlite3, age; service users |
| `source` | shallow clone into `/opt/forkmesh-instance/src` |
| `build` | venv and `python -m forkmesh_host build` without ForkMesh-only packages (`FORKMESH_EXCLUDED_PLUGINS`), then `app/tools/apply_instance_brand.py` |
| `configure` | `/etc/forkmesh-instance/app.env` with fresh `DATA_KEY`, `ADMIN_PASS` and router keys (kept on re-runs) and `instance.json` without secrets |
| `database` | migrations into a single-node SQLite database |
| `services` | `forkmesh-instance.service` (uvicorn on loopback, sandboxed) |
| `https` | Caddyfile for the app and the Git gateway paths, HSTS; waits for DNS |
| `admin` | installs the signed Git node daemon and runs `instance-bootstrap` |
| `repo` | waits for `<owner>/forkmesh` to answer `info/refs` |
| `done` | prints the final URL |

- The setup runs as a detached systemd service; the SSH session only follows
  its log, so a dropped connection does not stop it. A lock prevents two
  concurrent runs. Each step skips work already done, so a re-run resumes.
- DNS counts as ready when public resolvers or `getent` return the form's
  address, an address on a global interface, or the server's outbound
  address. A Cloudflare-proxied name is detected; the SSL/TLS mode must be
  Full (strict). Caddy trusts `Cf-Connecting-Ip` only from Cloudflare's
  published ranges.
- `--update` pulls, rebuilds, migrates and restarts, keeping secrets, data
  and admin; local source changes are saved to a patch file first.
  `--config FILE` takes the same JSON as the page for a manual install.

## Bootstrap (`instance_bootstrap.py`)

`python -m forkmesh_host instance-bootstrap` reads JSON on stdin and creates
or updates the admin (verified, admin, two-factor required at first login),
other owners, the organization, the `forkmesh` node account and the
`<owner>/forkmesh` alias. `INSTANCE_FLAGSHIP_NODE=forkmesh` lets the single
node's own signed refs digest count as the flagship proof;
`HOSTED_GIT_CORE_NODES=forkmesh` makes it the core for new repositories. The
app does not redirect Git clients to its own origin.

## White label

- `INSTANCE_NAME` names the instance: email sender and card, page titles,
  and `app/public/instance-brand.js`, which renames the product in visible UI
  text (never user content) and hides switched-off plugin UI.
- `GET /api/instance` returns name, branding, plugins, offsite hosts and
  theme.
- **Logo:** a `data:` URL (PNG, JPEG, WebP or SVG, 200 KB max, type detected
  from bytes, unsafe SVG refused). The brand tool writes it over the logo and
  favicon assets and the web manifest; `INSTANCE_LOGO` points email at it.
- **Theme** (`app/src/instance_theme.py`): one `instance_settings` row
  (`k = 'theme'`) holding `{"dark": {...}, "light": {...}}` of known token
  names to `#rrggbb` colours, max 8 KB, no raw CSS. Served as
  `/instance-theme.css`; set at setup from a preset name or a full colour
  object (the start page's picker), or later with `PUT /api/instance/theme`
  by an admin. The installer passes an object to `instance-bootstrap` as
  JSON and refuses one with other modes, odd token names or non-hex values.
- **Plugins** (`app/src/instance_features.py`): ForkMesh-company features
  (world, hackathon, ads, rewards, blog, status, developer docs, website
  pages and others) are off on an instance unless listed in
  `INSTANCE_PLUGINS`. A disabled plugin's routes answer 404 before any other
  route; tagged UI (`data-instance-plugin`) is hidden.
- **Pages** (`app/src/instance_pages.py`): an instance renders its own
  `/desktop` and `/docs` guide pages.

## Directory

After a successful launch, `instance_directory.publish` writes an
`instance_directory` row keyed by domain (name, URL, logo, choices, World
slot). The World has 12 instance slots. Cron phase `instance-health` probes
`https://<domain>/readyz`; two misses in a row hide the entry, a day offline
pings the admins once per outage. The admin console's Instances view can
hide, show or delete entries (CSRF-protected and audited); deleting does not
touch the instance.

## Mail (`app/src/mail_providers.py`)

`MAIL_PROVIDER` selects `mailtrap`, `sendgrid`, `mailgun` or `none`;
otherwise SMTP or Mailtrap by configuration. `MAIL_SENDER` and
`MAIL_SENDER_NAME` set the sender. Budgets, suppression and the mail log apply
to every provider.

## Limits

- Public sign-up needs Turnstile keys in `app.env`; it fails closed without
  them.
- Without a fingerprint the first host key is trusted (and shown).
- SSH credentials pass through the app once, in memory only.
- The instance app's egress guard refuses forkmesh.com; nothing needs it.
- Brand renaming happens in the browser; plain-text server messages still
  say ForkMesh.

## Tests

`app/tests/test_instance_launch.py`, `test_install_instance_script.py`,
`test_start_instance_page.py`, `test_instance_white_label.py`,
`test_instance_theme.py`, `test_instance_pages.py`,
`test_instance_directory.py`, `test_instance_git_routing.py`, and
`app/host/tests_unit/test_ssh_helper.py`, `test_instance_bootstrap.py`.
