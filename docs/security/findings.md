# Security findings register

Reviewed through: `c029745dd` on 2026-09-23 (seed; no commit after it has had a Sentinel review yet)

Sentinel ([sentinel.md](sentinel.md)) keeps this page: fixed findings with the
test that guards each fix, and open items that are already public. Unfixed
findings that are not public are tracked on the organization task board in
the `security` department and appear here only once their fix is on main.
Update the checkpoint line after each review.

Status words: **Fixed** means the fix is on main and the Guard test fails
without it, and the guard runs in a gate: the `security` suite of
`tools/run_critical_tests.py` (CI, the quality gate and the auto-deploy
must-pass set) holds the S-2, S-5 and C-H5 guards, the account TOTP gate and
the destructive-delete contract; the Go guards run with `go test` in
`server/` in the same gates. Until 2026-10-15 none of these ran in any gate.
**Fixed, unguarded** means no test guards it yet. **Partial** and **Open** are
used only for items already public. Add a finding as one row;
the last cell names the guard test (paths must exist).

## Fixed

IDs: `C-` code audit, `I-` infrastructure audit, `S-<n>` later reviews.

| ID | Finding | Severity | Status | Guard |
| --- | --- | --- | --- | --- |
| C-C2 | The runner daemon ran git in the job's tree with root's rights | Critical | Fixed | `server/runner_publish_test.go` |
| C-H1 | Every core held a DNS-edit token for the whole zone | High | Fixed | `deploy/tests/test_next_vault.py` |
| C-H3 | Cluster members and their addresses came from the database, and the admin console could write host tables | High | Fixed | `app/tests/host/test_host_cluster_security.py`, `app/tests/test_admin_database.py` |
| C-H5 | A local-looking Host header created a demo admin | High | Fixed | `app/tests/test_login_key_binding.py` |
| C-H6 | No request body caps or read timeouts | High | Fixed | `app/tests/host/test_host_request_guard.py`, `deploy/tests/test_render.py` |
| C-M1 | No outbound (SSRF) guard on the App's fetch | Medium | Fixed | `app/tests/host/test_host_egress.py` |
| C-M2 | Caddy's admin API listened on TCP | Medium | Fixed | `deploy/tests/test_render.py` |
| C-M3 | certsync trusted any chain and served private keys to any mesh-secret holder | Medium | Fixed | `deploy/tests/test_certsync.py` |
| C-M4 | Raft and the mesh were reachable over loopback, and the mesh read bodies before checking signatures | Medium | Fixed | `deploy/tests/test_render.py`, `app/tests/host/test_host_cluster_security.py` |
| C-M5 | The edge-client header was never verified | Medium | Fixed | `app/tests/host/test_host_request_guard.py` |
| C-M6 | Webhooks and ActivityPub could deliver to the other deployment | Medium | Fixed | `app/tests/test_repository_webhooks.py`, `app/tests/test_ap_noop_profile_updates.py` |
| C-M8 | Every empty SELECT cost a leader read | Medium | Fixed | `app/host/tests_unit/test_d1_confirm_misses.py` |
| C-M9 | Nightly backups were plaintext | Medium | Fixed | `deploy/tests/test_next_backup.py` |
| C-L | No HSTS on the app and world hosts | Low | Fixed | `deploy/tests/test_render.py` |
| I-M1 | Caddy's admin API was open to every local user | Medium | Fixed | `deploy/tests/test_render.py` |
| I-M2 | Raft and the mesh were open to any local process | Medium | Fixed | `deploy/tests/test_render.py` |
| I-M3 | Secret-holding processes could swap to disk | Medium | Fixed | `deploy/tests/test_render.py` |
| I-M4 | Caddy was barely sandboxed | Medium | Fixed | `deploy/tests/test_render.py` |
| I-L1 | The provider console was a root back door | Low | Fixed | `deploy/tests/test_next_fleet.py` |
| I-L2 | A leftover default account had sudo | Low | Fixed | `deploy/tests/test_next_fleet.py` |
| I-L3 | sshd allowed more forwarding than needed | Low | Fixed | `deploy/tests/test_next_fleet.py` |
| I-L4 | Certificate sync was half rolled out | Low | Fixed | `deploy/tests/test_next_fleet.py`, `deploy/tests/test_render.py` |
| I-L5 | API and redirect responses lacked HSTS and nosniff | Low | Fixed | `deploy/tests/test_render.py` |
| I-L7 | Runner jobs could reach the metadata service and private networks | Low | Fixed | `deploy/tests/test_render.py` |
| I-L8 | The mirror daemon could read the whole journal | Low | Fixed | `deploy/tests/test_render.py` |
| I-L9 | Any process could ptrace another of the same user | Low | Fixed | `deploy/tests/test_render.py` |
| I-L10 | Relayed visitors shared one rate-limit bucket | Low | Fixed | `app/tests/host/test_host_request_guard.py` |
| S-1 | Caddy dropped the security headers on `/office` and `/world` | Medium | Fixed | `deploy/tests/test_caddy_served_headers.py` |
| S-2 | Any account could park its own link code and redeem it for someone else's node (`reclaim-node`, `link-node`, `finalize`), swapping in its key and taking the node and its repos | Critical | Fixed | `app/tests/test_users_nodes_claim_link.py` |
| S-3 | A password on `POST /api/accounts/profile` opened a five-year session with no TOTP and no failure count | High | Fixed | `app/tests/test_account_totp_gate.py` |
| S-4 | TOTP codes had no miss limit of their own | Medium | Fixed | `app/tests/test_account_totp_gate.py` |
| S-5 | The ForkBot "start an agent" command trusted the `sender` in the body, so anyone could start an agent on the repo owner's machine | High | Fixed | `app/tests/test_forkbot.py` |
| S-6 | Microjob claims on a repo the claimant owns (directly, through a node or an org role) were approved | Medium | Fixed | `app/tests/test_microjobs.py`, `app/tests/test_microjobs_own_repository.py` |
| S-7 | An org bot token kept working after its creator left the org or lost the right to make tokens | Medium | Fixed | `app/tests/test_org_bot_retired_scopes.py` |
| S-8 | Org guests could read team rosters | Low | Fixed | `app/tests/test_org_team_roster_guest.py` |
| S-9 | Org guests and auditors counted as insiders for org calendars (list, read, create), and a removed member could still read and edit the org events they had made | Medium | Fixed | `app/tests/test_calendar.py` `test_organization_guests_and_removed_members_get_no_insider_access` |
| S-10 | Symlinks committed under `.forkmesh/` let issue, pull and discussion intake write attacker bytes through them (a visitor's attachment could overwrite another repo's `post-receive` hook) | High | Fixed | `server/intake_test.go` `TestCollaborationIntakeRefusesCommittedSymlinks` |
| S-11 | A pull event `type` became part of a file name, so `x/../..` escaped the pull folder through the loopback collaboration endpoint | Medium | Fixed | `server/intake_test.go` `TestPullEventTypeCannotNameAPathOutsideThePull` |
| S-12 | The root runner daemon read job-owned provider login files with `os.ReadFile`, so a job could plant a symlink (read a root-only file) or a FIFO (hang every claim) | Medium | Fixed | `server/runnerauth_test.go` `TestRunnerCredentialReadRefusesJobPlantedLinksAndFifos` |
| S-13 | `--restore-drill` as root ran git hooks and `core.hooksPath` restored from the backup | Medium | Fixed | `server/nodeops_test.go` `TestNodeRestoreDrillRunsNoHookFromTheArchive` |
| S-14 | A user-run mirror's `Content-Type` was copied onto App-origin repo reads, so a mirror could serve HTML or SVG script on the App origin | Medium | Fixed | `app/tests/test_edge_routing.py` |
| S-15 | `/dashboard//evil.example/x` redirected off-site | Medium | Fixed | `app/tests/test_dashboard_repo_redirect_open_redirect.py` |
| S-16 | The site-wide header hook replaced the stricter CSP a page had set (analysis pages, hackathon admin, email verify, session handoff) | Low | Fixed | `app/tests/test_browser_security_headers.py` |
| S-17 | A control character before `javascript:` slipped past the markdown link filter (stored XSS in notes, tasks and comments) | Medium | Fixed | `app/tests/test_repo_readme_markdown.py` |
| S-18 | Chat attachments kept the sender's MIME type, so an HTML or SVG attachment opened in a new tab ran on the App or World origin | Medium | Fixed | `app/tests/test_chat_shared_modules_frontend.py`, `world/tests/test_world_office_meeting_frontend.py` |
| S-19 | Mirror-supplied image diff data broke out of the `src` attribute | Medium | Fixed | `app/tests/test_repo_image_diff_preview.py` |
| S-20 | Fork row links accepted `/\host` and `/<tab>/host` | Low | Fixed | `app/tests/test_repo_image_diff_preview.py` |
| S-21 | `forkmesh://control/cloudflare?mode=vultr` read a token from the clipboard and created a mirror at once, handing it the saved Cloudflare token | High | Fixed | `desktop/tests/validate_system_link_vultr_confirm.py` |
| S-22 | A branch or base ref with a quote ran shell code in the agent worktree script | Medium | Fixed | `desktop/tests/test_agent_prompt_title.cpp` |
| S-23 | The IDE bridge lockfile holding its token was readable by other local users, and the token compare was not constant-time | Low | Fixed | `desktop/tests/test_ide_bridge.cpp` |
| S-24 | A code-approved phone could rename its stored user to the owner and pass every owner gate | High | Fixed | `desktop_go_new/internal/control/owner_claim_test.go` |
| S-25 | The agent socket folder in `/tmp` could be pre-created by another local user | Medium | Fixed | `desktop_go_new/internal/agentproc/socket_dir_test.go` |
| S-26 | A package migration could `END` the host transaction and `ATTACH` a database at any path | High | Fixed | `marketplace/tests/test_marketplace_lifecycle.py` |
| S-27 | Symlinks in a git or local package source copied local files into the package | Medium | Fixed | `marketplace/tests/test_marketplace_lifecycle.py` |
| S-28 | A committed `base: --output=/path` in a pull record made the MCP server's `git diff` truncate any file | Medium | Fixed | `tools/test_forkmesh_mcp_server.py` |
| S-29 | An image layer could write through a symlinked folder or hard-link host files in the baseline puller | Low | Fixed | `app/tests/test_linux_release_baseline.py` |
| S-30 | The vault fill could land on a page that navigated after the host check | Low | Fixed | `marketplace/packages/browser-extension/tests/background_vault_harness.js` |
| S-31 | The mirror gateway's handoff `next` accepted `/\host` and CR/LF | Low | Fixed | `app/tests/test_mirror_gateway.py` |
| S-32 | A branch name could inject script into the branch review page's `onclick` | Low | Fixed | `tools/test_branch_pr_review_guard.py` |
| S-33 | A cloned repository's tree could write into the phone's own `.git` folder (any case) | Medium | Fixed | `GitCheckoutTest`, `LocalEngineWorkflowTests` `testCheckoutRefusesATreeThatWritesIntoTheGitDirectory` |
| S-34 | The iOS push session copy could be restored from a backup onto another phone | Low | Fixed | `StoreConfigTests.testEveryKeychainItemStaysOnThisDevice` |
| S-35 | Client headers could set the mesh routing headers `x-fm-do-url`, `x-fm-do-id`, `x-fm-do-cf` on a cross-core Durable Object call | Low | Fixed | `app/tests/host/test_host_cluster_security.py` |
| S-36 | `/_app/_host/*` on a per-node host reached host-internal paths | Low | Fixed | `deploy/tests/test_render.py` `test_node_relay_never_reaches_host_internal_paths` |
| S-37 | The World dev server accepted any `Host` (DNS rebinding) and passed foreign origins upstream as its own | Low | Fixed | `world/tests/test_world_dev_server.py` |

