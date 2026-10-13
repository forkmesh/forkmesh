# Native app progress

A living tracker for the native iOS and Android apps. Every
workstream updates its own row when it merges (see "How to update this file"
at the end).

- **Home repository: production ForkMesh** (this repository), since
  2026-09-27. From 2026-09-24 native work ran in the ForkMesh Next repository
  (`~/projects/forkmesh-backup/src`); its copies of `native/` and
  `docs/native/` were removed once everything was brought back here. Rows
  below that name a "Next" branch refer to branches in that backup.
- **Where the phone code builds today:** the production repository's
  `native/ios` (Codemagic clones the public production mirrors). Workstream
  branches are `agent/native-ws*` there, merged into `agent/native-gate2`;
  server and relay changes land in this repository first.
- **Default instance: `app.forkmesh.com`** (Config/Version.xcconfig). The cutover is done: app.forkmesh.com and world.forkmesh.com run on the Next stack and the next.* hosts redirect there, so the built-in "ForkMesh Next" project is being retired (`agent/native-next-alias`, merged: the next.* hosts are aliases of ForkMesh; an upgrade drops an unused Next row, folds a used one into an unused ForkMesh, or keeps it as "ForkMesh (from Next)"; a permanent redirect to a known project opens that project). Formerly: **ForkMesh Next** (`app.next.forkmesh.com`). Production
  ForkMesh and self-hosted instances are extra projects (WS1).
- Plans:
  - [PARITY.md](PARITY.md): the phone feature matrix, WS1–WS7, the watch app,
    and the bundle ids.
  - [DESKTOP_PARITY.md](DESKTOP_PARITY.md): desktop parity, Gate 2, WS8–WS21.
  - [NAVIGATION.md](NAVIGATION.md): tabs, routes, the iPad sidebar, and the
    checklist for new screens.

Last updated: 2026-09-24 (Gate 2 in review).

## Workstreams

Status words: **Not started**, **In progress**, **In review**, **Merged**,
**Blocked** (with the reason in "Remaining").

**Codemagic is paused** (free plan; the monthly macOS minutes are used up). Until they reset, every merge is verified on Linux: `swift test` for ForkMeshCore, `native/ios/tools/check_swift_syntax.sh` for the app and extensions, and a by-hand type-check review of each UI change. When minutes return: one combined build per merge batch with `FORKMESH_NATIVE_CORE_TESTS=0`, TestFlight first, no re-runs without a fix.

Build results name the Codemagic workflow: `ios-native-build` (a Debug build,
an unsigned archive, then the ForkMeshCore tests) or `ios-native-adhoc`.

