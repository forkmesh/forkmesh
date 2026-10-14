# Pull request rules

Repositories and organizations can set rules every pull request must meet:
tests, documentation, size, commit message format, secrets, protected paths,
and a required AI model. The pull request page lists each rule, whether it
passes, and how to fix it. Each rule either **blocks** the merge (no waiver
reaches it) or only **flags** the pull request (`warn`).

| Scope | Where the rules live | Who changes them |
| --- | --- | --- |
| Repository | `.forkmesh/pull-rules.json` on the base branch | Anyone who can push to that branch (or **Settings → Pull request rules**) |
| Organization | Stored by the hosted service, applied to every linked repository | Organization owners and admins |

Both sets apply. A pull request is judged by the rules on its **base** commit,
never its head, so it cannot loosen its own rules.

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
    { "id": "model", "kind": "model", "models": ["claude-opus-5-5"] }
  ]
}
```

Every rule takes `id` (lowercase, digits, dashes, up to 48 characters, unique),
`kind`, and optional `title`, `branches` (up to 16 base-branch patterns; `*`
matches within a segment, `**` across segments; empty means all),
`enforcement` (`block` default, or `warn`), and `suggestion` (fix text).

| Kind | Checks | Fields |
| --- | --- | --- |
| `tests` | A change to `paths` includes a change to `testPaths`. | `paths`, `testPaths` (sensible defaults) |
| `docs` | A change to `paths` includes a change to `docsPaths`. | `paths`, `docsPaths` |
| `size` | At most `maxFiles` files and `maxLines` changed lines. | `maxFiles`, `maxLines` |
| `commits` | Every non-merge subject matches `subjectPattern` and fits `maxSubjectLength`. | RE2 pattern, length |
| `secrets` | No added line looks like a private key or common cloud/API credential, or matches `patterns`. | `patterns`, `paths` |
| `paths` | No changed file matches `paths`. | `paths` |
| `model` | Every commit carries a verified model attestation for one of `models`. | `models` (1–16) |

Patterns without `/` match a file name at any depth; patterns with `/` match
from the root. Regular expressions use RE2. A rules file that does not parse
locks every merge it covers.

## Model attestation

Commit trailers such as `Co-Authored-By` are ignored. The `model` rule accepts
a commit only with a verified `ForkMesh-Attestation` trailer:

1. When a hosted pool runner claims an agent job, the router signs a **model
   grant** naming the runner key, repository, branch, provider, model, and
   lease. Personal runners, mirrors, and desktops never receive one.
2. The runner daemon rewrites each new commit with `ForkMesh-Agent:
   <provider>/<model>` and `ForkMesh-Attestation: <grant>.<signature>`, signed
   with its node key over tree, parents, message, and grant. The sandboxed agent
   process never holds that key.
3. At merge, the host verifies the grant under the trusted router key, the
   commit signature under the granted key, and the model against the rule.

Editing, amending, or rebasing an attested commit breaks the signature. A merge
commit that only brings in the base passes if its tree equals the automatic
merge of its parents. The proof covers which model the runner ran, not the
provider's internals.

## Enforcement points

| Step | Behaviour |
| --- | --- |
| Pull request page | The host evaluates both rule sets on current base and head; a failing blocking rule locks the merge button. |
| Hosted merge request | Organization rules are evaluated on the pinned commits; refusal is `rules_required`. |
| Repository host merge | The Go merge executor evaluates the base's rules file and refuses with `rules_required`. A headless Python mirror cannot verify signatures and refuses every merge into a base with a rules file. |

Rules gate merges made through ForkMesh; a direct push to the base branch is
not stopped, the same limit as [signed required checks](required-checks.md).
Organization rule management needs `organization.pull_rules.manage`
([Organization roles](../security/organization-roles.md)).
