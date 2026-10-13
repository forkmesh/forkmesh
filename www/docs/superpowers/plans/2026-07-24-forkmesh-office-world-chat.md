# ForkMesh Office World Chat Implementation Plan

> **Historical record (2026-07-24).** This design or plan was written before
> the repository layout moved (`cloudflare_worker/` became `app/`, the
> marketing Worker became `www/`, World code moved to `world/`, `qt_client/`
> became `desktop/`), so its paths and step lists are out of date. It is kept
> for the reasoning. The shipped code is the authority:
> `world/public/world/world-office*.js`, `world/src/world_office_routes.py`,
> `world/src/world_durable_objects.py` (`ForkMeshOfficeRoom`),
> `app/src/chat_channels_api.py`, and `app/public/chat.js`. See
> `docs/world/README.md` in the repository for the current World architecture.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a spatial `ForkMesh Office` landmark to ForkMesh World that opens the existing encrypted chat only after explicit entry, preserves the visible world in a compact dock, and keeps authorization and private chat data inside the chat client.

**Architecture:** Keep Three.js geometry and proximity in `world-scene.js`, put prompt and iframe lifecycle in a focused `world-office.js` controller, and render the same `/chat` client in a compact `embed=office` mode.
The world presence service gains only the consent-aware coarse activity `visiting-office`; it never receives chat content, private room identity, or membership.

**Tech Stack:** Python Cloudflare Worker, vanilla JavaScript custom elements, Three.js, Web Crypto, WebSocket, pytest, Node syntax checks, and Playwright.

---

## Preconditions and Working Rules

- Work on branch `feat/chat-world`.
- Run every command from the repository root, the directory that contains `cloudflare_worker`.
- Preserve the existing unrelated `wrangler.toml` modification.
- Do not run `no-mistakes axi` unless the user explicitly asks for it.
- Use the approved design in `cloudflare_worker/docs/superpowers/specs/2026-07-24-forkmesh-office-world-chat-design.md` as the product contract.
- Start every behavior change with a failing end-user-oriented test or the closest isolated browser contract.
- Keep every bridge payload free of message content, encryption material, session material, attachment data, and private room identifiers.
- Inspect every updated desktop, landscape, and portrait screenshot before accepting it.

## Task 1: Add Office Landmark and Presence Metadata

**Files:**

- Modify: `cloudflare_worker/tests/test_world_backend.py`
- Modify: `cloudflare_worker/tests/test_world_frontend.py`
- Modify: `cloudflare_worker/src/world.py`
- Modify: `cloudflare_worker/public/world/world-data.js`
- Modify: `cloudflare_worker/public/world/index.html`

- [ ] **Step 1: Write failing presence allowlist coverage.**

Add a test beside the existing activity allowlisting cases in `tests/test_world_backend.py`:

```python
def test_world_presence_allows_the_consent_aware_office_activity():
    current = world.default_presence("peer", 1000)
    _, result = world.sanitize_message({
        "type": "presence",
        "activityCategory": "visiting-office",
    }, current, 2000)

    assert result["activityCategory"] == "visiting-office"
```

- [ ] **Step 2: Write failing world metadata and fallback coverage.**

Extend `tests/test_world_frontend.py` with exact contracts:

```python
def test_forkmesh_office_is_a_navigable_world_landmark():
    assert 'id: "office"' in DATA
    assert 'label: "ForkMesh Office"' in DATA
    assert 'shortLabel: "Office"' in DATA
    assert "position: [11, 0, -21]" in DATA
    assert 'id: "visiting-office"' in DATA


def test_static_world_fallback_links_to_chat():
    assert 'href="/chat"' in INDEX
    assert "Open ForkMesh chat" in INDEX
```

- [ ] **Step 3: Run the focused tests and confirm they fail for the missing feature.**

Run:

```bash
python -m pytest cloudflare_worker/tests/test_world_backend.py cloudflare_worker/tests/test_world_frontend.py -q
```

Expected: the new assertions fail because `office`, `visiting-office`, and the fallback chat link do not exist yet.

- [ ] **Step 4: Add the bounded presence value.**

Add `visiting-office` to `WORLD_ACTIVITY_VALUES` in `src/world.py`.
Do not add a world WebSocket message type or any chat field.

- [ ] **Step 5: Add the landmark and activity metadata.**

Add this entry to `LANDMARKS` in `public/world/world-data.js` using the same complete metadata schema as its neighboring landmarks:

