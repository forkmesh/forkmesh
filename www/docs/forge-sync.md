# Two-way forge sync

A ForkMesh repository can stay connected to a repository on GitHub or on a
self-hosted Gitea or Forgejo server, so a community can adopt ForkMesh without
leaving the forge it already works on. Issues, their comments, labels,
assignees, open/closed state, edits, and comment deletions flow both ways.

| Provider | Tier | Adapter |
| --- | --- | --- |
| GitHub (github.com, GitHub Enterprise Server) | Centralized | `github` |
| Gitea, Forgejo, Codeberg | Self-hosted | `gitea` |

Branches and tags keep using the ref sync from
[Connecting to GitHub](github-sync.md) (`github_forkmesh_bridge.py git-sync`),
which works with any Git remote: it creates missing refs and fast-forwards,
never forces, and reports diverged branches. Pull requests, reviews, and
discussions are still imported one way by that bridge.

## Connect

```bash
# GitHub
python3 tools/forge_sync.py connect --provider github --repository octo/app

# Gitea / Forgejo
python3 tools/forge_sync.py connect --provider gitea --repository octo/app \
  --base-url https://git.example.org --slo-minutes 15
```

`connect` writes `.forkmesh/forge-sync.json`, the sync ledger. Only issues
opened on ForkMesh *after* connecting are created on the provider; pass
`--export-existing` to publish older ones too. Provider API origins must use
HTTPS.

Then run a pass on a schedule (for example every 5–15 minutes) from a checkout,
and commit and push the changed `.forkmesh` files:

```bash
FORKMESH_FORGE_TOKEN=… FORKMESH_BRIDGE_SIGNING_KEY="$(cat sync-key.pem)" \
  python3 tools/forge_sync.py sync --report sync-report.json
git add .forkmesh && git commit -m "forge-sync" && git push
```

The token needs read/write access to issues. The signing key is an Ed25519 key
in the same format as the GitHub bridge key; it signs every record the sync
writes into ForkMesh.

Exit codes: `0` converged, `1` unresolved conflicts, `2` configuration error,
`75` a retryable provider failure stopped the run (progress is kept).

## How changes are merged

The ledger remembers, per issue, a digest of each field (title, body, state,
labels, assignees) and of each comment as it was when both sides last agreed.
On every pass:

- If only one side changed a value, it is copied to the other side.
- If both sides changed it to the same value, nothing is written.
- If both sides changed it differently, it becomes a **conflict**. Neither
  side is written, and the conflict is reported on every pass until someone
  resolves it.

Nothing is overwritten silently. Repeated passes over unchanged state write
nothing on either side.

### Source-of-truth rules

A repository can publish a rule per field in the ledger, for example:

```json
"sourceOfTruth": { "state": "provider", "labels": "forkmesh" }
```

Each field is `manual` (the default: conflicts wait for a person),
`forkmesh`, or `provider`. When a rule settles a conflict, the discarded value
is still recorded in the ledger `history`, so it can be restored.

### Resolving conflicts

```bash
python3 tools/forge_sync.py status
python3 tools/forge_sync.py resolve --issue 12 --field body --take forkmesh
```

`--field` is `title`, `body`, `state`, `labels`, `assignees`,
`comment:<event id>`, or `issue`. The next pass copies the chosen side over the
other; the discarded value goes into `history`.

An `issue` conflict means the linked provider issue disappeared or was
replaced (for example, deleted and recreated with the same number). The
replacement is imported under a new ForkMesh number and the original record is
left untouched. `--take forkmesh` recreates the issue on the provider;
`--take provider` stops syncing that issue.

## Loop prevention

- A value the sync writes becomes the new agreed value, so its echo on the next
  pass compares equal and is not copied back.
- Issues and comments created on the provider carry a hidden
  `forkmesh-forge-sync:v1` marker and a visible "Synced from ForkMesh" credit
  line. Records written into ForkMesh use `forge-<provider>-` event ids.
- If the ledger is lost or rolled back, or a pass crashes between creating a
  provider issue and saving the ledger, the next pass re-links records by those
  markers instead of creating duplicates.
- Records imported by the one-way GitHub bridge (`github-` ids) are never
  exported back.

## Deletions

Deleting a comment on the provider adds a signed `delete` event in ForkMesh.
Deleting a comment in ForkMesh deletes it on the provider. A deletion on one
side that races an edit on the other is a conflict, so no edit is lost. Issues
themselves are never deleted by the sync.

## Identities

Everything imported from a provider is signed by the sync key and credited as
`<provider>:<login>` (for example `github:alice`). The colon can never appear in
a ForkMesh account name, so provider text cannot pass as a native user's.

Owners can link provider logins to ForkMesh accounts in the ledger:

```json
"identities": { "github:alice": "alice", "gitea:bob": "bob" }
```

Links only translate assignees in both directions; authorship stays credited
to the provider login. A link is **ambiguous**, and ignored with a warning,
when one login maps to several accounts (logins are compared
case-insensitively) or one account is linked from several logins of the same
provider. ForkMesh assignees without a link are kept in ForkMesh and not sent
to the provider.

## Rate limits, outages, and retries

Rate limits (`429`, or `403` with an exhausted quota) are retried after
`Retry-After`; server errors and network failures back off exponentially.
When retries run out the pass stops, keeps everything it already synchronized,
and reports `quota_exceeded` or `provider_unavailable`. An issue that failed
is recreated on the next pass. A rejected token (`401`) fails immediately.

## Pause and resume

```bash
python3 tools/forge_sync.py pause --reason "relabeling on GitHub"
python3 tools/forge_sync.py resume
```

A paused connection sends no requests to the provider and reports `paused`.

## Status, lag, and telemetry

`status` and the `--report` JSON (`forkmesh-forge-sync-report-v1`) use the
shared [synchronization status](sync-status.md) model and include:

| Measure | Meaning |
| --- | --- |
| `lagMs` / `sloMs` / `sloMet` | Time since the oldest unconverged change, or since the last complete pass, against the published SLO (default 15 minutes) |
| `convergenceRate` | Linked issues with no conflict and no failure |
| `conflictRate` | Open conflicts per linked issue |
| `retries` / `retrySuccesses` | Provider requests retried, and those that then succeeded |
| `conflicts` | Every unresolved conflict, with both values |
| `failing` | Issues whose last attempt failed |

Reports never contain tokens.

## Limits

- Pull requests, reviews, releases, and discussions are not yet synchronized
  back to providers.
- Milestones, reactions, and attachments are not synchronized.
- Issues are never deleted on either side.

The contracts, including edits, deletions, force updates, rate limits,
outages, and identity ambiguity for both adapters, are covered by
[`app/tests/test_forge_sync.py`](../../app/tests/test_forge_sync.py).
