# Marketplace extraction inventory

**Related issue:** #653 — Inventory optional ForkMesh features for Marketplace extraction
**Reviewed:** 2026-09-14 against the tree at this commit
**Companion:** issue #677 holds the repo-wide opportunity inventory and the
stingy shortlist. This document answers only the 24 candidate rows filed on
#653, one decision each.

A row is **Extract** when the feature can leave the core without breaking
repository integrity, identity, authorization, Git transport, or the issue
model, and the host seam it needs is named. **Split** extracts a named part and
keeps the rest in core. **Defer** means the row is plausible, but a
prerequisite in the "Blocked on" column must land first. **Keep** means the
authoritative behaviour stays in core; a package may only present it.

Only Extract rows, and the extractable half of Split rows, are ready to become
linked sub-issues. Every child issue must link back to #653.

## Summary

| # | Candidate | Type | Decision | Blocked on | Order |
| --- | --- | --- | --- | --- | --- |
| D1 | Theme and icon packs | `theme` | Extract (themes shipped) | Icon-set entry in the `theme` descriptor | 1 |
| D2 | Chat connectors beyond Discord | `extension.native` | Defer | `desktop.chat-provider` socket generalized from Discord wiring | 3 |
| D3 | Agent provider and model adapters | `agent` / `service.agent` | Split | `desktop.agent-provider` socket for runtimes | 3 |
| D4 | Voice dictation and speech-to-text | `extension.native` | Defer | Speech-provider seam; tests for Whisper/Parakeet | 3 |
| D5 | Backup and export destinations | `service.hosting` | Keep | Host-side encryption before any adapter sees bytes | — |
| D6 | VS Code and future IDE integrations | `extension.vscode` | Split | IDE runtime that installs VSIX files | 2 |
| D7 | Actions templates and runner integrations | `service.development` | Split | `content.template` socket (templates only) | 2 |
| A1 | Notes workspace | `extension.native` + App | Extract (page shipped) | Durable Object socket; account data scope; down migrations | 2 |
| A2 | Calendar and planning views | `extension.native` + App | Extract (page shipped) | Same as A1, plus cron and notification-kind sockets | 2 |
| A3 | Microjobs and contributor-reward board | `service.development` | Keep | — (settlement is core) | — |
| A4 | Repository social-banner generator | `extension.native` | Defer | Card-template input to `og_card.py`; head-meta socket | 4 |
| A5 | Contributor, referral, usage analytics widgets | `extension.native` | Split | Read-only analytics API for `dashboard.page` | 4 |
| A6 | External repository import and sync connectors | `service.development` | Split | Provider-adapter socket; `cli.command` for `tools/` sync | 4 |
| W1 | Status obelisk | `virtual.object` | Extract | Element loader generalized as a package entrypoint | 1 |
| W2 | Orbit sculpture | `virtual.object` | Extract | Same as W1 | 1 |
| W3 | Aurora beacon | `virtual.object` | Extract | Same as W1 | 1 |
| W4 | Repository graph visualization | `virtual.world` | Defer | `world.data-source` plus entity-selection hook | 3 |
| W5 | Workshop and data-model analysis | `virtual.world` | Removed (2026-10-07) | none | none |
| W6 | Office furniture and meeting-room modules | `virtual.object` | Split | District contract already exists (furniture only) | 3 |
| W7 | Avatar face and outfit packs | `virtual.avatar` / `virtual.clothing` | Defer | `world.avatar-part` socket; catalog-driven relay allowlist | 4 |
| W8 | Locomotion, emote, interaction packs | `virtual.animation` | Split | Emote socket; catalog-driven emote allowlist | 4 |
| W9 | Ambient music and spatial audio | `virtual.audio` | Split | `world.audio` generalized from the district sound emitter | 3 |
| W10 | Event, advertising, and link kiosks | `virtual.object` | Split | Banner-board host (exists) plus `world.panel` | 3 |
| W11 | Social and communication panels | `virtual.object` | Split | `world.panel` socket | 3 |

## Lifecycle owners

Each row names the host that owns its install/remove transaction. A package
never owns its own teardown enforcement.

