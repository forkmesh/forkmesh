# Terminology and voice guide

ForkMesh is a Git forge. People already know how repositories, branches, forks,
pull requests, and reviews work, so ForkMesh keeps those words and explains its
network ideas (mirrors, seeding, trusted maintainers) where they come up. It
does not rename Git workflows.

The machine-readable contract is [`terminology.json`](terminology.json). It holds
the approved glossary, the discouraged-term list, allowances, and compatibility
aliases. `tools/terminology_lint.py` checks primary surfaces against it.

## Approved glossary

| Say | Meaning | Instead of |
| --- | --- | --- |
| **repository** | A Git repository: its files, commits, branches, and tags, with its issues, pull requests, and releases. Use "repo" only in URLs, commands, and tight labels. | project (for source code) |
| **branch** | A named line of commits. The default branch is the one a clone checks out. | |
| **commit** | A recorded change, identified by its hash. | |
| **fork** | Your own copy of a repository under your account. Change it and open pull requests back to the original. | |
| **clone** | Download a repository with Git. | |
| **issue** | A tracked bug report, task, or request. | |
| **pull request** | A proposal to merge commits from one branch or fork into another, with discussion, checks, and reviews. "PR" is fine in compact labels and CLI output. | merge request, patch |
| **review** | An approval, a request for changes, or comments on a pull request. | peer approval |
| **mirror** | A complete copy of a repository on another host that stays in sync and can serve clones when the source is offline. | peer, seed node, mesh node |
| **seed** | The first push that fills a new mirror from an existing copy. After seeding, the mirror keeps itself in sync. | seeder |
| **trusted maintainer** | A person the owner trusts to merge, release, and change settings. Every mirror accepts their signed actions. | delegate |
| **host** | A computer that serves repositories: your machine, a server you run, or a ForkMesh-managed host. | mesh node |
| **desktop app** | The ForkMesh desktop application. Write "desktop app (Qt)" only in build instructions. | Qt client, Qt app |

## Voice guide

- **Lead with the Git task.** "Open a pull request", "Add a mirror", "Clone
  again". Name the button or tab the person will press.
- **Explain a network idea once, in place.** The first time a page mentions
  seeding, say what it does: "ForkMesh seeds the new mirror with a first push;
  after that it stays in sync."
- **Protocol terms stay in advanced detail.** Operator runbooks, protocol specs,
  and diagnostics may say node, relay, attestation, lease, or outbox. A primary
  journey (home page, onboarding, getting started, repository pages, mobile
  screens) uses the plain explanation below, or puts it next to the term.
- **Errors say what happened and what to do next,** in the same words as the
  surface: "The mirror merge could not reach the relay. Retry in a moment."
