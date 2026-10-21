# External repository imports and resumable migrations

Racing a shutdown, a takedown, a lost account, or censorship? Start with
[preservation-first crisis onboarding](crisis-preservation.md).

## Setup

On **Repositories**, choose **New repository**, select **GitHub / GitLab /
Codeberg**, paste the URL (a gitea.com URL works there too), and select
**Start resumable import**. A read-scoped
provider token is optional for public repositories and required for private
ones. Tokens are used per request and never stored, logged, or returned, so a
private import asks for one again on **Continue import**.

Imports live apart from the live catalog: `/api/repositories` lists published
ForkMesh repositories, `/api/repository-imports` lists external metadata entries
and stubs. An external entry is never routed or called mirrored until a node
hosts it (see [Ending the provider dependency](#ending-the-provider-dependency)).

## API

```http
POST /api/repository-imports                     # metadata entry or stub (mode: import | stub)
POST /api/repository-imports/discover            # expand a GitLab group or a Codeberg or Gitea profile, max 200 public repos
POST /api/repository-imports/migrations          # durable job, 202 with a mig_… id
POST /api/repository-imports/migrations/{id}/resume
GET  /api/repository-imports/migrations/{id}
GET  /api/repository-imports/migrations/{id}/objects?type=reviews&limit=100
GET  /api/repository-imports/migration-metrics   # setupP50Ms, setupP95Ms, transfer, retries
DELETE /api/repository-imports/{id}
```

Only canonical `github.com`, `gitlab.com`, `codeberg.org`, and `gitea.com`
HTTPS URLs are accepted. Codeberg (Forgejo) and gitea.com share one adapter,
because both serve Gitea's `/api/v1`; other Gitea or Forgejo instances are
not accepted, so the import cannot be pointed at an arbitrary host. `targetOwner` is the caller or an organization they administer.
Each `/resume` call advances one provider page and returns the next checkpoint;
clients repeat it until `completed`, `429`, or an explicit error. Setup time
(form to accepted job) and transfer time are measured separately; the setup
gate is p50 at most 180,000 ms.

Every adapter emits the same object families: `git_refs`, `issues`,
`pull_requests` (GitHub pull requests and GitLab merge requests), `reviews`,
`releases`, `documentation`, `labels`, `users`, and `collaboration_history`.
Each object keeps the original provider payload plus a common projection.
Credential-shaped fields are redacted and counted. Git objects travel through
the normal bare-mirror path; wikis, workflow logs, and artifact bytes are
reported as out of scope. Provider rate limits are honored: the job becomes
`rate_limited` and resumes on the same page.

## Idempotence, checkpoints, and signed completion

Object identity is `SHA-256(object family + preserved source id)`. Objects are
upserted before the checkpoint advances, so a crash can repeat work but never
duplicate it. An object must be absent from two consecutive complete scans
before it is pruned. On completion the sorted `(objectType, sourceKey,
payloadHash)` tuples are hashed and signed; run numbers and timestamps are
excluded, so an unchanged source yields the same state hash on every rerun.

## Failure, abandonment, and rollback

- Retry a `paused`, `failed`, or `rate_limited` job with `/resume`.
- `POST …/{id}/abandon` stops work but keeps objects; resume with `{"recover": true}`.
- `POST …/{id}/rollback` removes the job, its objects, and its telemetry. It
  keeps the external listing and never touches the source forge.
- A missing optional stream goes to `report.skipped`. A required page the
  provider will not serve pauses the job; resume with `{"deferFailure": true}`
  to continue and leave that work listed as retryable.
- A single record that cannot be stored is counted as failed; the rest of its
  page is imported.

## Migration reports

```http
GET  /api/repository-imports/migrations/{id}/report[?sequence=N]
POST /api/repository-imports/migrations/{id}/retry
```

Every import ends with a signed `forkmesh-repository-migration-report-export-v1`
report (owner-only, `no-store`), issued on completion, abandonment, and each
retry. Sections: `summary`, `reconciliation.rows`, `refs`, `artifacts`,
`checksums`, `retries`, `failures`, `externalDependencies`, `verification`,
`remediation`.

Each provider record lands in exactly one class: `imported`, `skipped`,
`failed`, `external`, or `unclassified`. Any `unclassified` record makes the
outcome `unreconciled`. Outcomes are `verified`, `completed_with_failures`,
`completed_with_unmeasured_sources`, `unreconciled`, and `abandoned`.

The report hash is signed with the relay's Ed25519 router key (published as
`routerPublicKey` on `GET /api/mirrors/https`) over
`forkmesh-repository-migration-report-v1\n<reportHash>`, or HMAC when no key is
configured. Reports chain through `sequence` and `supersedes`. `/retry` queues
only failed pages and deferred streams (`409 no_failed_work` if none).

`tools/verify_migration_report.py` checks a report without ForkMesh code:
hash, signature, reconciliation arithmetic, chain, and, with `--objects`, every
payload hash. Exit 0 verified, 1 failed check, 2 unreadable input.

```bash
python3 tools/verify_migration_report.py report.json \
  --objects objects.json --public-key "$ROUTER_PUBLIC_KEY"
```

Tests: `app/tests/test_repository_migration.py`,
`app/tests/test_repository_migration_report.py`.

## Visibility and statuses

Anonymous listings filter private entries before decryption; a private id
returns the same `404` whether absent or forbidden. Statuses:
`external_repository`, `stub_only`, `mirror_requested`, `partially_mirrored`,
`actively_mirrored`, `mirror_unavailable`, `archived`. The mirrored states
require a matching catalog record; `actively_mirrored` also needs a live host
heartbeat.

## Ending the provider dependency

Public imports become ForkMesh-hosted repositories:

1. `tools/hosted_repository_sync.py` runs on mirror nodes from
   `forkmesh-hosted-repository-sync.timer` (every 30 minutes), reads the public
   import listing, and keeps a `git clone --mirror` of each eligible entry
   under `/srv/forkmesh-git/imports/<node>/<name>.git`.
2. The node publishes a signed catalog record, so the gateway serves the copy
   with no provider in the path.
3. The relay links that record to the import entry, which becomes
   `actively_mirrored` and routes `/<targetOwner>/<name>` to the hosting node.

Only canonical public provider URLs without credentials qualify. Private and
archived imports are never hosted. A node caps its imports root at 20 GiB and a
first clone at 10 minutes; over budget an entry is deferred. Deleting the
import removes the hosted copy on the next run; nothing is deleted at the
provider.

## Mirror volunteers, invitations, and logos

- `POST /api/repository-imports/{id}/mirror-volunteers` needs a mirror node the
  caller owns (`eligible_owned_mirror_node_required` otherwise).
- `POST /api/repository-imports/{id}/invitations` is limited to
  provider-verified administrators, needs a durable provenance record
  (`public_for_invitations`, `prior_consent`, or `owner_supplied`) and
  `confirm: true`, is rate limited, and stores only masked addresses and blind
  indexes. Every message carries opt-out and abuse-report links.
- Each entry gets a locally generated SVG logo (`aiGenerated: false`).
  `POST /api/repository-imports/{id}/logo-suggestions` accepts PNG, JPEG, or
  WebP for owner approval.