- **Desktop:** `desktop/src/ForkMeshPackageRegistry.cpp` (settings under
  `market/packages/<id>/`, `uninstall()`), driven by
  `MainWindow::setPackageInstalled` → `syncPackageRuntime` in
  `desktop/src/MainWindowMarket.cpp`.
- **App:** `app/src/plugin_data.py` (schema up/down, export, purge) behind the
  `dashboard.page` and `worker.http` sockets discovered by
  `marketplace/tools/plugin_sockets.py`. `dataLifecycle.scope` currently
  accepts only `none` and `organization`.
- **World:** the generated `world/public/world/world-marketplace-packages.js`
  (`install(host)` contract) and the store-element loader in
  `world/public/world/elements/index.js`.
- **External removal:** `marketplace/tools/marketctl.py` and its migration
  ledger (`marketplace/tests/test_marketplace_lifecycle.py`).

## Desktop candidates

### D1. Custom theme and icon packs — Extract

- **Host API:** `theme` packages are parsed by `ForkMeshPackageRegistry`
  (`themeBase`, `themePalette`, `themeStyleSheetPath`) and applied by
  `MainWindow::applyTheme`. Icons have no seam: they are compiled resources
  (`:/icons/octicons`, `:/icons/codicons`) re-tinted by `refreshThemedIcons()`.
- **Existing package:** `marketplace/packages/winamp` (palette + QSS).
- **Permissions:** none. An icon pack is declarative content.
- **Package-owned state:** `app/theme` (`market:<id>`), `app/themeBaseDark`;
  an icon pack would add only its own assets.
- **Teardown:** already correct for themes — uninstall resets `app/theme` to
  `system`, and a missing or damaged package falls back to `system`. Icon packs
  must fall back to the bundled resource per glyph and keep themed tinting.
- **Migration compatibility:** none; icon names must stay the octicon/codicon
  names checked by `desktop/tests/validate_octicon_names.py`.
- **Focused tests:** `desktop/tests/test_package_registry.cpp` (theme parsing).
  Icon packs add: missing glyph falls back; uninstall restores bundled icons.
- **Order:** first. Lowest risk, precedent shipped.

### D2. Chat and community connectors beyond Discord — Defer

- **Host API:** none generic. Discord is wired through Discord-specific signals,
  `discord-message:`/`discord-user:` id prefixes and `m_discordPackageActive`
  (`desktop/src/MainWindowSetup.cpp` `attachDiscordPackage` /
  `detachDiscordPackage`). `desktop/src/ChatBackend.h` is the mesh transport,
  not a connector seam.
- **Blocked on:** a `desktop.chat-provider` socket (channels, messages, sends,
  presence, credential handle, teardown) that Discord itself moves onto first,
  so the second connector is not another hand-wired branch.
- **Permissions:** `network.<provider>`, `chat.sidebar.channels`,
  `settings.<credential>` — the Discord manifest is the template.
- **Package-owned state:** credential, cached history, avatars, sidebar group,
  all under `market/packages/<id>/data`.
- **Teardown:** the Discord detach path is the contract: stop the client,
  delete the credential, purge channel group, cache and avatars.
- **Migration compatibility:** follow the Discord legacy-token migration
  pattern for any pre-package keys.
- **Must stay core:** `RoomSeal`/`RoomCrypto`, signed admin delete, chat
  membership. A connector bridges messages; it never enters the sealed path.
- **Focused tests:** `desktop/tests/test_chat_transport.cpp`
  (`forkmesh-discord-tests`), `marketplace/tests/test_marketplace_lifecycle.py`.
  Add a socket contract test that runs against Discord before a new provider.

### D3. AI agent provider and model adapters — Split

- **Extract — model descriptors (`agent`):** shipped for Ollama as
  `marketplace/packages/gemma-4` with `marketplace/agent.schema.json`.
  `PackageAgentContribution::isValid` accepts only `local-llama`/`ollama` and a
  loopback endpoint; further model packs reuse that unchanged.
