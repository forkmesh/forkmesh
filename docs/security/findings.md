# Security findings register

Reviewed through: `c029745dd` on 2026-09-23 (seed; no commit after it has had a Sentinel review yet)

Sentinel ([sentinel.md](sentinel.md)) keeps this page. It lists fixed
findings with the commit and the test that guard them, and the open items that
are already public. Unfixed findings that are not public are tracked on the
organization task board in the `security` department and appear here only once
their fix is on main.

The seed was built on 2026-09-30 against main at `2742797b8`. It checked every
finding of the 2026-09-24 code and infrastructure audits of the Next stack
(`~/projects/forkmesh/logs/fleet/security-audit-{code,infra}-2026-09-24.md`
on the operator box) against the tree. The checkpoint is the last main commit
before those audits, because the 2,214 commits since then have not had a
review. Sentinel's first run reviews them in slices of 400.

Status words: **Fixed** means the fix is on main and the Guard test fails
without it. **Fixed, unguarded** means the fix is on main but no test guards
it. **Superseded** means the 2026-09-24 production cutover (`3890c327c`,
[CUTOVER.md](../CUTOVER.md)) made Next the only deployment, so a finding about
two deployments sharing data no longer applies. **Partial** and **Open** are
used only for items already public.

## Fixed

Code audit, 2026-09-24:

| ID | Finding | Severity | Status | Fix | Code | Guard |
| --- | --- | --- | --- | --- | --- | --- |
| C2 | The runner daemon ran git in the job's tree with root's rights | Critical | Fixed | `21e710117` | `server/runner.go` (hardened daemon git, publish through a daemon-owned bare repo) | `server/runner_publish_test.go` |
| H1 | Every core held a DNS-edit token for the whole zone | High | Fixed | `961b29166`, `4fde6c50e` | `deploy/fleet.toml` (`reconciler = "dry"`, token not in the vault) | `deploy/tests/test_next_vault.py` |
| H3 | Cluster members and their addresses came from the database, and the admin console could write host tables | High | Fixed | `208503fd5` | `app/host/forkmesh_host/cluster.py`, `app/src/admin_console.py` (`ADMIN_HIDDEN_TABLES`) | `app/tests/host/test_host_cluster_security.py`, `app/tests/test_admin_database.py` |
| H5 | A local-looking Host header created a demo admin | High | Fixed | `208503fd5` | `app/src/entry.py` (demo login refused on `FORKMESH_PLATFORM=vps`) | `app/tests/test_login_key_binding.py` |
| H6 | No request body caps or read timeouts | High | Fixed | `32d456a84`, `98931d549` | `app/host/forkmesh_host/request_guard.py`, `deploy/core/render.py` (`request_body` caps), `app/host/forkmesh_host/serve.py` | `app/tests/host/test_host_request_guard.py`, `deploy/tests/test_render.py` |
| M1 | No outbound (SSRF) guard on the App's fetch | Medium | Fixed | `32d456a84`, `1479692b2` | `app/host/forkmesh_host/egress.py`, `app/host/forkmesh_host/js.py` | `app/tests/host/test_host_egress.py` |
| M2 | Caddy's admin API listened on TCP | Medium | Fixed | `98931d549` | `deploy/core/render.py` (`admin unix//run/caddy/admin.sock`) | `deploy/tests/test_render.py` |
| M3 | certsync trusted any chain and served private keys to any mesh-secret holder | Medium | Fixed | `98931d549` | `deploy/core/certsync.py` | `deploy/tests/test_certsync.py` |
| M4 | Raft and the mesh were reachable over loopback, and the mesh read bodies before checking signatures | Medium | Fixed | `98931d549`, `208503fd5` | `deploy/core/render.py` (nftables output rules), `app/host/forkmesh_host/durable_cluster.py` | `deploy/tests/test_render.py`, `app/tests/host/test_host_cluster_security.py` |
| M5 | The edge-client header was never verified | Medium | Fixed | `32d456a84` | `app/host/forkmesh_host/asgi.py`, `app/host/forkmesh_host/request_guard.py` | `app/tests/host/test_host_request_guard.py` |
| M6 | Webhooks and ActivityPub could deliver to the other deployment | Medium | Fixed | `208503fd5` | `app/src/repository_webhooks.py`, `app/src/fediverse_routes.py` | `app/tests/test_repository_webhooks.py`, `app/tests/test_ap_noop_profile_updates.py` |
| M8 | Every empty SELECT cost a leader read | Medium | Fixed | `32d456a84` | `app/host/forkmesh_host/d1_rqlite.py` | `app/host/tests_unit/test_d1_confirm_misses.py` |
| M9 | Nightly backups were plaintext | Medium | Fixed | `cf12c5a8c` | `tools/next_backup.py` (age) | `deploy/tests/test_next_backup.py` |
| L | No HSTS on the app and world hosts | Low | Fixed | `98931d549` | `deploy/core/render.py` | `deploy/tests/test_render.py` |

