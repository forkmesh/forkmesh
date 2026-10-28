# Repository security advisories

Private vulnerability reporting, a private fix discussion, published
advisories with OSV export, and credit for the people who found and fixed the
problem. The model follows GitHub Security Advisories closely enough that
maintainers and researchers recognise it.

Code: `app/src/security_advisories.py` (API, pages, OSV, ledger),
`app/src/advisory_cvss.py` (CVSS 3.1 and 4.0 base scores),
`app/public/dashboard/js/07-repo-security.js` (the repository Security tab and
the profile's Security credits), migration `0277_security_advisories`.
Tests: `app/tests/test_security_advisories.py`.

## Who sees what

| Viewer | Triage, draft, closed | Published, withdrawn |
| --- | --- | --- |
| Reporter | their own reports, comments | public view, plus the thread |
| Maintainer (repository owner, or organization `admin`/`maintain`) | every advisory of the repository | everything |
| Collaborator added by a maintainer | that advisory, comments, edit while triage/draft | public view, plus the thread |
| Anyone else | `404 {"ok":false,"error":"not_found"}` | public view only |

The 404 is the same body for a missing advisory, an advisory of another
repository, a private draft, a private repository and a repository that does
not exist, so the API is not an existence oracle. Repository read access is
`_repository_access_context`, so a private repository's advisories are only
reachable by its readers. Maintainer rank comes from `_account_owns_node` and
`_org_linked_repo_permission` (`team_permission_rank >= maintain`); turning
reporting on or off needs `admin`.

The reporter is a row in `security_advisory_members` (role `reporter`), as is
each collaborator (role `collaborator`). Removing a collaborator removes the
row, and their next read is a 404.

## Reporting

Any signed-in account with a verified email can report from the Security tab
when reporting is on. It is on by default for public repositories and off for
private ones (`security_advisory_settings`, one row per repository once an
admin changes it). A report starts in `triage`, notifies the maintainers
(`_org_fanout_recipients`, ping kind `security_advisory`) and, when
`SECURITY_EMAIL` is set, mails the site's security contact that a report was
filed (kind `security_report` in the mail log): the repository, the advisory
ID, the severity and a link, never the title or description. The contact is
site-wide and covers private repositories too, while the report may only be
read by the reporter, the maintainers and the people they invite, so the
proof of concept never leaves the site by email. The report form says so. A reporter
may have ten open triage reports per repository, and a repository holds at most
200 in triage. A maintainer using the same button creates a `draft` instead and
nobody is mailed.

States: `triage -> draft -> published -> withdrawn`, with `closed` reachable
from triage and draft and `closed -> draft` to reopen. Only maintainers move
states, add collaborators or credits, link a fix, publish and withdraw.
Reporters edit their report while it is in triage; collaborators edit while it
is in triage or draft.

## Record

`security_advisories.data` is `encrypt_row` of the whole record: title,
description (Markdown), severity, CWE IDs, affected products, references, CVE
ID, fix link, reporter, collaborators, history and the signed ledger events.
Comments, members and credits are encrypted the same way in their own tables.
Only the ID, state, timestamps and blind indexes are plaintext.
`public_data` stays empty until the advisory is published; it then holds the
public view (no reporter unless credited, no collaborators, comments or
history) so the instance lists and feeds never decrypt.

- **ID**: `<PREFIX>-xxxx-xxxx-xxxx` from GHSA's alphabet
  (`23456789cfghjmpqrvwx`). The prefix is `SECURITY_ADVISORY_ID_PREFIX`
  (`[A-Z][A-Z0-9]{1,15}`), default `FMSA`. Changing it affects new advisories
  only; any prefix resolves.
- **Severity**: a CVSS 3.1 (or 3.0) or 4.0 base vector, scored by
  `advisory_cvss.py`, or a manual `low`/`moderate`/`high`/`critical`. A vector
  wins and sets the level (0.1-3.9 low, 4.0-6.9 moderate, 7.0-8.9 high,
  9.0-10 critical). The CVSS 4.0 macro-vector table and interpolation are
  FIRST's reference calculator (BSD-2-Clause), ported from the RedHat `cvss`
  package; the test cross-checks both versions against that package when it is
  installed. A 3.x vector lists the eight base metrics first, in the
  specification's order, and may then carry temporal and environmental
  metrics (`E`, `RL`, `RC`, `CR`/`IR`/`AR`, `MAV` ... `MA`), which published
  advisories often include; they are validated and kept in the vector but do
  not change the base score. Every metric value is a single letter from its
  allowed set. Request bodies that cannot be encoded as UTF-8 (a lone
  surrogate) are refused with `invalid_json`.
- **CVE ID**: entered by hand. There is no CNA integration.
- **Affected products**: ecosystem (OSV's names) and package, or a folder of
  the repository, an affected range (`>= 1.0, < 1.4.2`) and patched versions.
- **Fix**: a branch, a pull request number and/or a commit. Publishing does
  not require it, nor a merged fix; the page and the publish button warn when
  no patched version is set.

## Publishing and the repository ledger

Publishing, editing a published advisory and withdrawing are signed by the
maintainer's browser key (the same `forkmesh.webIssueKey` the issue forms
use), like issue events:

1. `{"action":"prepare","kind":"publish|update|withdraw","fields":{...}}`
   returns the OSV record the advisory will have, its `contentHash`
   (SHA-256 of the canonical JSON: Python `json.dumps` with `sort_keys`,
   `,`/`:` separators and `ensure_ascii=False`, UTF-8; this is not RFC 8785,
   so a third-party verifier must reproduce exactly that form). The browser
   signs the `contentHash` the server returns without recomputing it, so the
   signature attests to the record the server prepared.
   a `preparedAt` time that becomes `published`/`modified`/`withdrawn`, and
   the record's `revision` (bumped on every stored change).
2. The browser signs
   `forkmesh-advisory-event-v2\n<owner>/<repo>\n<kind>\n<id>\n<author key>\n<ts>\n<contentHash>`
   (the v2 shape of `events.event_signing_messages`, bound to the repository
   key) and sends `{"action":"sign", ...}` with the same `preparedAt` and
   `revision`.
3. The server rebuilds the record for that `preparedAt`, refuses it when the
   advisory changed since (a different `revision` or an older `preparedAt`
   is `stale_prepare`, a different hash `content_changed`), checks the
   signature with `ed25519_verify` and `_bind_session_author`, stores it, and
   commits `.forkmesh/advisories/<ID>.json` to the default branch through the
   web file-edit path (`repository_file_edits`).

The ledger file is
`{"format":"forkmesh-advisory-v1","id","repository","events":[...]}`; each
event carries `type`, `ts`, `author`, `authorName`, `repoKey`, `contentHash`,
`sig` and the `osv` it signed, so every event verifies on its own from the
file alone and the git history mirrors it like `.forkmesh/issues`. When no host
takes the commit (no live host, a protected branch, a private repository), the
advisory is still published with ledger state `pending` and a maintainer can
retry; the signed events are kept and written on the next try.

Only public repositories can publish. Credits accepted after a signed event
appear in the live OSV export at once and in the ledger with the next signed
update.

## Public surfaces

- `/<owner>/<repo>/security`: policy (`SECURITY.md`, `.github/SECURITY.md` or
  `docs/SECURITY.md`, else a default text), published advisories, the report
  button, and for maintainers and reporters the private list.
- `/<owner>/<repo>/security/advisories/<ID>`: the advisory, inside the
  repository page like an issue.
- `/advisories`: every published advisory on the instance.
- `/advisories.json`: OSV records, newest first; `?since=` (RFC 3339 or ms,
  on `modified`) and `?limit=` (at most 500).
- `/advisories/<ID>.json` and `/api/repo/<owner>/<repo>/security/advisories/<ID>/osv.json`:
  one OSV 1.6 record. `/advisories/<ID>` redirects to the repository page.
- `/security-researchers` and `/api/security-advisories/hall-of-fame`.

The instance-wide surfaces (`/advisories`, `/advisories.json` and
`/advisories/<ID>.json`) read the stored public view, so they re-check that
each advisory's repository is still public (`_still_public`: the alias's
backing node, then `_repo_is_private`, failing closed): a repository made
private after publishing takes its advisories, and its name, off them at once.
The researchers page counts credits only and names no repository.