```javascript
{
  id: "office",
  label: "ForkMesh Office",
  shortLabel: "Office",
  eyebrow: "COLLABORATION / 14",
  icon: "⌁",
  color: "#9ef7c6",
  position: [11, 0, -21],
  summary: "Meet collaborators through ForkMesh's existing encrypted channel system.",
  metaphor: "An open office lobby where the chat terminal becomes available after deliberate entry.",
  reality:
    "The office embeds the normal authorized chat client. Avatar position never grants room access or exposes private membership.",
  status: "Encrypted chat available",
  statusTone: "live",
  bullets: [
    "Guests can use only public World #general.",
    "Registered users see only public and authorized private channels.",
    "Messages, room keys, tokens, and attachments never enter world presence.",
    "The hidden chat iframe disconnects after leaving the office.",
  ],
  primary: { label: "Walk to the office", action: "office" },
  secondary: { label: "Open full chat", href: "/chat" },
}
```

Add the matching `visiting-office` option to `ACTIVITY_OPTIONS`.
Place the Office in the guided tour and navigation using the established data-derived behavior rather than a duplicate hard-coded list.

- [ ] **Step 6: Add the non-WebGL fallback destination.**

Add this link to the fallback navigation in `public/world/index.html`:

```html
<a href="/chat">Open ForkMesh chat</a>
```

- [ ] **Step 7: Run the focused tests and confirm they pass.**

Run:

```bash
python -m pytest cloudflare_worker/tests/test_world_backend.py cloudflare_worker/tests/test_world_frontend.py -q
```

Expected: all selected tests pass.

- [ ] **Step 8: Commit the metadata slice.**

```bash
git add cloudflare_worker/tests/test_world_backend.py cloudflare_worker/tests/test_world_frontend.py cloudflare_worker/src/world.py cloudflare_worker/public/world/world-data.js cloudflare_worker/public/world/index.html
git commit -m "feat: add ForkMesh Office world metadata"
```

## Task 2: Build the Testable Office Interaction Controller

**Files:**

- Create: `cloudflare_worker/tests/test_world_office_frontend.py`
- Create: `cloudflare_worker/public/world/world-office.js`

- [ ] **Step 1: Write a failing Node contract for proximity hysteresis.**

Create `tests/test_world_office_frontend.py` with a helper that imports the ES module through Node and asserts JSON output.
Cover these transitions:

```javascript
[
  nextOfficeZoneState("distant", 6.6),
  nextOfficeZoneState("distant", 6.5),
  nextOfficeZoneState("nearby", 7.4),
  nextOfficeZoneState("nearby", 7.6),
]
```

Expected output:

```json
["distant", "nearby", "nearby", "distant"]
```

- [ ] **Step 2: Write failing static security and lifecycle contracts.**

Add assertions that the new controller source will contain:

```python
assert 'const OFFICE_CHAT_PATH = "/chat?embed=office"' in OFFICE
assert 'event.origin !== window.location.origin' in OFFICE
assert 'event.source !== frame.contentWindow' in OFFICE
assert 'type: "office-chat-suspend"' in OFFICE
assert "window.setTimeout" in OFFICE
assert "2000" in OFFICE
```

Also assert the controller does not contain bridge fields named `text`, `plaintext`, `roomKey`, `token`, `attachment`, or `privateChannelId`.
Parse bridge object literals narrowly so ordinary UI copy such as `textContent` does not cause a false positive.

- [ ] **Step 3: Run the controller tests and confirm the missing module failure.**

Run:

```bash
python -m pytest cloudflare_worker/tests/test_world_office_frontend.py -q
```

Expected: failure because `public/world/world-office.js` does not exist.

- [ ] **Step 4: Implement pure proximity behavior.**

Start `public/world/world-office.js` with:

```javascript
export const OFFICE_ENTER_DISTANCE = 6.5;
export const OFFICE_EXIT_DISTANCE = 7.5;
const OFFICE_CHAT_PATH = "/chat?embed=office";
const OFFICE_UNLOAD_DELAY_MS = 2000;

export function nextOfficeZoneState(currentState, distance) {
  const threshold = currentState === "nearby"
    ? OFFICE_EXIT_DISTANCE
    : OFFICE_ENTER_DISTANCE;
  return Number.isFinite(distance) && distance <= threshold
    ? "nearby"
    : "distant";
}
```