- **Defer — runtime adapters (`service.agent`):** `AgentRuntimeRegistry` has a
  fixed `builtInRuntimes()` list; `CodexAppServerSession` and
  `ClaudeStreamSession` implement `AgentRuntimeSession`. Extraction needs a
  `desktop.agent-provider` socket carrying only the model/session/transcript/
  usage protocol.
- **Permissions:** `network.model.download`, `process.ollama`,
  `storage.model.write`, `repository.agent.write` (high).
- **Package-owned state:** `managedModels`, Ollama weights; runtime adapters
  would own resume ids and transcript styling only.
- **Teardown:** exists for models — `removePackageAgentModels` runs
  `ollama rm` and cancels an in-progress pull; warns if weights remain.
- **Migration compatibility:** `agentRuntimeIdForLegacyProvider` must keep
  mapping stored provider names when a runtime moves into a package.
- **Must stay core:** `AgentWorktree`, `AgentJail`, guardrails, the capability
  broker, approvals, cancellation, CLI account state.
- **Focused tests:** `desktop/tests/test_agent_runtime_contract.cpp`,
  `test_codex_app_server.cpp`, `test_agent_transport.cpp`,
  `test_package_registry.cpp`.

### D4. Voice dictation and speech-to-text providers — Defer

- **Host API:** none. Whisper.cpp and Parakeet are a hard-coded choice inside
  `MainWindow` (`startVoiceCapture` in `MainWindowChat.cpp`,
  `installWhisperCpp` in `MainWindowSettings.cpp`, engine helpers in
  `MainWindowInternal.h`).
- **Blocked on:** a speech-provider seam (record → transcribe → text to
  composer, with cancellation) and focused tests; there are none for either
  engine today.
- **Permissions:** `audio.microphone.record` (sensitive),
  `network.model.download`, `process.transcriber`, `storage.model.write`.
- **Package-owned state:** `voice/engine`, `voice/whisperDir`,
  `voice/whisperModel`, `voice/parakeetModel`, `voice/inputDevice`;
  `<AppLocalData>/whisper.cpp`, `<AppLocalData>/parakeet`; temporary WAVs.
- **Teardown:** kill recorder/transcriber processes, delete the engine
  directory, drop keys, hide voice buttons.
- **Migration compatibility:** the `voice/*` keys live outside
  `market/packages/*`; moving them needs a one-time key migration, and "no
  engine set" currently means Whisper.
- **Must stay core:** `WorldSpeechBridge` origin pairing tokens.
- **Focused tests:** `desktop/tests/test_world_speech_bridge.cpp` exists;
  provider install/remove and silence-hallucination filtering need new tests.

### D5. Backup and export destination adapters — Keep

- **Why:** `LocalBackupStore` and `exportCompleteSystemBundle` archives contain
  identity keys, the Cloudflare API token and the tip-wallet secret. A
  destination adapter would receive account-security material.
- **Revisit when:** the host encrypts the archive before handing opaque bytes to
  an adapter and restore verification stays host-owned. Only then is a
  `service.hosting` destination with `network.<provider>` and a credential
  handle a candidate.
- **Teardown if revisited:** stop timers and uploads, revoke the remote
  credential, never delete local snapshots.
- **Since 2026-09-27:** the `backups` package organizes the snapshots and other
  local backups on its own rail page. Archive creation, restore and the hourly
  schedule stay in the host and reach it only through
  `desktop/src/BackupHostFeed.h`; no destination adapter exists.
- **Focused tests:** `desktop/tests/test_crypto.cpp` (hourly backups, identity
  key export), `desktop/tests/test_control_node.cpp`.

### D6. VS Code and future IDE integrations — Split

- **Extract — editor extensions (`extension.vscode`):**
  `marketplace/packages/forkmesh-ide` owns the source and is opt-in, but the
  desktop has no IDE runtime; `setPackageInstalled` reports it as managed by a
  compatible runtime. Blocked on a VSIX-installing runtime; JetBrains/Zed
  variants follow the same manifest.
- **Keep — `ClaudeIdeBridge`:** the in-app MCP/WebSocket bridge enforces
  `repositoryPathAllowed` on open/diff/selection and accepts agent edits.
- **Permissions:** `repository.issues.read`, `process.agent.start` (high),
  `storage.handoff.write`, `extension.claude.invoke`.
