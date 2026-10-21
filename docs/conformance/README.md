# Conformance suites

`conformance/<suite>/v1/` holds versioned, machine-readable contracts: test
vectors, fixtures, ledgers and retained evidence. Each suite is shared by the
implementations it binds (the Python app, the Go daemon, the desktop, mobile)
and backs one public page under `www/docs/`. A version directory is frozen
once published; a breaking change adds `v2` beside it.

## Code map

| Suite | Contents | Checked or produced by | Public page |
| --- | --- | --- | --- |
| `availability-contract` | `model.json`, `fixture.json`, `generate_fixture.py` | `app/src/availability_contract.py`, `tools/forkmesh_availability_contract.py`; `app/tests/test_availability_contract.py` | `www/docs/availability-contract.md` |
| `availability-proof` | `model.json`, `fixture.json`, `generate_fixture.py` | `app/src/availability_proof.py`, `tools/forkmesh_availability_proof.py`; `app/tests/test_availability_proof.py` | `www/docs/availability-proof.md` |
| `category-comparisons` | `ledger.json` | `tools/category_comparisons.py`; `app/tests/test_category_comparisons.py` | `www/docs/category-comparisons.md` (generated) |
| `certification` | `program.json`, `implementations.json`, `results.json`, `adapters/`, `evidence/` | `tools/protocol_certification.py`; `app/tests/test_protocol_certification.py` | `www/docs/protocol/certification.md`, `www/docs/protocol/certification-registry.md` (generated) |
| `crisis-preservation` | `model.json`, `fixture-input.json`, `fixture.json`, `generate_fixture.py` | `app/src/crisis_preservation.py`, `tools/forkmesh_crisis_preservation.py`; `app/tests/test_crisis_preservation.py` | `www/docs/crisis-preservation.md` |
| `dependencies` | `contract.json` | `tools/dependency_conformance.py`; `app/tests/test_dependency_workflows.py` | `www/docs/dependency-workflows.md` |
| `disaster-recovery` | `runbook.json`, `exercises/`, `evidence/` | `tools/forkmesh_disaster_recovery.py`; `app/tests/test_disaster_recovery.py` | `www/docs/disaster-recovery/` |
| `external-dependencies` | `manifest.json` | `tools/external_dependency_inventory.py`; `app/tests/test_external_dependency_map.py` | `www/docs/external-dependency-map.md` |
| `funding` | `program.json`, `rounds/` | `tools/public_interest_funding.py`; `app/tests/test_public_interest_funding.py` | `www/docs/funding/` |
| `git` | `matrix.json`, `results.json`, `evidence/` | `tools/git_conformance.py` (`validate`, `render`, `run`, ...); `app/tests/test_git_workflow_conformance.py` | `www/docs/git-compatibility.md` (generated), `www/docs/operations/git-conformance.md` |
| `governance` | `charter.json`, `participants.json`, `records/` | `tools/protocol_governance.py`; `app/tests/test_protocol_governance.py` | `www/docs/governance/` |
| `hidden-unicode` | `vectors.json`, `generate_vectors.py` | `app/src/hidden_unicode.py`, `www/public/safe-input.js`, `desktop/src/HiddenUnicode.cpp`, `native/ios/ForkMeshCore/Sources/FMBase/HiddenUnicode.swift`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/security/HiddenUnicode.kt`; `app/tests/test_hidden_unicode.py`, `desktop/tests/test_hidden_unicode.cpp` | `docs/security/hidden-unicode.md` |
| `incentives` | `requirements.json` | `app/tests/test_incentive_independence.py` | `www/docs/incentive-independence.md` |
| `integrations` | `contract.json`, `examples/` | `tools/forkmesh_integration.py`; `app/tests/test_integration_contract.py` | `www/docs/integrations.md` |
| `protocol` | `manifest.json`, `threats.json`, `vectors/`, `generate_vectors.py`, `verify_suite.py` | `app/tests/test_protocol_suite.py`; clean-room `verify_suite.py` | `www/docs/protocol/` |
| `secrets` | `corpus.json`, `build_corpus.py` | `tools/secret_gate.py`; `app/tests/test_secret_gate.py` | `www/docs/security/secret-scanning.md` |
| `service-roles` | `manifest.json` | `tools/forkmesh_service_roles.py`; `app/tests/test_service_roles_contract.py` | `www/docs/operations/service-roles.md` |
| `social-signals` | `vectors.json` | `tools/social_signals.py`; `app/tests/test_social_signals.py` | `www/docs/social-signals.md` |
| `sync-status` | `model.json`, `fixture.json`, `generate_fixture.py` | `app/src/sync_status.py`, `server/syncstatus.go`, `native/ios/ForkMeshCore/Sources/FMAccount/SyncStatus.swift`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/api/SyncStatus.kt`, `tools/forkmesh_sync_status.py`; `app/tests/test_sync_status.py` | `www/docs/sync-status.md` |
| `transports` | `cases.json` | `tools/forkmesh_transports.py`; `app/tests/test_transports.py` | `www/docs/transports.md` |
| `trust-lists` | `vectors.json`, `generate_vectors.py` | `app/src/trust_lists.py`, `app/public/trust-lists.js`, `desktop/tests/test_trust_lists.cpp`, `native/ios/ForkMeshCore/Sources/FMAccount/TrustLists.swift`, `native/android/core/src/main/kotlin/com/forkmesh/nativeapp/security/TrustLists.kt`, `tools/forkmesh_trust_lists.py`; `app/tests/test_trust_lists.py` | `www/docs/trust-lists.md`, `www/docs/design/scoped-trust-lists.md` |

## Architecture

A suite is data, not code. Implementations load the same fixture or vectors
and must produce the recorded outputs, so the app, the Go daemon, the desktop
and the mobile apps cannot drift apart silently. Generator scripts
(`generate_fixture.py`, `generate_vectors.py`, `build_corpus.py`) rebuild the
expected outputs or, with `--check`, fail when they are stale. Ledger suites
(`git`, `category-comparisons`, `governance`, `funding`, `certification`) are
rendered by their tool's `render` command into Markdown under `www/docs/`
(most also write JSON under `www/public/docs/`). Each suite's JSON shape is
defined by its generator or consuming tool. Suites are versioned by
directory; there is no other configuration.

## Build, run, test

```sh
python3 conformance/protocol/v1/verify_suite.py
python3 conformance/sync-status/v1/generate_fixture.py --help
python3 tools/git_conformance.py validate
python3 -m pytest app/tests/test_protocol_suite.py app/tests/test_secret_gate.py app/tests/test_transports.py
```

After changing a rendered ledger, run its tool's `render` command so the
generated page matches; the matching test fails otherwise. CI runs
`tools/git_conformance.py validate`, `render --check` and `run` from
`.forkmesh/ci.yml`.

## Notes

- `secrets/v1/corpus.json` contains synthetic secret-shaped strings only; its
  file names (for example `docs/security.md`) are fixture paths, not files in
  this repository. Likewise, paths such as `docs/notes.md` or `docs/guide.md`
  in tests are fixtures.
- `social-signals/v1/vectors.json` and `disaster-recovery/v1/runbook.json`
  are covered by signatures or attested digests; rehosting tools never
  rewrite them (`FROZEN_FILES`), and a hand edit breaks verification.
- Suites that talk to themselves over loopback TCP (stock Git, integration
  contract) run in CI under `unshare -rn` with `lo` up, because a host
  application firewall can drop loopback connections and stall a run.
  Reproduce CI hangs locally the same way.