- [ ] **Step 5: Implement the controller factory.**

Export `createWorldOfficeController({ root, world, chatPath = OFFICE_CHAT_PATH })`.
The returned API must include:

```javascript
{
  setProximity,
  focusOffice,
  enterOffice,
  collapse,
  destroy,
}
```

Implement these invariants:

- `setProximity("nearby")` reveals the real entry button but does not assign iframe `src`.
- `setProximity("distant")` immediately hides the prompt and collapses an active console.
- `focusOffice()` calls `world.focusLandmark("office")` and records the triggering control for focus restoration.
- `enterOffice()` returns early unless proximity is `nearby` and `world.enterOffice()` succeeds.
- Successful entry assigns only the same-origin `/chat?embed=office` URL and focuses the console heading.
- `collapse()` sends `{ type: "office-chat-suspend" }`, hides the console immediately, restores focus, and removes the iframe source after 2,000 milliseconds.
- `E` enters only when nearby and when the event target is not an input, textarea, select, button, or editable element.
- `Escape` collapses only an active console.
- `destroy()` clears the unload timer and removes every installed event listener.

- [ ] **Step 6: Validate parent-child messages.**

Accept `office-chat-ready` and `office-chat-room-changed` only when both checks pass:

```javascript
if (event.origin !== window.location.origin) return;
if (event.source !== frame.contentWindow) return;
```

Use `office-chat-ready` only to clear the bounded loading state.
Do not retain or expose the room identifier from `office-chat-room-changed` in the world shell.

- [ ] **Step 7: Run unit and syntax checks.**

Run:

```bash
python -m pytest cloudflare_worker/tests/test_world_office_frontend.py -q
node --check cloudflare_worker/public/world/world-office.js
```

Expected: all checks pass.

- [ ] **Step 8: Commit the controller slice.**

```bash
git add cloudflare_worker/tests/test_world_office_frontend.py cloudflare_worker/public/world/world-office.js
git commit -m "feat: add world office interaction controller"
```

## Task 3: Construct the Three.js Office Landmark

**Files:**

- Modify: `cloudflare_worker/tests/test_world_office_frontend.py`
- Modify: `cloudflare_worker/public/world/world-scene.js`

- [ ] **Step 1: Add failing scene contracts.**

Assert the scene source includes:

```python
assert "function createForkMeshOffice(" in SCENE
assert 'office: createForkMeshOffice' in SCENE
assert 'userData.landmark = "office"' in SCENE
assert "onOfficeProximity" in SCENE
assert "enterOffice()" in SCENE
```

Also use a small Node test for `nextOfficeZoneState` rather than duplicating threshold math inside the scene test.

- [ ] **Step 2: Run the test and confirm the missing scene behavior.**

Run:

```bash
python -m pytest cloudflare_worker/tests/test_world_office_frontend.py -q
```

Expected: the new scene assertions fail.

- [ ] **Step 3: Import the shared office state primitives.**

Add this import to `public/world/world-scene.js`:

```javascript
import {
  nextOfficeZoneState,
} from "./world-office.js";
```

Accept an `onOfficeProximity = () => {}` callback in the existing scene factory options.

- [ ] **Step 4: Build the office using the existing low-poly scene language.**

Implement `createForkMeshOffice(THREE, position, interactive, animated)` with:

- A charcoal concrete shell.
- Dark green structural metal.
- Mint-tinted glazing.
- Warm emissive desk lighting.
- An open front and visible entrance.
- A reception desk, shared worktable, wall display, and chat terminal.
- A canvas-texture sign whose exact text is `FORKMESH OFFICE`.
- Shadow participation consistent with neighboring buildings.

Every clickable office mesh must set:

```javascript
mesh.userData.landmark = "office";
interactive.push(mesh);
```

Register the factory in the existing landmark factory map as `office: createForkMeshOffice`.

- [ ] **Step 5: Add scene-level proximity and entry APIs.**

Track the current office zone and calculate horizontal distance from the avatar to the office entrance during the existing landmark-distance update.
Call `onOfficeProximity(nextState)` only when the state changes.

Expose this method in the returned scene API:

```javascript
enterOffice() {
  if (officeZoneState !== "nearby") return false;
  cameraFocus = "office";
  moveTarget.set(office.position[0], currentFloorY, office.position[2] + 2.2);
  return true;
}
```

