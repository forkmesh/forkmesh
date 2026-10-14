# Self-setup node: DNS in, node running, no desktop

A signed-in user can bring up a mirror node on their own server without the
desktop: one install command (SSH) or one cloud-init script installs the
native `forkmesh-mirror-node` service, links it to their account, waits for
their DNS record, and serves the node over HTTPS through Caddy with a Let's
Encrypt certificate. Whole white-label instances use a different path
([../INSTANCES.md](../INSTANCES.md)).

## Flow

1. **Command.** `POST /api/accounts/link-install` returns a fresh six-digit
   installer link code parked for the signed-in account. The command is:
   `curl -fsSL https://forkmesh.com/install.sh | FORKMESH_PUBLIC_HOSTNAME=<host> [FORKMESH_NODE_NAME=<node>] FORKMESH_LINK_CODE=<code> FORKMESH_OWNER=<user> bash`
   (as cloud-init user data, prefixed with `#!/bin/bash` and
   `export HOME=/root`). A relay on another origin adds `FORKMESH_HOST=<origin>`.
2. **Link code** (`_account_link_install()` in `app/src/entry.py`). The session
   proves the user half. The server mints the code itself and never reuses a
   parked code, so a browser cannot adopt someone else's node. One code per
   account every 10 s (`LINK_INSTALL_COOLDOWN_MS`); codes live 30 minutes
   (`LINK_CODE_TTL_MS`) and are redeemed through the normal finalize path.
3. **Installer** (`www/public/install.sh`). With `FORKMESH_PUBLIC_HOSTNAME`
   set, `provision_direct_https()`:
   - initialises the mirror config, reusing an existing identity;
   - writes the managed Caddyfile (`direct_https_bootstrap.py`), using the DNS
     provider API when a token is set and manual DNS otherwise;
   - registers the link code, then starts the service;
   - with manual DNS, prints the exact record (`<host> A <ip>`) and polls
     public DNS every 15 s for up to `FORKMESH_DNS_WAIT_SECONDS` (default
     3600);
   - reloads Caddy only once DNS resolves, so the first ACME attempt succeeds
     and does not spend the failed-validation allowance;
   - waits for `https://<host>/health` and the public manifest.
   If DNS never resolves, Caddy stays enabled and obtains the certificate
   later; `FORKMESH_REQUIRE_DIRECT_HTTPS=1` makes that a failure instead.

## Tests

- `app/tests/test_users_nodes_claim_link.py`: minting, session requirement,
  throttling, no reuse of parked codes.
- `app/tests/test_installer_headless_checkout.py`: manual-DNS order (register,
  start, wait for DNS, Caddy) and the record-type helper.
- `world/tests/test_world_instance_launcher.py`.