OSV mapping: `summary` is the title, `details` the description, `aliases` the
CVE, `severity` the vector (`CVSS_V3`/`CVSS_V4`),
`database_specific.severity` the level and `cwe_ids` the CWEs. A package entry
becomes an `ECOSYSTEM` range from the affected range (`>=`/`>` is
`introduced`, `<` is `fixed`, `<=` is `last_affected`; without an upper bound
the first patched version is `fixed`); a folder entry becomes a `GIT` range on
the repository, fixed at the linked commit when it is a full hash. References
are the advisory (`ADVISORY`), the links (`WEB`), the fix pull request and
commit (`FIX`) and the repository (`PACKAGE`).

## Credits

Maintainers credit accounts as finder, reporter, analyst, coordinator,
remediation developer/reviewer/verifier, tool, sponsor or other. The reporter
is credited as reporter when they file (they consented by filing); a
maintainer crediting themselves is accepted at once; everyone else starts
`pending`. The credited account accepts or declines on the advisory page or in
**Security credits** on their own profile
(`POST /api/security-advisories/credits/<name>`, only as that account). They
are told when the advisory is published, or when the credit is added if they
can already read it. Only accepted credits show publicly, in OSV `credits` and
on the profile (`GET /api/security-advisories/credits/<name>`; the owner also
sees pending and declined ones). That list drops advisories whose repository
is now private or gone, like the feed and the researchers page, for every
caller including the credited account.