- **Package-owned state:** the editor extension's own storage and handoff
  files; the bridge's `~/.claude/ide/<port>.lock` stays host-owned.
- **Teardown:** uninstall the VSIX and delete handoff files; the bridge's
  `stop()` already removes its lockfile.
- **Focused tests:** `test_vscode_extension_is_marketplace_owned_and_opt_in` in
  `marketplace/tests/test_marketplace_lifecycle.py`,
  `desktop/tests/test_ide_bridge.cpp`.

### D7. Reusable Actions workflow templates and runner integrations — Split

- **Extract — templates (`service.development` content):** no template code
  exists yet. A template pack copies `.forkmesh/*.yml` into a repository after
  explicit user action; parsing stays in `ActionFile`, and approvals still bind
  to the tree and execution digest, so templates grant no authority.
  Blocked on a `content.template` socket.
- **Keep — runners:** `ActionRunner` (bwrap sandbox), `ActionStore` approvals
  and `actions/variables` secrets, and `MirrorActionsConfiguration` (Git push
  and mirror state) are trust boundaries.
- **Permissions (templates):** `repository.workflow.write` (sensitive, per
  explicit action).
- **Package-owned state:** none after copy; files written to a repository
  belong to that repository and survive uninstall.
- **Teardown:** remove the template list from the picker only.
- **Focused tests:** `desktop/tests/test_action_security.cpp` must still pass
  unchanged for templated workflows; add template-copy and uninstall tests.

## Dashboard (App) candidates

### A1. Notes workspace — Extract (page shipped, Worker still core)

- **Host API:** `dashboard.page` is used by `marketplace/packages/notes`.
  The Worker side is `builtin:notes`: `app/src/notes.py`, the
  `ForkMeshNoteRoom` Durable Object in `app/src/entry.py`, and routes in
  `app/src/app_runtime.py`.
- **Blocked on:** a `worker.http` move off `builtin:notes`, a Durable Object
  socket for the note room, and an account data scope because notes are
  personal as well as organization-owned (`dataLifecycle.scope` has no
  `account` value).
- **Permissions:** `notes.read`, `notes.write`, `repository.metadata.read`.
- **Package-owned state:** D1 `notes`, `note_shares`, `note_versions`,
  `note_links`, `note_views`; encrypted content and attachments.
- **Teardown:** export then purge per scope; no purge exists today.
- **Migration compatibility:** DDL is in `app/src/schema.py`; moving it means
  restamping the schema fingerprint (migration literal plus `entry.py` pin).
  `0120_notes.sql` and `0121_note_views.sql` have no down files.
- **Focused tests:** `app/tests/test_notes.py`,
  `app/tests/test_productivity_plugins.py` (pins `builtin:notes`; update in the
  same change), `app/tests/test_dashboard_notes_sidebar_frontend.py`.
- **Dependency note:** the World notes panel (W11) and desktop notes follow
  this package; they must not keep a parallel core copy.

### A2. Calendar and planning views — Extract (page shipped, Worker still core)

- **Host API:** `dashboard.page` is used by `marketplace/packages/calendar`;
  the Worker side is `builtin:calendar` in `app/src/calendar_api.py`.
- **Blocked on:** everything in A1, plus a cron socket (reminders run in
  `app/src/cron_activity.py`) and a notification-kind socket
  (`calendar_invitation`, `calendar_reminder`, `calendar_rsvp` are core kinds).
- **Permissions:** `calendar.events.read`, `calendar.events.write`,
  `repository.activity.read`.
- **Package-owned state:** D1 `calendar_events`, `calendar_attendees`,
  `calendar_reminder_deliveries`.
- **Teardown:** stop reminder delivery, export, purge, then retire the
  notification kinds so existing inbox rows render as plain text.
- **Migration compatibility:** `0130_calendars.sql` has no down; DDL in
  `schema.py` needs the same fingerprint restamp as A1.
- **Focused tests:** `app/tests/test_calendar.py`,
  `app/tests/test_calendar_signed_session.py`,
  `app/tests/test_productivity_plugins.py`.
