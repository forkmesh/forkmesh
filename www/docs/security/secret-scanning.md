# Secret scanning before replication

A ForkMesh node copies every object it receives to the mirrors, forks,
clones, and backups that replicate the repository. **Nobody can recall a
pushed object.** Quarantine and history rewrites stop future serving on nodes
you control. They cannot erase copies that already exist elsewhere. So the
only reliable remediation for a leaked credential is to rotate it, and the
only reliable prevention is to stop it before the first remote receives it.

Every boundary below prints the same permanence warning whenever it reports
a finding.

## One scanner contract

`tools/secret_gate.py` is the contract (`forkmesh-secret-scan-v1`). It uses
only the standard library, so it runs as `python3 -I` beside the gateway
tools. `python3 tools/secret_gate.py contract` prints it. It defines:

- **Rules.** 19 patterns: GitHub classic and fine-grained tokens, AWS access
  and temporary keys, Slack, OpenAI, Anthropic, Stripe secret and restricted
  keys, Google API keys, OAuth tokens and client secrets, SendGrid, Twilio,
  npm, HashiCorp Vault, Cloudflare API tokens, PEM private keys, and a
  generic password/token assignment. Every rule also carries rotation
  guidance for its issuer.
- **Placeholder filter.** The generic assignment rule skips values that
  advertise themselves as placeholders ("example", "changeme", `${VAR}`,
  `{{ }}`, `$VAR`, and similar). Provider-prefixed rules have no filter.
- **Inline ignore marker.** A `forkmesh-secret-scan:ignore-line` comment on
  a line skips matches on it. The marker covers a whole logical line, so a
  marker at the end of a backslash-continued command applies to every
  physical line in it. An organization can refuse markers.
- **Value-free findings.** A finding names the rule, path, line, Git blob id,
  and a fingerprint. The fingerprint is derived from the rule, path, and blob
  id, never from the matched value, so no report, audit record, or evidence
  file holds the value or a digest of it.

The desktop scanner (`RepoSecurity::secretPatterns()`) carries the same
patterns and placeholder markers. `app/tests/test_secret_gate.py` fails if
they drift apart.

## Where it runs

| Boundary | What happens | How |
| --- | --- | --- |
| Local commit | Blocks or warns on added lines before the commit exists | `secret_gate.py install-hooks` (pre-commit) |
| Local push | Blocks or warns on every object the push would send, before the remote receives any of them. This catches commits made with `--no-verify`. | `secret_gate.py install-hooks` (pre-push) |
| Desktop push | Blocks with a review dialog | `RepoSecurity::findSecretsInPush` |
| Review diff | Grades the added lines of a patch. The desktop's branch review, Source Control and agent Code diffs show a red banner above the diff listing each match, with a button that appends the ignore marker to that line | desktop `findSecretsInDiff`, `SecretDiffBanner`, `secret_gate.py scan-diff` |
| Node receipt (HTTPS and SSH gateways) | Refuses the push before the node stores anything; Git discards its receive quarantine | `secrets` in the [node operator policy](../operations/node-operator-policy.md) |
| Sync and mirror intake | Detection is **after replication**. `enforce` reports the repository, quarantines it under `block`, and returns the response plan below. | `node_operator_policy.py enforce` |
| Public security report | Reports findings without values | `tools/security_scan.py` |