The researchers page ranks accepted credits on published (not withdrawn)
advisories by points: critical 10, high 6, moderate 3, low 1, unrated 1. It is
cached for five minutes per process and refreshed on any publish or decision.
A credit decision or a recorded ledger commit changes the public view (OSV
`credits`), so it moves the view's `updatedAt` and the row's `updated_at` to
now: OSV `modified` advances and scanners polling `?since=` see the change.

## Repository API

`/api/repo/<owner>/<repo>/security`:

| Method and path | Who | Does |
| --- | --- | --- |
| `GET` | readers | overview: published list, the viewer's private list, `canReport`, `reportingEnabled` |
| `POST /settings {"reporting":bool}` | admin | reporting switch |
| `POST /advisories {fields}` | verified account | report (triage) or, for a maintainer, a draft |
| `GET /advisories/<ID>` | members; anyone once published | record, comments, credits, permissions, ledger |
| `POST /advisories/<ID>` `edit`, `comment` | members (edit by state) | |
| `POST /advisories/<ID>` `state`, `collaborator`, `credit`, `prepare`, `sign`, `ledger-retry` | maintainers | |

## Storage and operations

Tables (migration 0277): `security_advisories`, `security_advisory_members`,
`security_advisory_comments`, `security_advisory_credits`,
`security_advisory_settings`. All carry `repo_bi`, so a repository delete
purges them (`_REPO_SCOPED_TABLES`), and all are in the re-key inventory
([REKEY.md](../REKEY.md)). Upstream `origin/main` ends at 0275 and the
deployed instance branch at 0276, so this is 0277 on both; whichever of two
branches lands second renumbers and re-stamps the schema fingerprint.

The access record that `context()` builds is passed around as `ctx` and is read
by key only (`ctx["admin"]`, never `ctx.get(...)`): the
`test_host_js_surface` inventory takes any `ctx.<attr>` chain in `app/src` to
be a Durable Object state, and the deploy agent's gate 2 refused
`ceb51dfce0b4` on 2026-10-20 because `ctx.get` is not on one. `context()`
always sets every key, so subscripting is safe.

## Not built yet

- CVE requests through a CNA.
- Temporary private forks for fix development (a fix branch or pull request is
  linked instead).
- Dependency alerts from the published feed.
- Signed credit acceptance in the ledger file (acceptances live in the
  database and the OSV export).