- **Order:** after A1, which proves the Durable Object and account-scope work.

### A3. Microjobs and contributor-reward board — Keep

- **Why:** `app/src/microjobs_runtime.py`, the `microjob-payouts` cron phase,
  `chain_intents` plans, signed pull markers and referral anti-farming sit on
  wallet custody and reward settlement. Paid claims are ledger history and must
  survive any uninstall.
- **Allowed later:** a presentation-only `dashboard.page` that reads the
  existing public board and never submits claims or payouts.
- **Focused tests:** `app/tests/test_microjobs.py`,
  `app/tests/test_bounty_payout.py`, `app/tests/test_reward_policy.py`,
  `app/tests/test_contributor_reward_sync.py`.

### A4. Repository social-banner generator — Defer

- **Finding:** no generator exists. What exists is the stateless Open Graph
  card renderer `app/src/og_card.py`, owner-uploaded banners in the core
  `repo_media` table, and a committed `.forkmesh/banner.png`.
- **Blocked on:** a card-template input to `og_card.py` and a head-meta socket;
  the card is served on core repository paths, not `/api/plugins/<id>`.
- **Candidate shape:** a declarative template pack (layout, colours, fonts)
  with no permissions and no state; the core renderer stays the only code.
- **Teardown:** revert to the default template; edge-cached cards expire.
- **Migration compatibility:** none; uploaded media remains core repository
  data.
- **Focused tests:** `app/tests/test_repo_og_card.py`,
  `app/tests/test_repo_social_banner.py`.

### A5. Contributor, referral, and usage analytics widgets — Split

- **Keep — collection:** referral counting runs on HTML GETs and inside account
  creation (`app/src/account_users.py`), feeds quarantine and microjob payouts,
  and uses core attribution cron phases. `app/src/attribution.py`,
  `app/src/usage_reporting.py` and `app/src/api_metrics.py` stay core.
- **Extract — widgets:** read-only `dashboard.page` views over a bounded,
  already-aggregated analytics API. Blocked on that API.
- **Permissions:** `analytics.aggregate.read` (low); no raw rows, no identity.
- **Package-owned state:** layout preferences only.
- **Teardown:** detach the page; collected data is unaffected.
- **Focused tests:** `app/tests/test_referrals.py`,
  `app/tests/test_admin_usage_report.py`; add a test that uninstalling widgets
  leaves counts unchanged.

### A6. External repository import and sync connectors — Split

- **Keep — import service:** `app/src/repository_imports.py` promotes rows into
  signed catalog and mirror routing; purging them would orphan routing.
  Contributor invitations carry abuse limits.
- **Extract later — provider adapters:** GitHub/GitLab/Bitbucket adapters in
  `app/src/repository_migration.py` and the `tools/` sync scripts
  (`forge_sync.py`, `hosted_repository_sync.py`, `github_issue_sync.py`) behind
  a provider-adapter socket and `cli.command`, with exact `outboundOrigins`.
- **Permissions:** `network.<provider>` with request-only tokens (never stored,
  as today), `repository.import.request`.
- **Package-owned state:** none beyond in-flight job progress; import rows stay
  core.
- **Teardown:** cancel in-flight jobs; completed imports remain.
- **Migration compatibility:** `0044`, `0056`, `0060`, `0152` have no downs and
  stay core.
- **Focused tests:** `app/tests/test_repository_imports.py`,
  `app/tests/test_hosted_import_promotion.py`,
  `app/tests/test_repository_migration.py`, `app/tests/test_forge_sync.py`.

## World candidates

### W1–W3. Status obelisk, orbit sculpture, aurora beacon — Extract

- **Host API:** the store-element loader. Each module in
  `world/public/world/elements/` exports `manifest` and
  `build({THREE, params, net, now})`; only the obelisk uses `net`, scoped to
  `/api/world/deploy-status`.
- **Blocked on:** letting a package entrypoint supply an element module, so the
  loader reads the catalog instead of `elements/index.js`.
- **Permissions:** `world.scene.write` (low); obelisk adds
  `world.deploystatus.read` (low).
- **Package-owned state:** element parameters in the visitor's
  `world_elements` record; visitors hide elements via
  `forkmesh.world.disabledElements.v1`.
