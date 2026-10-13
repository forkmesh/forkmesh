# Public and Private Chat Channels Implementation Plan

> **Historical record (2026-07-24).** This design or plan was written before
> the repository layout moved (`cloudflare_worker/` became `app/`, the
> marketing Worker became `www/`, World code moved to `world/`, `qt_client/`
> became `desktop/`), so its paths and step lists are out of date. It is kept
> for the reasoning. The shipped code is the authority:
> `world/public/world/world-office*.js`, `world/src/world_office_routes.py`,
> `world/src/world_durable_objects.py` (`ForkMeshOfficeRoom`),
> `app/src/chat_channels_api.py`, and `app/public/chat.js`. See
> `docs/world/README.md` in the repository for the current World architecture.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let administrators choose public or private visibility during channel creation and atomically invite registered users to private channels while preserving encrypted history and strict server authorization.

**Architecture:** Visibility remains inside the encrypted channel record and defaults to private for legacy data. The focused channel API validates all initial members before one D1 batch, while list, room-access, and socket gates treat public as available to active registered users and private as membership-gated. The existing `/chat` dialog receives a visibility dropdown and searchable checkbox picker backed by the public account directory.

**Tech Stack:** Python 3.12, Cloudflare Python Workers, D1 SQLite, Durable Objects, vanilla JavaScript, pytest, and Playwright.

---

## File Map

- Modify `src/chat_channels_api.py` for visibility validation, filtered listing, atomic initial membership, and private-only member management.
- Modify `src/entry.py` for WebSocket visibility authorization, D1 batch support already exposed by the runtime, demo-login null binding, and Durable Object storage conversion.
- Modify `public/chat.html` for the visibility dropdown, searchable registered-user picker, visibility badge, and responsive styling.
- Modify `public/chat.js` for visibility state, user-directory selection, creation payloads, public/private labels, and private-only management controls.
- Modify `public/docs/protocol/index.html` for authenticated public-channel and private initial-invite contracts.
- Modify `tests/test_chat_channels_api.py` for API behavior and transactional rollback.
- Modify `tests/test_chat_channel_room_access.py` for public/private WebSocket admission.
- Modify `tests/test_private_chat_channels_frontend.py` for UI source contracts.
- Modify `browser_tests/tests/private-chat-channels.spec.js` for the rendered creation and access journey.

### Task 1: Pin Real-Worker Persistence and Demo Login Fixes

**Files:**

- Modify: `tests/test_login_key_binding.py`
- Modify: `tests/test_chat_history_byte_budget.py`
- Modify: `src/entry.py`

- [ ] **Step 1: Add a failing demo-account binding regression**

Assert that `_ensure_local_demo_account` supplies a concrete SQL-null-compatible `ip_bi` value when it calls `_save_account_full`.
Use the existing extracted-function harness so the regression exercises the same call boundary as local login.

- [ ] **Step 2: Add a failing Durable Object storage-conversion regression**

Return a JavaScript-like object with `to_py()` from fake Durable Object storage and require `_retention_ingress_admitted` to preserve the stored byte count.
Require all storage writes to receive a JavaScript-converted object rather than a raw Python dictionary.

- [ ] **Step 3: Run the regressions and confirm failure**

Run `uv run --isolated --with pytest python -m pytest -q tests/test_login_key_binding.py tests/test_chat_history_byte_budget.py`.
Expected result: the new null-binding and conversion assertions fail before production edits.

- [ ] **Step 4: Apply the minimal fixes**

Pass `ip_bi=to_js(None)` from the local demo bootstrap so D1 receives SQL null instead of JavaScript undefined.
Normalize storage reads through `to_py()` and wrap ingress state writes with `to_js()`.

- [ ] **Step 5: Re-run the focused regressions**

Run the same pytest command.
Expected result: all selected tests pass.

### Task 2: Add Visibility and Atomic Initial Members to the API

**Files:**

- Modify: `tests/test_chat_channels_api.py`
- Modify: `src/chat_channels_api.py`

- [ ] **Step 1: Extend the fake runtime with transactional batch behavior**

Add `batch(statements)` that executes every `(sql, args)` tuple inside one SQLite transaction and rolls back the complete batch when any statement fails.

- [ ] **Step 2: Write failing creation tests**

Cover `public` creation with zero members, `private` creation with `alice` and `bob`, duplicate normalized usernames, missing users, non-array members, member overflow, and public creation with members.
Assert that invalid member input leaves both channel tables empty.

- [ ] **Step 3: Write failing list and room-access tests**

Assert that active registered users see all public channels plus only their private memberships.
Assert that uninvited users can access public room credentials but receive `404 not_found` for private room credentials.
Assert that legacy encrypted records without visibility remain private.

- [ ] **Step 4: Run the API test and confirm failure**

Run `uv run --isolated --with pytest python -m pytest -q tests/test_chat_channels_api.py`.
Expected result: visibility fields and initial membership behavior are absent.

- [ ] **Step 5: Implement normalized visibility and bounded member validation**

Add `_visibility(record)` that returns `public` only for an exact public value and returns `private` otherwise.
Validate `visibility` against `{"public", "private"}` and `members` as a list of no more than 500 strings.
Resolve each member through `runtime.account`, deduplicate canonical usernames, and skip current administrators because their access is implicit.

