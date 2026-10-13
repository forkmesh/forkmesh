# Mobile feature-parity audit

Historical record: this audit covers the retired Flutter app, which has been
removed from the repository. The native iOS and Android apps track parity in
[`docs/native/PARITY.md`](../../../docs/native/PARITY.md) and
[`docs/native/DESKTOP_PARITY.md`](../../../docs/native/DESKTOP_PARITY.md).

Audited 2026-09-17 against `mobile/` (Flutter), `desktop/` (Qt), and the web
surface (`app/public/` dashboard + standalone pages, backed by `app/src/`).

This document answers one question: **what can a user do on web or desktop that
they cannot do on the phone?** The checklist at the end is the backlog that
closes it.

The existing [`capability-matrix.json`](../capability-matrix.json) already
records surface coverage, but at 16 coarse capabilities (last updated
2026-07-23) it marks whole areas `native` on Flutter when only a slice of them
is present — `collaboration` is `native` on all three surfaces, yet the phone
has no chat channels, no DMs, no reactions and no attachments. Treat the matrix
as the boundary contract (what may *never* leave the owner device) and this
document as the feature ledger.

---

## 1. What mobile has today

The Flutter app is three pages — **Code**, **Agents**, **Chat** — with the
tools (Activity, Notes, Tasks, Calories, Vault, World, Desktop on this network)
listed in the same top-left menu, and **Settings** behind the top-bar avatar.

| Area | On the phone today |
| --- | --- |
| Repositories | Network catalog, publish/import, per-repo detail with 11 tabs: Code, Issues, Pulls, Discussions, Commits, Mirrors, Releases, About, Actions, Agents, Worktrees (`mobile/lib/screens/repo_detail_screen.dart`) |
| Code browsing | Tree, blob preview (text/image/video), syntax highlighting, in-repo search, branch selector, "go to file", commit diffs |
| Collaboration | Create issue / pull / discussion, comment, close/reopen, approve, review note, sub-issues, issue filters by status/label/assignee, create release, issue bounties |
| Local git | init, import from folder or `.zip`, file CRUD, stage/unstage, discard, commit, branch create/checkout, log, clone, push, fetch, sync state (`mobile/lib/services/git/git_workspace.dart`) |
| Agents | Read-only session list, transcript, status, linked PR (`agents_screen.dart`, `agent_screen.dart`) |
| Chat | `#general` / `#random` plus server-announced channels, presence, local mesh (BLE/LAN) transport, sealed rooms |
| Organizations | List, create, delete, members, teams, team members, linked repos |
| Account | Login, signup (external web), device registration, push (iOS), notifications with 5 filters, node list, relay config, import tokens, funding/payout display, legal links |
| Activity | Network stats, leaderboards, contributors, mainnodes, projects, node activity, payout readiness, funds received |
| Other | Notes, Tasks, Calories (hand-ported), Telemetry/stall screen, bug report overlay, World via external URL |

Publishing is done the ForkMesh way: issues, pulls, discussions, About edits
and branch ops are signed records pushed over git and drained through
`/api/sync` (`mobile/lib/services/node_sync_service.dart`), not REST writes.

---

## 2. Gap inventory

### 2.1 Whole sections that do not exist on mobile

Desktop navigation rail (`desktop/src/MainWindowNavigation.cpp`, section
indices in `desktop/src/MainWindowInternal.h`) and the web sidebar
(`app/public/dashboard/partials/sidebar.html`) versus the phone:

| Section | Web | Desktop | Mobile |
| --- | --- | --- | --- |
| Notes (markdown, versions, sharing) | ✅ package page | ✅ rail | ✅ page menu (no live co-editing socket) |
| Calendar (personal + org, events) | ✅ package page | ✅ rail | ❌ (notifications deep-link nowhere) |
| Tasks / organization tasks | ✅ package page | ✅ rail | ✅ page menu (no attachments or agent routing) |
| Ledger (public financials, pending payments) | ✅ `/ledger` | ✅ rail | ❌ |
| Market / packages | ✅ `forkmesh.com/market` | ✅ rail | ❌ |
| Microjobs | ✅ `/microjobs` | — | ❌ |
| Referrals | ✅ `/referral` | — | ❌ |
| Hackathon | ✅ `/hackathon/me` | — | ❌ |
| Leaderboards page | ✅ `/leaderboards` | — | partial (inside Activity) |
| Status page | ✅ `/status` | ✅ Component health | ❌ |
| Network / protocol page | ✅ `/dashboard/network` | ✅ rail | partial (Activity) |
| Profile / contributions | ✅ `/dashboard/profile` | ✅ Team | ❌ |
| Mirror payouts | ✅ `/mirror-payouts` | ✅ Ledger | ❌ |
| Users (admin) | ✅ admin console | ✅ rail | ❌ |
| Control node / hosts | deep-link | ✅ rail | ❌ |
| Files browser | — | ✅ rail | ❌ |
| Stats (BI tiles) | — | ✅ rail | ❌ |
| Component health | — | ✅ rail | ❌ |
| Coves (encrypted document vaults) | — | ✅ | ❌ |
| Merge queue | — | ✅ | ❌ |
| MCP connector | — | ✅ Settings tab | ❌ |
| Code map (3D) | ✅ `/assets/codemap` | ✅ | ❌ |

