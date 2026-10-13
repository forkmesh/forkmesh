# Self-setup node: DNS in, node running, no desktop (2026-09-29)

Since 2026-10-07 the World's plot signs no longer show this panel: they open
the "Start your own instance" page, which sets up a whole instance from
ForkMesh's servers ([INSTANCES.md](../INSTANCES.md)). The link-code endpoint
and the installer's `FORKMESH_PUBLIC_HOSTNAME` path below are unchanged and
still work for a mirror node set up by hand.

A user can bring up a full ForkMesh node from the World's "Start your world
here" panel without installing ForkMesh Desktop. They pick **Vultr server** or
**SSH server**, type the DNS name they will point at the machine, and get one
command (SSH) or one Cloud-Init script (Vultr). Running it installs the native
`forkmesh-mirror-node` service, links it to their account, waits for their DNS
record, and serves the node over HTTPS through Caddy with a Let's Encrypt
certificate.

## Flow

1. **World panel** (`world/public/world/world.js`, `continueStartWorldServer()`).
   The visitor must be signed in. The panel posts to
   `POST /api/accounts/link-install`, which returns a fresh six-digit installer
   link code already parked for their account, and builds:
   - SSH: `curl -fsSL https://forkmesh.com/install.sh | FORKMESH_PUBLIC_HOSTNAME=<host> [FORKMESH_NODE_NAME=<node>] FORKMESH_LINK_CODE=<code> FORKMESH_OWNER=<user> bash`
   - Vultr: the same line under `#!/bin/bash` and `export HOME=/root`, pasted
     into Vultr → Deploy → Additional Features → Cloud-Init User-Data.
     `install.sh` runs under `set -u` and reads `$HOME`, which cloud-init does
     not always set.
   A World served from another origin adds `FORKMESH_HOST=<origin>` so the node
   registers with the relay that minted the code.
   **Use ForkMesh Desktop instead** keeps the old handoffs: `forkmesh://…?mode=ssh`
   for SSH and `launchVultrMirrorFromWorld()` for Vultr.
2. **Link code** (`app/src/entry.py`, `_account_link_install()`). The session
   is the proof of the user half, the same way the desktop's signed
   `link-node` offer is. The server mints the code itself and skips any code
   that already has a row, so a browser can never adopt a node that someone
   else's install parked. One code per account every 10 s
   (`LINK_INSTALL_COOLDOWN_MS`, a throttle row keyed `link-install:<user>` in
   `link_codes`). The code lives `LINK_CODE_TTL_MS` (30 min); the node
   redeems it through the usual finalize path.
3. **Installer** (`www/public/install.sh`). On a display-less root install
   `prepare_root_mirror_service()` stages the systemd unit, then
   `provision_direct_https()` runs whenever `FORKMESH_PUBLIC_HOSTNAME` is set:
   - initialize the mirror config (`--disable-cloudflared`), identity reused;
   - write the managed Caddyfile with `direct_https_bootstrap.py`, using
     Cloudflare DNS when `CLOUDFLARE_API_TOKEN` is set and `--skip-dns`
     otherwise;
   - register the link code, then start `forkmesh-mirror-node.service`;
   - without a Cloudflare token, `_wait_for_public_dns()` prints the exact
     record (`<host> A <ip>`) and polls public DNS (Cloudflare DNS-over-HTTPS,
     then `getent`) every 15 s for up to `FORKMESH_DNS_WAIT_SECONDS`
     (default 3600);
   - only then reload Caddy, so its first ACME attempt already sees the record
     and does not use up Let's Encrypt's failed-validation allowance;
   - wait for `https://<host>/health` and the public-origin manifest.
   If DNS never resolves, Caddy is still enabled and the install reports the
   address as "live once DNS points here"; Caddy gets the certificate on its
   own later. `FORKMESH_REQUIRE_DIRECT_HTTPS=1` turns that case into a failure.

## Fixed along the way

`provision_direct_https()` could not have run on a fresh machine before this
change: it required `LAUNCH_MODE=service`, but a fresh service is
`service-staged` until the endpoint exists, and it read the undefined
`$SYSTEMD_SERVICE_USER` and `$FORKMESH_VERSION` under `set -u`. It also never
registered the link code, so a node installed that way was never attached to
an account. The desktop's Vultr path was unaffected because it sends its own
pipeline (`MainWindowHostInstall.cpp`).

## Tests

- `app/tests/test_users_nodes_claim_link.py`: `link-install` mints a code the
  node redeems, needs a session, is throttled, never reuses a parked code.
- `app/tests/test_installer_headless_checkout.py`: manual-DNS path order
  (register → start → wait for DNS → Caddy) and the record-type helper.
- `world/tests/test_world_instance_launcher.py`,
  `world/browser_tests/tests/world.spec.js`: the panel and its command.
