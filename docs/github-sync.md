# Connecting a repository to GitHub

A GitHub Actions workflow syncs a ForkMesh repository with GitHub on events and
every 15 minutes. Refs move both ways by fast-forward only (never forced or
deleted; diverged branches are reported). Issues, discussions and pull requests
import one way into `.forkmesh` records; for two-way issues see
[Two-way forge sync](forge-sync.md).

```bash
python3 tools/github_forkmesh_bridge.py connect --github OWNER/NAME --forkmesh OWNER/NAME --key-out KEY.pem
```

`connect` writes `.forkmesh/github-sync.json`, `.forkmesh/github-bridge.py` and
`.github/workflows/forkmesh-github-sync.yml`. Set secrets
`FORKMESH_BRIDGE_SIGNING_KEY` and `FORKMESH_SYNC_SSH_KEY` and variable
`FORKMESH_SYNC_REMOTE` (optionally `FORKMESH_SYNC_KNOWN_HOSTS`).

Rules: imported events are signed by the bridge key with `authorName`
`github:LOGIN`; GitHub numbers are kept unless taken; event ids derive from
GitHub ids so reruns are idempotent; ForkMesh-side edits survive; pull request
code is fetched as data and never executed (payloads over 8 MiB skipped).
Subcommands `git-sync`, `export` and `run --dry-run` run pieces by hand; tests
are `app/tests/test_github_forkmesh_bridge.py`. To also deploy a Cloudflare
Worker from the mirror, see [Mirror and deploy a Cloudflare Worker](cloudflare-workers.md).