Adapt names to the existing vector and movement types.
Respect the existing reduced-motion setting when framing the office.

- [ ] **Step 6: Preserve deliberate entry on building click.**

Keep the existing interactive landmark click behavior focused on `focusLandmark("office")` and movement toward the entrance.
Do not call `enterOffice()` from the raycast click handler.

- [ ] **Step 7: Run the focused contracts and JavaScript syntax check.**

Run:

```bash
python -m pytest cloudflare_worker/tests/test_world_office_frontend.py cloudflare_worker/tests/test_world_frontend.py -q
node --check cloudflare_worker/public/world/world-scene.js
```

Expected: all checks pass.

- [ ] **Step 8: Commit the scene slice.**

```bash
git add cloudflare_worker/tests/test_world_office_frontend.py cloudflare_worker/public/world/world-scene.js
git commit -m "feat: build ForkMesh Office landmark"
```

## Task 4: Replace the Global World Chat Modal With Spatial Office UI

**Files:**

- Modify: `cloudflare_worker/tests/test_world_frontend.py`
- Modify: `cloudflare_worker/tests/test_world_office_frontend.py`
- Modify: `cloudflare_worker/public/world/world.js`
- Modify: `cloudflare_worker/public/world/world.css`

- [ ] **Step 1: Replace the old modal contract with failing Office contracts.**

Replace `test_chat_opens_inside_the_world_without_popup_permission` in `tests/test_world_frontend.py`.
Assert that:

```python
assert "data-world-office-focus" in APP
assert "data-world-office-enter" in APP
assert "data-world-office-chat" in APP
assert "data-world-office-frame" in APP
assert "Visit ForkMesh Office" in APP
assert "ForkMesh Office chat" in APP
assert 'office: "visiting-office"' in APP
assert "/chat?embed=office" not in APP
assert "data-world-chat-open" not in APP
assert "data-world-chat-frame" not in APP
assert "/dashboard/chat" not in APP
```

The negative embed URL assertion guarantees that the iframe source is not present in initial markup.

- [ ] **Step 2: Run the tests and confirm the legacy modal causes failure.**

Run:

```bash
python -m pytest cloudflare_worker/tests/test_world_frontend.py cloudflare_worker/tests/test_world_office_frontend.py -q
```

Expected: failures identify the current global chat modal and missing Office UI.

- [ ] **Step 3: Add semantic prompt and console markup.**

In the world component template in `public/world/world.js`, add:

```html
<section class="world-office-prompt" data-world-office-prompt hidden>
  <p>ForkMesh Office is open</p>
  <button type="button" data-world-office-enter>
    Enter ForkMesh Office <kbd>E</kbd>
  </button>
</section>

<section
  class="world-office-chat"
  data-world-office-chat
  aria-hidden="true"
  aria-labelledby="world-office-chat-title"
>
  <header class="world-office-chat__heading">
    <div>
      <span>Encrypted workspace</span>
      <h2 id="world-office-chat-title" tabindex="-1">ForkMesh Office</h2>
    </div>
    <button type="button" data-world-office-close aria-label="Collapse ForkMesh Office chat">×</button>
  </header>
  <p data-world-office-loading role="status">Opening encrypted chat...</p>
  <iframe
    data-world-office-frame
    title="ForkMesh Office chat"
    sandbox="allow-forms allow-same-origin allow-scripts"
    referrerpolicy="same-origin"
  ></iframe>
</section>
```

Keep the iframe without `src` in initial markup.

- [ ] **Step 4: Redirect both Chat shortcuts.**

Replace top-bar and quick-dock links with controls carrying `data-world-office-focus` and accessible text `Visit ForkMesh Office`.
They must call `officeController.focusOffice()` and must not navigate or open a panel immediately.
Route the landmark detail action `office` through the same focus method after closing the detail panel.

- [ ] **Step 5: Wire the controller to the scene.**

Import `createWorldOfficeController` in `world.js`.
Declare `let officeController = null`, create the scene with `onOfficeProximity: (state) => officeController?.setProximity(state)`, and create the controller immediately after the scene API exists.
Destroy the controller in the custom element teardown path.

Extend `updateLocation()` with `office: "visiting-office"` in its automatic activity map.
The existing `presenceActivity()` privacy gate must continue returning `hidden` whenever activity sharing is disabled.

Remove:

- The old `.world-chat-backdrop` markup.
- The old `.world-chat` markup.
- `openWorldChat()`.
- `closeWorldChat()`.
- Delegated `[data-world-chat-open]` and `[data-world-chat-close]` behavior.
- The `/dashboard/chat` iframe path handling.

- [ ] **Step 6: Add precise responsive styling.**

Desktop requirements in `world.css`:

```css
.world-office-chat {
  position: fixed;
  inset-block: var(--world-topbar-height) 0;
  inset-inline-end: 0;
  width: min(420px, calc(100vw - 24px));
  transform: translateX(100%);
}

.world-office-chat[data-open="true"] {
  transform: translateX(0);
}
```

Mobile requirements:

```css
@media (max-width: 720px) {
  .world-office-chat {
    inset: auto 0 0;
    width: 100%;
    max-width: none;
    height: min(58svh, 560px);
    transform: translateY(100%);
    padding-bottom: env(safe-area-inset-bottom);
  }

  .world-office-chat[data-open="true"] {
    transform: translateY(0);
  }
}
```

Fit these declarations to existing custom properties and stacking contexts.
Do not add a full-screen backdrop.
Add `min-width: 0` on grid and flex children, and verify no horizontal overflow at 320 CSS pixels.

- [ ] **Step 7: Add reduced-motion and focus-visible behavior.**

Disable prompt, dock, and camera transition animation under `prefers-reduced-motion: reduce`.
Give the entry, collapse, and full-chat controls visible keyboard focus rings.

- [ ] **Step 8: Run contracts and syntax checks.**

Run:

```bash
python -m pytest cloudflare_worker/tests/test_world_frontend.py cloudflare_worker/tests/test_world_office_frontend.py -q
node --check cloudflare_worker/public/world/world.js
node --check cloudflare_worker/public/world/world-scene.js
```

Expected: all checks pass and no legacy world chat modal marker remains.

- [ ] **Step 9: Commit the world shell slice.**

```bash
git add cloudflare_worker/tests/test_world_frontend.py cloudflare_worker/tests/test_world_office_frontend.py cloudflare_worker/public/world/world.js cloudflare_worker/public/world/world.css
git commit -m "feat: open chat through the ForkMesh Office"
```

## Task 5: Add the Compact Office Mode to the Encrypted Chat Client

**Files:**

- Create: `cloudflare_worker/tests/test_chat_office_embed_frontend.py`
- Modify: `cloudflare_worker/public/chat.html`
- Modify: `cloudflare_worker/public/chat.js`

- [ ] **Step 1: Write failing static embed contracts.**

Create `tests/test_chat_office_embed_frontend.py` with assertions for:

```python
assert 'chatQuery.get("embed") === "office"' in CHAT
assert 'type: "office-chat-ready"' in CHAT
assert 'message.type !== "office-chat-suspend"' in CHAT
assert "chatSuspended" in CHAT
assert "if (chatSuspended) return" in CHAT
assert 'target="_top"' in HTML
assert '[data-chat-embed="office"]' in HTML
```

Assert that the embed CSS hides the site header, people pane, explanatory footer, and administrator mutation controls while retaining the room pane, main message pane, status, composer, attachment picker, and paste behavior.

- [ ] **Step 2: Add failing error-message contracts.**

Assert exact actionable strings:

```python
assert "Your session expired. Log in again to use authorized channels." in CHAT
assert "Chat relay unavailable" in CHAT
```

Add a Node-executed or narrowly extracted function test that maps HTTP 401 and invalid-session failures to the expired-session message.

- [ ] **Step 3: Run the new and existing chat tests and confirm only new behavior fails.**

Run:

```bash
python -m pytest cloudflare_worker/tests/test_chat_office_embed_frontend.py cloudflare_worker/tests/test_private_chat_channels_frontend.py cloudflare_worker/tests/test_chat_attachments_frontend.py cloudflare_worker/tests/test_dashboard_chat_persist_frontend.py -q
```

Expected: only Office embed and newly explicit error contracts fail.

- [ ] **Step 4: Detect embed mode before first paint.**

Set an early root attribute from the query string so full-page chrome does not flash:

```html
<script>
  if (new URLSearchParams(location.search).get("embed") === "office") {
    document.documentElement.dataset.chatEmbed = "office";
  }
</script>
```

Define the same state once in `chat.js`:

```javascript
const chatQuery = new URLSearchParams(window.location.search);
const isOfficeEmbed = chatQuery.get("embed") === "office";
let chatSuspended = false;
```