### 2.2 Repository tabs missing on mobile

Desktop repo detail carries 18 tabs (`desktop/src/MainWindowRepoDetail.cpp`,
`m_repoDetailStack`); mobile carries 11. Missing:

- **Security** — scan results, findings, human review (`/api/security-scans/…`)
- **Quality** — code health scoring
- **Insights** — repo analytics, burnup (`IssueBurnup`)
- **Projects** — project boards, linked issues (`/api/…/projects`)
- **Artifacts** — build artifacts
- **Size map** — repository size breakdown (`/api/repo/…/sizes`)
- **Shortcuts** — repo-scoped action shortcuts
- **Repo Settings** — visibility/privacy, logo/banner, collaborators, shares
- **Branches** as a first-class tab (mobile folds branches under Commits)

Web additionally has **Insights** and **Projects** tabs the phone lacks.

### 2.3 Actions missing inside tabs mobile already has

| Missing action | Endpoint the phone never calls |
| --- | --- |
| Star / unstar a repo | `/api/repo/{o}/{r}/star` |
| Watch / subscribe | `/api/repo/{o}/{r}/subscribe` |
| Fork | `/api/repo/{o}/{r}/forks`, `/api/repository-forks` |
| Share a repo | `/api/repo/{o}/{r}/shares` |
| Change repo visibility | `/api/repo/{o}/{r}/privacy` |
| Set repo logo/banner | `/api/repo/{o}/{r}/logo`, `/logo-suggestions` |
| Merge a pull request | `/api/repo/{o}/{r}/pulls/{n}/merge` |
| Update a PR from base | `/api/repo/{o}/{r}/pulls/{n}/update` |
| Compare branches / merge preview | `/api/repo/{o}/{r}/compare`, `/merge-preview` |
| Repo stats & sizes | `/api/repo/{o}/{r}/stats`, `/sizes` |
| Search across all repos | `/api/repositories/search` |
| Send an agent a prompt | `/api/repo/{o}/{r}/agents/{id}/prompt` |
| Hand off an agent session | `/api/repo/{o}/{r}/agents/{id}/handoff` |
| Publish to the fediverse | `/api/repo/{o}/{r}/ap-publish` |
| Mirror reachability check | `/api/repo/{o}/{r}/mirrors/{n}/reachability` |
| Org succession, Discord, fediverse, bot tokens | `/api/orgs/{o}/succession`, `/discord`, `/fediverse`, `/bot-tokens` |

Mobile consumes roughly 25 distinct API paths; the Worker exposes well over 200.

### 2.4 Chat

Web chat (`app/public/chat*.js` plus `dashboard-chat.js` — 11 modules) has attachments, threads,
moderation, rich text, date grouping, issue filing from a message, per-channel
crypto and room transport. Mobile chat is a single text composer.

Missing on mobile:

- User-created public/private channels (`/api/chat/channels`) — mobile hardcodes
  `#general` and `#random`
- Direct messages (`/api/chat/direct-messages`) — the "People" list shows
  presence but opens no DM
- Attachments and image upload
- Threads and replies
- Reactions
- Moderation controls
- Mentions and mention notifications
- File an issue from a chat message
- Channel webhooks (`/api/chat/channels/{id}/webhooks`)

### 2.5 Settings

Desktop settings has 14 tabs (`desktop/src/MainWindowSettings.cpp`): Profile,
Quick Setup, General, Apps, Repos, SSH Keys, Pings, Agents, Voice, Secrets,
Organizations, MCP, Security, Data. Web settings has Account, Password, Active
sessions, SSH keys, Registered keys, Nodes & devices, Organizations, Plan usage,
Patreon/membership, Profile picture, Public profile, Verification and payout,
Appearance, Pings, Danger zone.

