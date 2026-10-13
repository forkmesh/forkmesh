# ForkMesh Office World Chat Design

> **Historical record (2026-07-24).** This design or plan was written before
> the repository layout moved (`cloudflare_worker/` became `app/`, the
> marketing Worker became `www/`, World code moved to `world/`, `qt_client/`
> became `desktop/`), so its paths and step lists are out of date. It is kept
> for the reasoning. The shipped code is the authority:
> `world/public/world/world-office*.js`, `world/src/world_office_routes.py`,
> `world/src/world_durable_objects.py` (`ForkMeshOfficeRoom`),
> `app/src/chat_channels_api.py`, and `app/public/chat.js`. See
> `docs/world/README.md` in the repository for the current World architecture.

## Summary

ForkMesh World will gain a new Three.js landmark named `ForkMesh Office`.
The office turns chat from a global utility overlay into a place visitors intentionally approach and enter.
When a visitor reaches the office entrance, the world reveals an explicit entry action.
After entry, a compact office chat console opens while the Three.js world remains visible and interactive.
The console reuses the existing encrypted chat client, channel authorization, retained history, clipboard image sharing, and document sharing.

## Current State

The world currently exposes Chat links in the top bar and quick dock from every location.
Those links open a large modal containing an iframe for `/dashboard/chat`.
The modal covers most of the world and has no relationship to avatar position or a world landmark.
The world-presence WebSocket carries movement, coarse presence, and limited interactions, while chat uses a separate encrypted room transport.
That separation is correct and must remain intact.

## Experience Goal

Chat should feel like entering a shared office without becoming harder to use or less secure.
The visitor should see a recognizable building, deliberately enter it, and continue seeing the world while chatting.
Proximity may reveal an action, but it must never open a panel, connect a chat socket, or steal keyboard focus automatically.
The feature must work for mouse, keyboard, touch, reduced-motion, and non-WebGL visitors.

## Office Landmark

The new landmark identifier is `office`.
Its label is `ForkMesh Office`, its short label is `Office`, and its initial scene coordinate is `[11, 0, -21]`.
This parcel is outside the interaction radii of the Organization Quarter and Security Workshop while remaining reachable from Town Square.
The landmark appears in the world map, object-label layer, quick navigation, distance display, technical detail panel, and guided tour.

The building uses the existing low-poly architectural language.
Its primary materials are charcoal concrete, dark green metal, mint glazing, and warm desk lighting.
The front remains visually open so visitors can understand that it is enterable.
A canvas-texture sign above the entrance reads `FORKMESH OFFICE`.
Interior geometry contains a reception desk, shared worktable, wall display, and chat terminal, but no decorative element may imply access to data that the current visitor cannot obtain.

Every interactive office mesh receives `userData.landmark = "office"`.
The landmark factory participates in the existing interactive mesh, shadow, label, and focus systems.

## Spatial Interaction State Machine

The office experience has five states.

1. `distant`: The landmark and map entry are visible, but no office-specific UI or chat connection exists.
2. `nearby`: The avatar is within 6.5 horizontal world units of the office entrance, so an `Enter ForkMesh Office` action appears.
3. `entering`: The visitor activates the action by clicking, tapping, or pressing `E`, and the camera frames the office interior.
4. `active`: The compact office console is visible and its chat iframe is allowed to connect.
5. `exiting`: The avatar crosses beyond 7.5 units, the console collapses immediately, and the iframe is unloaded after a two-second grace period.

Separate enter and exit thresholds provide hysteresis so small avatar movements do not flicker the console.
The `nearby` state never activates the chat connection by itself.
Clicking the building focuses the landmark and moves the avatar toward the entrance, but still requires the explicit entry action.
Pressing `Escape` collapses the console without moving the avatar.
After a manual collapse, the console stays closed until the visitor activates the entry action again or leaves and re-enters the zone.
Every collapse sends the suspend event and unloads the iframe after the same two-second grace period so a hidden chat socket does not remain connected.