- **Teardown:** `dispose()` already frees textures, geometry and materials, and
  the scene removes the group.
- **Migration compatibility:** purchases in `world_element_purchases` are
  licences and must survive uninstall, per the lifecycle contract. Prices and
  parameter rules in `world/src/world_element_store.py` must come from the
  manifest instead of a parallel table.
- **Focused tests:** `world/tests/test_world_element_store.py`; add
  install → place → uninstall → reinstall keeps the purchase.
- **Order:** first World extraction. Publish separately if ownership or pricing
  differs.

### W4. Repository graph visualization — Defer

- **Host API:** none. `world/public/world/world-repository-graph.js` builds
  entities; the scene's `updateRepositoryGraph` and node selection live in
  `world.js` / `world-scene.js`.
- **Blocked on:** `world.data-source` (bounded repository preview snapshot) and
  an entity-selection hook that opens the core detail panel.
- **Permissions:** `repository.metadata.read` (low), `world.scene.write`.
- **Package-owned state:** none; built in memory.
- **Teardown:** dispose graph meshes and remove raycast entries; audit the
  current rebuild path for leaks before extraction.
- **Focused tests:** `world/tests/test_world_data_truth.py`,
  `world/tests/test_world_frontend.py`.

### W5. Workshop and data-model analysis experiences — Removed

World Code Workshops were never used and were removed on 2026-10-07 instead of
extracted: the analyzers, the `/api/world/workshops` API, its four D1 tables
(dropped by migration 0238), the dashboard's agent deep link and the workshop
chat room are gone. See [docs/world/README.md](world/README.md).

### W6. Office furniture and meeting-room modules — Split

- **Extract — furniture and layouts:** the district `install(host)` contract
  and `marketplace/packages/forkmesh-sky-office` are the precedent: package
  geometry, walk surface, camera bounds and `dispose`.
- **Keep — meeting room and access:** `world/public/world/world-office-meeting.js`,
  the `ForkMeshOfficeRoom` Durable Object, entry and meeting tickets, floor
  access by team, encrypted meeting chat, and D1 attendance/task tables.
- **Permissions (furniture):** `world.scene.write`.
- **Teardown:** remove meshes, raycast (`interactive`) and animation
  (`animated`) entries; restore default walk surface.
- **Focused tests:** `world/tests/test_world_office_frontend.py`,
  `world/tests/test_world_office_protocol.py`,
  `marketplace/packages/forkmesh-sky-office/tests/test_sky_office.py`.

### W7. Avatar face and outfit packs — Defer

- **Host API:** none. Faces and outfits are built in `world-scene.js`
  (`createAvatar`, `applyOutfit`, instanced parts) from
  `OUTFIT_COLOR_OPTIONS` / `OUTFIT_STYLE_OPTIONS` in `world-data.js`.
- **Blocked on:** a `world.avatar-part` socket and a relay allowlist
  (`WORLD_OUTFIT_COLOR_VALUES` / `WORLD_OUTFIT_STYLE_VALUES` in
  `world/src/world.py`) derived from the verified catalog, with a fallback look
  for peers who lack the pack.
- **Permissions:** `world.avatar.appearance` (low).
- **Package-owned state:** selected ids in `forkmesh.world.settings.v1`;
  presence `outfitStyle`/`faceImage` values.
- **Teardown:** dispose canvas textures and shared instanced parts; reset the
  saved selection to a bundled default.
- **Migration compatibility:** saved settings and live presence reference ids;
  an unknown id must render the default, never fail validation.
- **Must stay core:** the Supporting-member outfit perk check and presence
  sanitization.
- **Focused tests:** `world/tests/test_world_outfits.py`,
  `world/tests/test_world_avatar_instancing.py`,
  `world/tests/test_world_saved_client_profile.py`.

### W8. Locomotion, emote, and interaction packs — Split

- **Extract later — emotes and gestures:** `playEmote` and emote sprites, via an
  emote socket and a catalog-derived `WORLD_EMOTE_VALUES` allowlist; peers
  without the pack show a generic gesture.