Mobile settings is missing:

- **SSH key management** (`/api/accounts/ssh-keys`) — currently deep-link only
- **Active sessions** list and revoke (`/api/accounts/sessions`,
  `/devices/revoke-all`)
- **Change password** (`/api/accounts/change-password`)
- **Profile picture upload** and public-profile editing
- **Plan usage / trial meters** (`/api/plans/usage`, `/api/plans/trial`)
- **Agent provider configuration** — no way to point an agent at a provider
- **Pause notifications** / per-category notification preferences
- **Secrets** and **Data** management

### 2.6 Structural gaps (not single features)

These are the multipliers — each one blocks a whole class of features.

1. **No package / extension host.** All 18 marketplace packages
   (`marketplace/packages/`) declare `desktop`, `dashboard-*`, `worker`,
   `world`, `agent`, `ide` or `browser-extension` entrypoints. Entrypoint kinds
   are free-form in `manifest.schema.json` — each surface reads the key it
   understands — so the blocker is not the schema but that no package declares a
   mobile entrypoint and the app has nothing to consume one. Calories is
   hand-ported into `mobile/lib/screens/calories_screen.dart` (2,634 lines) as a
   one-off, and so are Notes and Tasks (`notes_screen.dart`,
   `tasks_screen.dart`). Until mobile can host a package, Calendar, CRM,
   Marketing, Competitor Watch, Timers and the rest can only ever arrive as more
   hand-ports.
2. **Deep links dead-end.** `notification_deep_link.dart` resolves only
   `owner/repo/...` paths. Notifications the app already renders icons for —
   `calendar_invitation`, `calendar_reminder`, `calendar_rsvp`,
   `plugin_activity`, `org_succession`, `pending_reward`, `mirror_request`,
   `credits_refilled`, `chat_webhook` — tap through to nothing.
3. **No global search.** Mobile has per-repo search only; desktop has a global
   search section (section 6) and the web has a header page filter.
4. **Trust lists are implemented but unwired.** `mobile/lib/services/trust_lists.dart`
   (1,251 lines) and its test exist; no screen imports it.
5. **`portable_identity.dart` is unwired** the same way — no UI references it.
6. **Local git stops short of collaboration.** No merge, no rebase, no conflict
   resolution, no tags, no stash, no remote management, no per-hunk staging.
7. **The native Android port doubles the cost.** `native/android/` targets
   parity with the Flutter app, so every item below is eventually two
   implementations. Decide per item whether Flutter-first or native-first.

### 2.7 Deliberate non-goals

Per `capability-matrix.json`, these stay off the phone by design and should
remain deep-links, not gaps:

- Cloudflare / D1 / DNS / tunnel bootstrap
- Mirror process control (start/stop/sync/keys)
- Repository decryption keys, wallet signing keys, SSH *private* keys
- Agent-provider login credentials
- Running coding agents locally (mobile stays read-only observer)

---

## 3. Checklist

Ordered by value per unit of work. Each item is independently shippable.

Re-checked against `mobile/lib` on 2026-09-24. Since the audit the app also
gained projects (several ForkMesh instances side by side, see
`mobile/docs/multi-relay.md`), the shared `PromptComposer`, the agent Watch
screen, local-network control of the desktop, and the Password Vault; none of
those were checklist items.

### Tier 1 — closes the biggest holes with existing APIs