- **Never rename identifiers for wording.** Change labels, not routes, JSON
  fields, event kinds, or flags. See [compatibility aliases](#compatibility-aliases).

## Protocol terms and plain explanations

| Protocol term | Plain explanation for primary surfaces |
| --- | --- |
| node | A running copy of ForkMesh (desktop app, server, or phone) that signs and syncs for an account. |
| relay | The ForkMesh web service that routes requests to healthy mirrors. It is not the canonical Git store. |
| refs attestation | The signed list of branch and tag commits a mirror must match before it can serve a repository. |
| signed repository proof | A host's signature over the branch and tag commits it serves. |
| commit DAG | Commit history: each commit points at its parents. |
| CRDT / fold | How signed issue and pull request events merge into one current state without conflicts. |
| lease | A short-lived claim that stops two hosts from processing the same issue or pull request at once. |
| outbox | The queue of signed events waiting to be delivered to other hosts. |
| opaque replica | An encrypted copy of a private repository that a host stores without being able to read it. |
| federation | Sharing repository activity with Mastodon and other ActivityPub servers. |

## Discouraged terms

`discouragedTerms` in the contract lists each term's pattern, its replacement,
and whether it must be **replaced** or may appear **explained**:

- Replace: merge request, peer approval, peer-to-peer / P2P, Qt client / Qt app
  / Qt desktop node, delegate, seeder / seed node, mesh node.
- Explain: attestation, refs pin / sigrefs, DAG, CRDT, gossip, lease, outbox.

Run the lint before changing user-facing copy:

```sh
python3 tools/terminology_lint.py            # primary surfaces; exits 1 on a finding
python3 tools/terminology_lint.py --extended # also report desktop, all pages, and all docs
python3 tools/terminology_lint.py --json     # counts for the measurement review
python3 tools/terminology_lint.py path/to/file.html
```

The lint reads only visible text: HTML text and `aria-label`, `title`,
`placeholder`, and `alt` values; Markdown outside code and link targets; and
sentence-like string literals in JavaScript and C++. Comments, code,
and URLs are skipped.

When a term is right in context, either:

- add an entry to `allowances` with the file, term id, and a `reason`. For an
  explain-category term, also give the exact `explanation` text. The lint fails
  if that text disappears from the file, and fails on an allowance that no
  longer matches anything.
- put `terminology: allow=<term-id>` in a comment on the same line or the line
  above.

Allowances exist for exact source names (GitLab calls them merge requests)
and personal biographies. They are not a way around the guide.

## Compatibility aliases

Wording changes never break scripts or links:

- **Routes.** Familiar forge URLs redirect permanently (308) to the repository
  page and keep the query string. `/OWNER/REPOSITORY/pull/12`,
  `/pull-requests/12`, `/merge_requests/12`, `/merge-requests`, and GitLab's
  `/-/merge_requests/12` go to `/OWNER/REPOSITORY/pulls/12`. GitLab-scoped tabs
  such as `/-/issues/5` and `/-/tree/main` drop the `/-/`. The older
  `/dashboard?section=repos` and `/referrals` redirects stay too.
- **Documentation URLs.** A renamed page keeps its old URL. The desktop app
  guide still lives at `/docs/qt-client/`.
- **APIs, events, config, and CLI.** Route names, JSON fields (`pull`, `pulls`,
  `repoKey`), signed event kinds, config keys, and command-line flags are never
  renamed for wording. If a clearer name is ever needed, it is added next to
  the old one, and both are accepted indefinitely.

Every alias in `compatibilityAliases` is a one-way door. Redirects are cached by
browsers, so an entry is never removed.

**Rollback.** A copy change is reverted by reverting its commit. Aliases stay,
because removing one would break links that already point at it.

## Measurement and closure gate

- **Discouraged-term count.** `python3 tools/terminology_lint.py --json --extended`
  reports `discouragedTermCount` and `countsByTerm`. Primary surfaces must stay
  at zero; record the extended count at each monthly review and track it down.
- **Alias usage.** Measure 308 responses on alias paths from Cloudflare zone
  analytics. The Worker adds no logging for aliases, consistent with its
  no-logging design.
- **Support vocabulary.** At the monthly review, sample support threads and bug
  reports and note which discouraged terms people used or asked about.
- **Task comprehension.** Content tests cannot prove this. For every
  five-person cohort, include maintainers, contributors, and operators. Without
  a glossary, each person tries to: fork a repository and open a pull request;
  review and merge a pull request; add a mirror and tell when seeding has
  finished; and say who can merge in a repository. A person passes by finishing
  all four without asking what a term means. A cohort passes at 4/5 or better.
  Record role, surface, pass/fail, the terms that caused hesitation, date,
  facilitator, and a link to consented research notes. Do not mark this gate
  complete with blank, synthetic, or inferred results.

## Linked automated evidence

- Glossary, lint behavior, primary-surface audit, failure paths, and aliases:
  `app/tests/test_terminology_contract.py`
- Repository deep links the aliases land on: `app/tests/test_repo_deeplink_frontend.py`
- Legacy page redirects: `app/tests/test_referrals.py`
- Documentation hosting and links: `www/tests/test_docs_library.py`

The `terminology-contract` quality gate runs the contract test in the `core` and
`release` profiles. Automated checks support closure; they do not replace a
human audit, the comprehension cohort, or production alias-usage evidence.
