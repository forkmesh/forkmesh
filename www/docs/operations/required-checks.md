# Signed required checks

A repository can require status checks before a pull request merges on a
mirror node. Each result is an Ed25519-signed attestation from a runner the
repository trusts, so the gate does not depend on any runner service or
storage location. Forged, stale, replayed, and unauthorized results cannot
satisfy it. The Go mirror node (`forkmesh-mirror-node`) enforces it for web
merges (`merge-pull`).

## Policy

Commit `.forkmesh/required-checks.json` to the base branch:

```json
{
  "schema": "forkmesh-required-checks-v1",
  "branches": ["main"],
  "maxAgeSeconds": 604800,
  "checks": [
    { "name": "CI tests", "signers": ["<runner public key, 43-character base64url>"] }
  ]
}
```

| Field | Rule |
| --- | --- |
| `schema` | Exactly `forkmesh-required-checks-v1`. |
| `branches` | Base branches covered; empty covers every base. |
| `maxAgeSeconds` | 60 to 31,622,400. Older results do not count. |
| `checks` | 1–32 checks with distinct names (1–128 bytes, no control characters). |
| `signers` | 1–16 canonical, prime-order Ed25519 runner keys per check. |

Unknown keys or any validation failure lock merges into covered branches; a
broken file never disables the gate. The policy is read from the **base
commit**, never the pull request's head. A runner prints its key with
`forkmesh-mirror-node --config <path> --public-key`.

## Reporting a result

```sh
printf '%s' '{"schema":"forkmesh-check-v1","owner":"example","repository":"demo","commit":"<head sha>","check":"CI tests","conclusion":"success","runId":"run-1234"}' \
  | forkmesh-mirror-node --config /etc/forkmesh/mirror-node.json --sign-check-attestation
```

The signer adds `issuedAt`, `signer`, and `signature`; callers cannot supply
them. `conclusion` is `success`, `failure`, `cancelled`, or `skipped`; only
`success` satisfies a check. Publish the JSON on the `forkmesh/pulls` ledger
branch as `.forkmesh/pulls/<N>/checks/<name>.json` (name matching
`[A-Za-z0-9][A-Za-z0-9._-]*\.json`; at most 256 files of 8 KiB each).

The signature covers these fields joined by LF, no trailing LF:

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

## Evaluation

For each required check the mirror discards attestations that fail to parse or
verify, name another owner, repository, or head commit, are signed by a key not
listed for that check, are older than `maxAgeSeconds`, or are dated more than
five minutes ahead. The newest remaining `issuedAt` wins; on a tie a
non-success beats a success. The merge proceeds only when every check's winner
is `success`; otherwise the executor records `checks_required` and moves no
refs. The review waiver (`overrideReview`) never waives required checks.

## Limits

- Patch-backed pulls have no published head to attest and cannot merge on the
  web under a covering policy.
- `tools/headless_mirror_refresh.py` has no Ed25519 verifier and refuses every
  merge whose base carries a policy.
- An attestation is bound to the owner name in the merge request.

Tests: `server/checks_test.go`, `server/merge_test.go`, and
`app/tests/test_pull_merge_executor.py`.