| WS | Status | Branch | Owner area | Last Codemagic build | Merged commit | Remaining |
|---|---|---|---|---|---|---|
| Gate: first compile and installer hooks | **Merged** | `agent/native-gate-hooks` | `AppServices` hooks, pinned Swift packages | `ios-native-adhoc`: first successful archive (`93a0d7089`). `ios-native-build`: the app compiles; the macOS ForkMeshCore run found two failures that Linux does not show, `HuggingFaceSnapshotTests.testCancellationKeepsPartialFile` and an abort in `LocalEngineInteropTests.testSystemGitVerifiesAndReadsAForkMeshGeneratedPack`; the tests now run last (`1e1b3c837`) | production `87faa9064`, imported into Next as `33b54a1` | Fix the two macOS-only core test failures |
| WS1: projects and multi-instance isolation | **In progress** | `agent/native-ws1-projects` | `NIC/FMAccount` projects, `ProjectScope`, the switcher | Not run yet | — | PARITY #12, #13, #14, #17; make Next the default project; no commits on the branch yet |
| WS2: push, pings, code-push | **In progress** | `agent/native-ws2-push` | APNs, ping settings, the notification extension | Not run yet | — | PARITY #47, #48, #50, #52, and the server's per-token APNs topic (PARITY 5.2) in Next's `app/src/apns.py`; no commits on the branch yet |
| WS3: composer, photos, dictation, model catalog | **In progress** | `agent/native-ws3-composer` | The composer, the left-aligned four-tab Liquid Glass row and the prompt button, `NIC/FMAgents` models | Not run yet | — | Core done at `df660ca52` (photo attachments, image blocks, Opus 5.5 catalog). Left: the composer UI, dictation, the tab row and prompt button, the chat persona (#38), and prompt drafts (DESKTOP_PARITY 1.20) |
| WS4: Agent Watch and Listen | **Merged** into main (2026-09-27) | `agent/native-ws4-agentwatch` | `NIC/FMAgentWatch`, the Watch screen, audio and haptics output | Not run yet | branch head `cd85059d5` | Its last two commits import CoreGraphics on Apple and split the long geometry expressions in `AgentWatchLayout` and `AgentWatchField` into typed lets, because the ad hoc archive failed there (CGRect geometry comes from CoreGraphics on Apple, and two expressions exceeded the type checker's budget). Also merges cleanly into `agent/native-gate2`. Left: the Codemagic type-check; prune the branch after Gate 2 lands |
| WS5: serve as a node, move the Git engine | **In progress** | `agent/native-ws5-node` | `NIC/FMChat/NodeSync`, `NIC/FMCode/Local`, `NIC/FMGit` | Not run yet | — | Engine moved into FMGit at `fc2e11687`. Left: the real `NodeSyncWorkspace` adapter and per-project roots (needs WS1) |
| WS6: watchOS app and build pipeline | **In progress** | `agent/native-ws6-watch` | Watch targets, the project generator, `codemagic.yaml` (native) | Not run yet | — | The whole of PARITY §4; no commits on the branch yet |
| WS7: hosts and agents over the node (desktop control relay) | **In progress** | `agent/native-ws7-hosts` | The owner control relay (`app/src/node_control.py` and the relay in `entry.py`), desktop `LocalControl` over the node socket, the phone's hosts list and live logs | Not run yet | — | Uncommitted: the server frame grammar and relay with tests (in the production worktree). Left: port the server half to Next's app, then the desktop side, the phone side, the hosts list and the live logs (DESKTOP_PARITY 2.5, 7.1, 7.2, 8.8) |
| Agent detail attachments (WS3 and WS7 follow-up) | **In review** | production `agent/native-agent-attachments` (from `agent/native-adhoc-phone`); Next `agent/native-agent-attachments` (server) | FMApi `AgentAttachments`, `AgentAttachmentStore`, `ApiService+AgentAttachments`; FMChat `LocalAgentTranscript`; `Features/Agents/AgentAttachmentViews.swift`; desktop `LocalControlAttachments`; Next relay attachment store (migration 0200) | Not run yet (ready for `ios-native-build`) | — | Photos on the prompt bubble (relay, LAN, this phone), rows for other files, full-screen viewer (zoom, share, save), optimistic follow-ups. Deploy Next (migration 0200, app, runner/node Go) and a desktop build before the relay and LAN halves show pictures; WS7's AllAgents screen does not show them yet |
| App Store cutover: native ships as `com.forkmesh.forkmesh` | **Merged** (through Gate 2) | production `agent/native-appstore` | Bundle ids, `FMAccount/FlutterMigration`, `codemagic.yaml` `ios-native-release`, retiring the Flutter iOS release | `ios-native-release` green 2026-09-24 (build 6ab4e572593b75b705aa0180): 1.4.0 (17) uploaded, VALID, TestFlight internal (`forkmesh-team`) IN_BETA_TESTING; phone-only | — | Run the first release with the widgets and the watch embedded (IOS_RELEASE.md, "Widgets, Live Activities and the watch") and check them on a phone and a watch; test the upgrade from the store Flutter build on a phone. The rest: IOS_RELEASE.md, "Open items" |
| Gate 2: extension points for WS8+ | **Merged** | production `agent/native-gate2`: `agent/native-appstore` final (`033604e27`, 1.4.0 (17)), WS7, WS1, agent attachments (`f69ecea82`) merged in | `AppServices` hooks, `Route` contract, desktop control families, extension targets, associated domains | `ios-native-build` 6ab4eb19984a34b5874b2733 on `3a544aa51` (Gate 2 + WS9, WS10, WS14, WS15): Debug app, watch app and unsigned archive **compile**; the macOS core tests fail only on the known `HuggingFaceSnapshotTests.testCancellationKeepsPartialFile` and the LocalEngine transport abort. Linux: 2024 tests, 0 failures | — | Codemagic compile of the batch. Next half done on Next `agent/native-next-server` (`8497a11` AASA on every host; `1194e1d` WS7 relay port and cloud-run stop; `1e929a5` `agent_approval` ping + `POST /api/notifications/agent-approval`; `deb4997` Live Activity push and `POST /api/accounts/live-activity-token`), not merged; migration `0200` may collide. app.next must never be redirected at the AASA path (Apple follows no redirect) |
| WS8: navigation shell and information architecture | **In review** | `agent/native-ws08-navigation` | `HomeShell`, `NI/ForkMesh/Navigation`, `NIC/FMApi/Navigation`, search, command palette | Merged into `agent/native-gate2` (`a22c8045f`); compile check pending | — | Deferred: per-tab `.searchable` (tab roots are other workstreams'), scroll-to-top on reselect (router exposes `reselects[tab]`; roots must observe it), running-agent capsule, iOS 18 sidebarAdaptable (iOS 17 kept), per-window routers, ⌘R, restored-object fallback toast, sounds toggle (WS18). Server: `/api/version` needs `minimumClientVersions.ios` |
| WS9: Inbox and notification actions | **In review** | `agent/native-ws09-inbox` | `Features/Inbox`, notification categories | Compiles in `agent/native-gate2` (Codemagic 6ab4eb19 on `3a544aa51`) | — | Deferred: review requests and assigned issues across repositories (no reviewer model or assignment index on the server; from pings only), uploading phone failure reports, `aps.category` for non-approval pings beyond approval/review/mention |
| WS10: agents over desktop control | **In review** | `agent/native-ws10-agentops` | Approvals, merge & clean up, bulk actions, usage, `D/LocalControlAgentOps` | Compiles in `agent/native-gate2` (Codemagic 6ab4eb19 on `3a544aa51`) | — | Deferred: 2.10, 2.12, 2.29, 2.36, worktree actions other than merge & clean up, 8.2 firewall rules. The desktop now pushes a signed `agent_approval` ping per new approval, phone connected or not (`0de5d8742`). Attachments follow-ups (AGENT_ATTACHMENTS.md): the All agents screen shows no attachments; picture replies (up to 3.4 MB) are not carried over the relayed channel; the desktop takes no photos in LAN prompts |
| WS11: code review and collaboration | **In review** | `agent/native-ws11-review` | `Features/Code` (except CI), `FMCode/Collab`, `FMDiff` | Merged into `agent/native-gate2` (`3ea474743`); compile check pending | — | Next lacks: discussion status/delete, file history by path, ZIP archive, conflict file contents with markers, branch delete / update from base, pull close/reopen/update, repository rename/visibility/archive/delete and settings, issue hard delete, label colours. Deferred: 3D code map (GPU), `.tree`/`.commits` ref and path. Branch review threads without a pull request (2026-10-02): in the branch diff, tap a line number (and a second one for a range) to comment; threads show under their lines, file-level and outdated ones at the top of the file, with Reply and Resolve. Same ledger and signing as the desktop (`.forkmesh/branch-reviews/<key>/` on `forkmesh/pulls`, `branch-comment` v2 signatures, POST `{branch, event}` to `/pulls`), anchor-text placement only; `FMCode/Collab/BranchReview.swift`. App compile check pending |
| WS12: system surfaces | **In review** | `agent/native-ws12-surfaces` | Widgets, Live Activities, App Intents, Spotlight, share extension | Merged into `agent/native-gate2` (`a22c8045f`); compile check pending | — | 2.35 phone side done; server-driven Live Activities not wired (desktop run state is E2E; cloud runs have no status hook); start tokens should be dropped on sign-out server-side. Siri agent commands (merge, stop, continue, tell, delete, answer requests, open screens) added; deferred: create intents, Focus filter, IndexedEntity, PR widget. The app App Group entitlement waits for provisioning |
| WS13: chat depth | **In review** | `agent/native-ws13-chat` | `ChatScreen`, `ChatHub`, threads, office rooms, Discord | Merged into `agent/native-gate2` (`ff752ac64`); compile check pending | — | Deferred: admin delete (admin-key lookup), Discord reactions/edits/attachments (read and send only), hiding idle visitors (relay has no last-seen), saving attachments to disk, typing in mesh and office rooms |
| WS14: CI, releases and security | **In review** | `agent/native-ws14-ci` | `RepoActivityTabs`, CI, releases, security, webhooks | Compiles in `agent/native-gate2` (Codemagic 6ab4eb19 on `3a544aa51`) | — | Desktop-only: the approval gate hash (6.2), creating releases and notes, deleting tags and artifacts, "Scan now", building and previewing a PR (5.20 first half). Logs are the last 16 KB (`logTail`); Next has no log stream |
| WS15: planning | **In review** | `agent/native-ws15-planning` | Notes, tasks, calendar, timers, projects, milestones | Compiles in `agent/native-gate2` (Codemagic 6ab4eb19 on `3a544aa51`) | — | Deferred: creating or editing projects and milestones (git records written by the desktop; no server route), commits on the calendar (local git log), EventKit export, images in notes, the quick-add image picker |
| WS16: desktop and fleet remote | **In review** | `agent/native-ws16-desktop-remote` | Host operations, Vultr, desktop files, log and health | Merged into `agent/native-gate2` (`058b481e4`); compile check pending | — | Not done: 4.17 writes (stage, commit, push; `files.browse.` is read-only), 4.11 editor, 7.10 Actions variables (desktop secrets; see WS21 `secrets.`), 12.12 component-health score (heavy desktop scan), 12.4 live log follow (no polling; paged), 8.4 node delete keys on name (verify) |
| WS17: teammates and agent insights | **In review** | `agent/native-ws17-teammates` | Teammates | Merged into `agent/native-gate2` (`058b481e4`); compile check pending | — | Kept on the desktop: log monitors, permissions, deleting a teammate. Agent insights (tool efficiency, guardrails, usage, network, the phone announcer) was removed from the phone apps on 2026-09-29 and stays on the desktop. Repository shortcuts came back on 2026-10-01 as a Shortcuts tile on the repository screen (iOS and Android), and moved the same day to a Shortcuts page in the navigator |
| WS18: settings, account, identity, devices | **In review** | `agent/native-ws18-account`; Next `agent/native-ws18-account` (`GET /api/accounts/export`) | Settings screens, SSH keys, MCP, recovery kit | Merged into `agent/native-gate2` (`ff752ac64`); compile check pending | — | Deferred: restoring a recovery kit as this phone's identity (sign-in flow), desktop backup status (`diag.backups` built, `6e1e5287e`; screen pending), trust lists in force (no store in Next); dictation locale (WS3) and local repo defaults (WS5) not yet read. Desktop-only: Cloudflare token, startup/footer options, sync timers, whisper.cpp/Parakeet, QSS skins |
| WS19: business packages and the Market | **In review** | `agent/native-ws19-packages`; Next `agent/native-ws19-packages` (`GET /api/market/packages`) | Market, CRM, marketing, analytics, ledger, tips | Merged into `agent/native-gate2` (`058b481e4`); compile check pending | — | Web/desktop only: writing ads, CRM outreach and mail links, deck links, shared sheets, analytics replay and heatmaps. Wallet: the desktop signs; the phone only requests |
| WS20: stats, health, logs, admin | **In review** | `agent/native-ws20-insights`; Next `agent/native-ws20-insights` (session-checked `/api/admin/phone/*`) | Activity, stats, network, admin console, MetricKit | Merged into `agent/native-gate2` (`058b481e4`); compile check pending | — | Desktop `stats.` family done (`cd337d3eb`: timeline, quality, repo, Ratchet, Sentry); 7.20 relay diagnostics done in WS13 |
| WS21: coves, secrets, AutoFill | **In review** | `agent/native-ws21-coves` (desktop `secrets.` family added) | `FMCoves`, `FMVault`, the AutoFill extension | Merged into `agent/native-gate2` (`058b481e4`); compile check pending | — | Invite and "cove opened" notifications need WS13 relay calls; saving a password from another app while locked is not offered by iOS. 2026-10-02 (`1224-vault-plug-fill-websites`): AutoFill ships in every build (cache in the keychain group, no App Group), app links, iOS 18 text insertion, CSV/ZIP import and CSV export, Credential Exchange (iOS 26) both ways; ForkMeshCore tests pass on Linux, the app and extension still need a Mac compile and a device run |
| WS22: ForkMesh Apps surfaces | **In review** | `511-e9a2ad3ea2ca96ca20d366a9c1f015` | `NIC/FMApi/Models/Apps.swift`, `InstalledAppsModel`, `ChatHub` apps, `OfficeChannelService.post`, `Features/Account/InstalledAppsScreen.swift`, `ChatAppBlocksView` | Not run yet (Codemagic paused); Linux: `swift test` FMAccount, FMApi, FMChat green, app target parse-checked | — | DESKTOP_PARITY 9.24, 13.16. Installed apps (Settings › Installed apps, and `/dashboard/apps`): scopes, last used, Revoke, organization requests with Approve and Deny, "Browse the App Directory" in the browser. Chat: app commands in the composer and the shell prompt, `/apps`, ephemeral replies kept on the phone, block kit buttons and selects, mention forwarding from public office rooms. Consent: `/oauth/authorize` is never parsed as a repository, so it opens in Safari. Left: an Xcode compile and a device run; the desktop's three surfaces; Android (AW22) |
| Genie: on-device voice assistant (issue 1309) | **In review** | `1309-add-another-button-top` | `FMAssistant/Genie*`, `Features/Genie`, the Genie button in `ShellBottomBar`/`HomeShell` | Not run yet | — | Type-check `AppleGenieEngine` (FoundationModels) and the SwiftUI views on Codemagic; try the voice loop on a phone. See GENIE.md |
| Genie spider: crawls the whole app, edge tuck, long-press menu, hints, health, Apple Intelligence + Gemma (issue 1400) | **In review** | `1400-convert-genie-circle-spider` | `FMAssistant/Genie{Vitals,Hints,Duo,Crawl}`, `Features/Genie/GenieSpiderLayer`, `GenieCrawler`, `GenieAvatar` | Not run yet | — | Type-check the spider views on Codemagic; try crawling, tucking and the menu on a phone. See GENIE.md |
| Genie hand play: TrueDepth + Vision hand tracking, pinch to grab and fling, push, two-hand rotate and resize, 112 tricks, AI improvisation (issue 1424) | **In review** | `1424-drudedepth-sensor-iphone` | `FMAssistant/Genie{Hands,HandPlay,Tricks}`, `Features/Genie/GenieDepthCamera`, `GenieHandCoach`, `GenieAvatar`, `GenieSpiderLayer` | Not run yet | — | Type-check `GenieDepthCamera` (AVFoundation, Vision) and the views on Codemagic; on a Face ID iPhone check the camera orientation (`.leftMirrored`), the depth units and the gesture thresholds. See GENIE.md, "Hand play" |
| Genie spider: reference look, real gravity (falls, bounces, grabs walls, stands on the tab bar, climbs to the top, flick to throw), raw Apple model with native tools and conversation memory (issue 1451) | **In review** | `1451-plesae-spder-geine-look` | `FMAssistant/Genie{Gravity,Brain}`, `Features/Genie/GenieCrawler`, `GenieMotion`, `GenieAvatar`, `AppleGenieEngine`, `GenieSpiderLayer`, `HomeShell` | Not run yet | — | Type-check `GenieSpiderLayer`/`HomeShell` on Codemagic; on a phone check tilt direction per orientation, flick feel, and small talk vs tool calls. See GENIE.md |

WS1–WS7 were started in the production repository before native work moved
to Next. Each carries its branch over to the Next repository (cherry-pick onto
Next's `main`) and puts the Next branch name in its row. WS7's server half
must land in Next's `app/`, because Next's app is the server the phone uses.
Desktop-side changes (`desktop/src`) are made in the desktop's own repository,
since `desktop/` in Next is reference only.

### Follow-ups merged into `agent/native-gate2` (Linux-verified, not yet compiled by Xcode)

- Type-check reviews of all UI merged since the last Codemagic build: shell and surfaces (defensive fixes `a0872457f`), code/chat/account (`agent/native-review-code`), packages/insights/coves (`agent/native-review-business`).
- ForkMesh Next retired as a built-in project (`agent/native-next-alias`).
- Desktop: signed approval push, `stats.`, `diag.backups` (`agent/native-desktop-followups`); desktop-commands consumer (`agent/native-desktop-commands`).
- Phone (`agent/native-phone-followups`): dictation language, local repo author and default branch, one sound setting (default off), cove invites and "cove opened" over the relay, the Backups section, scroll-to-top on tab reselect (Agents, Code, Chat) and tab-scoped search, attachments on the All agents screen, branch "Commits" route, code-map reference ids.
- Repo-bound v2 event signing, always on: iOS and Android inbox, review and issue writers sign `forkmesh-<kind>-v2\n<owner>/<repo>\n` (`agent/native-v2-signing`, `fix/docs-audit-item6`; see `docs/security/README.md`).
- Open: Inbox scroll-to-top; commits by branch (needs a `ref` on the mirror history read in Next); relay picture replies and photos in LAN prompts (designed in AGENT_ATTACHMENTS.md).

## Capability checklist

A box is ticked when the capability is **Done** on iOS. Ticks are seeded
from the "Native iOS" columns of PARITY.md and DESKTOP_PARITY.md as of
2026-09-24. The workstream that owns an item is in parentheses. Desktop-only
rows of DESKTOP_PARITY.md are left out, since the phone will never have them.

### From PARITY.md (phone feature matrix, iOS column)


**Onboarding, auth and identity**

- [x] #1 Log in with password and TOTP, bound to the device key
- [x] #2 Sign up
- [x] #3 Guest mode and the guest gate on writes
- [x] #4 Device Ed25519 identity and owner-device enrolment, needed to read sealed agent…
- [x] #5 Portable identity recovery kit
- [x] #6 Account nodes, sessions and this-device cards; revoke
- [x] #7 iOS UDID lookup, iOS only
- [x] #8 User-presence gate
- [x] #9 Sign out and delete account

**Projects (multi-instance: ForkMesh, ForkMesh Next, self-hosted)**

- [x] #10 Project list, active project, probing a new instance, per-instance feature flags
- [x] #11 Separate login per project
- [ ] #12 Full per-project isolation: node key, owner key, vault device copy, local repos, agent… (WS1)
- [ ] #13 Project runtime: every service is rebuilt on a switch rather than re-pointed; removing… (WS1)
- [ ] #14 Quick project switcher (WS1)
- [x] #15 Projects card in Settings
- [x] #16 All-networks catalog
- [ ] #17 Per-project legal links (WS1)

**Shell, navigation and theme**

- [x] #18 Home shell: Code, Agents and Chat pages; Tools menu; bottom prompt bar; bell; avatar
- [x] #19 Theme, accents, frosted surfaces, design kit
- [x] #20 World

**Code, repositories and Git**

- [x] #21 Repository catalog
- [x] #22 Repository detail: code explorer, go-to-file, highlighted preview; commits; mirrors;…
- [x] #23 Issues: filters, detail, timeline, sub-issues, subscribe, new issue
- [x] #24 Pull requests: detail, timeline, merge plan and merge
- [x] #25 Discussions, including federated replies
- [x] #26 Branch diff and diff viewer
- [x] #27 Quick edit over the API into a proposed change or PR
- [x] #28 Code editor with key bar, and the highlighter
- [x] #29 Local repositories: create, import zip, browse, edit, commit, publish, PR
- [x] #30 Clone to device from the mesh
- [x] #31 Mirror import and import tokens
- [x] #32 Signed inbox writes
- [x] #33 Bounties, funding and the donation wallet panels
- [ ] #34 Serve as a node (WS5)

**Organizations and chat**

- [x] #35 Organizations: list, create, members, teams, repo links, delete
- [x] #36 Chat: channels, roster, DMs, unread banner, link text
  - 2026-10-02 (`1239-direct-message-view-look`, FMChat tests green on Linux, app files parse-checked, not yet compiled by Xcode): DMs and private groups look like iMessage: tailed bubbles, time lines, tapbacks, jumbo emoji, typing bubble, shared background, group name and photo, people management, photos, camera and voice messages. See `native/ios/docs/FMChat.md`, "Conversations".
  - 2026-10-02 (`1247-chat-page-way-create`, FMChat tests green on Linux, app files parse-checked, not yet compiled by Xcode): Slack-style workspace. A New message composer whose To: field autocompletes people (online, past DMs, office members, account search) and channels; one person opens a server DM, several make a private group. Jump to / search with `from:` `in:` `has:` filters, Mentions, Saved items, Star and Mute, per-room drafts, a Direct messages section, Share message, Remind me, formatting, `#channel` and `:emoji:` completion, `/msg` `/dm` `/shrug` `/me` `/mute` `/search` `/saved`. See `native/ios/docs/FMChat.md`, "Slack-style workspace".
- [x] #37 Room crypto and per-device seal
- [ ] #38 Chat persona: speak as the account or a stable "Guest ####", never as the node key (WS3)
- [x] #39 Local mesh room over the LAN

**Tools: notes, tasks, calories, vault, activity**

- [x] #40 Notes: list, search, editor, visibility, history, share
- [x] #41 Tasks: create, check in, complete, reply
- [x] #42 Calories: ring, charts, meals, goals, challenges, photo estimate
- [x] #43 Password vault: fmv1 suite, unlock, reveal and copy with a 30 s clipboard clear,…
- [x] #44 Activity: network stats, leaderboards, payouts, relay connection log
- [x] #45 Sponsored ads

**Pings, notifications and push**

- [x] #46 Pings inbox: filters, swipe delete, mark read, context sheet, "start agent from ping"
- [x] #46a Delete all pings (iOS and Android): a red trash button beside the Inbox options menu, and on
  the account Pings screen's header, deletes everything in one tap with no confirm (the user's request,
  2026-10-02); it is greyed out while the inbox is empty and the result is reported inline (iOS: a line at the top of the
  Inbox list or under the Pings filter bar; Android: a line above the
  Inbox list or the Pings list; no snack bar since 2026-10-09, the user asked for no toasts). It sends one
  `DELETE /api/notifications` with `all: true` and empties the list only once the server accepts, since
  the offline queue holds per-ping operations and cannot replay "all". On success the queued ping
  operations and the phone's own failure pings are dropped too. Since 2026-10-01 it also clears the
  assigned-task rows, so the Inbox is empty afterwards: a cleared task (and a swiped-away one) is kept in
  `inbox/dismissed-tasks.v1` by its `task:<id>@<updatedAt>` row id, so it stays hidden across reloads and
  comes back only when the task changes. Tasks are not deleted on the server; the Tasks screen still
  lists them.
- [x] #46b Ping detail page (iOS `PingDetailScreen.swift`, Android `PingDetailScreen.kt`). Tapping a ping
  row opens it instead of jumping straight to the link, so a ping with no link still leads somewhere and
  the reader sees everything before acting. Opening it marks the ping read. It shows the whole title and
  body, the pills, and a Details list built by `pingDetailFacts`: kind, received time and age, read state,
  repository, sender, source, link, the scalar `meta` fields and the id. A button names what the link
  opens ("Open issue #12", "Open in browser"). The other actions are Send to agent (iOS), Copy text,
  Copy link (iOS), Share, ping settings, and Delete after a confirm. Android gained the one-ping
  `DELETE /api/notifications` with `id` that iOS already used for swipe delete. The iOS context sheet is
  gone, since the page replaces it.
- [ ] #47 Ping settings screen: per-kind switches, pause, quiet hours, email, push card with… (WS2)
- [ ] #48 Code-push pings (WS2)
- [x] #49 Live account events and the agent-list doorbell
- [ ] #50 APNs register, unregister, environment, test push, badge, pending tap (WS2)
- [x] #51 Notification service extension: `GET /api/push/alert`, portrait, badge, de-duplication
- [ ] #52 Per-project push: one keychain item per project host; the extension picks the project… (WS2)
- [x] #53 In-app deep links from a ping

**Agents**

- [x] #54 Agents tab: one Agents list (each row marked Desktop or Cloud) and a New segment; session detail; sealed-session notice
- [x] #55 Queue a run on a desktop or mirror; steer, retry, cancel; task-chain stages
- [x] #56 Coding agent on the phone, using the user's own Anthropic or OpenAI key against a…
- [ ] #57 Model catalog (WS3)
- [x] #58 Provider-login terminal
- [x] #59 Desktop on this network: pairing code, start, prompt, stop, continue, transcript, send…
- [ ] #60 Agent Watch screen (WS4)
- [ ] #61 Listen sonifier engine: motions, voices on the D-major pentatonic grid, marimba synth… (WS4)
- [ ] #62 Sound and haptics output (WS4)
- [x] #64 On-device Gemma 4 through MLX
  - 2026-10-01 (`feat/ios-gemma-chat-entry`, Linux-verified, not yet compiled by Xcode): "Chat with Gemma" is always the first row of Settings › On-device AI, disabled with the one missing step and its fix when the chat cannot run; the Agents tab tool row opens the same chat. See `native/ios/docs/FMLocalAI.md`, "Chat entry".
  - 2026-10-04 (`agent/21-have-the-local-gemma-model`, not yet compiled by Xcode or the Linux toolchain): the composer's model dropdown offers "Gemma 4 · iPhone", which runs the agent on this iPhone with on-device Gemma. See `native/ios/docs/FMAgents.md`, "Quick launch from the composer".

**Composer**

- [ ] #65 One floating frosted composer for every agent prompt and chat: growing field, "+",… (WS3)
- [ ] #66 Photo attachments: camera or library; downscale to 2048 px; JPEG q80 with all metadata… (WS3)
- [ ] #67 Dictation (WS3)
- [ ] #68 Image blocks through the providers and the session queue (WS3)

**Status, settings, diagnostics and cross-cutting**

- [x] #69 User status and presence
- [x] #70 Settings: profile, avatar, payout address, this device, serve as node, room…
- [x] #71 Telemetry screen and strip, log categories
- [x] #72 Performance monitor, debug overlay, stall log
- [x] #73 Bug report button and sheet
- [x] #74 Trust lists v1 resolver
- [x] #75 Accessibility labels (WS8)
- [x] #76 Privacy manifest

### From DESKTOP_PARITY.md (desktop parity; desktop-only rows left out)


**1. Shell, navigation, search and window chrome**

- [ ] 1.1 Activity rail: Repo, Repos, Market, Ledger, Tasks, Calendar, Notes, Pings, Agents,… (WS8)
- [ ] 1.2 Back/forward history with a history menu, restored per page and sub-place (WS8)
- [ ] 1.3 Project picker in the top bar (WS1)
- [x] 1.4 Relay/server picker with favicons; add, rename, remove, switch (WS1)
- [ ] 1.5 Global search overlay: repositories, issues, pull requests, commits, files, packages,… (WS8)
- [ ] 1.6 Header search dropdown and the full results page, with buckets (WS8)
- [ ] 1.7 Saved searches, each with the counts from its last run (WS8)
  - [ ] Follow-up: the desktop's "Security pertinent" flag and alarm (2026-09-30, [../desktop/saved-searches.md](../desktop/saved-searches.md)). `SavedSearch` in `NIC/FMApi/Search/SavedSearches.swift` and Android `GlobalSearch.kt` have no flag yet; phones already get the desktop's alarm Ping.
- [ ] 1.8 Find in page (WS11)
- [ ] 1.9 Keyboard shortcuts across the app (WS8)
- [ ] 1.10 Command palette: act on anything from the keyboard (WS8)
- [ ] 1.13 Themes, including package skins (WS18)
- [x] 1.14 Interface sound cues (WS8)
- [ ] 1.15 Onboarding tour with a spotlight overlay (WS8)
- [ ] 1.16 First-run choice and Quick Setup (WS18)
- [ ] 1.18 Footer status bar: Git identity, commit, diagnostics tiles, website status lights (WS20)
- [ ] 1.19 Floating prompt overlay (WS3)
- [ ] 1.20 Prompt drafts kept on disk across a restart or crash (WS3)
- [ ] 1.21 Spell-check squiggles in the prompt (WS3)
- [x] 1.22 Sponsored strip
- [ ] 1.23 Membership prompt (WS18)
- [ ] 1.26 Desktop notifications and the tray (WS2)
- [ ] 1.28 "Build too old" gate for retired builds (WS8)

**2. Agents**

- [ ] 2.1 Sessions list: status, provider, model, branch, turns, tokens, cost, files changed,… (WS7, WS10)
- [ ] 2.2 Sort and filters (WS10)
- [ ] 2.3 Launch: repo, issue, provider, model, permission mode, thinking level, priority, images (WS3, WS10)
- [x] 2.4 Transcript
- [x] 2.5 Steer, stop and continue a running session (WS7)
- [ ] 2.6 Session detail card (WS10)
- [ ] 2.7 Merge & clean up: merge the agent branch into base, remove the worktree, delete the… (WS10)
- [ ] 2.8 Open a PR for a finished run automatically (WS10)
- [x] 2.9 Cloud sessions: list, detail, follow-up, delete
- [ ] 2.10 Move to cloud (WS10)
- [ ] 2.11 Stop all, Start all, Update all (WS10)
- [ ] 2.12 Run order and priority (WS10)
- [ ] 2.13 Approvals: the command approval chip and dialog, external-call gate prompts,… (WS10)
- [ ] 2.14 Capability grants (WS10)
- [ ] 2.16 Per-agent network sampler (WS17) — dropped from the phone apps on 2026-09-29 with Agent insights; desktop only
- [ ] 2.17 Watch: the fleet as a field of bodies (WS4)
- [ ] 2.18 Listen: a run played as music (WS4)
- [ ] 2.19 Announcer (WS17) — dropped from the phone apps on 2026-09-29 with Agent insights; desktop only
- [ ] 2.20 Usage meters and usage-limit reminders (WS10)
- [ ] 2.21 Several agent accounts per provider (WS10)
- [x] 2.22 Provider-login terminal for the desktop and for hosts
- [ ] 2.26 Tool efficiency board, ledger, usage table, improvement log; turning off tool families (WS17) — dropped from the phone apps on 2026-09-29 with Agent insights; desktop only
- [ ] 2.27 Guardrails board and ledger (WS17) — dropped from the phone apps on 2026-09-29 with Agent insights; desktop only
- [ ] 2.28 Repository Shortcuts tab: shortcuts and skills and the skill usage chart (WS17) — back on the phones on 2026-10-01 as the repository's Shortcuts tile, now a navigator page (NAVIGATION.md §2.0.29) (list, read, run prompt shortcuts and skills; since the same day also script shortcuts, with their output streamed to the phone); since 2026-10-05 prompt shortcuts and skills also run on the iPhone itself, and with no desktop the page lists the phone's mirror of the repository (NAVIGATION.md §2.0.57; not yet on Android, not yet compiled by Xcode); editing and the usage chart stay on the desktop
- [ ] 2.29 Issue looper (WS10)
- [ ] 2.30 Runs linked to an organization task, with status synced to the task (WS15)
- [x] 2.31 Agent recycle bin (WS10): list, restore, delete for good, empty and retention over `agentops.bin*` on iOS and Android (2026-10-02, DESKTOP_AGENTS.md "Recycle bin from the phone")
- [x] 2.32 Agent records synced to the account
- [x] 2.33 Local models
- [ ] 2.34 High-memory processes panel (WS16)
- [ ] 2.35 Agent events: finished, failed, needs attention (WS12)
- [ ] 2.36 Project agent access per project (WS10)

**3. Teammates (the Team page)**

- [ ] 3.1 Teammate grid and dashboard (WS17)
- [ ] 3.2 Teammate editor: identity, provider and model, backup models, task agents, skills,… (WS17)
- [ ] 3.3 Run the team now; generate and read briefings (WS17)
- [ ] 3.4 Schedules; enable or disable all (WS17)
- [ ] 3.5 Team stats and task grades (WS17)
- [ ] 3.6 Prompt, stop or continue a teammate (WS17)

**4. Repositories and code**

- [ ] 4.1 Network repository catalog: remote and private/local tabs, commit sparkline, merge… (WS11)
- [ ] 4.2 Fork or mirror a network repository (WS11)
- [ ] 4.3 Admin purge of a repository (WS20)
- [x] 4.4 Repository overview: README, latest commit, files
- [x] 4.5 File tree, preview, file tabs, go to file
- [x] 4.6 Edit a file and save; propose a PR from an edit
- [ ] 4.7 New, rename and delete file entries (WS11)
- [ ] 4.8 File history (WS11)
- [ ] 4.9 Code map: a treemap with the source drawn in each cell, and a 3D view (WS11)
- [ ] 4.11 Files page: the real filesystem, editor tabs, find and replace, go to line, commit… (WS16)
- [ ] 4.12 Code editor: highlighting, find and replace, go to line (WS11)
- [ ] 4.13 Symbol outline and trace (WS11)
- [x] 4.14 Commits list: stats, load more, file rows, markers
- [ ] 4.15 Commit workspace: changes, one commit, and a range page (WS11)
- [ ] 4.16 Commit graph (WS11)
- [ ] 4.17 Source control: stage, unstage, discard, revert to base, ignore, delete, commit,… (WS16)
- [ ] 4.18 Branches: local and remote list, new, delete, delete merged, compare with base, range… (WS11)
- [ ] 4.20 Worktrees: list, switch, merge into main, remove, update from main, edit or resolve… (WS10)
- [x] 4.21 Diff viewer: unified and side by side, sticky file header, "Viewed" stamps with… (WS11)
- [ ] 4.22 Merge conflict editor and AI conflict fix (WS11)
- [x] 4.23 Stash-free flows
- [ ] 4.24 Fetch, pull and sync a repository; sync badges (WS16)
- [ ] 4.25 Download a ZIP (WS11)
- [ ] 4.26 Fork a repository; configure the fork source (WS11)
- [ ] 4.27 Repository settings: General, Git, Agents, Automation, Danger zone; remotes; relay… (WS11)
- [ ] 4.28 Repository permissions (WS11)
- [ ] 4.29 Insights tab (WS20)
- [ ] 4.30 Quality tab (WS20)
- [ ] 4.31 Repository stats and Ratchet mode (WS20)
- [x] 4.32 Clone to device; mirror import
- [x] 4.34 Reference links (WS11)

**5. Issues, pull requests, discussions, projects and tasks**

- [ ] 5.1 Issues list: tabs, filters, query, labels, milestones, priority chips, progress drag (WS11)
- [ ] 5.2 Issue thread: sub-issues, checklist, reactions, federated thread (WS11)
- [ ] 5.3 New issue with images; quick add with history; issue from a chat message; publish to… (WS11)
- [ ] 5.4 Inline edits: title, labels, milestone, dates, priority, assignees; label definitions (WS11)
- [ ] 5.5 Issue board (WS11)
- [ ] 5.6 Burnup chart (WS20)
- [ ] 5.7 `@model` mentions in threads, and "Ask AI" (WS11)
- [ ] 5.8 Reprioritize the backlog; prioritize from the README (WS11)
- [ ] 5.9 Link an issue to a PR, and close linked issues (WS11)
- [ ] 5.10 Copy an issue or its thread; send it to the prompt (WS11)
- [ ] 5.11 Delete an issue (WS11)
- [ ] 5.12 PR list with a branch-state column (WS11)
- [ ] 5.13 PR detail: Conversation, Commits, Files and Checks (WS11)
- [ ] 5.14 Inline review comments, batched review, thread replies and resolution, thread anchoring (WS11)
- [ ] 5.15 Submit a review (WS11)
- [ ] 5.16 Merge with the plan, blockers and conflict card; close, reopen; update the branch (WS11)
- [ ] 5.17 AI review; fix findings or conflicts with an agent; apply a suggestion (WS11)
- [ ] 5.18 New PR from a branch, a directory, a source, or an imported patch (WS11)
- [ ] 5.19 Merge queue per repository: enable, pause, add, remove, reorder, process (WS11)
- [ ] 5.20 Build and preview a PR; run or approve its checks (WS14)
- [ ] 5.21 Bounties: fund a merged PR, auto-bounty, watch the payout (WS19)
- [ ] 5.22 Discussions: list, detail, create, comment, status, delete, inbox sync, federated replies (WS11)
- [ ] 5.23 Repository projects with a Gantt chart and linked issues (WS15)
- [ ] 5.24 Milestones: create, update, progress (WS15)
- [ ] 5.25 Organization tasks: list, detail, history, create or quick add, follow-up, assign to… (WS15)
- [x] 5.26 Signed inbox writes for issues, PRs and comments
- [ ] 5.27 Contributor reward estimates and payout receipts (WS19)

**6. Actions (CI), security, webhooks and releases**

- [ ] 6.1 Actions: workflows, runs table, run log, step progress, manual run, node picker,… (WS14)
- [ ] 6.2 Approve, reject, rerun or stop a run; the approval gate (WS14)
- [ ] 6.3 Send a failed run to an agent (WS14)
- [ ] 6.4 Checks on PRs (WS14)
- [ ] 6.8 Security tab: dependency scan, vulnerability snapshot, open a file at a line (WS14)
- [ ] 6.9 Report a vulnerability (WS18)
- [ ] 6.10 Repository webhooks (WS14)
- [x] 6.10a Mirror a repository to GitHub or GitLab, iOS and Android (2026-10-13, [../app/provider-mirrors.md](../app/provider-mirrors.md))
- [ ] 6.11 Releases: list, detail, new release, notes written by an agent, delete a tag, prune tags (WS14)
- [ ] 6.12 Announce a release on the Fediverse or Mastodon (WS14)
- [ ] 6.13 Artifacts tab: list, delete, prune (WS14)
- [ ] 6.15 Mirror nodes tab per repository: status lights, online only, SSH metadata, a… (WS14)
- [ ] 6.16 Mirror push (WS16)
- [ ] 6.17 Mirror sync verdicts and human-readable sync status (WS14)

**7. Hosts, mirrors, fleet and the Control page**

- [ ] 7.1 Hosts table: saved hosts, fleet summary, probe, health (WS7)
- [ ] 7.2 Live view of host logs (WS7)
- [ ] 7.3 Add or forget a host; SSH password prompt (WS16)
- [ ] 7.4 Install, reinstall, update from source, deploy to all in parallel, uninstall (WS16)
- [ ] 7.5 Host link code (WS16)
- [ ] 7.6 Vultr mirror provisioning: stages, progress, resume, DNS, seed push, publication check (WS16)
- [ ] 7.7 Destroy a Vultr instance; reconcile the desired fleet, with a countdown (WS16)
- [ ] 7.8 Host disk usage browser (WS16)
- [ ] 7.9 Install the agent CLIs on a host (WS16)
- [ ] 7.10 Host actions configuration (WS16)
- [ ] 7.11 Control > Mirror client (WS16)
- [ ] 7.12 Control > Keys and wallet (WS18)
- [ ] 7.15 Control > API token (WS18)
- [ ] 7.17 Mirror gateway health (WS16)
- [ ] 7.19 Serve as a node: chat history, heartbeat, node event socket, catalog publish (WS5)
- [ ] 7.20 Node self-diagnostics pushed in heartbeats (WS20)
- [x] 7.21 SSH terminal to a saved host, through the desktop
- [x] 7.22 Install (binary) on one host

**8. Network, mesh, nodes and users**

- [ ] 8.1 Network page: Relays, Nodes, Hosts, Endpoints, WebSocket objects, Web requests (WS20)
- [ ] 8.2 Outbound firewall: rules, recent decisions, and Allow/Deny/Deny-once prompts (WS10)
- [ ] 8.3 Nodes directory: platform, online, owner, catalog info (WS20)
- [ ] 8.4 Delete a node completely: destroy its Vultr server, remove its DNS (WS16)
- [ ] 8.5 Node profile: hosting stats, account status, linked nodes, link a node to a user, SOL… (WS18)
- [x] 8.7 Local mesh room
- [x] 8.8 Local control: the phone pairs with and drives the desktop (WS7)
- [ ] 8.9 Users admin: all users, signup and referral codes, activity, quarantine and… (WS20)
- [x] 8.10 HTTP/3 client
- [x] 8.11 App log archive and web-request log
- [ ] 8.12 QR codes (WS18)

**9. Pings, chat, DMs and World**

- [x] 9.1 Pings page: list, delete selected, web alerts pager, open a row, acknowledge, re-raise…
- [ ] 9.2 Pause pings menu; send a ping to the prompt (WS9)
- [ ] 9.3 Ping settings: kinds, email, quiet hours, desktop-only kinds (WS2)
- [ ] 9.4 Chat channels, roster, unread, typing, avatars, sender profile (WS13)
- [ ] 9.5 Threads (WS13)
- [ ] 9.6 Reactions (WS13)
- [ ] 9.7 Edit or delete your own message; admin delete (WS13)
- [ ] 9.8 Channel groups: create, collapse, move a channel (WS13)
- [ ] 9.9 Private channels: add, invite, rename, delete; channel settings; room seal prompt (WS13)
- [x] 9.10 Direct messages
- [ ] 9.11 Mention autocomplete, emoji picker, insert link (WS13)
- [ ] 9.12 Ask ForkBot in chat (WS13)
- [ ] 9.13 Turn a message into an issue or a prompt (WS13)
- [ ] 9.14 Media in chat (WS13)
- [ ] 9.15 Office channels (WS13)
- [ ] 9.16 The desktop's `#team` room for teammates (WS17)
- [ ] 9.17 Discord bridge (WS13)
- [ ] 9.18 Visitor presence and the welcome announcement (WS13)
- [ ] 9.19 Chat history limits and local persistence (WS13)
- [x] 9.20 User status and avatars
- [x] 9.21 Room crypto and room seal
- [x] 9.22 World
- [ ] 9.24 ForkMesh Apps in chat: slash commands, replies only you see, interactive app messages, mentions (WS22)

**10. Notes, calendar and timers**

- [x] 10.1 Notes: local and cloud list, Markdown editor with live preview, visibility, share,…
- [ ] 10.2 Notes: dropped or pasted images; attach a conversation; turn a note into a task or an… (WS15)
- [ ] 10.3 Calendar: month and week views, personal and organization events, repository commits… (WS15)
- [ ] 10.4 Timers: the top-bar clock; timer, countdown and alarm chips that survive a restart;… (WS15)

**11. Marketplace and packages**

- [ ] 11.1 Market: All, Installed and Not installed; install and remove; package settings;… (WS19)
- [ ] 11.2 Package extensions: native package pages, log tabs, search contributions (WS19)
- [x] 11.3 CRM: pipeline stage strip, contacts, organizations, activity, outreach, tracked… (WS19; iOS and Android 2026-10-02)
- [ ] 11.4 Marketing: content plan and ads across 100 channels (WS19)
- [ ] 11.5 Product analytics: events, funnels, cohorts, session replay, heatmaps, flags,… (WS19)
- [ ] 11.6 Competitor watchlist (WS19)
- [ ] 11.7 ForkMesh Ads for advertisers and admin review (WS19)
- [ ] 11.8 Ledger: public financial records, your own expense and income records, wallet… (WS19)
- [ ] 11.9 Tips between members (WS19)
- [ ] 11.10 Treasury donation dialog (WS19)
- [x] 11.11 Password vault
- [ ] 11.12 Vault sharing with a user or organization; fills in other apps (WS21)
- [x] 11.14 Calories
- [x] 11.15 Capture: screen region, markup, attach to a prompt
- [ ] 11.16 Sentry: unresolved issues, Log activity lights (WS20)
- [x] 11.19 World packages

**12. Stats, health, logs and diagnostics**

- [ ] 12.1 Stats board: tiles for repositories, issues, agents, tool efficiency, host load and… (WS20)
- [x] 12.4 Log page > Activity: the desktop's log with categories, filters, follow, pop-out, full log (WS16)
- [ ] 12.5 Log page > the 30-day activity timeline (WS20)
- [ ] 12.6 Log page > Most frequent, Usage, D1 usage, Build, Cron (WS20)
- [ ] 12.7 Log page > Database: live table inventory, paged rows, audited insert, update and delete (WS20)
- [ ] 12.8 Admin error console and website status probes (WS20)
- [x] 12.9 The desktop's stall watchdog and stall log; send one to an agent (WS16)
- [ ] 12.10 Crash handler, crash log scan, crash repair dispatch (WS20)
- [ ] 12.12 Desktop health summary (WS16)
- [ ] 12.13 Failures of local operations reported as toasts or pings (WS9)
- [x] 12.14 Bug report

**13. Settings (every section of Settings)**

- [x] 13.1 Profile: rename, avatar, account profile dialog
- [ ] 13.2 General: startup, appearance, footer, repository sync (WS18)
- [ ] 13.4 Repos: sync interval, default locations (WS18)
- [ ] 13.5 SSH Keys: list, add, remove public keys (WS18)
- [ ] 13.6 Pings (WS2)
- [ ] 13.7 Agents: providers, backend, usage, behaviour, which models the composer shows, recycle bin (WS18)
- [ ] 13.8 Voice: install whisper.cpp or Parakeet, mic test (WS18)
- [ ] 13.9 Secrets: import tokens; Actions variables and secrets (WS21)
- [x] 13.10 Organizations
- [ ] 13.11 MCP connector (WS18)
- [x] 13.12 Phone: pairing code and approved phones (WS18)
- [ ] 13.13 Security (WS18)
- [ ] 13.14 Data: data folders, export and import the configuration, restore, hourly backups, back… (WS18)
- [ ] 13.15 One scrolling settings page with a section list (WS18)
- [ ] 13.16 Installed apps: scopes, last used, revoke; organization install requests (WS22)

**14. Accounts, identity, keys and devices**

- [ ] 14.1 Switch account, recent accounts, log out, uninstall (WS18)
- [ ] 14.2 Identity key and the "back up your key" nag (WS18)
- [x] 14.4 Owner-device enrolment
- [x] 14.5 Trust lists (WS18)
- [ ] 14.6 Link a node to a user with a code or QR; poll for the grant (WS18)
- [ ] 14.7 Solana balance of a node (WS19)

**15. Projects and multi-instance**

- [ ] 15.1 Projects registry (WS1)
- [ ] 15.2 Relay scope of each repository record (WS1)

**16. Coves (encrypted file vaults) and secrets**

- [ ] 16.1 Coves: create, unlock with a password, invite a user, explorer tree, ten quick-create… (WS21)
- [ ] 16.2 Repository settings > Coves (WS21)

**17. Releases of the desktop itself, updates and backups**


**18. Background capabilities**

- [x] 18.4 Discussion inbox drain with backoff
- [ ] 18.5 Ping sync queue for offline pings (WS9)

## Android (`native/android`)

**Release status (paused 2026-09-24).** Decided: native Android ships to
everyone as `com.forkmesh.forkmesh` 1.4.0 (versionCode 10400), signed with the
v3 release key, through an Android-only signed channel
(`.forkmesh/releases/android/`) that creates NO new git tag, because deployed
desktops treat the newest tag of any name as an update and would rebuild.
Paused by the user before anything went public. Resume from branch
`merge/native-android` (production `main` 275c5ee4b + `agent/native-android-parity`
+ `release/native-android-apk`, not merged to `main`): finish the channel
tooling (publisher `--app-version`/`--app-version-code`, release host
`--android-channel --commit --version [--dry-run]`, homepage and `/status`
preferring the android channel, a guard that `latest` never loses the macOS or
Windows asset), dry run, publish to the cores via `config/fanout/release-hosts`,
then verify the homepage, `/status`, `aapt2 dump badging` (10400) and the signer
(`e8c028a4…`).

Since the 2026-09-24 production cutover, `app.forkmesh.com` is served by the
Next stack and `next.*` redirects there. The Android app defaults to ForkMesh
(`app.forkmesh.com`); its built-in "ForkMesh Next" project is folded into it
(AWf).

The release host builds the APK from `native/android` as
`com.forkmesh.forkmesh` (`BUILD_APK_SCRIPT` in `tools/forkmesh-release-host.py`),
so it installs over the retired Flutter APK and migrates its data (ANDROID.md
section 4). Local builds default to `com.forkmesh.nativeapp`, which installs
beside it.

- **Branch:** production `agent/native-android-parity` (worktree
  `~/projects/forkmesh/worktrees/native-android-parity`). Area branches are
  `agent/native-android-<area>` off it. Server work lands in Next first.
- **Build:** `./gradlew :core:coreTest :app:assembleDebug` on the deploy box
  (no Codemagic). Branch tip `93f5f244b` (comments stripped; reasons in `docs/native/ANDROID.md`): 1533 core tests pass, debug APK builds; 145 commits, about 118k lines over production `main`.
- **Same API contracts as iOS.** Android mirrors the iOS workstream decisions
  and reuses their server routes; it adds no routes of its own unless a row
  says so.

### Android workstreams

| AW | Status | Branch | Owner area (`A/feature/…`, `C/…`) | Mirrors | Remaining |
|---|---|---|---|---|---|
| AW0: integrate stranded ports | **Merged** into the branch | `agent/native-android-parity` | — | — | `native/code`, `native/chat` and `native/localrepos` merged; the uncommitted tools core from 2026-09-14 recovered. Its Compose half and one `LocalRepoScreenTest` case are set aside for AW3 and AW4 |
| AW1: projects, multi-instance, Flutter data migration, app id switch | **Merged** into the branch (62617e41c) | `agent/native-android-aw1` | `feature/account`, `C/account/projects`, `C/migration` | WS1, iOS `FlutterMigration` (`28664ba2d`) | Device test of the migration from a phone that ran the last Flutter APK (`BUILD_APK_SCRIPT` now builds with Gradle). The shipped APK is signed with the **v3** key (`e8c028a4…`) at versionCode 10204; native defaults to 10205 |
| AW2: agents | **Merged** into the branch (62617e41c) | `agent/native-android-aw2` | `feature/agents`, `C/agents` | WS7, WS10 (partly) | Device run; seams to local repos and the mesh are wired by the integration branch `agent/native-android-awi` |
| AW3: tools | **Merged** into the branch (62617e41c) | `agent/native-android-aw3` | `feature/tools`, `C/tools` | — | Device run; device unlock needs Android 9+ |
| AW4: local repositories and code gaps | **Merged** into the branch (62617e41c) | `agent/native-android-aw4` | `feature/localrepos`, `feature/code`, `C/localrepos`, `C/code` | WS5 (engine) | Device run; in-editor search and go-to-line are desktop-parity rows |
| AW5: chat, mesh, node | **Merged** into the branch (62617e41c) | `agent/native-android-aw5` | `feature/chat`, `C/chat` | WS5 | Status lookup by account name for people not live on the relay; per-project World address |
| AW6: composer, photos, dictation, direct send | **Merged** into the branch (a2dca0cdd) | `agent/native-android-aw6` | `feature/composer`, `C/composer` | WS3 | Device run of picker, camera and mic; chat input and session drafts moved to the gap sweep `agent/native-android-awg` |
| AW7: Agent Watch and Listen | **Merged** into the branch (78aaeab72) | `agent/native-android-aw7` | `feature/agentwatch`, `C/agentwatch` | WS4 | Device run of sound and vibration; reduced-motion default for vibration is in the gap sweep. Engine matches the golden fixture (`goldens.json`), audio within 2 per sample |
| AW8: hosts and live logs | **Merged** into the branch (a2dca0cdd) | `agent/native-android-aw8` | `feature/hosts`, `C/hosts` | WS7 | Host operations beyond WS7 (DESKTOP_PARITY 7.3–7.11) are WS16 |
| AW9: pings, code-push, background connection | **Merged** into the branch (a2dca0cdd) | `agent/native-android-aw9` | `feature/account/notifications`, `feature/push`, `C/push` | WS2 | Device check of Doze, reboot, the Flutter upgrade and the Android 12+ restart refusal. Service type `remoteMessaging`; revive is a JobScheduler job (no WorkManager, which would pull in Room) that reads the inbox directly when a restart is refused |
| AWi: cross-area wiring and the project-switch contract | **Merged** into the branch (a2dca0cdd) | `agent/native-android-awi` | `feature/Features.kt` `connectAreas` | — | Keeps the Flutter launcher and autofill component names for the in-place swap |
| AWt: emulator verification, Flutter → native upgrade | **Merged** into the branch (2b8ea6345) | `agent/native-android-awt` | fixes only | — | Guest smoke walk and the Flutter 1.3.1+16 → native in-place upgrade on the API 36 emulator passed after two migration fixes (Flutter data was deleted without a verified session; leftover ESP keysets made every secret "undecryptable"). Not verified: a signed-in upgrade and the deletion after the relay accepts the session |
| AWg: gap sweep | **Merged** into the branch (35217cc49) | `agent/native-android-awg` | leftovers from AW1–AW9 | — | Chat status fallback, chat on the shared composer, session drafts, New = send straight, DM notifications, World per project, prompt button in the tab row, vibration off under reduced motion |
| AWt2: second emulator pass on the merged branch | **Merged** into the branch (7588c7b2f) | `agent/native-android-awt2` | fixes only | — | Seven fixes: banners under the status bar, hub guest prompts cancelled, guest gates on business screens and Coves, public owner in Coves and all-networks, timers never rang, system-bar icons ignored the app theme, a Next guest landed on the welcome screen after the upgrade, launcher label under `com.forkmesh.forkmesh`. Upgrade from Flutter (Next project, guest, dark, purple) passed: 8 secrets, 8 preferences, 0 unreadable, idempotent, Flutter data kept |
| AWz: route guest gate, Projects entry, unknown `forkmesh://` paths | **Merged** into the branch (bfd151553) | `agent/native-android-awz` | navigation | — | Guest gate at the router (`GuestGate` in core) for links, shortcuts, widgets, tiles and notification taps; Projects opens the projects list; every `forkmesh://` link lands somewhere |
| AWf: fixes the iOS side found (SHA-1 collision screening, clone from mirrors by public owner, shallow depth-25 clone, pasted images), and folding the "ForkMesh Next" project into ForkMesh after the cutover | **Merged** into the branch (1a74226e7) | `agent/native-android-awf` | `C/git`, `C/localrepos`, agents clone path, `feature/composer` | `fix/native-clone-and-paste` | Pure-Kotlin SHA-1DC; shallow depth-25 single-branch clones; clone from mirrors by public owner with failover (plus the catalog merge of mirror rows under the public owner); pasted images; ForkMesh default and the Next fold. Open: public-owner routing of repository reads; `contentReceiver` paste untested on this text field |
| AW10: navigation, Inbox, system surfaces | **Merged** into the branch (87ac601be) | `agent/native-android-aw10` | `feature/navigation`, `feature/inbox`, `feature/surfaces` | WS8, WS9, WS12 | Done: 1.2, 1.5–1.7, 1.9, 1.10, 1.15, 1.28 (needs the server to publish `minimumClientVersions.android`), 2.35, 9.2, 12.13, 18.5; four-tab shell with a You hub, App Links, widgets (RemoteViews, no Glance), shortcuts, a quick-settings tile, share target. Partial: 1.1, 1.14, 11.15. Server: `/.well-known/assetlinks.json` (and iOS `apple-app-site-association`) on the Next stack, not deployed |
| AW11: agent ops and teammates | **Merged** into the branch (3fe9353d2) | `agent/native-android-aw11` | `feature/agents`, `feature/teammates` | WS10, WS17 | Done: 2.2, 2.6, 2.7, 2.11, 2.13, 2.14, 2.16, 2.19, 2.20, 2.26–2.28, 3.1–3.6, 9.16. Partial: 4.20 (list only, as iOS). Not done on either platform: 2.8, 2.10, 2.12, 2.21, 2.29, 2.31, 2.36, 8.2 |
| AW12: code review, CI, releases, security | **Merged** into the branch (d9656d2f2) | `agent/native-android-aw12` | `feature/code`, `C/ci` | WS11, WS14 | Done: 4.2, 4.9 (flat), 4.13, 4.16, 4.25, 5.4, 5.5, 5.7, 5.8, 5.10, 5.11, 5.14–5.16, 5.19, 6.1–6.4, 6.10, 6.12, 6.15, 6.17, desktop commands. Partial: 1.8, 4.12, 4.18, 4.22, 4.26, 4.27, 4.34, 5.2, 5.9, 5.13, 6.8, 6.11, 6.13. Missing: 4.7, 4.8, 4.15, 4.28, 5.17, 5.18, 5.20, 5.22 |
| AW13: chat depth | **Merged** into the branch (587239110) | `agent/native-android-aw13` | `feature/chat` | WS13 | Done: 9.4–9.9, 9.11–9.13, 9.15, 9.17–9.19. Partial: 9.14 (video saves only). Admin delete missing on iOS too. Leftovers in `agent/native-android-awx` |
| AWx: leftovers (notification reply to chat, admin delete, view profile, chat video, interface sounds) | **Merged** into the branch (8b6e4ae2e) | `agent/native-android-awx` | chat, push, navigation | — | Notification reply to chat, View profile from a message, chat video playback, interface sounds (1.14, off by default; the switch comes with AW16). Admin delete skipped: iOS has none |
| AW14: planning and packages | **Merged** into the branch (916f2ee7b) | `agent/native-android-aw14` | tools | WS15, WS19 | Done: 2.30, 5.23–5.25, 5.27, 10.2 (convert only, as iOS), 10.3, 10.4 (AlarmManager, `SCHEDULE_EXACT_ALARM`), 11.1, 11.3–11.9. Partial: 11.10 (no QR yet; AW16 adds a pure-Kotlin encoder), 14.7 (own wallet only). Not done on either platform: 11.2, 5.21 |
| AW15: desktop remote and insights | **Merged** into the branch (9f67ea2bf) | `agent/native-android-aw15` | hosts, tools | WS16, WS20 | Done: 2.34, 4.11, 4.17, 4.24, 6.16, 7.3–7.11, 7.17, 8.4, 12.4, 12.9, 12.12; 1.18 (via Status), 4.3, 4.29–4.31, 5.6, 8.1, 8.3, 8.9, 11.16, 12.1, 12.5, 12.7, 12.8, 12.10 (ApplicationExitInfo in place of MetricKit); notification Merge, Continue, Retry. Partial: 12.6 (as iOS). 7.20 done by AWd (52bd585b8): roster diagnostics on the Nodes directory |
| AW16: settings, account, coves, secrets | **Merged** into the branch (f3f820372) | `agent/native-android-aw16` | account, vault | WS18, WS21 | Appearance (theme, accent), guest gates, public-owner reads fixed. Done: 6.9, 7.12, 7.15, 8.5, 8.12 (pure-Kotlin QR encoder, bit-exact with the desktop), 13.2–13.15, 14.2, 16.1, 16.2, 13.9. AWy (8f1f12312): 11.12 was already done by AW3; 14.6 camera QR scanner (Camera2, pure-Kotlin decoder); 1.23 membership status. Partial: 1.13 (no skins). As iOS: 1.16 (Quick Setup in Settings only), 14.1 (switch = sign out and in) |
| AWp: gap sweep | **In review** (worktree branch, not merged) | `worktree-agent-a087089b1cb5482fa` | calories, composer, settings, code review, local repos, agents, inbox | WS1–WS3 leftovers, commits after 2026-09-18 | Re-checked the Android column against the code; the tracker had missed work from 2026-09-22 to 09-27. Done: Calories log by prompt, Quick add and live search; the "Where you're signed in" sessions card; public-owner merge routing and post-merge cache drop; org agent queue by public owner; local repo rebase and orphan adoption; desktop files in the Flutter app's folder; avatar refresh; relay-feature gating in Settings; project-aware login subtitle; vault recovery gate; sign-in terminal copy/restart; relative ping links; branch history; `newIssue` route; default branch for new repos. Table and reasons: ANDROID.md §12. Open: device run of all of it; project quick-switcher sheet; World host edit; see ANDROID.md §12 |
| AW22: ForkMesh Apps surfaces | **In review** | `511-e9a2ad3ea2ca96ca20d366a9c1f015` | `C/apps` (`AppsApi`, `ChatApps`, block kit, slash and mention patterns), `feature/account/apps/InstalledAppsScreen.kt`, `feature/chat/AppBlocksView.kt`, `ChatHub`, `OfficeChannelService.sendFrame` | WS22 | DESKTOP_PARITY 9.24, 13.16, as iOS. Route `InstalledApps` (Settings › Installed apps, `/dashboard/apps`). `/oauth/...` and `/dashboard/apps/<page>` open in a real browser: the verified App Link claims all of app.forkmesh.com, so the intent gets a browser selector instead of resolving back to this app. Guests and preview sessions never call `/api/apps/*`. `/apps` answers in the status line above the chat composer (a snackbar until 2026-10-09). Linux: `:core:coreTest` 1573 passed, `:app:compileDebugKotlin` green. Left: an emulator run |

**Decision (AW9, 2026-09-24): our own background connection, no FCM or
Google dependency.** A foreground service (`remoteMessaging` or `dataSync`
type, whichever targetSdk 37 allows for this use) with a low-importance
persistent notification keeps the existing account event socket open, with
exponential backoff, and respects Doze; WorkManager or AlarmManager revives
it. It starts on boot (`RECEIVE_BOOT_COMPLETED`), stops on sign-out, and has
a per-project Settings switch that explains the battery cost. The reconnect
and backoff logic is tested in `:core`. No server change is needed: pings
already travel over the event socket.

### Android column (PARITY.md numbering)

Android status words match PARITY.md. "Done" means written and built into the
debug APK with its `:core` tests green; nothing has been run on a device
since the 2026-09-14 emulator check. "core" means the logic is in `:core`
with tests but no screen reaches it.

| # | Capability | iOS | Android | AW |
|---|---|---|---|---|
| 1 | Log in with password and TOTP | Done | Done | — |
| 2 | Sign up | Done | Done | — |
| 3 | Guest mode and the guest gate | Done | Done | — |
| 4 | Device identity and owner-device enrolment | Done | Done | — |
| 5 | Portable identity recovery kit | Done | Done (with a UI) | — |
| 6 | Account nodes, sessions, this device; revoke | Done | Done | — |
| 7 | iOS UDID lookup | Done | N/A | — |
| 8 | User-presence gate (biometrics) | Done | Done | AW1 |
| 9 | Sign out and delete account | Done | Done | — |
| 10 | Project list, probing, per-instance flags | Done | Done | AW1 |
| 11 | Separate login per project | Done | Done | AW1 |
| 12 | Full per-project isolation | WS1 | Done | AW1 |
| 13 | Project runtime rebuilt on a switch | WS1 | Done | AW1 |
| 14 | Quick project switcher | WS1 | Done | AW1 |
| 15 | Projects card in Settings | Done | Done | AW1 |
| 16 | All-networks catalog | Done | Done | AW1 |
| 17 | Per-project legal links | WS1 | Done | AW1 |
| 18 | Home shell: Code, Agents, Chat, Tools, prompt bar, bell, avatar | Done | Done | AW2, AW3, AW5 |
| 19 | Theme, accents, frosted surfaces, design kit | Done | Done | — |
| 20 | World | Done | Done | AW5 |
| 21 | Repository catalog | Done | Done | — |
| 22 | Repository detail: explorer, preview, commits, mirrors, releases | Done | Done | — |
| 23 | Issues | Done | Done (reachable from the repository detail since AW0) | — |
| 24 | Pull requests, merge plan and merge | Done | Done | — |
| 25 | Discussions | Done | Done | — |
| 26 | Branch diff and diff viewer | Done | Done | AW4 |
| 27 | Quick edit into a proposed change or PR | Done | Done | AW4 |
| 28 | Code editor with key bar | Done | Done | AW4 |
| 29 | Local repositories | Done | Done | AW4 |
| 30 | Clone to device | Done | Done | AW4 |
| 31 | Mirror import and import tokens | Done | Done | — |
| 32 | Signed inbox writes | Done | Done | — |
| 33 | Bounties, funding, donation panels | Done | Done | AW4 |
| 34 | Serve as a node | WS5 | Done | AW5 |
| 35 | Organizations | Done | Done | — |
| 36 | Chat: channels, roster, DMs, unread, link text | Done | Done | AW5 |
| 37 | Room crypto and per-device seal | Done | Done (core) | — |
| 38 | Chat persona | WS3 | Done | AW5 |
| 39 | Local mesh room over the LAN | Done | Done | AW5 |
| 40 | Notes | Done | Done | AW3 |
| 41 | Tasks | Done | Done | AW3 |
| 42 | Calories | Done | Done | AW3 |
| 43 | Password vault | Done | Done | AW3 |
| 44 | Activity | Done | Done | AW3 |
| 45 | Sponsored ads | Done | Done | — |
| 46 | Pings inbox | Done | Done | — |
| 47 | Ping settings screen | WS2 | Done | — |
| 48 | Code-push pings | WS2 | Done | AW9 |
| 49 | Live account events and the agent-list doorbell | Done | Done | — |
| 50 | Push register, test, badge, pending tap | WS2 | Done (via the background connection) | AW9 |
| 51 | Notification service extension | Done | N/A | — |
| 52 | Per-project push | WS2 | Done | AW9 |
| 53 | In-app deep links from a ping | Done | Done | AW2 |
| 54 | Agents tab and session detail | Done | Done | AW2 |
| 55 | Queue, steer, retry, cancel a run | Done | Done | AW2 |
| 56 | On-device coding agent with the user's key | Done | Done | AW2 |
| 57 | Model catalog | WS3 | Done | AW6 |
| 58 | Provider-login terminal | Done | Done | AW2 |
| 59 | Desktop on this network (LAN control) | Done | Done | AW2 |
| 60 | Agent Watch screen | WS4 | Done | AW7 |
| 61 | Listen sonifier engine | WS4 | Done | AW7 |
| 62 | Sound and haptics output | WS4 | Done | AW7 |
| 64 | On-device Gemma | Done | N/A (iOS only, MLX) | — |
| 65 | Floating composer | WS3 | Done | AW6 |
| 66 | Photo attachments | WS3 | Done | AW6 |
| 67 | Dictation | WS3 | Done | AW6 |
| 68 | Image blocks through the providers | WS3 | Done | AW6 |
| 69 | User status and presence | Done | Done | — |
| 70 | Settings | Done | Done | — |
| 71 | Telemetry screen and strip | Done | Done | AW3 |
| 72 | Performance monitor, overlay, stall log | Done | Done | AW3 |
| 73 | Bug report | Done | Done | AW3 |
| 74 | Trust lists resolver | Done | Done | — |
| 75 | Accessibility labels | Done | Partial | all |
| 76 | Privacy manifest | Done | N/A | — |
| A1 | Vault autofill into other apps (Android only) | N/A | Done | AW3 |
| A2 | Bluetooth RFCOMM mesh link (Android only) | N/A | Done (pending device check) | AW5 |
| A3 | Share and `PROCESS_TEXT` intents | N/A | Done | AW3 |
| A4 | Flutter data migration on the first native launch | Done (`28664ba2d`) | Done | AW1 |

Totals (N/A excluded, 75 rows, at `f3f820372`): Android **Done 74**, **Partial 1**,
**Missing 0**. The 2026-09-27 sweep (AWp, ANDROID.md §12) found that rows 6
(sessions card), 24 (merge by public owner), 29 (orphaned local repos), 42
(Calories prompt, Quick add, live search) and 70 (relay-feature gating) were
really Partial until AWp. Desktop-parity items (DESKTOP_PARITY.md) are all
Missing on Android except those that come with a row above; AW10+ tracks
them.

## How to update this file

- **When a workstream starts**, set its status to **In progress** and fill in
  its branch.
- **After each Codemagic run** on the branch, replace "Last Codemagic build"
  with the workflow, the result (green, or red and why in a few words), the
  build number and the date. Record only the latest run.
- **When a workstream merges**, in the same merge:
  1. set the status to **Merged** and fill in "Merged commit" (the merge
     commit on Next's `main`);
  2. empty "Remaining", or move what is left into a follow-up row;
  3. tick the checklist boxes for every capability that is now Done, and
     update the "Native iOS" cell of that row in DESKTOP_PARITY.md or
     PARITY.md, so the two agree;
  4. update the headline counts in DESKTOP_PARITY.md with its counting
     script.
- **One writer per row.** A workstream edits only its own row and its own
  checklist items, which keeps merges of this file conflict-free. The
  coordinator edits the header and the Gate rows.
- **New capability.** Add the row to DESKTOP_PARITY.md first (with a
  workstream), then its checkbox here, and its route in NAVIGATION.md §8.