Infrastructure audit, 2026-09-24. Rows marked (tooling) are enforced by
`tools/next_fleet.py`; they are on a host once its `base,secrets,files,start`
install steps have run after `98931d549`.

| ID | Finding | Severity | Status | Fix | Code | Guard |
| --- | --- | --- | --- | --- | --- | --- |
| M1 | Caddy's admin API was open to every local user | Medium | Fixed | `98931d549` | `deploy/core/render.py` | `deploy/tests/test_render.py` |
| M2 | Raft and the mesh were open to any local process | Medium | Fixed | `98931d549` | `deploy/core/render.py` (nftables `meta skuid` output rules; rqlite mTLS not adopted) | `deploy/tests/test_render.py` |
| M3 | Secret-holding processes could swap to disk | Medium | Fixed | `98931d549` | `deploy/core/templates/` (`MemorySwapMax=0` on rqlite, Caddy, certsync) | `deploy/tests/test_render.py` |
| M4 | Caddy was barely sandboxed | Medium | Fixed | `98931d549` | `deploy/core/templates/caddy-dropin.conf` | `deploy/tests/test_render.py` |
| L1 | The provider console was a root back door (tooling) | Low | Fixed | `98931d549` | `tools/next_fleet.py` | `deploy/tests/test_next_fleet.py` |
| L2 | A leftover default account had sudo (tooling) | Low | Fixed | `98931d549` | `tools/next_fleet.py` | `deploy/tests/test_next_fleet.py` |
| L3 | sshd allowed more forwarding than needed (tooling) | Low | Fixed | `98931d549` | `tools/next_fleet.py` | `deploy/tests/test_next_fleet.py` |
| L4 | Certificate sync was half rolled out (tooling) | Low | Fixed | `98931d549` | `tools/next_fleet.py`, `deploy/core/render.py` | `deploy/tests/test_next_fleet.py`, `deploy/tests/test_render.py` |
| L5 | API and redirect responses lacked HSTS and nosniff | Low | Fixed | `98931d549` | `deploy/core/render.py` | `deploy/tests/test_render.py` |
| L7 | Runner jobs could reach the metadata service and private networks | Low | Fixed | `98931d549` | `deploy/core/render.py` (nftables), `IPAddressDeny` in the runner scope | `deploy/tests/test_render.py` |
| L8 | The mirror daemon could read the whole journal | Low | Fixed | `98931d549` | daemon unit without `systemd-journal` | `deploy/tests/test_render.py` |
| L9 | Any process could ptrace another of the same user | Low | Fixed | `98931d549` | `deploy/core/templates/sysctl.conf` | `deploy/tests/test_render.py` |
| L10 | Relayed visitors shared one rate-limit bucket | Low | Fixed | `32d456a84` | `app/host/forkmesh_host/asgi.py` | `app/tests/host/test_host_request_guard.py` |

Found since the audits:

| ID | Finding | Severity | Status | Fix | Code | Guard |
| --- | --- | --- | --- | --- | --- | --- |
| S-2026-09-30-1 | Caddy dropped the security headers on `/office` and `/world` | Medium | Fixed | `d03333412` | `deploy/core/caddy_routes.py` | `deploy/tests/test_caddy_served_headers.py` |

Full sweep, 2026-10-12 (branch `1930-full-security-sweep-system`, eight
parallel audits by area). Each guard fails without its fix.

