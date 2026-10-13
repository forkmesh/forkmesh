# External repository imports and resumable migrations

Racing a shutdown, a takedown, a lost account, or censorship? Start with
[preservation-first crisis onboarding](crisis-preservation.md). It orders this
import against the time and access you have left, and reports what is
actually preserved.

## Three-minute setup path

On **Repositories**, choose **New repository**, select **GitHub / GitLab /
Codeberg**, paste the repository URL, and select **Start resumable import**. A
read-scoped provider token is optional for public repositories and required for
private repositories. ForkMesh durably accepts the job after one repository
lookup; repository size and collaboration-history transfer are not part of the
hands-on setup timer.

The status panel reports these clocks separately:

- **Hands-on setup** runs from opening the form until the durable job is
  accepted. The browser supplies a monotonic duration and the relay rejects
  implausible values; a request-duration fallback is labelled separately.
- **Transfer** starts when the first provider page is requested and ends when
  the canonical state is signed. Closing the page does not discard progress.

The repositories list shows **Continue import** for an interrupted, paused, or
rate-limited job. Public imports resume without a credential. A private import
asks for a current token again because ForkMesh never stores provider tokens.
The setup telemetry endpoint reports `setupP50Ms` and `setupP95Ms` separately,
both overall and by supported provider:

```http
GET /api/repository-imports/migration-metrics
Authorization: Bearer <ForkMesh session>
```

`setupMedianWithinThreeMinutes` is true only when recorded p50 is at most
180,000 ms. Transfer p50, completion, retries, object volume, and abandonment
are separate fields, so a large repository cannot make setup appear slow or a
fast form cannot hide a stalled transfer.

The checked conformance dataset deliberately uses setup samples of 60, 120,
and 240 seconds for each provider. Its reported result is therefore p50 **120
seconds** (passes the three-minute gate) and p95 **240 seconds**, separately,
for GitHub, GitLab, and Codeberg. These are deterministic acceptance samples,
not a claim about production users; the endpoint above is the source for
current operational p50/p95 after rollout.

ForkMesh keeps GitHub, GitLab, and Codeberg metadata imports in a separate data
model from its live mirror catalog:

- `/api/repositories` contains published ForkMesh mirrors.
- `/api/repository-imports` contains external metadata entries and stubs.

