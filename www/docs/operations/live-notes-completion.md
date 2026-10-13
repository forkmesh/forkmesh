# ForkMesh plan and live-notes completion record

Updated for the v0.7.0 production release on 2026-07-24 America/New_York.
The earlier `plan.md` and `live-notes.md` files were retired at the owner's
direction after their actionable requirements were implemented. This document
is the canonical checked completion record so that retired or intentionally
skipped prose is not silently recreated or represented as unfinished work.

## Release completion checklist

- [x] Implement the multiplayer Three.js World as the immediate website
  experience.
- [x] Render privacy-safe identity badges and configurable public presence
  without exposing raw IP addresses or precise location.
- [x] Remove laptop and phone props from people in the World.
- [x] Make movement and presence visible between independent visitors.
- [x] Keep World chat embedded and allow guests to use the dedicated public
  Town Square room without weakening authenticated room permissions.
- [x] Implement repository and organization spaces, personal offices,
  repository visualization, events, workshops, Fediverse/community surfaces,
  and the shared World clock.
- [x] Implement repository imports, honest non-mirrored stubs, mirror
  registration, health checks, routing, integrity controls, and private
  repository concealment.
- [x] Route `forkmesh/forkmesh` through the two independently operated healthy
  mirrors in round-robin order while keeping the selected origin masked.
- [x] Keep Jett as an owner of the `forkmesh` organization.
- [x] Remove the automated user abuse/quarantine protection runtime, API,
  scheduled jobs, data tables, and World jail visualization.
- [x] Preserve security findings, rate limits, repository-integrity checks,
  audit logs, and manual moderation controls that do not automatically
  quarantine visitors.
- [x] Implement daily security scans, careful trust language, GitHub issue
  synchronization, digest integrations, media controls, and administrative
  permission boundaries.
- [x] Preserve the non-custodial community reward program and the
  `TREASURY_SOLANA_ADDRESS` fallback.
- [x] Allow voluntary, user-signed contributions to the reward pool without
  promising ownership, returns, or custody.
- [x] Implement SSH public-key registration and restrictive Git
  upload/receive support.
- [x] Verify an actual SSH key can read, push a disposable branch, remove that
  branch, be revoked, and then be denied.
- [x] Bump the product, Worker, and desktop source version to v0.7.0.
- [x] Pass the complete source, Worker, browser, Qt, and Flutter release gates.
- [ ] Commit and push the exact v0.7.0 source state.
- [ ] Deploy Cloudflare from that exact commit and confirm the public version
  reports the same revision.
- [ ] Refresh both mirrors to the exact pushed commit and re-run public
  page/ref/clone/fsck/round-robin checks.
- [ ] Re-run production two-client movement and embedded-chat checks.
- [ ] Send and verify the final global in-World completion announcement.

The only excluded item is the final qualitative `plan.md` line that the owner
explicitly asked to skip; it is not claimed as objectively complete.

The capability inventory and intentional surface exceptions are recorded in
`www/docs/capability-matrix.md` and `www/docs/capability-matrix.json`.

## Reward-program checklist

- [x] `TREASURY_SOLANA_ADDRESS` remains a supported public community-pool address.
  A non-empty `COMMUNITY_REWARD_POOL_ADDRESS` is an explicit override; otherwise
  the Worker uses `TREASURY_SOLANA_ADDRESS`.
- [x] Users can voluntarily join or fund the program with a direct, user-signed
  wallet transfer. ForkMesh does not take custody, create a user wallet, promise
  returns, sell ownership, or grant governance/reward entitlement for a
  contribution.
- [x] The public address, user-owned funds, pending allocations, completed
  transfers, and visual-only fountain effects are distinguished in the UI.
- [x] Pool signing remains on the first instance owner's local Qt control node.
  Private signer material is not accepted by the Worker.
- [x] Production validation fails closed on invalid cluster, RPC, or pool-address
  combinations. Devnet remains an explicit development/test mode.

Operational details are in `community-reward-pool.md` and
`community-reward-pool-signer.md` in this directory.

## SSH Git checklist

- [x] Authenticated users can register, list, and revoke validated OpenSSH public
  keys. Normalized key material is encrypted at rest; private keys are never
  uploaded.
- [x] The Cloudflare Worker is the HTTPS authorization plane, not an SSH server.
  A separately operated OpenSSH gateway uses a restrictive
  `AuthorizedKeysCommand` and forced Git commands.
- [x] Only `git-upload-pack` and `git-receive-pack` are accepted. Repository
  allowlists, repository-root containment, local read-only enforcement, current
  account identity, and Worker owner/share/organization permissions are all
  checked.
- [x] SSH URLs remain hidden unless the gateway host, ASCII base64url bearer token,
  and explicit per-repository allowlist are all valid. Setting the allowlist to
  `disabled` fails closed.
- [x] Pasting a public key does not itself prove private-key possession. OpenSSH
  proves possession when the connection is made; an account-specific fresh key
  is recommended to avoid conventional public-key registration squatting.
