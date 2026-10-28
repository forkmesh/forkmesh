# Security findings register

Reviewed through: `fc80139d04` on 2026-10-28 (full-system sweep, two rounds; S-38 to S-83)

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
| S-24 | A code-approved phone could rename its stored user to the owner and pass every owner gate | High | Fixed | `desktop/tests/test_local_control.cpp` `testCodePairedUserStays` |
| S-25 | The agent socket folder in `/tmp` could be pre-created by another local user | Medium | Removed | The Go engine that made the folder was removed on 2026-10-16; the desktop makes no such folder |
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
| S-38 | Raw repository reads answered by the host fast path kept the gateway's guessed XML type (`.atom`, `.rdf`, `.mml`, …) inline and dropped its CSP, so a committed file ran script on the App origin | High | Fixed | `app/tests/host/test_repo_fastpath.py`, `app/tests/test_mirror_gateway.py` |
| S-39 | Revoking the device that held the account's primary key left that key trusted for signed APIs, rotation and admin requests | High | Fixed | `app/tests/test_mobile_node_devices.py`, `app/tests/test_key_rotation.py`, `app/tests/test_admin_totp_required.py` |
| S-40 | `claim-node`, `claim-confirm` and `link-self` checked a password with no attempt budget and no TOTP | High | Fixed | `app/tests/test_account_totp_gate.py`, `app/tests/test_users_nodes_claim_link.py` |
| S-41 | Organization guests passed the Office tasks gate (every task, the `security` department included) and auditors could write | High | Fixed | `world/tests/test_world_office_tasks.py` |
| S-42 | Organization guests got member access through the package runtime (CRM, Marketing, Analytics, Competitor), the member email list and Vault organization grants; auditors could write | High | Fixed | `app/tests/test_mail_budget.py`, `marketplace/packages/crm/tests/test_member_emails.py`, `marketplace/packages/vault/tests/test_vault_api.py`, `marketplace/packages/analytics/tests/test_analytics.py` |
| S-43 | The CRM one-off email send had no outreach gate, no cap and a caller-chosen sender over the shared SMTP account | High | Fixed | `marketplace/packages/crm/tests/test_crm.py`, `app/tests/test_mail_budget.py` |
| S-44 | Repository workflows committed in a checkout ran on a schedule or event with no approval, and `desktop.*` steps ran the checkout's own cleanup script | High | Fixed | `desktop/tests/test_repo_workflow.cpp`, `desktop/tests/validate_repo_workflow_approval.py` |
| S-45 | The Patreon email fallback matched an account whose email was never confirmed | Medium | Fixed | `app/tests/test_patreon_runtime.py` |
| S-46 | Microjob claims were deduplicated by repository name and full URL, so fork copies and query variants paid again, and the daily caps could be raced | Medium | Fixed | `app/tests/test_microjobs.py` |
| S-47 | Gateway reads resolved a commit id that only hidden refs (branch trash, merge staging, a fork pool) reach | Medium | Fixed | `app/tests/test_mirror_gateway.py` |
| S-48 | The root runner daemon read a Codex job's last message through a path the job could replace with a symlink or FIFO | Medium | Fixed | `server/agents_test.go` |
| S-49 | Links in issues, pulls, discussions and the Markdown preview opened any URL scheme | Medium | Fixed | `desktop/tests/test_crypto.cpp`, `desktop/tests/validate_body_link_schemes.py` |
| S-50 | The MCP server followed committed symlinks in the pull, issue, project and milestone stores | Medium | Fixed | `tools/test_forkmesh_mcp_server.py` |
| S-51 | The host fast path kept serving cached commit-pinned reads after a repository went private or was removed | Medium | Fixed | `app/tests/host/test_repo_fastpath.py` |
| S-52 | Notes shared with an organization reached its guests, and auditors got editor rights | Medium | Fixed | `app/tests/test_notes.py` |
| S-53 | A revoked organization app install could be authorized again by any member or guest; guests saw the organization's private apps, commands and package state | Medium | Fixed | `app/tests/test_apps_oauth.py`, `app/tests/test_apps_commands.py` |
| S-54 | Succession approvals counted members added after the case opened | Medium | Fixed | `app/tests/test_organization_succession.py` |
| S-55 | The Discord OAuth callback was not bound to the browser that started it | Medium | Fixed | `app/tests/test_organization_discord.py` |
| S-56 | The ActivityPub inbox let a key from one account on a server send Undo or a self-Delete naming another account there | Low | Fixed | `app/tests/test_ap_inbox_load.py` |
| S-57 | `/api/security/report` had no per-source budget, and a flood evicted stored reports | Low | Fixed | `app/tests/test_security_report_alarm.py` |
| S-58 | A hosted fork made private stayed in its parent's fork list, advisory credits listed repositories gone private, and dissolving an organization left its office chat | Low | Fixed | `app/tests/test_repo_forks.py`, `app/tests/test_security_advisories.py`, `app/tests/test_org_office_chat.py` |
| S-59 | A contributor `pr/` branch push ran its own workflows on a mirror with Actions on | Low | Fixed | `server/actions_test.go` |
| S-60 | Each IPv6 address got its own login and front-door rate-limit bucket | Low | Fixed | `app/tests/host/test_host_request_policy.py`, `app/tests/test_login_key_binding.py` |
| S-61 | Two outside mirrors could serve or pin the flagship repository, and the cores' sync followed redirects to them, so a forged `main` could reach the operator checkout and the auto-deploy | Critical | Fixed | `app/tests/test_https_mirror_routing_integration.py`, `server/core_upstream_redirect_test.go`, `tools/test_source_publisher.py` |
| S-62 | Desktop auto-update installed a `release.json` from the newest-dated tag without checking its signature, repository, channel or version | Critical | Fixed | `desktop/tests/test_release_trust.cpp`, `desktop/tests/validate_auto_update_release_trust.py` |
| S-63 | Web file edits, release tags and other web commits to a core-held repository skipped the organization-owner push rule | High | Fixed | `app/tests/test_repository_file_edits.py`, `app/tests/test_repository_releases.py` |
| S-64 | A code-paired phone could claim the desktop owner's name at pairing and pass every owner-only family | Medium | Fixed | `desktop/tests/test_local_control.cpp` |
| S-65 | Key rotation bound any public key with no proof of possession, including another account's device key | Medium | Fixed | `app/tests/test_key_rotation.py`, `app/tests/test_git_push_route.py` |
| S-66 | `install.sh` accepted an older or wrong-channel signed manifest, and the release host signed candidate data it had not verified | Medium | Fixed | `app/tests/test_installer_release.py`, `app/tests/test_release_host_completion.py` |
| S-67 | An anonymous instance launch could rewrite or delete any domain's instance directory entry | Medium | Fixed | `app/tests/test_instance_directory.py` |
| S-68 | A public channel webhook pushed any account's text to every user, and a few accounts could exhaust the global channel cap | Medium | Fixed | `app/tests/test_chat_channels_api.py` |
| S-69 | A `keyId` with dot segments got past the ActivityPub actor check (S-56) | Medium | Fixed | `app/tests/test_ap_inbox_load.py` |
| S-70 | Workflow approval (S-44) was skipped by enable and schedule toggles, package runs, phone workflow runs and phone shortcuts, and more link views opened any URL scheme | Medium | Fixed | `desktop/tests/validate_repo_workflow_approval.py`, `desktop/tests/validate_body_link_schemes.py`, `desktop/tests/test_repo_workflow.cpp` |
| S-71 | iOS chat media and SMB file names could write outside the app's temporary folder, and Android acted on unstamped ping intents | Medium | Fixed | `native/ios/ForkMeshCore/Tests/FMChatTests/ChatMediaFileTests.swift`, `native/ios/ForkMeshCore/Tests/FMFilesTests/FileLocalTargetTests.swift`, `native/android/core/src/test/kotlin/com/forkmesh/nativeapp/security/PushIntentTrustTest.kt` |
| S-72 | Microjob referral dedupe merged different posts on one host and split one post across subdomain, case, slug and query variants | Medium | Fixed | `app/tests/test_microjobs.py` |
| S-73 | A member removed from a private channel stayed in a call already running | Low | Fixed | `app/tests/test_chat_call_tickets.py`, `app/tests/test_chat_group_calls_client.py`, `app/tests/test_android_group_call_page.py` |
| S-74 | A signed issue edit, comment or vote replayed under a new id or target was applied again | Low | Fixed | `app/tests/test_issue_submission_inbox.py`, `server/issue_placeholder_test.go` |
| S-75 | Login, profile and claim answered faster for unknown accounts | Low | Fixed | `app/tests/test_login_key_binding.py`, `app/tests/test_account_totp_gate.py` |
| S-76 | Badges, referral surfaces, the contributors board and a `?viewer=` follow flag exposed private profiles | Low | Fixed | `app/tests/test_badges.py`, `app/tests/test_referrals.py`, `app/tests/test_boot_endpoint_caches.py`, `app/tests/test_leaderboards_hub.py` |
| S-77 | The data export named the moderator and note behind an action, and the public status page repeated exception text | Low | Fixed | `app/tests/test_account_data.py`, `app/tests/test_status_page.py` |
| S-78 | The live note socket checked no Origin, and the host's stale and about caches outlived a repository going private and replayed one visitor's CORS headers | Low | Fixed | `app/tests/test_notes_signed_session.py`, `app/tests/host/test_repo_fastpath_stale.py`, `app/tests/host/test_repo_ssr.py` |
| S-79 | Bug report, product analysis, instance launch, guestbook and signup budgets counted each IPv6 address separately, and anonymous attachments had no global budget | Low | Fixed | `app/tests/test_bug_reports.py`, `app/tests/test_product_analysis.py`, `app/tests/test_login_key_binding.py` |
| S-80 | An anonymous ForkBot chat request could name any account as an issue's opener | Low | Fixed | `app/tests/test_forkbot.py` |
| S-81 | The shared package mail sender had no per-account cap and used a caller-chosen display name | Low | Fixed | `app/tests/test_mail_budget.py` |
| S-82 | Commit-id reads could force unbounded graph walks on the gateway | Low | Fixed | `app/tests/test_mirror_gateway.py` |
| S-83 | One request from a fleet runner could get its address probe-banned from the App | Low | Fixed | `deploy/tests/test_no_main_node.py` |

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