- [ ] **Step 6: Write the channel and memberships atomically**

Seal `{name, createdBy, visibility}` and every `{username}` member payload before persistence.
Call `runtime.batch([(channel_sql, channel_args), *member_statements])` once and audit only after it succeeds.
Return `visibility` and the initial member summaries in the `201` response.

- [ ] **Step 7: Implement visibility-aware listing and access**

For non-admin listing, left join the requester membership, decrypt each bounded channel record, and include public records or joined private records.
For room access, require membership only when `_visibility(record) == "private"`.
Reject member listing and mutation on public channels with `members_not_allowed`.

- [ ] **Step 8: Re-run the API tests**

Run `uv run --isolated --with pytest python -m pytest -q tests/test_chat_channels_api.py`.
Expected result: all API tests pass.

### Task 3: Enforce Visibility at the WebSocket Boundary

**Files:**

- Modify: `tests/test_chat_channel_room_access.py`
- Modify: `src/entry.py`

- [ ] **Step 1: Write failing socket admission tests**

Extend the socket harness so the channel query returns encrypted `data` and the decrypt stub returns either public or private visibility.
Assert that an active registered non-member is admitted to public channels and rejected from private channels.
Assert that disabled and node-kind accounts are rejected from both.

- [ ] **Step 2: Run the socket tests and confirm failure**

Run `uv run --isolated --with pytest python -m pytest -q tests/test_chat_channel_room_access.py`.
Expected result: public non-members are rejected.

- [ ] **Step 3: Update the admission query and policy**

Select `data,key_version` from `chat_channels`, decrypt the record, and default missing visibility to private.
Check membership only for a non-admin private-channel requester.
Keep all missing and unauthorized responses as `404 not_found` before Durable Object lookup.

- [ ] **Step 4: Re-run the socket tests**

Run the same pytest command.
Expected result: all socket authorization tests pass.

### Task 4: Build the Public/Private Creation Interface

**Files:**

- Modify: `tests/test_private_chat_channels_frontend.py`
- Modify: `public/chat.html`
- Modify: `public/chat.js`

- [ ] **Step 1: Write failing frontend contracts**

Require `#chat-channel-visibility`, `#chat-channel-initial-members`, a registered-user search input, public/private accessible room labels, and a creation body containing `visibility` and `members`.
Require the initial-member region to be hidden and cleared when Public is selected.

- [ ] **Step 2: Run the frontend contract and confirm failure**

Run `uv run --isolated --with pytest python -m pytest -q tests/test_private_chat_channels_frontend.py`.
Expected result: the new controls and payload are missing.

- [ ] **Step 3: Add the semantic form controls**

Add a labeled visibility select with Private first and Public second.
Add a private-only searchable checkbox list with a live selection count and an empty-state message.
Add a compact selected-channel visibility badge and preserve the dialog's narrow-screen fit.

- [ ] **Step 4: Populate and filter registered users**

Retain normalized active usernames returned by `/api/accounts/users` in a client-side map.
Render matching checkboxes while excluding the current administrator.
Preserve selections across search filtering and clear them after successful creation or visibility change to Public.

- [ ] **Step 5: Send the complete creation request**

Rename the creation helper to `createChannel(name, visibility, members)` and send all three normalized fields.
Store the returned `visibility` in the channel map.
Show member management only when the active channel is private and manageable.

- [ ] **Step 6: Re-run the frontend contracts**

Run the same pytest command.
Expected result: all frontend contracts pass.

### Task 5: Prove the Rendered Journey and Update the Protocol

**Files:**

- Modify: `browser_tests/tests/private-chat-channels.spec.js`
- Modify: `public/docs/protocol/index.html`

- [ ] **Step 1: Extend the Playwright API fixture**

Return `admin`, `alice`, and `bob` from the registered-user directory.
Persist channel visibility and initial members from the POST body.
List public channels for all authenticated users and private channels only for administrators or members.

- [ ] **Step 2: Exercise private creation with initial users**

Select Private, search for Alice, select her, create the channel, and verify Alice receives immediate access after session switch.
Verify Bob cannot see the private channel.

- [ ] **Step 3: Exercise public creation**

Select Public and verify the user picker disappears.
Create the channel and verify Bob can see and join it while the create control remains hidden.
Verify a guest session remains restricted to `#general`.

- [ ] **Step 4: Check responsive layout and durable encrypted content**

At 390 by 844 pixels, assert the dialog and document have no horizontal overflow.
Send text, a pasted image, and a selected document, reload, and assert all three replay in the same channel.

- [ ] **Step 5: Update protocol documentation**

Document the visibility field, active-registered-user scope for public channels, private initial members, legacy-private default, and atomic creation behavior.

- [ ] **Step 6: Run focused and broad verification directly**

Run `uv run --isolated --with pytest python -m pytest -q tests/test_chat_channels_api.py tests/test_chat_channel_room_access.py tests/test_chat_history_byte_budget.py tests/test_login_key_binding.py tests/test_private_chat_channels_frontend.py tests/test_local_migrations.py`.
Run `cd browser_tests && npx playwright test tests/private-chat-channels.spec.js tests/chat-persistence.spec.js tests/chat-attachments.spec.js`.
Expected result: both commands pass and the real local browser flow retains the created channels and encrypted content after refresh.