## Open, already public

| Item | Severity | Status | Where described | What remains |
| --- | --- | --- | --- | --- |
| TM-004 legacy issue, discussion and pull records bind no repository | High | Partial | protocol threat model, [README.md](README.md) | v1 intake closes at `EVENT_SIGNATURE_V1_INTAKE_UNTIL_MS` |
| TM-006 catalog v1 signatures bind no visibility | High | Open | protocol threat model | v2 signing exists; v1 still accepted |
| TM-008 a relay or mirror can withhold events | High | Open | protocol threat model | not started |
| TM-014 tunnelled node traffic trusts the relay | High | Open | protocol threat model | not started |
| TM-018 no account-key recovery protocol | High | Open | protocol threat model | not started |
| Runner jobs share one uid; no private tmp or read-only system in the job scope | High | Partial | [RUNNERS.md](../RUNNERS.md) | job user, egress rules, owner opt-in and per-job grants are in |
| Runner jobs run with the persistent HOME, not their session home | High | Open | [RUNNERS.md](../RUNNERS.md) | `HOME` must be set inside the scope after the uid switch |
| Executable proof for agent boundaries | High | Partial | [agent-host-boundaries.md](agent-host-boundaries.md) | extend fixtures and checks |

## Private and operator-owned

Non-public open items stay off this page until fixed. Operator-owned items
(disk and swap encryption on the operator box, a passphrase on the fleet SSH
key, moving retired keys and the backup decryption key offline, a scoped
cloud sub-user, provider 2FA, CAA records) are tracked on the task board.
