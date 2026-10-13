# Mirror fleet operations

Running the managed mirror fleet (Vultr hosts named `mirrorN`) and repairing
it by hand. Host inventory changes often; ask the desktop's Hosts page, not
this page, which hosts exist. No IP addresses or keys belong here.

## Files on a mirror

| Path | What |
| --- | --- |
| `/usr/local/bin/forkmesh-mirror-node` | The daemon (no self-update). |
| `/usr/local/share/forkmesh/tools/mirror_gateway.py` | The HTTPS gateway it supervises. |
| `/etc/forkmesh/mirror-node.json` | Daemon config (strict). |
| `/etc/forkmesh/node.env` | Optional environment file. |
| `/etc/forkmesh/log-viewer-keys` | Ed25519 keys allowed to follow `/v1/node/logs`. |
| `/var/lib/forkmesh/` | State: `HOME` of `forkmesh-node`. |
| `/var/lib/forkmesh/repositories/<owner>-<repo>.git` | Served bare repositories. |
| `/var/lib/forkmesh/.local/share/ForkMesh/ForkMesh/mirror-gateway/` | Gateway config, `pull-jobs/`, `agent-jobs/`, `actions-control/`, node-ops records. |
| `/var/lib/forkmesh/.local/share/ForkMesh/ForkMesh/identity/ed25519.pem` | Node identity key. Never copy it off the host except with `--export-identity`. |
| `/usr/local/bin/claude`, `/usr/local/bin/codex` | Agent CLIs, root-owned. |
| `/etc/ssh/sshd_config.d/10-forkmesh-hardening.conf` | Key-only sshd. |
| `/etc/fail2ban/jail.d/forkmesh-sshd.local` | sshd jail (copy of `server/packaging/fail2ban/forkmesh-sshd.local`). |

Run Git on a mirror as the service account:
`runuser -u forkmesh-node -- git --git-dir=/var/lib/forkmesh/repositories/<owner>-<repo>.git …`.
A plain root Git write leaves root-owned objects and later "dubious ownership"
errors.

## Install and update

- **Install:** desktop Hosts page (Vultr), `desktop/src/MainWindowHostInstall.cpp`.
  It uploads the desktop's own `forkmesh-mirror-node` and gateway, writes the
  config, the sshd drop-in, the fail2ban jail, the log-viewer key, opens
  80/tcp, 443/tcp and 443/udp when ufw is active, and installs the agent CLIs.
- **Update:** Hosts → Update (per host), Install (binary), or the fleet run.
  Each ends with the agent-CLI refresh described in
  [agent-jobs.md](agent-jobs.md#keeping-the-clis-current).
- **By hand:** upload the binary, verify its sha256, run
  `runuser -u forkmesh-node -- /path/forkmesh-mirror-node.new --config /etc/forkmesh/mirror-node.json --check`,
  keep the old binary beside it, swap, run `--set-version <v>` (the reported
  version comes from the config), restart, and wait for `/healthz`.
- **Autoscaling:** the desktop keeps a target count of catalog-healthy mirrors
  (`planMirrorFleetReconciliation` in `desktop/src/ControlNode.cpp`) and
  destroys the newest unhealthy managed mirror first.

## Hardening

- `tools/harden_mirror_sshd.sh <host-ip>...`: installs the key-only drop-in,
  inserts `Include /etc/ssh/sshd_config.d/*.conf` at line 1 of
  `sshd_config` if missing (sshd keeps the first value it reads), validates
  with `sshd -t`, reloads, and proves key login works and passwords fail.
- `tools/mirror_fail2ban.sh <host-ip>...`: installs fail2ban and the jail
  (journal backend, aggressive, 5 in 10 min → 1 h, growing to 1 week).
- Both scripts use the fleet key from `$(forkmesh_ssh_dir)/vultr_mirror_ed25519`
  (`tools/forkmesh-paths.sh`) or `FORKMESH_FLEET_SSH_KEY`.

## Diagnosis

1. `GET https://forkmesh.com/api/repo/<owner>/<repo>/mirrors` is what the
   desktop's deploy check and the autoscaler read. A mirror counts only when it
   is online with integrity ok, a last sync, clone available, and a healthy
   and fresh endpoint. `endpoint: ""` means it never registered;
   `endpointFresh: false` with a healthy endpoint means its refs digest is not
   an attested pin (it is behind or ahead of anything attested).
2. On the host: `journalctl -u forkmesh-mirror-node`,
   `curl -s 127.0.0.1:8791/v1/status`, `curl -s 127.0.0.1:8791/readyz`
   (`readyz` also needs a recent successful sync, so it stays false while the
   relay is down).
3. Host logs reach the desktop's log over HTTP/3 (`/v1/node/logs`) or SSH.

## Traps when repairing by hand

- **The "synced" time lies.** It is the catalog lease, not Git freshness.
- **Pins gate routing.** Never push a mirror to a ref set the source did not
  attest. If you do it to every mirror, every clone answers
  `503 mirror_unavailable`. Recover by pushing the source's exact current refs
  and restarting the daemons.
- **Split brain holds user data.** Mirrors that each took intake have unique
  commits. Stop the daemons, fetch each mirror's `main`, `forkmesh/pulls` and
  `refs/pr/*`, union-merge (intake touches only `.forkmesh/**`), push the
  result everywhere, start the daemons. Never force-push over a mirror's
  unique intake.
- **Two merge bases freeze convergence.** After a mirror merges its own
  divergence, the source and fleet can share two merge bases. Both the desktop
  (`desktop/src/UpstreamCheckoutSync.cpp`) and the daemon's issue
  reconciler (`server/issue_reconcile.go`) handle it; a node still running an
  older build needs a manual `git merge --no-ff <mirror main>` in the source
  checkout.
- **Feed loops.** A mirror whose upstream is `app.forkmesh.com` may be fed by
  the fleet itself; with no source publishing, nothing new enters. The deploy
  box pushes the fleet from the flagship bare mirror with a user timer
  (`forkmesh-mirror-fanout`, not in this repository): forced for `agent/*` and
  tags, fast-forward only for `main` and `forkmesh/pulls`.
- **Unborn HEAD / empty repositories** publish nothing and look like a dead
  node to the autoscaler; see [README.md](README.md#traps).
