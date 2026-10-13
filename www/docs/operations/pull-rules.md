# Pull request rules

Teams can set requirements that every pull request must meet: tests,
documentation, size, commit message format, secrets, protected paths, and a
required AI model. Every open pull request is checked against the rules. The
pull request page lists each rule, whether it passes, what is wrong, and how to
fix it.

Each rule either **blocks** the merge or only **flags** the pull request. A
blocking rule cannot be waived. The owner's review waiver does not reach it,
because the repository host that runs the merge evaluates the rules again on
the exact commits being merged.

Rules come from two places:

| Scope | Where the rules live | Who changes them |
| --- | --- | --- |
| Repository | `.forkmesh/pull-rules.json` committed on the base branch | Anyone who can push to that branch. Use **Settings → Pull request rules** on the repository page, which commits the file to the default branch. |
| Organization | Stored on forkmesh.com and applied to every repository the organization links | Organization owners and admins: **Organizations → (your organization) → Pull request rules**. Every member can read them. |

Both sets apply. A pull request is judged by the repository rules on its own
**base** commit, never by its head, so a pull request cannot loosen the rules
it is judged by.

## Rules per branch

Every rule takes an optional `branches` list of base-branch patterns. `*`
matches within one path segment and `**` matches across segments:
`["main", "release/*"]` covers `main` and `release/2.0`, but not
`release/2.0/hotfix`. An empty list covers every base branch.

## The rules file

```json
{
  "schema": "forkmesh-pull-rules-v1",
  "rules": [
    { "id": "tests", "kind": "tests", "branches": ["main"] },
    { "id": "docs", "kind": "docs", "paths": ["api/**"], "enforcement": "warn" },
    { "id": "small", "kind": "size", "maxFiles": 40, "maxLines": 800 },
    { "id": "commits", "kind": "commits", "subjectPattern": "^(feat|fix|docs|chore)(\\(.+\\))?: ", "maxSubjectLength": 72 },
    { "id": "secrets", "kind": "secrets" },
    { "id": "workflows", "kind": "paths", "paths": [".github/workflows/**", "deploy/**"] },
    { "id": "opus", "kind": "model", "models": ["claude-opus-5-5", "gpt-5.6-sol"] }
  ]
}
```

Fields every rule takes:

| Field | Rule |
| --- | --- |
| `id` | Lowercase letters, digits and dashes, up to 48 characters, unique in the file. |
| `kind` | One of the kinds below. |
| `title` | Optional label shown on the pull request (up to 120 characters). |
| `branches` | Optional base-branch patterns, up to 16. |
| `enforcement` | `block` (the default) or `warn`. |
| `suggestion` | Optional fix text shown when the rule fails (up to 600 characters). A clear default is shown otherwise. |

| Kind | What it checks | Fields |
| --- | --- | --- |
| `tests` | When a changed file matches `paths`, at least one changed file must match `testPaths`. | `paths` (default: common source extensions such as `*.go`, `*.py`, `*.ts`), `testPaths` (default: `**/tests/**`, `**/test/**`, `*_test.*`, `test_*`, `*.test.*`, `*.spec.*`, `*Test.*` and similar). |
| `docs` | When a changed file matches `paths`, at least one changed file must match `docsPaths`. | `paths` (same default), `docsPaths` (default: `**/docs/**`, `*.md`, `*.rst`, `*.adoc`). |
| `size` | The pull request changes at most `maxFiles` files and `maxLines` added plus deleted lines. | `maxFiles`, `maxLines` (at least one). |
| `commits` | Every non-merge commit subject matches `subjectPattern` and is at most `maxSubjectLength` characters long. | `subjectPattern` (an RE2 regular expression), `maxSubjectLength` (at least one). |
| `secrets` | No added line looks like a private key, an AWS, GitHub, Slack, Anthropic, OpenAI, Google or Stripe credential, or matches one of `patterns`. | `patterns` (up to 16 extra regular expressions), `paths` (only scan these). |
| `paths` | No changed file matches `paths`. | `paths` (required). |
| `model` | Every commit was written by one of `models`, proven as described below. | `models` (1–16 model ids). |

Path patterns without a `/` match a file name at any depth (`*.go`). Patterns
with a `/` match from the repository root (`docs/**`, `**/docs/**`).

Regular expressions use RE2 syntax: no lookaround, backreferences or possessive
repeats. A rules file that does not parse **locks every merge it covers** until
it is fixed. Saving rules on the website checks them first.

## The required AI model, and why it cannot be forged

A commit trailer such as `Co-Authored-By` or `ForkMesh-Agent:` is text anyone
can type. The `model` rule ignores it. It accepts a commit only when the
commit carries a `ForkMesh-Attestation` trailer that verifies end to end:

1. When a ForkMesh-operated (pool) runner claims an agent job, forkmesh.com
   signs a **model grant** with its router key. The grant names the runner and
   its public key, the repository, the agent branch, the provider, the model
   the job runs (Opus 5.5 when a Claude job names none), the lease and its
   lifetime. Personal runners, mirrors and desktops never receive one.
2. When the run finishes, the runner daemon rewrites each new commit with two
   trailers: `ForkMesh-Agent: <provider>/<model>` and `ForkMesh-Attestation:
   <grant>.<signature>`. The signature is made with the runner's own node key
   over the commit's tree, its parents, its message, and the grant. The agent
   process runs sandboxed as a different user and never holds that key.
3. At merge time the repository host checks every commit the pull request adds.
   The grant must verify under the router key the host already trusts. The
   runner signature must verify under the key the grant names. The model must
   be on the rule's list.

What this means in practice:

- Editing, amending or rebasing an attested commit by hand changes its tree or
  parents, so the signature no longer verifies. Copying the trailer to another
  commit fails the same way.
- A grant for one model cannot vouch for another: the model is inside the
  router-signed grant.
- A merge commit that only brings the base branch in (the **Update branch**
  button, or a clean `git merge main`) passes without an attestation. The rule
  checks that its tree is exactly the automatic merge of its parents, so a
  merge commit that sneaks in its own changes fails.
- Commits written by people, on a desktop, or on a personal runner fail the
  rule. That is the point: nobody outside the ForkMesh runner fleet holds a
  key that can produce the proof.
- The proof says which model the runner **ran**, through the provider's own
  command-line tool. It cannot see inside the provider's service.

To meet the rule, start the change as an agent on the website or a phone and
choose an allowed model. The job runs on a ForkMesh runner, and the pull
request it opens carries attested commits.

## Where the rules are enforced

| Step | What happens |
| --- | --- |
| Pull request page | The repository host evaluates both rule sets on the current base and head and returns each rule's result. The page shows the results and locks the merge button while a blocking rule fails. |
| Merge request on forkmesh.com | When the repository's organizations set rules, forkmesh.com asks the host to evaluate them on the exact pinned commits before it queues the merge. It refuses with `rules_required` if a blocking rule fails or no host can check them. |
| Merge on the repository host | The Go merge executor evaluates `.forkmesh/pull-rules.json` from the pinned base commit and refuses with `rules_required`. A headless Python mirror cannot verify signatures, so it refuses every merge into a base that has a rules file. |

Rules gate merges made through ForkMesh (the website and the phones). Someone
who can push straight to the base branch, or merge on their own desktop, is
not stopped by them. That is the same limit [signed required
checks](required-checks.md) have.

## Related

- [Signed required checks](required-checks.md)
- [Organization roles](../security/organization-roles.md): `organization.pull_rules.manage`