- [x] Raw SSH uses a separately controlled TCP endpoint; an HTTP Worker does
  not terminate raw SSH. Deployments may use direct DNS plus firewalling,
  Cloudflare Spectrum, or an appropriate Tunnel configuration. An HTTP Worker
  cannot terminate raw SSH.

Deployment and hardening details are in `git-ssh-gateway.md` in this directory.

## Commit-history rewrite

The local rewrite completed successfully for 8,703 commits across 18 local
branches and 2 tags. It did not include remote-tracking, pull, stash, backup, or
other operational refs, and it did not push.

Canonical private recovery directory:

```text
/home/f/projects/forkmesh/.git/forkmesh-history-rewrite/20260724T004019Z
```

The directory is mode `0700`; its bundle, original-message export, mapping, and
manifests are mode `0600`. `pre-rewrite.bundle` verifies as a complete bundle.
The 20 original scoped tips also remain under:

```text
refs/forkmesh-history-backup/20260724T004019Z/
```

`original-commit-messages.jsonl` contains every original ID, full title,
description, exact message bytes, and identity headers. It must remain private
and must not be committed or published. `old-to-new.json` contains the complete
8,703-entry mapping. All rewritten commits were independently checked for
unchanged tree, mapped parent order/topology, exact author/committer headers and
timestamps, stripped invalid signatures, and the one-line name-redaction
policy. The main tree remained
`a807d623a608d45e805e48ba2b9dc89b736dcfb5`, and the worktree-status fingerprint
remained `adc3c1ba7e22be67cc89200d197da433e6cfce656178a4bceafb56dea5c8e085`.

A guarded preflight at `20260724T003653Z` found a redaction-placeholder edge
case and stopped before changing refs. Its private bundle/backup refs remain as
additional recovery material, but `20260724T004019Z` is the completed canonical
rewrite.

To restore the 20 scoped refs locally:

```bash
python3 tools/rewrite_commit_messages.py restore \
  --artifact-dir /home/f/projects/forkmesh/.git/forkmesh-history-rewrite/20260724T004019Z \
  --confirm RESTORE-COMMIT-HISTORY
```

The rewrite invalidates signed commit objects, signed annotated tags, old
commit URLs/external references, and scan records pinned to old IDs. Publishing
the rewritten history requires a separately authorized, coordinated
force-update. Mirrors and existing clones must preserve a backup and re-clone
or reset explicitly to the exact new tips; ordinary merges must not reconnect
the old and new graphs. Never use a blind `push --mirror`.

## Final validation evidence

- `python3 -m pytest -q app/tests`: 1,763 passed, 1 skipped,
  0 failed in 158.88 seconds. The skip is the optional `jsonschema` package
  contract check; the same file otherwise passed 16 tests, and runtime schema
  validation remains covered.
- `python3 tools/quality_gate.py run --profile release`: passed Python syntax,
  35 security/privacy contracts, 1 performance-budget test, 4 quality-gate
  contracts, and 21/21 Playwright browser tests.
- The final standalone Playwright suite passed 22/22 tests after the
  organization-alias listing regression was added.
- `cmake --build desktop/build -j2 --target check`: 11/11 passed.
- `/home/f/.forkmesh/toolchain/flutter/bin/flutter analyze`: no issues.
- `/home/f/.forkmesh/toolchain/flutter/bin/flutter test`: 126 passed.
- Production JavaScript syntax, Python compile-all, `git diff --check`, all 7
  branch/PR guard checks, and the complete MCP end-to-end script passed.
- `git fsck --full --no-dangling`, bundle verification, exact ref/backup
  comparison, tag peeling, mapping cardinality, and all-commit rewrite
  invariants passed.
- The full Worker suite covers the migrations, repository privacy, mirror
  routing, reward, SSH, security, Fediverse, World, and automation contracts.

## Production evidence

- [x] Cloudflare DNS resolves the main site and both independently operated
  mirror hostnames.
- [x] Both mirror gateways and tunnels are active, their signed health proofs
  report integrity `ok`, and both bare repositories pass strict `git fsck`.
- [x] The canonical `forkmesh/forkmesh` page resolves its organization alias,
  renders the repository tree and README, keeps the mirror hostname masked,
  and reports both replicas online.
- [x] Five fresh production browser contexts loaded the canonical repository
  without a page error or `mirror_unavailable` response.
- [x] The repository social logo renders from a locally generated, fully
  encoded SVG data URL; no private source or external model is involved.
- [x] A real registered SSH key completed read, disposable-branch push,
  verification, deletion, revocation, and post-revocation denial.
- [x] The configured public reward-pool address is readable on mainnet-beta,
  while wallet keys remain local and self-custodial.
- [x] A contribution preparation smoke test created only an expiring unsigned
  intent. No wallet transfer was signed or broadcast during deployment.
- [x] Jett is recorded as an owner of the `forkmesh` organization.
- [x] Automated user quarantine tables and runtime routes are absent from the
  production schema and Worker.