| ID | Finding | Severity | Status | Code | Guard |
| --- | --- | --- | --- | --- | --- |
| S-2026-10-12-29 | Any account could park its own link code and redeem it for someone else's node (`reclaim-node`, `link-node`, `finalize`), swapping in its key and taking the node and its repos | Critical | Fixed | `app/src/entry.py` `_link_code_rotation_allowed` (a code may replace a node's key only when its user owns the node), `app/src/route_handlers.py` `_account_finalize` | `app/tests/test_users_nodes_claim_link.py` |
| S-2026-10-12-30 | A password on `POST /api/accounts/profile` opened a five-year session with no TOTP and no failure count | High | Fixed | `app/src/entry.py` `_account_profile` (failures count against the login budget; TOTP unless the request also carries that account's session) | `app/tests/test_account_totp_gate.py` |
| S-2026-10-12-31 | TOTP codes had no miss limit of their own | Medium | Fixed | `app/src/entry.py` `_account_totp_ok` (10 misses per 15 minutes per account) | `app/tests/test_account_totp_gate.py` |
| S-2026-10-12-32 | The ForkBot "start an agent" command trusted the `sender` in the body, so anyone could start an agent on the repo owner's machine | High | Fixed | `app/src/forkbot.py` (the requester is the signed-in session; a desktop without a session token can no longer start agents through ForkBot chat) | `app/tests/test_forkbot.py` |
| S-2026-10-12-33 | Microjob claims on a repo the claimant owns (directly, through a node or an org role) were approved | Medium | Fixed | `app/src/microjobs.py`, `app/src/microjobs_runtime.py` | `app/tests/test_microjobs.py`, `app/tests/test_microjobs_own_repository.py` |
| S-2026-10-12-34 | An org bot token kept working after its creator left the org or lost the right to make tokens | Medium | Fixed | `app/src/entry.py` `_org_bot_token_context` | `app/tests/test_org_bot_retired_scopes.py` |
| S-2026-10-12-35 | Org guests could read team rosters | Low | Fixed | `org_team_members_handler` requires `organization.roster.read` | `app/tests/test_org_team_roster_guest.py` |
| S-2026-10-12-36 | Org guests and auditors counted as insiders for org calendars (list, read, create), and a removed member could still read and edit the org events they had made | Medium | Fixed | `app/src/calendar_api.py` (`_org_insider`, insider roles on list and create, no auditor writes, owner shortcut only for personal events) | `app/tests/test_calendar.py` `test_organization_guests_and_removed_members_get_no_insider_access` |
| S-2026-10-12-1 | Symlinks committed under `.forkmesh/` let issue, pull and discussion intake write attacker bytes through them (a visitor's attachment could overwrite another repo's `post-receive` hook) | High | Fixed | `server/intake.go` (`refuseCollaborationLinks` after the sparse checkout) | `server/intake_test.go` `TestCollaborationIntakeRefusesCommittedSymlinks` |
| S-2026-10-12-2 | A pull event `type` became part of a file name, so `x/../..` escaped the pull folder through the loopback collaboration endpoint | Medium | Fixed | `server/intake.go` (`pullEventTypePattern`) | `server/intake_test.go` `TestPullEventTypeCannotNameAPathOutsideThePull` |
| S-2026-10-12-3 | The root runner daemon read job-owned provider login files with `os.ReadFile`, so a job could plant a symlink (read a root-only file) or a FIFO (hang every claim) | Medium | Fixed | `server/runnerauth.go` (reads through `readOwnedFile` when a job uid is set) | `server/runnerauth_test.go` `TestRunnerCredentialReadRefusesJobPlantedLinksAndFifos` |
| S-2026-10-12-4 | `--restore-drill` as root ran git hooks and `core.hooksPath` restored from the backup | Medium | Fixed | `server/nodeops_backup.go` (`core.hooksPath=/dev/null`, no fsmonitor, no auto maintenance) | `server/nodeops_test.go` `TestNodeRestoreDrillRunsNoHookFromTheArchive` |
| S-2026-10-12-5 | A user-run mirror's `Content-Type` was copied onto App-origin repo reads, so a mirror could serve HTML or SVG script on the App origin | Medium | Fixed | `app/src/edge_routing.py` (`active_content_type`: active types become an `application/octet-stream` attachment; every proxied read gets `default-src 'none'`) | `app/tests/test_edge_routing.py` |
| S-2026-10-12-6 | `/dashboard//evil.example/x` redirected off-site | Medium | Fixed | `app/src/static_routes.py` `dashboard_repo_redirect` | `app/tests/test_dashboard_repo_redirect_open_redirect.py` |
| S-2026-10-12-7 | The site-wide header hook replaced the stricter CSP a page had set (analysis pages, hackathon admin, email verify, session handoff) | Low | Fixed | `app/src/entry.py` `_apply_browser_security_headers` keeps a page's own CSP | `app/tests/test_browser_security_headers.py` |
| S-2026-10-12-8 | A control character before `javascript:` slipped past the markdown link filter (stored XSS in notes, tasks and comments) | Medium | Fixed | `app/public/dashboard/js/06-repo-content.js` `mdSafeUrl` | `app/tests/test_repo_readme_markdown.py` |
| S-2026-10-12-9 | Chat attachments kept the sender's MIME type, so an HTML or SVG attachment opened in a new tab ran on the App or World origin | Medium | Fixed | `app/public/chat-attachments.js` `attachmentBlobType` (used by `chat.js`, `dashboard-chat.js`, `world-office-meeting.js`) | `app/tests/test_chat_shared_modules_frontend.py`, `world/tests/test_world_office_meeting_frontend.py` |
| S-2026-10-12-10 | Mirror-supplied image diff data broke out of the `src` attribute | Medium | Fixed | `06-repo-content.js` `renderDiffImagePreview` (base64 only) | `app/tests/test_repo_image_diff_preview.py` |
| S-2026-10-12-11 | Fork row links accepted `/\host` and `/<tab>/host` | Low | Fixed | `06-repo-content.js` `repoForkPath` | `app/tests/test_repo_image_diff_preview.py` |
| S-2026-10-12-12 | `forkmesh://control/cloudflare?mode=vultr` read a token from the clipboard and created a mirror at once, handing it the saved Cloudflare token | High | Fixed | `desktop/src/MainWindowControlNode.cpp` (the link only fills the form; the user clicks Create) | `desktop/tests/validate_system_link_vultr_confirm.py` |
| S-2026-10-12-13 | A branch or base ref with a quote ran shell code in the agent worktree script | Medium | Fixed | `desktop/src/AgentWorktree.h` `addScript` | `desktop/tests/test_agent_prompt_title.cpp` |
| S-2026-10-12-14 | The IDE bridge lockfile holding its token was readable by other local users, and the token compare was not constant-time | Low | Fixed | `desktop/src/ClaudeIdeBridge.cpp` | `desktop/tests/test_ide_bridge.cpp` |
| S-2026-10-12-15 | A code-approved phone could rename its stored user to the owner and pass every owner gate | High | Fixed | `desktop_go_new/internal/control/server.go` `admitStored` | `desktop_go_new/internal/control/owner_claim_test.go` |
| S-2026-10-12-16 | The agent socket folder in `/tmp` could be pre-created by another local user | Medium | Fixed | `desktop_go_new/internal/agentproc/module.go`, `sys_unix.go` | `desktop_go_new/internal/agentproc/socket_dir_test.go` |
| S-2026-10-12-17 | A package migration could `END` the host transaction and `ATTACH` a database at any path | High | Fixed | `marketplace/tools/marketplace_lib.py` `TRANSACTION_SQL_RE` (the words are refused anywhere, so `CASE … END` is refused too) | `marketplace/tests/test_marketplace_lifecycle.py` |
| S-2026-10-12-18 | Symlinks in a git or local package source copied local files into the package | Medium | Fixed | `marketplace_lib.py` `install_package` | `marketplace/tests/test_marketplace_lifecycle.py` |
| S-2026-10-12-19 | A committed `base: --output=/path` in a pull record made the MCP server's `git diff` truncate any file | Medium | Fixed | `tools/forkmesh_mcp_server.py` `pull_ref` | `tools/test_forkmesh_mcp_server.py` |
| S-2026-10-12-20 | An image layer could write through a symlinked folder or hard-link host files in the baseline puller | Low | Fixed | `tools/package/linux-baseline/oci_rootfs.py` `safe_members` | `app/tests/test_linux_release_baseline.py` |
| S-2026-10-12-21 | The vault fill could land on a page that navigated after the host check | Low | Fixed | `marketplace/packages/browser-extension/content/extension/vault.js` `fillInPage` | `marketplace/packages/browser-extension/tests/background_vault_harness.js` |
| S-2026-10-12-22 | The mirror gateway's handoff `next` accepted `/\host` and CR/LF | Low | Fixed | `tools/mirror_gateway.py` | `app/tests/test_mirror_gateway.py` |
| S-2026-10-12-23 | A branch name could inject script into the branch review page's `onclick` | Low | Fixed | `tools/branch_pr_review.py` (`data-branch`) | `tools/test_branch_pr_review_guard.py` |
| S-2026-10-12-24 | A cloned repository's tree could write into the phone's own `.git` folder (any case) | Medium | Fixed | `native/android/.../GitRepository.kt`, `native/ios/.../FMGit/Engine/GitRepository.swift` `collectTreeEntries` | `GitCheckoutTest`, `LocalEngineWorkflowTests` `testCheckoutRefusesATreeThatWritesIntoTheGitDirectory` |
| S-2026-10-12-25 | The iOS push session copy could be restored from a backup onto another phone | Low | Fixed | `native/ios/Shared/PushCredentialStore.swift` (`ThisDeviceOnly`) | `StoreConfigTests.testEveryKeychainItemStaysOnThisDevice` |
| S-2026-10-12-26 | Client headers could set the mesh routing headers `x-fm-do-url`, `x-fm-do-id`, `x-fm-do-cf` on a cross-core Durable Object call | Low | Fixed | `app/host/forkmesh_host/cluster.py` `_MESH_STRIP` | `app/tests/host/test_host_cluster_security.py` |
| S-2026-10-12-27 | `/_app/_host/*` on a per-node host reached host-internal paths | Low | Fixed | `deploy/core/render.py` (404 inside `handle_path /_app/*`) | `deploy/tests/test_render.py` `test_node_relay_never_reaches_host_internal_paths` |
| S-2026-10-12-28 | The World dev server accepted any `Host` (DNS rebinding) and passed foreign origins upstream as its own | Low | Fixed | `world/tools/world_dev_server.py` | `world/tests/test_world_dev_server.py` |

## Superseded by the cutover

| ID | Finding | Why it no longer applies |
| --- | --- | --- |
| Code C1 | Next ran on production's `DATA_KEY` | Re-keyed (`a0e05c519`, [REKEY.md](../REKEY.md), guarded by `app/tests/host/test_next_rekey.py`), then the cutover made Next production. Reset and verify tokens still carry no deployment id; that matters again only if a second deployment shares the key. |
| Code H2 | Next contacted real users with production credentials | Next is production; `SENTRY_DSN` and APNs are production's on purpose (`optional_secrets` in `deploy/fleet.toml`). |
| Code M7 | Production cookies crossed into Next | One deployment. |
| Code L | `ADMIN_PATH` reused from production | One deployment. |

## Open, already public

| Item | Severity | Status | Since | Where it is described | What remains |
| --- | --- | --- | --- | --- | --- |
| TM-004 legacy issue, discussion and pull records bind no repository | High | Partial | 2026-07 audit | [threat-model.md](../../www/docs/protocol/threat-model.md), [README.md](README.md) | Verifiers accept v2 and the web and Go daemon sign it (`app/tests/test_event_repo_binding.py`, `server/event_signing_test.go`); the desktop, iOS and Android still sign v1 on main, with their v2 signing on unmerged branches. v1 intake closes 2027-01-01. |
| TM-006 catalog v1 signatures bind no visibility | High | Open | 2026-07 audit | [threat-model.md](../../www/docs/protocol/threat-model.md) | v2 catalog signing exists; v1 is still accepted. |
| TM-008 a relay or mirror can withhold events | High | Open | 2026-07 audit | [threat-model.md](../../www/docs/protocol/threat-model.md) | No work on any branch. |
| TM-014 tunnelled node traffic trusts the relay | High | Open | 2026-07 audit | [threat-model.md](../../www/docs/protocol/threat-model.md) | No work on any branch. |
| TM-018 no account-key recovery protocol | High | Open | 2026-07 audit | [threat-model.md](../../www/docs/protocol/threat-model.md) | No work on any branch. |
| Runner jobs share one uid; no `PrivateTmp` or `ProtectSystem` in the scope (code H4, infra H1) | High | Partial | 2026-09-24 | [RUNNERS.md](../RUNNERS.md) "All jobs share one uid" | Job user, egress rules, owner opt-in and per-job grants are in (`server/sandbox_test.go`, `app/tests/test_runner_leases.py`); per-job uids and filesystem protection are not. |
| Runner jobs run with the persistent HOME, not their session home | High | Open | 2026-09-29 | [RUNNERS.md](../RUNNERS.md) "Known gap (found 2026-09-29)" | Nothing re-sets `HOME` inside the scope, so a job can write the persistent home's settings and read its provider login. |
| Executable proof for agent boundaries (issue #708) | High | Open | 2026-09 | issue #708 | No owner before Sentinel. |

## Private and operator-owned

- Eleven more items from the 2026-09-24 audits are open or partial and are
  not public. They are kept off this page until fixed. Sentinel's first run
  files each one that is not already on the task board.
- The operator box (infrastructure H2) and account-level items are the
  operator's: disk and swap encryption, a passphrase on the fleet SSH key,
  plaintext pre-age backups and retired keys to move offline, a limited Vultr
  sub-user, provider 2FA, and a CAA record. They are tracked on the board as
  "Rotate credentials", "DATA_KEY in plaintext in ForkMesh.conf and in 34 local
  plain backups" and "Move DATA_KEY.next-pre-cutover, backup-age.key and
  ids-operator.key to offline storage", all blocked on the operator.
