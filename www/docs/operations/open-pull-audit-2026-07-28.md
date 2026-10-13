# Open pull-request audit — 2026-07-28

This audit covers every pull request whose signed record on
`forkmesh/pulls` at `9870aa843` said `status: open`. Evidence was evaluated
against `main` at `f669d334e`.

For every record, the audit checked whether its exact head still existed on
the authoritative remote, whether the stored patch applied to the current
index, whether the reverse patch applied (evidence that the change was already
represented), and whether a live head passed `git diff --check` and a
`git merge-tree` conflict check. The security-trigger column is a conservative
content/path scan that requires review; it is not itself a vulnerability
finding.

| PR | Head evidence | Patch evidence | Security triggers | Disposition |
|---:|---|---|---|---|
| 1 | deleted | `bfc1ceb77d3c`; conflicts | none | Close: stale against current main |
| 2 | deleted | `b6527c3714a1`; conflicts | credential paths, dynamic code | Close: stale; do not reconstruct without a new security review |
| 3 | deleted | `7229eed7d3f2`; conflicts | credential paths, dynamic code | Close: stale; do not reconstruct without a new security review |
| 4 | deleted | `f2e9510cadfd`; conflicts | credential paths, dynamic code | Close: stale; do not reconstruct without a new security review |
| 5 | deleted | `0ee9bc0ceb6f`; conflicts | credential paths | Close: stale against current main |
| 6 | deleted | `d8b92af1a43d`; conflicts | credential paths | Close: stale against current main |
| 7 | deleted | `be9576b26dc1`; conflicts | none | Close: stale against current main |
| 8 | deleted | `acfa5e68a8a9`; conflicts | credential paths | Close: stale against current main |
| 9 | deleted | `7bcf93f62f39`; conflicts | none | Close: stale against current main |
| 10 | deleted | `13e9e4d89461`; applies, 13 files | credential paths | Hold: recreate a signed branch, review mobile/private-node contracts, then run focused mobile tests |
| 11 | deleted | `45cab08356f1`; applies, 76 files | credential paths | Hold: recreate a signed branch; the owned-Git engine needs a dedicated security and data-loss review |
| 12 | deleted | `48acf7596e13`; applies, 60 files | none | Hold: recreate a signed branch; verify SHA1DC provenance/licenses and run iOS packaging tests |
| 13 | deleted | `d0c4782e9d82`; conflicts | credential paths | Close: stale against current main |
| 15 | deleted | `5df7d1a9b12b`; conflicts | credential paths, dynamic code, HTML sinks | Close: superseded by the current organization/team implementation |
| 16 | deleted | `2154ae396599`; conflicts | credential paths, dynamic code, HTML sinks | Close: superseded by the current issue-list implementation |
| 25 | deleted | `4a9fca9af09d`; applies, 5 files | none | Hold: recreate a signed branch and run discussion lifecycle/authorization tests |
| 29 | deleted | no stored patch | none | Close: no reviewable change |
| 30 | deleted | `61a23267c7ad`; reverse applies | none | Close: already represented in main |
| 31 | deleted | `3a328984a28a`; conflicts, 104 files | credential paths, dynamic code, HTML sinks | Close: stale against current repository UI |
| 32 | deleted | `cf32b585c4d7`; conflicts | credential paths | Close: superseded by current profile navigation |
| 33 | deleted | `bda59d06a2f6`; conflicts | credential paths, dynamic code | Close: stale against current Actions state handling |
| 34 | deleted | `b1b10d8cc15f`; conflicts | HTML sinks | Close: superseded by the current repository graph |
| 35 | deleted | `22a1909ab220`; conflicts | none | Close: stale against current network logging |
| 36 | deleted | `5d9e0e7cdfbd`; reverse applies | none | Close: already represented in main |
| 37 | deleted | no stored patch | none | Close: no reviewable change |
| 39 | deleted | `0dfc3af460d1`; conflicts | credential paths | Close: stale against current Sentry handling |
| 40 | deleted | `6d0a71e94072`; conflicts | credential paths | Close: stale against current homepage/migrations |
| 42 | deleted | `7216e38556f1`; applies, 1 file | none | Close: completed one-off production-gate probe, not a product change |
| 43 | deleted | `3d83ee7a7d3d`; conflicts | none | Close: superseded by current promise/proof paths |
| 45 | deleted | `e9a44f5d8c67`; conflicts | credential paths, dynamic code, HTML sinks | Close: superseded by current channel/chat implementation |
| 46 | deleted | `8679e917b6b8`; conflicts | none | Close: superseded by current Office chat implementation |
| 47 | deleted | `0ed6064b525b`; conflicts | credential paths, dynamic code, HTML sinks | Close: superseded by current Office meeting implementation |
| 48 | deleted | `4954735b630a`; conflicts | none | Close: stale browser snapshot |
| 49 | deleted | `23fd55e267e8`; conflicts | none | Close: stale browser snapshot |
| 50 | deleted | `fb5e8a8b75dc`; conflicts | none | Close: stale browser snapshot |
| 51 | deleted | `f91f31bd5989`; conflicts | none | Close: stale browser snapshot |
| 52 | live `fix/chats` during review; deleted after merge | clean merge; `git diff --check` passed | none | Merged at `3eb404f641`; source-fragment cleanup and 31 focused tests passed at `86f8e23f0d`; merged branch deleted |
| 53 | deleted | `ea8c8013c1a3`; conflicts | credential paths, dynamic code | Close: superseded by current direct-message implementation |
| 54 | deleted | `764fa1b9bf75`; conflicts | credential paths, dynamic code, HTML sinks, World writes | Close: superseded by current parallel CI/agent implementation |
| 55 | deleted | `377a5c8361a4`; conflicts | credential paths, dynamic code, HTML sinks | Close: superseded by current recovery-alert controls |
| 56 | deleted | `c1dcb10334e0`; conflicts | credential paths, dynamic code, HTML sinks | Close: superseded by current hard-coded provider icon handling |
| 57 | deleted | `b3ba961f8763`; conflicts | HTML sinks | Close: superseded by current public-chat workbench |
| 58 | deleted | `5daedb0bbdb3`; conflicts, 403 files | credential paths, dynamic code, HTML sinks, shell download | Close: stale agent snapshot; the requested camera behavior is already implemented separately |

## Result

- 43 signed records were marked open.
- 1 live PR was cleaned, tested, and merged.
- 4 deleted-head PRs have patches that still apply but require intentionally
  recreated signed branches and focused review before any merge.
- 38 records should close because they are stale, empty, already represented,
  superseded, or a completed one-off probe.

This file is the durable disposition record. It does not rewrite signed pull
metadata by hand; normal ForkMesh pull synchronization should close merged PR
#52, and the remaining close recommendations can be applied through the
authenticated pull-request workflow.
