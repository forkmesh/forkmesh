# Signed required checks

A repository can require status checks before a pull request merges on a
mirror node. Each check result is an Ed25519-signed attestation from a runner
the repository trusts. The gate never depends on a particular runner service,
on where a result was stored, or on who committed it. Forged, stale, replayed,
and unauthorized results cannot satisfy it.

This page covers the merge gate the Go mirror node (`forkmesh-mirror-node`)
enforces for web merges (`merge-pull`). It is one slice of issue #496, signed
delivery pipelines. Approval holds, protected-push enforcement, and deployment
events are separate work.

## Turning the gate on

Commit `.forkmesh/required-checks.json` to the base branch:

```json
{
  "schema": "forkmesh-required-checks-v1",
  "branches": ["main"],
  "maxAgeSeconds": 604800,
  "checks": [
    {
      "name": "CI tests",
      "signers": ["<runner public key, 43-character base64url>"]
    }
  ]
}
```

| Field | Rule |
| --- | --- |
| `schema` | Exactly `forkmesh-required-checks-v1`. |
| `branches` | Base branches the gate covers. An empty list covers every base. |
| `maxAgeSeconds` | From 60 to 31,622,400. Older results do not count. |
| `checks` | 1–32 checks with distinct names (1–128 bytes, no control characters, no surrounding spaces). |
| `signers` | 1–16 distinct runner keys for each check. Keys must be canonical, prime-order Ed25519 keys (FM-SIG-001). |

The file must have exactly these keys. Any parse or validation failure locks
merges into the covered branches. It never switches the gate off.

The mirror reads the policy from the **base commit being merged into**. It
never reads the pull request's head, so a proposal cannot loosen its own gate.
To change or remove the gate, push to the base branch.

To get a runner key, run `forkmesh-mirror-node --config <path> --public-key` on
the runner node.

## Reporting a result

A runner signs one attestation per check run:

```sh
printf '%s' '{"schema":"forkmesh-check-v1","owner":"forkmesh","repository":"forkmesh","commit":"<head sha>","check":"CI tests","conclusion":"success","runId":"run-1234"}' \
  | forkmesh-mirror-node --config /etc/forkmesh/mirror-node.json --sign-check-attestation
```

The signer adds `issuedAt` from its own clock, plus `signer` and `signature`
from its node identity. A caller cannot supply any of the three. `conclusion`
is one of `success`, `failure`, `cancelled`, or `skipped`. Only `success`
satisfies a check.

Publish the signed JSON on the pull-request ledger branch (`forkmesh/pulls`) as
`.forkmesh/pulls/<N>/checks/<name>.json`. The file name must match
`[A-Za-z0-9][A-Za-z0-9._-]*\.json`. The mirror reads at most 256 files, each up
to 8 KiB.

### Signing bytes

The signature covers this FM-ENC-007 canonical string: fields joined by LF,
with no trailing LF.

```
forkmesh-check-v1
<owner, lowercase>
<repository>
<commit, lowercase hex>
<hex SHA-256 of the check name>
<conclusion>
<runId>
<issuedAt, decimal milliseconds>
```

## What satisfies the gate

For each required check, the mirror discards every attestation that:

- does not parse with exactly the documented keys, or whose signature does not
  verify (**forged**);
- names a different owner, repository, or commit than the merge request's
  owner, repository, and exact head commit (**replayed**);
- is signed by a key the policy does not list for that check, including a key
  trusted only for another check (**unauthorized**);
- is older than `maxAgeSeconds`, or dated more than five minutes in the future
  (**stale**).

Of the attestations left, the newest `issuedAt` wins. When two share a
timestamp, a non-success result beats a success. An old success replayed
beside a newer failure therefore cannot mask it. A newer successful re-run does
supersede an earlier failure. The merge proceeds only when every required
check's winning result is `success`.

Otherwise the executor records `checks_required` and moves no refs. The
dashboard explains the refusal. The owner's review waiver (`overrideReview`)
waives the peer-review gate only, never required checks.

## Limits and fail-closed cases

- **Patch-backed pulls.** They have no published head commit for a runner to
  attest, so a covered patch-backed pull cannot merge on the web. Merge it from
  the desktop client, or exclude its base in `branches`.
- **Headless Python mirrors.** `tools/headless_mirror_refresh.py` has no
  Ed25519 verifier. It refuses every merge whose base carries a policy, with
  `checks_required`, so it cannot become a way around the gate.
- **Organization aliases.** An attestation is bound to the owner name in the
  merge request. Sign it for the name the repository is merged under.

## Evidence

- `server/checks_test.go` covers forged, edited, rebound, unauthorized, stale,
  future-dated, and cross-repository, cross-owner, or cross-commit replayed
  results. It also covers supersession and ties, multi-check policies, malformed
  policies, weak Ed25519 keys (identity, small-order, mixed-order,
  non-canonical, negative zero), and the signer helper.
- `server/merge_test.go` (`TestMergeExecutorEnforcesSignedRequiredChecks`)
  starts from a policy pushed to `main`. It shows the merge refused with no
  checks, with the review waiver, and with forged, stale, and replayed results,
  then merged once a trusted runner's fresh success lands.
  `TestMergeExecutorFailsClosedOnAnInvalidChecksPolicy` shows that an invalid
  policy locks merges.
- `app/tests/test_pull_merge_executor.py` shows the headless executor failing
  closed.