- **Keep — locomotion:** movement tuning, walk surfaces, collision, jetpack and
  zip-line physics, and office scene rules.
- **Permissions:** `world.avatar.animate` (low).
- **Teardown:** clear `emoteSprites` and reset avatar pose.
- **Focused tests:** `world/tests/test_world_wave_button.py`,
  `world/tests/test_world_movement_responsiveness.py`.

### W9. Ambient music and spatial audio packs — Split

- **Extract — sound packs:** generalize the district sound emitter
  (`setSoundEmitter`, named sound events in the sky-office package) into
  `world.audio` with licensed assets and volume metadata.
- **Keep — media rooms:** `world/src/world_community_api.py` and D1
  `world_media_spaces`, `_items`, `_roles`, `_schedules`, `world_media_playback`
  (roles and moderation).
- **Permissions:** `world.audio.play` (low).
- **Teardown:** stop sources, release buffers; the host keeps the single
  user-gesture `AudioContext`.
- **Focused tests:** `world/tests/test_world_community.py`,
  `world/tests/test_world_frontend.py`.

### W10. Community event, advertising, and link kiosks — Split

- **Extract — board presentation:** event bulletin and link-kiosk boards on the
  banner-board host (`host.createSocialBanner`, `host.disposeObject3D`), as the
  X and Reddit packages do; the kiosk dialog needs `world.panel`.
- **Keep — governance and money:** `world/src/community_ads.py` proposals,
  votes, moderation and revenue; `world/src/world_link_kiosk.py` SOL rewards;
  `world/src/world_events.py` writes and moderation.
- **Permissions:** `world.events.read`, `world.links.read` (low),
  `external.link.open` (sensitive).
- **Teardown:** dispose board textures and raycast entries.
- **Focused tests:** `world/tests/test_world_link_kiosk.py`,
  `world/tests/test_world_community_ads.py`, `world/tests/test_world_events.py`,
  `world/tests/test_world_social_banners.py`.

### W11. Optional social and communication panels — Split

- **Extract — provider panels:** `world/public/world/world-discord-panel.js`
  belongs in the existing `discord` package, and `world-notes.js` in the
  `notes` package (A1), both through a `world.panel` socket.
- **Keep — World general chat:** it shares `world-general` with the desktop and
  dashboard chat bridge, and office chat is encrypted.
- **Permissions:** inherited from the owning package (`notes.read`,
  Discord's organization connector permissions).
- **Teardown:** close panels, the notes WebSocket and `window` message
  listeners.
- **Focused tests:** `world/tests/test_world_discord_frontend.py`,
  `world/tests/test_world_general_chat_board.py`.

## Keep in core

Unchanged from the issue: repository identity and authorization, signed
collaboration records, Git read/write and mirror integrity, the package
verifier and permission host, account and session security, and the minimal
repository and issue browser. This review adds these specific boundaries found
while checking the candidates:

- Backup archive contents and restore (D5).
- Agent worktrees, jail, guardrails and approvals; the Claude IDE bridge (D3, D6).
- Actions runners, approvals, variables and mirror actions (D7).
- Reward settlement, microjob payouts and referral collection (A3, A5).
- Import promotion into catalog and mirror routing (A6).
- Office meetings and access tickets, media-room roles, ad governance and
  kiosk rewards (W6, W9, W10).
- Relay presence and emote validation, movement and collision (W7, W8).

## Migration order

1. **No new socket:** D1 theme packs (icon-set entry), W1–W3 elements.
2. **Finish started packages:** A1 Notes, then A2 Calendar, off `builtin:*`;
   D6 IDE runtime; D7 template socket.
3. **One socket, several packages:** `world.panel` (W10, W11), `world.audio`
   (W9), district furniture (W6), `world.data-source` (W4),
   `desktop.chat-provider` (D2), `desktop.agent-provider` (D3), speech provider
   (D4).
4. **Catalog-driven relay allowlists and analytics APIs:** W7, W8, A4, A5, A6.

Each step lands with install, uninstall, reinstall and restart tests in
`marketplace/tests/test_marketplace_lifecycle.py` or the host's focused suite,
and its sub-issue links back to #653.