An external entry is never routed, cloned, or described as mirrored until its
status is linked to a separately published and verified ForkMesh mirror.
ForkMesh does not own or control an external repository. A signed import
receipt therefore means the provider *metadata* is captured; the Git contents
become independent of the provider only once a node hosts them, which is what
[Ending the provider dependency](#ending-the-provider-dependency) describes.

## Importing metadata

An authenticated user can submit:

```http
POST /api/repository-imports
Content-Type: application/json

{
  "sourceUrl": "https://github.com/owner/repository",
  "providerToken": "request-only token, if needed",
  "targetOwner": "user-or-organization",
  "mode": "import",
  "sessionToken": "ForkMesh account session"
}
```

`mode` is `import` or `stub`. Only canonical `github.com`, `gitlab.com`, and
`codeberg.org` HTTPS repository URLs are accepted. `targetOwner` may be the
signed-in user or an organization where that user is an owner or administrator.
Provider access tokens are used for that request and never written to D1, a
Worker log, the response, or browser storage. Private repositories require a
token that can read the repository.

The World importer can expand a Codeberg user/organization profile into its
public repositories before importing them one at a time:

```http
POST /api/repository-imports/discover
Content-Type: application/json

{
  "sourceUrl": "https://codeberg.org/m33",
  "sessionToken": "ForkMesh account session"
}
```

Discovery is authenticated, paginated, restricted to canonical Codeberg URLs,
and bounded to 200 repositories. It never imports a private repository.

A community member can list a public stub without claiming ownership. If a
provider administrator later imports the same stable provider repository id
with a current token, they can adopt moderation of the entry; the original
lister remains attributed. A read-only collaborator cannot adopt or overwrite a
private import.

The legacy `POST /api/repository-imports` endpoint creates a bounded external
metadata entry or stub. A full migration uses the durable endpoint instead:

```http
POST /api/repository-imports/migrations
Content-Type: application/json

{
  "sourceUrl": "https://github.com/owner/repository",
  "providerToken": "request-scoped token, if needed",
  "setupDurationMs": 42000,
  "sessionToken": "ForkMesh account session"
}
```

The `202` response is the end of setup and includes a deterministic `mig_…`
job id. `POST /api/repository-imports/migrations/{id}/resume` advances exactly
one provider page and returns the next durable checkpoint. The web UI repeats
that request while it remains open. API clients should do the same and stop on
`completed`, HTTP `429`, or another explicit error. Provider credentials are
request-scoped, never placed in a job/checkpoint/report, and should be supplied
again only when resuming a private source.

### Supported fidelity contract

All three adapters—GitHub, GitLab, and Codeberg—emit the same versioned object
families:

| Family | Preserved source records |
| --- | --- |
| `git_refs` | Branch and tag names, object ids, and protection metadata |
| `issues` | Complete issue provider payloads, authors, labels, state, and timestamps |
| `pull_requests` | Pull/merge request payloads, heads/bases, state, and timestamps |
| `reviews` | Reviews, review discussions, and inline review comments available from that provider |
| `releases` | Releases, tags, asset manifests/links, and timestamps |
| `documentation` | Provider README document, exact encoding/content, path, and blob id |
| `labels` | Provider label ids, names, colors, descriptions, and timestamps |
| `users` | Contributor/collaborator source identities and provider metadata |
| `collaboration_history` | Issue comments/events, merge-request notes, and commit comments exposed by the adapter |

Each encrypted object retains the original JSON provider payload alongside a
common projection. The source provider, repository id, object id/id kind,
stream, relationships, URLs, and timestamps are preserved for audit and later
two-way sync. Credential-shaped fields fail closed into an explicit redaction
marker and increment the signed report’s redaction count; unsupported values,
oversize objects, or invalid response shapes fail the page rather than being
silently dropped.

Git repository object transfer continues through ForkMesh’s normal bare-mirror
path; the adapter preserves ref names and object ids so the mirror result can be
compared without rewriting Git ids. Provider wikis are separate Git
repositories on all supported platforms and are listed explicitly as requiring
a separate mirror operation. Workflow logs and artifact bytes are also
reported as out of scope rather than presented as migrated. Private source is
never sent to an image generator or other external model.

Provider rate-limit headers are honored. When a limit is reached, further
calls stop, the checkpoint remains on the unprocessed page, `Retry-After` is
forwarded, and the job becomes `rate_limited`. Resuming repeats that page; its
deterministic keys make this safe even if the first response reached storage
but the client missed the acknowledgement.

## Idempotence, checkpoints, and signed completion

Object identity is `SHA-256(object family + preserved source id)`. A page uses
an upsert on that key before its phase/stream/page checkpoint advances. A crash
between those writes can therefore repeat work but cannot create a duplicate;
a crash after the checkpoint cannot precede its object writes. Pull requests
are imported before provider-specific review streams are constructed, so those
relationships survive a process restart too.

A new completed-source scan increments the job generation. Objects observed
again are updated in place. An object must be absent from two consecutive
complete scans before pruning, so offset-page movement at an active source can
delay a deletion but cannot silently delete a live record; provider edits never
create a second logical record. When all phases finish,
ForkMesh sorts the surviving `(objectType, sourceKey, payloadHash)` tuples,
hashes the canonical document, and authenticates that digest with the relay’s
`hmac-sha256` migration key. Local run numbers, retry counts, and timestamps are
excluded, so an unchanged source produces the exact same state hash and
signature on every rerun.

The job owner can audit the preserved objects and receipt without accessing D1:

```http
GET /api/repository-imports/migrations/{id}
GET /api/repository-imports/migrations/{id}/objects?type=reviews&limit=100
```

The object export returns at most 200 records and an opaque `nextCursor` for
the next `after=` request. Both endpoints are owner-only and `no-store`.
Provider tokens are absent from all responses and durable rows.

## Failure, abandonment, and rollback

- Retry a `paused`, `failed`, or `rate_limited` job with `/resume`; the stored
  phase, stream, and page are authoritative.
- `POST …/{id}/abandon` stops automatic work but retains objects and the
  checkpoint. Resume it with `{"recover": true}`.
- `POST …/{id}/rollback` removes the job, its imported objects, and its run
  telemetry. It deliberately preserves the external repository listing and
  never changes the source forge.
- A provider endpoint missing only for an adapter-declared optional stream is
  added to `report.skipped`. A required page the provider will not serve pauses
  the job and appears in `report.failures`. Resume to request that page again,
  or resume with `{"deferFailure": true}` to continue the import. The page and
  the rest of its stream then become retryable failed work, and the report
  lists them as unmeasured instead of calling the import complete.
- A single provider record that cannot be stored without loss (an unsupported
  value, an oversize object) no longer fails its whole page. It is counted as
  failed with its source id and reason, and the rest of the page is imported.

## Migration reports

Every import ends with a signed report: when it completes, when it is
abandoned, and again whenever failed work is retried. It answers what arrived,
what was skipped, what failed, and what remains outside ForkMesh.

```http
GET  /api/repository-imports/migrations/{id}/report
GET  /api/repository-imports/migrations/{id}/report?sequence=1
POST /api/repository-imports/migrations/{id}/retry
```

Both are owner-only and `no-store`. A job that has not ended yet returns an
unsigned `draft` built on request. The export body is machine-readable
(`forkmesh-repository-migration-report-export-v1`) and contains:

| Section | What it shows |
| --- | --- |
| `summary` | Plain-language lines: records imported, skipped, failed, and external |
| `reconciliation.rows` | Per object type (and `release_assets`): `source`, `imported`, `skipped`, `failed`, `external`, `unclassified` |
| `refs` | Branch and tag names with their commit ids, plus a digest over all of them |
| `artifacts` | Release assets, their provider checksums, and `retained: false` |
| `checksums` | The object manifest digest, the signed state receipt, and release coverage |
| `retries` | Provider rate-limit retries and every failed-work retry with its recovery count |
| `failures` | Failed record count, failure events with source ids, and the retryable work list |
| `externalDependencies` | Wikis, release bytes, missing tags, manifests, and provenance |
| `verification` | One result per check, each linked to the report section or export that reproduces it |
| `remediation` | What to do about each failure reason, with the retry action where one applies |

**Classification.** Each record a provider returns is counted once, on its
page, and lands in exactly one class. `imported` means it is stored.
`skipped` means another type already holds it (a pull request listed on the
GitHub issues endpoint) or the provider listed it twice. `failed` means it
could not be stored. `external` means the source declares it but its bytes
were not copied (release assets). Anything left over is `unclassified`, fails
the reconciliation check, and makes the outcome `unreconciled`. Page entries
are keyed by stream and page, so a page fetched again after a crash replaces
its own counts. Outcomes are `verified`, `completed_with_failures`,
`completed_with_unmeasured_sources`, `unreconciled`, and `abandoned`.

**Signature and history.** The report is canonical JSON. Its SHA-256
`reportHash` is signed over
`forkmesh-repository-migration-report-v1\n<reportHash>` with the relay's
Ed25519 router key, whose public half the relay publishes as
`routerPublicKey` on `GET /api/mirrors/https`. A deployment without that key falls back to the relay HMAC
and labels the signature `hmac-sha256`. Each report carries `sequence` and
`supersedes` (the previous report's sequence and hash), so a retry or a new
full scan adds to the chain instead of replacing it. The export includes the
whole chain; the five most recent reports keep their full body and older ones
keep their hash and signature.

**Retrying only failed work.** `POST …/retry` on a completed import queues
exactly the failed pages and deferred streams, then `/resume` works through
that queue. A retried page replaces what its failed attempt counted. A deferred
stream follows its pagination to the end. When the queue drains, a new signed
report supersedes the old one. Retry does not extend the original transfer
measurement. It returns `409 no_failed_work` when nothing failed and
`409 migration_not_completed` while the import is still running.

**Independent verification.** `tools/verify_migration_report.py` imports no
ForkMesh code. It recomputes the report hash, the Ed25519 signature, the
reconciliation arithmetic, and the supersession chain. Given the object export,
it also recomputes every payload hash, source key, and the manifest digest, so
editing a report or any imported record after signing is detected:

```bash
python3 tools/verify_migration_report.py report.json \
  --objects objects.json --public-key "$ROUTER_PUBLIC_KEY"
```

`objects.json` may be a list of records or saved `…/objects` pages. Exit status
`0` means verified, `1` means a check failed, and `2` means the input could not
be read. Without `--public-key` the key embedded in the report is used, and the
output says so.

**Telemetry.** Reports, exports, retries, and deferrals are written to the
audit log (`repository_migration.report`, `.report_export`, `.retry_failed`,
`.defer_failure`). A rollback records the final report hash. The migration
metrics add `reports.unclassifiedRecords` (the release gate is zero),
`failedRecords`, `externalRecords`, and `retryRecovery.recoveredWork`.

## Automated evidence

The executable fixture suite is
[`app/tests/test_repository_migration.py`](../../app/tests/test_repository_migration.py).
It runs GitHub-, GitLab-, and Codeberg-shaped repositories containing every
supported object family through the real service and SQLite schema. It covers
payload/timestamp/source-id fidelity, secret redaction, duplicate suppression,
process interruption, rate-limit retry, source deletion, repeated signed-state
convergence, abandonment/recovery, rollback, credential non-persistence, and
setup p50/p95 reporting. The dashboard asset and existing external-import
contracts run beside it:

```bash
pytest -q app/tests/test_repository_migration.py \
  app/tests/test_repository_migration_report.py \
  app/tests/test_repository_imports.py app/tests/test_dashboard_bundle.py
```

[`app/tests/test_repository_migration_report.py`](../../app/tests/test_repository_migration_report.py)
covers the reports on all three providers: fixture totals reconcile with no
unclassified remainder, the independent verifier catches report and content
tampering, retry requests only the failed pages and supersedes the report with
its history kept, deferred outages are recovered, a crash-redo page is not
counted twice, and abandoned, rolled-back, and in-progress imports report
honestly without the provider token.

The linked
[`repository-migration-conformance.yml`](../../.github/workflows/repository-migration-conformance.yml)
runs that evidence on Linux, macOS, and Windows for every pull request and
retains the JUnit result for 30 days, including failure-path output.
The same suite is a required `repository-migration-conformance` core/release
gate, and the 180,000 ms setup-p50 ceiling is registered as a non-weakening
performance budget in
[`quality-gates.json`](engineering/quality-gates.json).

The upgrade/runtime schema parity is pinned by migration
[`0152_repository_migration_jobs.sql`](../../app/migrations/0152_repository_migration_jobs.sql),
and the user-visible setup/transfer split is sourced from
[`new-repo-modal.html`](../../app/public/dashboard/partials/new-repo-modal.html).

## Visibility and statuses

Anonymous listing queries filter to `is_private=0` in D1 before any record is
decrypted. A private id returns the same `404` response when it is absent,
guessed, or requested by the wrong account.

Supported status values are:

- `external_repository`
- `stub_only`
- `mirror_requested`
- `partially_mirrored`
- `actively_mirrored`
- `mirror_unavailable`
- `archived`

The two mirrored states require a matching catalog record. `actively_mirrored`
also requires a current live-host heartbeat.

An owner or organization administrator can delete an external listing with
`DELETE /api/repository-imports/{id}`. The website provides select-all and bulk
deletion controls for only the imports the current account can manage. Deleting
the listing never deletes anything from the source provider.

## Ending the provider dependency

An import that only ever links back to GitHub, GitLab, or Codeberg leaves the
user exactly as dependent on that provider as before. Public imports are
therefore materialized as real ForkMesh-hosted repositories:

1. `tools/hosted_repository_sync.py` runs on mirror nodes from
   `forkmesh-hosted-repository-sync.timer` (every 30 minutes). It reads the
   public listing at `<workerOrigin>/api/repository-imports` — the origin comes
   from the node's own configuration, never a hard-coded host — and keeps a
   bare `git clone --mirror` of each eligible entry under
   `/srv/forkmesh-git/imports/<node>/<name>.git`, refreshed with
   `remote update --prune` and compacted after any real ref change.
2. `register` publishes a node-signed catalog record for each hosted clone, so
   the copy is served by the ordinary mirror gateway with no provider in the
   path.
3. The relay links that record back to the import entry
   (`_promote_hosted_repository_import`): the entry becomes
   `actively_mirrored`, gains `mirror.owner`/`mirror.name`, reports
   `sourceCodeFetched`, and the importer receives a `repository_hosted` ping.
   The card in **Repositories** then reads *Fully hosted by ForkMesh* and links
   to `/<targetOwner>/<name>` instead of the provider.
4. `/<targetOwner>/<name>` resolves through `_hosted_repository_import_route`
   to whichever node published the hosted copy. Any publishing node qualifies —
   the resolved owner/name must still appear in the public catalog, so an
   unpublished or blocked pair is never routed to.

Eligibility is deliberately narrow:

- All three supported providers qualify. Only each provider's canonical public
  host is cloned (`github.com`, `gitlab.com`, `codeberg.org`), only in that
  provider's canonical path shape (GitHub and Codeberg name one owner and one
  repository; GitLab allows nested groups), and only when the URL carries no
  credentials, query, or fragment.
- Private imports are never materialized, and an archived entry is dropped.
- Hosted names are one per mirror node, so when two public imports share a
  repository name only the first is hosted. Promotion prefers the import whose
  default branch matches the branch the node published, because a catalog
  record carries no import id to match on.
- Deleting the import entry removes the hosted clone and its catalog record on
  the next run; nothing is ever deleted at the provider.
- A node bounds its imports root (20 GiB) and each first clone (10 minutes).
  Over budget or over time, the entry is deferred or retried rather than
  hosted, so one large repository cannot stop the others from being served.
  `prepare` reports `repositoryCount`, `failedCount`, and `deferredCount`.

The provider remains recorded as attribution and as the upstream the hosted
copy follows. It stops being required to read, clone, or serve the repository.

## Mirror volunteers

`POST /api/repository-imports/{id}/mirror-volunteers` records a request only
when the authenticated account supplies a mirror node it owns: a node-kind
account (a desktop node's own session, or a node linked to the user's account
under Settings › Nodes). A user account cannot volunteer its own name, since no
process could serve that mirror; the request fails with
`eligible_owned_mirror_node_required` and the web app shows the desktop
handoff steps instead. Public reads show the number of volunteers, not their
node/device names.

## Contributor invitations

Only an importer whose provider metadata proves Maintainer/administrator
permission (or a ForkMesh moderator) can create invitations:

```http
POST /api/repository-imports/{id}/invitations
```

The contributor must occur in imported contributor metadata. The inviter must
reference a durable eligibility record; a request-time `basisConfirmed`
boolean is rejected. The supported evidence sources are:

- `public_for_invitations` — a public provider contributor snapshot included an
  explicit public contact address. ForkMesh immediately converts it to a keyed
  blind index and does not copy the raw address into the import record.
- `prior_consent` — the recipient used a signed-in account with a verified
  email to create an import-specific consent record at
  `POST /api/repository-imports/{id}/invitation-consent`.
- `owner_supplied` — a current provider-verified administrator registered the
  address and a source reference at
  `POST /api/repository-imports/{id}/invitation-provenance`. ForkMesh retains a
  keyed digest of the reference, not the raw reference.

The invitation request supplies the returned `provenanceId`. The Worker
matches that record to the exact repository, contributor, and recipient blind
indexes both before the preview and atomically again when reserving the send.
Revoked, mismatched, guessed, and boolean-only evidence fails closed.

Sending requires an explicit `confirm: true`; otherwise the endpoint returns an
invitation preview. Limits apply per inviter, per repository, and per recipient.
Every message identifies the inviter and includes token-protected opt-out and
abuse-report controls. The raw invitation address exists only during the
delivery call; the durable invitation keeps a masked address, a keyed blind
index for matching/cooldown/opt-out checks, and a one-way action-token digest.
Provenance creation, consent/revocation, and sends append metadata-only
sensitive-action audit records.

Provider administrator proof expires after 24 hours. Refresh the import with a
new one-request token before another owner-only action; ForkMesh never retains
the token to refresh permission silently.

Email links use a non-mutating `GET` confirmation response. The recipient must
submit `POST` to opt out or report, so automated email-link scanners cannot
change state.

## Repository logos

Every external entry has a deterministic, locally generated abstract SVG based
on non-sensitive repository name, language, topic, and description metadata.
Owner-authorized local imports may also supply bounded file-structure labels,
framework names, and a project category; file contents are never logo inputs.
It does not copy provider/framework marks and does not use an external model.
The response marks this as `aiGenerated: false`.

Authenticated users can submit PNG, JPEG, or WebP alternatives through:

```http
POST /api/repository-imports/{id}/logo-suggestions
```

The uploader must confirm that they hold the necessary rights. Suggestions
remain pending until a provider-verified owner or ForkMesh moderator approves
one. A verified owner can also replace the generated logo directly. Uploaded
SVG is rejected to avoid script-capable image content, and only one approved
official logo can exist per external entry.
