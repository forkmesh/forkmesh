# FMMiniApps

Mini-apps: the owner's desktop background services ([service plug-ins](../../../docs/desktop/service-plugins.md))
shown on the phone as native SwiftUI cards. `import FMMiniApps` (with `FMControl` for the link
and `FMBase` for JSON). The screens are `ForkMesh/Features/MiniApps/` (Desktop → Background
services).

## Why it is shaped this way

- **No resident daemon on iOS.** The phone never runs a service. The services run in
  `forkmesh-core` on the desktop, which keeps working while the desktop window is closed and can
  start at login. The phone is a client that can be suspended at any moment.
- **Data, not code.** A mini-app is a `MiniAppCard` (title, rows, actions) returned by the
  plug-in's `card` method and drawn by the app's own SwiftUI. Nothing downloaded is executed, so
  App Store rule 2.5.2 holds, and a plug-in can only ask the phone to call one of its own
  `remoteMethods`.
- **Owner only.** The core answers `plugins.list` and `plugin.<id>.<method>` from a phone only for
  the desktop's owner and only for methods the manifest lists in `remoteMethods`.

## Contract (`MiniAppContract`)

| Name | Value |
| --- | --- |
| Core protocol | 2 (`ipc.Protocol`) |
| Plug-in API | 1 |
| Catalog | `plugins.list` → `{plugins: [{id, name, version, summary, methods, events, running}]}` |
| Call | `plugin.<id>.<method>`; the card is `plugin.<id>.card` |
| Events | `plugin.<id>.<event>` for the plug-in's `remoteEvents` |
| Permanent errors | `forbidden`, `not_owner`, `unknown_method`, `unknown_plugin`, `bad_params`, `plugin_invalid`, `plugin_untrusted`, `plugin_disabled`, `plugin_needs_consent`, `plugin_failed` |

`MiniAppCard(object, for: descriptor)` bounds what a desktop can send: 50 rows (labels 80
characters, values 400), 6 actions, titles 40 characters, `refreshSeconds` at least 30, and
drops actions whose method the descriptor does not list or that call `card` itself.

## Living with iOS background limits

| Situation | What happens |
| --- | --- |
| Desktop reachable (`MiniAppReach.live`, LAN or relay) | Calls go straight through; the card is cached in prefs (`miniapps/card/<id>`) |
| Desktop away (`.desktopAway`) | Screens show the cached card and its age; actions become `MiniAppIntent`s in the `MiniAppOutbox` (prefs `miniapps/outbox`, 100 at most, 24 h each) |
| Link comes back | `MiniAppsFeature` flushes the outbox in order: sent intents leave, permanent errors drop them, a transient error keeps the rest for later (5 tries each) |
| App in the background | `BGAppRefreshTask` `com.forkmesh.miniapps.refresh`, no sooner than 15 minutes (or the shortest card `refreshSeconds` above that), only when a card refreshes or intents wait; each wake flushes the outbox and refreshes cards within the system's budget |
| Something happened on the desktop | `remoteEvents` reach a connected phone live; when the app is suspended, the existing push path (`FMPush`) is how the desktop reaches it |

`Config/ForkMesh-Info.plist` declares the task identifier and the `fetch` background mode;
`AppDelegate` registers the handler at launch (iOS requires registration before launch ends).

## API

```swift
let client = MiniAppClient(store: settings.prefs, link: { hub.currentLink })
let apps = try await client.catalog()
let card = try await client.card(for: app)
switch try await client.perform(card.actions[0], on: app) {
case .done(let result): …
case .queued(let id): …
}
let report = await client.flushOutbox()
let token = client.observe(app) { event, data in … }
```

Tests: `FM_ONLY=FMMiniApps swift test` in `ForkMeshCore` (runs on Linux).
