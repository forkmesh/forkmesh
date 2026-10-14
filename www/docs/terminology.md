# Terminology and voice guide

ForkMesh keeps standard Git forge words and explains its network ideas in
place. The contract is [`terminology.json`](terminology.json);
`tools/terminology_lint.py` checks primary surfaces against it.

## Approved glossary

| Say | Meaning | Instead of |
| --- | --- | --- |
| **repository** | A Git repository with its issues, pull requests and releases. | project |
| **branch** | A named line of commits. | |
| **fork** | Your own copy of a repository. | |
| **pull request** | A proposal to merge commits, with discussion and reviews. | merge request, patch |
| **review** | Approval, change request or comments on a pull request. | peer approval |
| **mirror** | A synced full copy on another host that can serve clones. | peer, seed node, mesh node |
| **seed** | The first push that fills a new mirror. | seeder |
| **trusted maintainer** | A person whose signed actions every mirror accepts. | delegate |
| **desktop app** | The ForkMesh desktop application. | Qt client |

## Protocol terms and plain explanations

| Protocol term | Plain explanation for primary surfaces |
| --- | --- |
| node | A running copy of ForkMesh that signs and syncs for an account. |
| relay | The web service that routes requests to healthy mirrors. |
| refs attestation | The signed branch and tag list a mirror must match. |
| signed repository proof | A host's signature over the refs it serves. |
| commit DAG | Commit history. |
| CRDT / fold | How signed events merge into one state without conflicts. |
| lease | A short claim that stops two hosts doing the same work. |
| outbox | Signed events waiting for delivery. |
| opaque replica | An encrypted copy a host stores but cannot read. |
| federation | Sharing activity with ActivityPub servers. |

## Rules

- Lead with the Git task; explain a network idea once, where it appears.
- `discouragedTerms` marks each term **replace** or **explain**. Exceptions go in
  `allowances` (with a `reason`) or a `terminology: allow=<term-id>` comment.
- Run `python3 tools/terminology_lint.py` (add `--extended` or `--json`).

## Compatibility aliases

Familiar forge URLs (`/pull/12`, `/merge_requests/12`, `/-/issues/5`) redirect
with 308. Routes, JSON fields, event kinds, config keys and flags are never renamed for wording; aliases are never removed. **Rollback.** Revert the copy change; aliases stay.

## Measurement

`--json --extended` reports `discouragedTermCount` (zero on primary surfaces).
Alias usage comes from Cloudflare zone analytics. Support vocabulary is sampled
monthly. For every five-person cohort of maintainers, contributors, and operators, test core tasks without a glossary. A cohort passes at 4/5 or better. Do not mark this gate complete with blank, synthetic, or inferred results.
Evidence: `app/tests/test_terminology_contract.py`.