- [ ] **Step 5: Add compact layout rules without changing normal `/chat`.**

Scope every layout override under `html[data-chat-embed="office"]`.
Hide:

- `.chat-site-header`.
- `.chat-people-pane`.
- The explanatory footer or note.
- Create-channel and membership mutation controls.

Keep:

- `.chat-rooms-pane`.
- `.chat-main-pane`.
- Room heading and connection status.
- `#chat-log`.
- `#chat-input` and `#chat-send`.
- Clipboard image handling.
- Attachment picker and previews.

Make the embed fill `100dvh`, use `min-width: 0`, and avoid nested page scrolling.

- [ ] **Step 6: Expose full chat management only to administrators.**

Add a hidden `Manage channels in full chat` link with `target="_top"`.
Reveal it only when `isOfficeEmbed && userSession()?.isAdmin` is true.
Do not show channel create, invite, remove-member, or delete controls inside the embed.

- [ ] **Step 7: Implement suspend-safe socket lifecycle.**

Add these guards:

```javascript
function scheduleReconnect() {
  if (chatSuspended) return;
  // Existing reconnect behavior follows.
}

async function connect() {
  if (chatSuspended) return;
  // Existing connect behavior follows.
}
```

On a valid same-origin parent message of type `office-chat-suspend`:

- Set `chatSuspended = true`.
- Clear the reconnect timer.
- Close the active socket.
- Do not clear retained room preference or message UI.
- Do not send chat data back to the parent.

Validate `event.origin === window.location.origin` and `event.source === window.parent`.

- [ ] **Step 8: Notify the parent only after initialization.**

At the end of successful UI initialization, send:

```javascript
if (isOfficeEmbed && window.parent !== window) {
  window.parent.postMessage({ type: "office-chat-ready" }, window.location.origin);
}
```

If room-change notification is retained, send only the event type or a public display label.
Do not send internal private channel identifiers.

- [ ] **Step 9: Make expired sessions and relay failures actionable.**

Normalize 401 and invalid-session responses before `privateChannelErrorMessage()`.
Map them to:

```text
Your session expired. Log in again to use authorized channels.
```

Map transport and 5xx relay failures to:

```text
Chat relay unavailable. Try again.
```

Keep the existing fallback to public World `#general` when a stored channel is no longer authorized.
Add a same-origin login link for the expired-session state and an explicit retry action for relay failure.

- [ ] **Step 10: Run all affected chat tests and syntax checks.**

Run:

```bash
python -m pytest cloudflare_worker/tests/test_chat_office_embed_frontend.py cloudflare_worker/tests/test_private_chat_channels_frontend.py cloudflare_worker/tests/test_chat_attachments_frontend.py cloudflare_worker/tests/test_dashboard_chat_persist_frontend.py -q
node --check cloudflare_worker/public/chat.js
```

Expected: all selected tests pass and ordinary `/chat` contracts remain unchanged.

- [ ] **Step 11: Commit the chat embed slice.**

```bash
git add cloudflare_worker/tests/test_chat_office_embed_frontend.py cloudflare_worker/public/chat.html cloudflare_worker/public/chat.js
git commit -m "feat: add compact office chat embed"
```

## Task 6: Cover the Real Office Journey in Playwright

**Files:**

- Modify: `cloudflare_worker/browser_tests/tests/world.spec.js`

- [ ] **Step 1: Replace the legacy modal E2E with the explicit-entry journey.**

Rename the test to:

```javascript
test("ForkMesh Office opens encrypted chat only after explicit entry", async ({
  page,
  context,
}) => {
```

Keep the real encrypted-envelope assertions from the existing test.
Change the setup and assertions to cover:

1. Initial world load creates zero chat WebSockets.
2. Clicking `Visit ForkMesh Office` focuses or moves toward the Office but creates zero chat WebSockets.
3. Entering the proximity zone reveals `Enter ForkMesh Office` but creates zero chat WebSockets.
4. Pressing `E` opens the dock and loads `/chat?embed=office`.
5. Exactly one chat WebSocket connects to World `#general`.
6. Sending text produces one encrypted persistent frame.
7. The page URL and tab count do not change.
8. Pressing `Escape` collapses the dock.
9. After 2,000 milliseconds, the iframe is unloaded and reconnect does not occur.