## Navigation Changes

The top-bar and quick-dock Chat controls no longer open the global chat modal.
They focus the `ForkMesh Office` landmark and move the avatar toward its entrance.
Their accessible names become `Visit ForkMesh Office`.
The existing large chat modal, backdrop, `/dashboard/chat` iframe, and associated open and close methods are removed from the world shell.
The normal full `/chat` page remains available outside the world.

## Office Chat Console

The office console loads `/chat?embed=office` in a same-origin sandboxed iframe only after explicit entry.
The standalone chat client remains the owner of channel listing, room access, key derivation, encrypted WebSockets, retained history, attachments, and error mapping.
The world shell does not reimplement or import cryptographic chat logic.

The `embed=office` presentation keeps the room selector, selected-room heading, message history, composer, clipboard paste, attachment picker, and connection status.
It hides the global site header, full people directory, explanatory footer, and administrator channel-creation controls.
Administrators receive a `Manage channels in full chat` link rather than mutation controls inside the compact console.
That link targets the top-level browsing context so the full chat page does not open inside the office iframe.

On desktop, the console is a 420-pixel right-side dock that does not add a full-screen backdrop.
On narrow screens, it becomes a bottom sheet capped at 58 percent of the small viewport height.
The sheet exposes a clear collapse handle and keeps a meaningful portion of the world visible.
The console must not create horizontal overflow at 320 CSS pixels.

The parent and iframe communicate only through same-origin `postMessage` events with explicit message types.
The child may send `office-chat-ready` and `office-chat-room-changed`.
The parent may send `office-chat-suspend` before unloading the iframe.
Neither side sends plaintext messages, encryption keys, session tokens, private channel identifiers, or attachment bytes through this bridge.

## Room and Identity Rules

Guests may use only the existing public World `#general` room and retain the current `World visitor` identity behavior.
Active registered users may see World `#general` plus every administrator-created public channel and private channel they are authorized to access.
Current administrators retain implicit access under the existing server authorization model.
The office console never grants access based on avatar position.
Every room list, room-access request, and WebSocket handshake continues through the existing chat authorization path.

The selected room is restored using the existing chat client local preference.
If the stored room is no longer authorized, the client removes it from the selector and returns to World `#general`.
The world-presence stream may expose the coarse activity `visiting-office` only when the visitor has enabled public activity sharing.
It must never expose the selected channel name, channel identifier, private membership, message activity, or typing state.

## Security and Privacy Boundaries

Chat frames continue to use the existing AES-256-GCM client transport and versioned room authorization.
The world-presence WebSocket must not accept or broadcast chat messages.
Office proximity is a presentation condition, not an authorization condition.
Guests cannot discover administrator-created channels.
Uninvited registered users cannot discover private channels.
Leaving the office unloads the iframe so its WebSocket is not kept alive invisibly.

The iframe sandbox remains limited to `allow-forms allow-same-origin allow-scripts`.
The embed URL must be same-origin and restricted to `/chat`.
The parent rejects bridge events from any other origin or source window.
The office scene and presence state never receive decrypted message content.

## Failure Behavior

If the chat iframe is still loading, the world remains interactive and the console shows a bounded loading state.
If the chat relay is unavailable, the office remains explorable and the console shows `Chat relay unavailable` with a retry action.
If the account session expired, the console shows `Your session expired. Log in again to use authorized channels.` and provides a same-origin login link.
If a room becomes unauthorized, the console returns to World `#general` without exposing the denied room in world UI.
If WebGL initialization fails, the static fallback contains a normal `Open ForkMesh chat` link.
If the visitor has reduced motion enabled, camera framing changes immediately without tweened movement.

## Accessibility

The office is present in the semantic world map independently of the WebGL canvas.
The entry prompt is a real button with visible text and an `E` keyboard hint.
Keyboard focus moves to the office console heading only after explicit entry.
Collapsing the console returns focus to the entry action or the control that focused the office.
The iframe has the title `ForkMesh Office chat`.
All entry, loading, connected, disconnected, and session-expired states have text equivalents and do not rely on color.
The interface respects existing safe-area insets and reduced-motion preferences.

