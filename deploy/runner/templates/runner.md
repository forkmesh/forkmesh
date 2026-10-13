# ForkMesh Next runner (managed by deploy/core/render.py)

This machine is an **untrusted, outbound-only** runner (docs/PLAN.md Phase 5).

- It holds no core secret: no DATA_KEY, no ADMIN_PASS, no rqlite password,
  no FORKMESH_MESH_SECRET, and it is not a WireGuard peer of the core.
- Inbound traffic: SSH only (nftables). No public hostname.
- The daemon (`forkmesh-mirror-node`, `"role": "runner"`) claims agent and
  Actions leases from https://app.next.forkmesh.com, runs one job at a time
  in its own systemd unit as the unprivileged user `forkmesh-job` (the
  daemon runs as root to start it, with runner.jobUid), pushes the result
  branch with the per-job capability from the lease, and wipes the workdir.
- The job user cannot read the node key or /etc/forkmesh (0700), and
  nftables refuses it the metadata service, private, CGNAT, loopback and
  WireGuard addresses. Provider logins live in /var/lib/forkmesh-job.
- Link it to an account with the daemon's link-code flow after install
  (docs/OPERATIONS.md).