Use a deterministic scene test hook or direct scene movement rather than timing avatar keyboard movement.
The hook must exercise the same proximity callback and controller used by production.

- [ ] **Step 2: Add a real attachment and retention journey.**

Within the Office frame:

- Paste a generated PNG through `ClipboardEvent` and assert one encrypted attachment envelope is sent.
- Select a small text or PDF fixture through the file input and assert its encrypted attachment metadata is sent.
- Echo retained encrypted frames from the mocked room socket.
- Collapse and re-enter the Office.
- Assert text, image, and document entries return from retained history.

Never inspect plaintext in the outer world frame.

- [ ] **Step 3: Add room authorization journeys.**

Add focused tests or a parameterized table for:

- Guest sees only World `#general`.
- Registered user sees World `#general` and authorized public channels.
- Invited user sees the invited private channel.
- Uninvited user does not see a private channel.
- Administrator sees authorized channels and the `Manage channels in full chat` link.
- Expired session sees the exact actionable login message.

Mock the existing `/api/chat/channels`, membership, room-key, and WebSocket routes.
Do not invent an Office-only authorization endpoint.

- [ ] **Step 4: Add lifecycle and bridge rejection coverage.**

Assert that:

- Leaving beyond 7.5 units collapses the dock.
- Re-entering within 6.5 units does not automatically reopen it.
- A message from another origin is ignored.
- A message from a same-origin sibling frame is ignored.
- `office-chat-ready` from the Office frame clears loading state.

- [ ] **Step 5: Run the new E2E tests and observe the initial red result.**

Run:

```bash
cd cloudflare_worker/browser_tests
npx playwright test tests/world.spec.js --grep "ForkMesh Office|office chat|authorized channels"
```

Expected before final integration adjustments: failures identify missing selectors, lifecycle details, or mock endpoints.

- [ ] **Step 6: Fix only integration defects revealed by the E2E.**

Keep fixes inside the ownership boundaries defined by the design:

- Scene issues in `world-scene.js`.
- Prompt and iframe issues in `world-office.js`.
- Shell wiring issues in `world.js`.
- Chat behavior issues in `chat.js` and `chat.html`.

Do not move encryption or authorization logic into the world controller.

- [ ] **Step 7: Run the E2E tests again.**

Run:

```bash
cd cloudflare_worker/browser_tests
npx playwright test tests/world.spec.js --grep "ForkMesh Office|office chat|authorized channels"
```

Expected: all Office journey tests pass.

- [ ] **Step 8: Commit the E2E slice and any integration fixes.**

```bash
git add cloudflare_worker/browser_tests/tests/world.spec.js cloudflare_worker/public/world cloudflare_worker/public/chat.html cloudflare_worker/public/chat.js
git commit -m "test: cover spatial office chat journeys"
```

## Task 7: Update and Inspect Responsive World Snapshots

**Files:**

- Modify: `cloudflare_worker/browser_tests/tests/world.spec.js`
- Modify: `cloudflare_worker/browser_tests/tests/world.spec.js-snapshots/world-desktop-linux.png`
- Modify: `cloudflare_worker/browser_tests/tests/world.spec.js-snapshots/world-landscape-linux.png`
- Modify: `cloudflare_worker/browser_tests/tests/world.spec.js-snapshots/world-portrait-linux.png`

- [ ] **Step 1: Make the screenshot state intentionally show the landmark and dock.**

For each existing viewport snapshot, deterministically focus the camera on the Office.
Capture one stable closed state that clearly shows the `FORKMESH OFFICE` banner.
Add an Office-open snapshot assertion where the current suite structure permits descriptive filenames without replacing unrelated baselines.

- [ ] **Step 2: Add explicit responsive geometry assertions.**

At desktop width, assert the open dock width is at most 420 CSS pixels and the canvas remains visible.
At mobile width, assert the open sheet height is at most 58 percent of the small viewport height.
At 320 CSS pixels, assert:

```javascript
expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(320);
```

Allow for the actual device viewport width rather than hard-coding 320 when the project helper provides it.

- [ ] **Step 3: Generate updated snapshots.**

Run:

```bash
cd cloudflare_worker/browser_tests
npx playwright test tests/world.spec.js --grep "snapshot|responsive" --update-snapshots
```

Expected: Playwright writes only the intended world baseline changes.

- [ ] **Step 4: Inspect every generated image manually.**

Open desktop, landscape, and portrait snapshots and reject them if any of these are present:

- The Office banner is illegible or clipped.
- Floating landmark labels collide with the banner.
- The dock hides the entire world.
- The mobile sheet exceeds its bound or ignores the safe area.
- Controls clip, overflow, or become too small to tap.
- The palette or geometry looks inconsistent with the existing low-poly world.

Fix visual defects and regenerate until all three states are deliberate and pixel-clean.

- [ ] **Step 5: Run the complete world browser file without snapshot updates.**

Run:

```bash
cd cloudflare_worker/browser_tests
npx playwright test tests/world.spec.js
```

Expected: the full world browser suite passes with no snapshot diff.

- [ ] **Step 6: Commit the visual baselines.**

```bash
git add cloudflare_worker/browser_tests/tests/world.spec.js cloudflare_worker/browser_tests/tests/world.spec.js-snapshots
git commit -m "test: add ForkMesh Office visual baselines"
```

## Task 8: Final Verification and Security Audit

**Files:**

- Verify all files changed by Tasks 1 through 7.
- Do not modify: `cloudflare_worker/wrangler.toml`.

- [ ] **Step 1: Run all focused Python frontend and backend tests.**

Run:

```bash
python -m pytest \
  cloudflare_worker/tests/test_world_backend.py \
  cloudflare_worker/tests/test_world_frontend.py \
  cloudflare_worker/tests/test_world_office_frontend.py \
  cloudflare_worker/tests/test_chat_office_embed_frontend.py \
  cloudflare_worker/tests/test_private_chat_channels_frontend.py \
  cloudflare_worker/tests/test_chat_attachments_frontend.py \
  cloudflare_worker/tests/test_dashboard_chat_persist_frontend.py \
  -q
```

Expected: all selected tests pass.

- [ ] **Step 2: Run syntax checks for every changed JavaScript module.**

Run:

```bash
node --check cloudflare_worker/public/world/world-data.js
node --check cloudflare_worker/public/world/world-office.js
node --check cloudflare_worker/public/world/world-scene.js
node --check cloudflare_worker/public/world/world.js
node --check cloudflare_worker/public/chat.js
```

Expected: every command exits with status 0.

- [ ] **Step 3: Run the complete world Playwright suite.**

Run:

```bash
cd cloudflare_worker/browser_tests
npx playwright test tests/world.spec.js
```

Expected: all tests and all snapshot assertions pass.

- [ ] **Step 4: Audit the privacy boundary with source searches.**

Run:

```bash
rg -n "office-chat|visiting-office|privateChannelId|roomKey|plaintext|attachment|token" cloudflare_worker/public/world cloudflare_worker/src/world.py
```

Expected:

- World files contain only Office lifecycle message types and the coarse activity.
- No world bridge object contains plaintext, token, key, attachment, or private room data.
- `src/world.py` contains `visiting-office` only as an allowlisted activity value.

- [ ] **Step 5: Confirm the old modal and dashboard chat iframe are gone.**

Run:

```bash
rg -n "data-world-chat|openWorldChat|closeWorldChat|/dashboard/chat" cloudflare_worker/public/world cloudflare_worker/tests/test_world_frontend.py cloudflare_worker/browser_tests/tests/world.spec.js
```

Expected: no matches.

- [ ] **Step 6: Check formatting, diff scope, and unrelated work preservation.**

Run:

```bash
git diff --check
git status --short
git diff --stat HEAD~7..HEAD
```

Expected:

- `git diff --check` reports no whitespace errors.
- `wrangler.toml` remains modified but uncommitted and unchanged by this feature.
- No generated changelog is modified.
- Every implementation commit contains only its intended slice.

- [ ] **Step 7: Review the acceptance criteria line by line.**

Confirm all acceptance criteria from the approved design specification are backed by a passing test or inspected screenshot.
Pay special attention to explicit entry, the 6.5 and 7.5 hysteresis thresholds, iframe unloading, guest and private-room authorization, clipboard images, documents, refresh retention, actionable failure states, and 320-pixel overflow.

- [ ] **Step 8: Report completion with evidence.**

Summarize:

- The user-visible Office journey.
- The security and privacy boundary.
- Exact test commands and pass counts.
- Screenshot viewports inspected.
- The preserved unrelated `wrangler.toml` change.
- The fact that `no-mistakes axi` was not run.

Do not claim completion until all commands above have run successfully and the visual baselines have been inspected.