## Architecture and File Boundaries

`public/world/world-data.js` owns the office landmark metadata, position, tour entry, and the consent-aware `visiting-office` activity option.
`public/world/world-scene.js` owns office geometry, interactive meshes, proximity calculation, camera framing, and office entry and exit callbacks.
`public/world/world-office.js` is a new focused controller for the entry prompt, console lifecycle, iframe bridge, responsive state, and focus restoration.
`public/world/world.js` wires the scene callbacks to the office controller and redirects existing Chat controls to the landmark.
`public/world/world.css` owns the entry prompt and office console presentation.
`public/world/index.html` adds the non-WebGL fallback chat destination and loads the focused office controller through the world module graph.
`public/chat.html` and `public/chat.js` own the `embed=office` presentation mode while retaining their existing chat responsibilities.
`src/world.py` extends the bounded activity allowlist with `visiting-office` and makes no chat-protocol changes.

The world controller must not gain encryption, room-access, retained-history, or attachment code.
The chat client must not gain Three.js position or proximity knowledge.
The office controller is the only module that knows about both world entry state and iframe lifecycle.

## Compatibility and Migration

No D1 schema migration is required.
No existing chat channel, key, membership, retention, or message format changes.
Existing `/chat` visitors receive the unchanged full-page layout when `embed=office` is absent.
Existing world URLs and landmark deep links remain valid.
The old world chat modal is replaced rather than supported in parallel, preventing two hidden chat iframes from connecting simultaneously.

## Testing Strategy

Scene-level contract tests verify the `office` landmark factory, coordinate, interactive metadata, proximity thresholds, hysteresis, and callback surface.
Frontend contract tests verify that `embed=office` hides only nonessential chrome while preserving rooms, messages, composer, paste, attachments, and status.
World Playwright tests walk or focus the avatar toward the office, confirm no chat connection in `distant` or `nearby`, enter explicitly, and verify the console opens without navigation or a new tab.
Authorization journeys verify guest, registered-public, invited-private, uninvited-private, administrator, and expired-session behavior.
Messaging journeys send text, paste an image, attach a document, refresh or re-enter the office, and confirm encrypted retained history returns.
Lifecycle tests leave the office and confirm the panel collapses, the iframe unloads after the grace period, and re-entry restores the last authorized room.
Desktop, landscape, and portrait snapshots verify the building banner, label placement, console proportions, safe areas, and absence of horizontal overflow.

## Acceptance Criteria

- ForkMesh World contains an interactive building visibly labeled `FORKMESH OFFICE`.
- The world map and accessible landmark navigation include the Office.
- Chat does not open or connect merely because the visitor approaches the building.
- An explicit click, tap, or `E` action enters the office and opens the compact console.
- The world remains visible while the office console is open.
- Desktop uses a right-side dock and mobile uses a bounded bottom sheet.
- Guests can use only World `#general`.
- Active registered users see only public and authorized private channels.
- Administrators manage channels through the full chat page rather than the office console.
- Text, clipboard images, and selected documents work and survive refresh or re-entry.
- Walking beyond the exit threshold closes the console and unloads its iframe.
- No chat content or private room identity enters the world-presence WebSocket.
- Expired sessions and relay failures produce actionable messages instead of the generic channel-request error.
- Existing full-page `/chat` behavior remains unchanged outside `embed=office`.
- Desktop, landscape, and portrait E2E snapshots pass without label collisions or overflow.

## Explicit Exclusions

Voice chat is not part of this feature.
Chat text does not appear as 3D speech bubbles above avatars.
There is no new chat protocol or world-socket message type.
The office does not add channel creation, membership mutation, or moderation controls.
The office does not infer chat presence, typing state, private-room membership, or activity from avatar location.