The desktop's diff banner writes the marker with `RepoSecurity::markFindingSafe`,
the same call as the push dialog's "Mark safe & commit". It edits the
checkout the diff was read from (the branch's worktree, the working tree, or
the agent's worktree) and leaves the change uncommitted. It refuses a line
that no longer holds a match and a file type with no comment syntax. A
branch with no checkout on this machine shows the banner with the buttons
disabled.

Install the client hooks once per checkout:

```sh
python3 tools/secret_gate.py install-hooks --repo . \
  --repository alice/app --org-policy ~/.config/forkmesh/secret-policy.json
```

The installer never overwrites a hook it did not write. It lists preserved
hooks in `preservedForeignHooks`.

## Organization and repository policy

The organization policy is a file the operator or organization owns. With no
policy, every rule blocks.

```json
{
  "schemaVersion": 1,
  "mode": "block",
  "rules": {"generic-secret-assignment": "warn"},
  "customRules": [
    {"id": "custom-acme-key", "title": "Acme deploy key",
     "pattern": "acme_live_[A-Za-z0-9]{32}", "severity": "critical",
     "action": "block", "rotation": "Revoke it in the Acme console."}
  ],
  "allowlist": [
    {"paths": ["tests/fixtures/fake/*"], "rules": ["*"],
     "reason": "synthetic credentials for parser tests"}
  ],
  "honorIgnoreMarkers": true,
  "allowRepositoryAllowlist": false,
  "allowRepositoryCustomRules": false,
  "overrides": {
    "allowedSignersPath": "/etc/forkmesh/secret-override-signers",
    "maxLifetimeDays": 90
  }
}
```

- `mode` and `rules` take `off`, `warn`, or `block`. A rule without an entry
  uses `mode`.
- A repository may commit `.forkmesh/secret-scan.json` with `mode`, `rules`,
  `customRules`, and `allowlist`. It can only **tighten** a rule, from off to
  warn or from warn to block. Whoever pushes controls that file, so its
  allowlist and custom patterns count only when the organization sets
  `allowRepositoryAllowlist` or `allowRepositoryCustomRules`. The scanner
  merges the repository policy from the pushed commits and from the stored
  default branch. Deleting the file in a push does not loosen anything.
- Allowlist entries need a reason and cannot cover every path. Allowlisted
  findings are still counted.
- Invalid regular expressions, patterns that match empty text, unknown rule
  ids, and unknown fields are refused.

## Signed overrides

A block can be lifted only by an override that an allowed signer signed with
an SSH key. The override names the exact finding fingerprints. A warning
needs no override.

```sh
python3 tools/secret_gate.py sign-override --key ~/.ssh/id_ed25519 \
  --signer alice@example.com --repository alice/app --days 30 \
  --reason "synthetic token for the webhook parser test" \
  --fingerprint <fingerprint from the blocked output> \
  > .forkmesh/secret-overrides/webhook-parser.json
```

The allowed signers file uses the OpenSSH format, restricted to the override
namespace:

```text
alice@example.com namespaces="forkmesh-secret-override" ssh-ed25519 AAAA...
```

Commit the override next to the fixture. It travels with the repository, so
every node that shares the organization's signers accepts the same push.

An override is **ignored**, and the reason is printed, if any of these hold:

- the signature does not verify for the named signer
- it names another repository
- it has expired, or its lifetime is longer than `maxLifetimeDays`
- the policy has no `allowedSignersPath`

Accepted overrides are written to the node audit log (`admit-push-secrets`)
with the signer, reason, expiry, and fingerprints.

## Measured accuracy

`conformance/secrets/v1/corpus.json` is the maintained corpus. It is
generated from the readable `build_corpus.py`, and every credential in it is
synthetic. `python3 tools/secret_gate.py corpus` runs the corpus through the
publication, review-diff, pre-commit, and git-objects boundaries.
Git-objects is the boundary that pre-push, node receipt, and stored-data
enforcement share.

| Boundary | Malicious | Benign | False negatives | False positives |
| --- | --- | --- | --- | --- |
| publication | 28 | 19 | 3 (10.7%) | 1 (5.3%) |
| review-diff | 28 | 19 | 3 (10.7%) | 1 (5.3%) |
| pre-commit | 28 | 19 | 3 (10.7%) | 1 (5.3%) |
| git-objects | 28 | 19 | 3 (10.7%) | 1 (5.3%) |

The bounds are at most 15% false negatives and 10% false positives. Every
misclassified case is a documented known limitation:

- A JSON object key (`"api_key": "..."`). The closing quote between the name
  and the colon defeats the assignment pattern.
- A token split by string concatenation.
- A key wrapped in base64.
- AWS's published documentation key id, which provider rules flag because
  they have no placeholder filter.

The test suite fails if any other case is misclassified, if a rule has no
malicious case, or if a scanner change fixes a known limitation without the
corpus saying so. To change the corpus, edit `build_corpus.py`, run it, and
commit both files.

## When a secret is found after replication

`enforce` output (and `python3 tools/secret_gate.py respond --report
<scan.json>`) returns this plan with per-rule rotation steps:

1. **Containment.** Quarantine the repository on every node you operate so
   it stops being served there. Remove the value from the default branch in a
   new commit. Pause automation that could reuse the credential.
2. **Rotation.** Revoke or roll the credential at its issuer first, deploy
   the replacement from a secret store, and review the issuer's audit log
   since first publication.
3. **Notification.** Tell the repository owner and security contact. Share
   the evidence record (rule, path, blob id, fingerprint), never the value.
   Follow third-party disclosure and breach-notification duties, and record
   the notice in the node case log.
4. **Residual risk.** Clones, forks, mirrors, backups, and caches that
   already received the objects keep them. Treat the secret as compromised
   until it is rotated.

## Telemetry

Everything is counted without values:

- **Scan reports.** Hooks and CLI commands carry per-boundary `counts`
  (`block`, `warn`, `overridden`, `allowlisted`), `invalidOverrides`, and
  skipped-file counts.
- **Node counters.** `node_operator_policy.py status` reports `secrets`:
  - `blockedPushes`, `warnedPushes`, and `warnings`
  - `overriddenFindings` and `allowlistedFindings`
  - `invalidOverrides`
  - `storedDetections`, which counts detections after replication
- **Audit log.** The hash-chained node audit log records `reject-push`
  (rule `secret`), `admit-push-secrets`, and `secret-detected-stored`.

## Not covered yet

- **Worker surfaces.** Browser uploads, provider imports, and other
  Worker-hosted writes do not run the contract yet. Content from those paths
  is scanned only when it reaches a node's `enforce`, which is after
  replication.
- **Desktop.** The desktop push dialog's "Push anyway" is an unsigned local
  bypass. The desktop does not read organization policy or signed overrides.
- **Upstream fetches.** The Go daemon's fetches are not scanned before they
  land. Run `enforce` after sync.
- **Scan limits.** Blobs larger than 1 MiB and binary blobs are skipped and
  counted, not scanned. Encoded or split secrets are missed (see Measured
  accuracy).
- **Owner notification.** It is a documented step, not an automatic message.
