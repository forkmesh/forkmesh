# Two-way forge sync

`tools/forge_sync.py` keeps ForkMesh issues and comments (fields, labels,
assignees, state, edits, comment deletions) in step with GitHub (`github`
adapter) or Gitea, Forgejo and Codeberg (`gitea` adapter). Branches and tags
use the ref sync in [Connecting to GitHub](github-sync.md).

## Usage

```bash
python3 tools/forge_sync.py connect --provider gitea --repository octo/app \
  --base-url https://git.example.org --slo-minutes 15
FORKMESH_FORGE_TOKEN=… FORKMESH_BRIDGE_SIGNING_KEY="$(cat sync-key.pem)" \
  python3 tools/forge_sync.py sync --report sync-report.json
python3 tools/forge_sync.py status
python3 tools/forge_sync.py resolve --issue 12 --field body --take forkmesh
python3 tools/forge_sync.py pause --reason "maintenance"; python3 tools/forge_sync.py resume
```

`connect` writes the ledger `.forkmesh/forge-sync.json`; commit it after each
pass. Exit codes: `0` converged, `1` conflicts, `2` configuration error, `75`
retryable provider failure. A paused connection sends no requests.

## Merge rules

- The ledger stores a digest of each field and comment as last agreed. A change
  on one side is copied over; differing changes on both sides become a
  conflict and nothing is written until it is resolved.
- `sourceOfTruth` sets a per-field rule: `manual` (default), `forkmesh` or
  `provider`. Discarded values are kept in ledger `history`.
- Synced records carry a `forkmesh-forge-sync:v1` marker so echoes and lost
  ledgers never create duplicates. Issues are never deleted.

## Identities

Imported records are signed by the sync key and credited as
`<provider>:<login>`. The ledger `identities` map links provider logins to
ForkMesh accounts for assignees only; ambiguous links are ignored.

## Rate limits and status

A rate limit (`429`, or `403` with an exhausted quota) waits for
`Retry-After`; other failures back off exponentially, and a stopped pass keeps
its progress. Reports (`forkmesh-forge-sync-report-v1`, see
[synchronization status](sync-status.md)) give lag against the SLO (default 15
minutes), convergence and conflict rates, and retries. They never contain
tokens.
