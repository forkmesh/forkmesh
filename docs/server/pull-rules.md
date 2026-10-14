# Pull request rules and model attestations

Repository and organization requirements checked against every pull request,
and the signed proof of which AI model wrote a commit. The user guide is
[`www/docs/operations/pull-rules.md`](../../www/docs/operations/pull-rules.md);
this page is the implementation.

## Code map

| Path | Responsibility |
| --- | --- |
| `server/pullrules.go` | `forkmesh-pull-rules-v1` parser (`ParsePullRulesPolicy`), evaluator (`evaluatePullRules`), merge gate (`pullRulesSatisfied`), the read-only `forkmesh.pull-rules-evaluate-v1` executor request |
| `server/agentattest.go`, `server/runner.go` | Router-signed model grant, runner commit attestation (`attestAgentCommit`, `attestCommits`) and verification (`verifyAgentCommit`) |
| `server/merge.go` | Routes evaluate requests; `executeMerge` refuses with `rules_required` after the required-checks gate |
| `tools/mirror_gateway.py` | `merge-preview` with `rules=1`, `pull`, `orgRules` asks the executor to evaluate |
| `tools/headless_mirror_refresh.py` | Python executor: fails closed (`rules_required`) on a base with `.forkmesh/pull-rules.json` |
| `app/src/pull_rules.py` | Organization rules (`org_pull_rules`, `GET/POST /api/orgs/<org>/pull-rules`), `GET /api/repo/<o>/<r>/pulls/<n>/rules`, `merge_gate` |
| `app/src/runner_leases.py`, `app/src/edge_routing.py` | Pool-runner claims carry `modelGrant` |
| `app/public/dashboard/js/*.js` | Rule builder, pull page display and merge lock, organization settings |

## Architecture

The node that hosts the repository is the only place with Git, so it is the
only rule evaluator. Two callers ask it:

1. **Display.** The pull page calls the relay's `/pulls/<n>/rules`. The relay
   loads the organization rules of every organization linking the repository
   (`org_repos` joined to `org_pull_rules`, combined with unique rule ids),
   and calls `merge-preview?rules=1&pull=<n>&orgRules=<base64url JSON>`. The
   gateway passes a `forkmesh.pull-rules-evaluate-v1` request to the merge
   executor command. The Go executor reads `pull.md` from the current
   `forkmesh/pulls` tip to learn the base branch, reads
   `.forkmesh/pull-rules.json` from the base commit, and evaluates both sets.
2. **Gate.** The Go executor evaluates the repository rules from
   `ExpectedBaseOid` against `ExpectedHeadOid` inside `executeMerge`.
   Organization rules never reach the merge request (its strict, digested
   format stays unchanged). Instead the relay runs the same evaluation on the
   pinned base and head before it queues the job (`merge_gate`), and refuses
   with 409 `rules_required` when a blocking rule fails or no host answers.
   Retries of an already queued request skip the gate; the job was admitted
   once.

Rule evaluation reads `git diff --numstat` and `--unified=0` from the merge
base to the head, and `rev-list head ^base` plus `cat-file --batch` for commits
(at most 1,000). Anything it cannot read becomes a failed rule, never a pass.

## Protocols and interfaces

Evaluate request (stdin of the merge executor command, at most 48 KiB):

```json
{"schemaVersion": 1, "type": "forkmesh.pull-rules-evaluate-v1",
 "owner": "alice", "repository": "project", "base": "<oid>", "head": "<oid>",
 "pullNumber": 7, "branch": "main", "orgRules": "<policy JSON text>"}
```

`pullNumber` wins over `branch`. The answer is `{"ok": true, "rules":
{available, branch, passed, blocked, rules: [{id, title, kind, source,
enforcement, status, problems, suggestion}]}}`, at most three problems per rule.

Model grant, signed with the router key (`runtime.sign_router`) and sent as
`modelGrant` (`base64url(message) "." base64url(signature)`):

```
forkmesh-agent-model-grant-v1
<runner> <runner public key> <owner> <repository> <refs/heads/agent/...>
<provider> <model> <lease id> <issuedAt ms> <expiresAt ms>
```

(one field per line). The relay issues it only to `scope='pool'` runners, and
only when the effective model is known: the job's model, or `claude-opus-5-5`
for a Claude job with none. A Codex job without a model gets no grant.

Commit attestation, the last line of the commit message:

```
ForkMesh-Agent: <provider>/<model>
ForkMesh-Attestation: <grant token>.<runner signature>
```

The runner signs `forkmesh-agent-commit-v1\n<tree>\n<parents, space
separated>\n<sha256 hex of the message up to the attestation line>\n<sha256
hex of the grant token>` with its node key. Verification needs the gateway's
`routerPublicKey` for the grant and the grant's runner key for the commit.

## Security

- The agent process cannot attest: it runs as `forkmesh-job`, and only the
  runner daemon holds the node key. The daemon checks that the grant names
  this runner, its key, the job's push ref, provider and model before it
  signs (`modelGrantFor`); otherwise commits stay unattested.
- A personal runner's operator is the job's own user, so personal runners get
  no grant. Desktops and mirrors never sign attestations.
- Attestations bind tree and parents, so rewriting history invalidates them.
  An unattested merge commit passes only if `git merge-tree --write-tree` of
  its two parents yields exactly its tree.
- The proof covers the `--model` the runner passed to the provider CLI, not
  the provider's service internals.

## Traps

- Organization rules travel to the node in the query string, so the relay
  caps them at 6 KiB (canonical JSON) and the gateway at the same size.
- The relay validates regular expressions with Python's `re` and rejects
  lookaround and backreferences; Go uses RE2. Anything RE2 still rejects makes
  the organization policy unreadable, which locks merges and says so on the
  pull page.
- `attestCommits` rewrites commits, so their ids change between the job's
  workdir and the pushed branch. A follow-up job clones the pushed branch
  again, so this never shows up as divergence.
- A rules file on a headless Python mirror locks web merges there (fail
  closed).

## Tests

Go `server/pullrules_test.go`; Python `app/tests/test_pull_rules.py`,
`test_runner_leases.py`, `test_mirror_gateway.py`,
`test_pull_merge_executor.py`, `test_repository_file_edits.py`.