- [ ] **Chat channels** — list, create, join user channels (`/api/chat/channels`)
- [ ] **Direct messages** — open a DM from the People list (`/api/chat/direct-messages`)
- [ ] **Star / unstar** on the repo header (`/api/repo/…/star`)
- [ ] **Watch / subscribe** on the repo header (`/api/repo/…/subscribe`)
- [ ] **Fork a repository** (`/api/repository-forks`)
- [x] **Merge a pull request** (`/api/repo/…/pulls/{n}/merge`, with `merge-preview`); shipped 2026-09-18/23. **Update from base** is still missing.
- [ ] **Global repository search** (`/api/repositories/search`)
- [ ] **SSH key management** in Settings (`/api/accounts/ssh-keys`) — replaces the deep-link
- [x] **Active sessions + revoke** in Settings (`/api/accounts/sessions`, per session; #725, 2026-09-18). `/devices/revoke-all` stays on the web.
- [ ] **Change password** in Settings (`/api/accounts/change-password`)
- [ ] **Fix dead-end deep links** — route calendar, plugin, reward, succession and
      mirror-request notifications somewhere, or stop rendering them as tappable

### Tier 2 — sections the phone is missing entirely

- [x] **Notes** — read, edit, version history, share (`/api/notes`)
- [ ] **Calendar** — personal + org events, RSVP (`/api/calendar`); unblocks three
      notification types already rendered
- [x] **Tasks** — organization task list and updates (`/api/tasks`)
- [ ] **Profile page** — own and other users: contributions, activity, repos
      (`/api/accounts/{name}`, `/contributions`, `/follow`)
- [ ] **Ledger** — public financial records and pending payments (`/api/ledger`)
- [ ] **Leaderboards** as a first-class screen (`/api/leaderboards`)
- [ ] **Status** — network and service health (`/api/status`)
- [ ] **Plan usage and trial meters** in Settings (`/api/plans/usage`, `/trial`)

### Tier 3 — repo tabs

- [ ] **Repo Settings tab** — visibility, description, logo/banner, collaborators, shares
- [ ] **Security tab** — scan results and findings (`/api/security-scans/…`)
- [ ] **Insights tab** — repo analytics and issue burnup
- [ ] **Projects tab** — boards and linked issues
- [ ] **Branches** promoted to its own tab with create/delete/compare
- [ ] **Compare / merge preview** between branches
- [ ] **Size map** (`/api/repo/…/sizes`)
- [ ] **Artifacts tab**

### Tier 4 — chat depth

- [ ] Attachments and image upload
- [ ] Threads and replies
- [ ] Reactions
- [ ] Mentions, with a notification for each
- [ ] File an issue from a chat message
- [ ] Moderation controls for channel owners

### Tier 5 — structural

- [ ] **Mobile package host** — define a `mobile` entrypoint key and build a
      host in the app that reads it, so packages arrive as packages instead of
      hand-ports. Retire the bespoke Calories screen onto it as the first
      consumer.
- [ ] **Wire trust lists** into a Settings screen, or delete
      `mobile/lib/services/trust_lists.dart` and its test
- [ ] **Wire or delete `portable_identity.dart`**
- [x] **Agent prompting** — send a prompt to a running session (follow-ups through `steerAgentSession`, with photos, 2026-09-24; retry of failed or stopped agents 2026-09-18)
      (`/api/repo/…/agents/{id}/prompt`) and hand off a session
- [ ] **Agent provider configuration** in Settings
- [ ] **Local git: merge, tags, conflict resolution, per-hunk staging**
- [ ] **Organization depth** — succession, Discord, fediverse, bot tokens, agent bots
- [ ] **Decide the native-Android policy** — which tier lands in `native/android/`
      first, and whether Flutter leads or follows
- [ ] **Refresh `capability-matrix.json`** — split `collaboration`, `repositories`
      and `agents` into rows fine-grained enough that a `native` Flutter mark
      means the feature is actually there

---

## 4. Evidence

| Claim | Where to check |
| --- | --- |
| Mobile navigation | `mobile/lib/screens/home_shell.dart` (`_destinations`) |
| Mobile repo tabs | `mobile/lib/screens/repo_detail_screen.dart:258-303` |
| Mobile API surface | `grep -oE "'/api/[^']*'" mobile/lib/**/*.dart` |
| Mobile local git | `mobile/lib/services/git/git_workspace.dart` |
| Desktop rail | `desktop/src/MainWindowNavigation.cpp:200-800`, `MainWindowChat.cpp:1726-1740` |
| Desktop sections | `desktop/src/MainWindowInternal.h` (`k*SectionIndex`), `MainWindowNavigation.cpp:3705` |
| Desktop repo tabs | `desktop/src/MainWindowRepoDetail.cpp:11705-11741` |
| Desktop settings tabs | `desktop/src/MainWindowSettings.cpp:373-416` |
| Web sidebar | `app/public/dashboard/partials/sidebar.html` |
| Web pages | `app/public/dashboard/js/10-boot.js` (`BUILTIN_PAGE_INITS`) |
| Web repo tabs | `app/public/dashboard/js/05-repo-list-explorer.js:2324` |
| Worker API surface | `app/src/urls.py`, `grep -ohE '"/api/[a-z0-9/_-]+"' app/src/*.py` |
| Package entrypoints | `marketplace/packages/*/manifest.json` |
