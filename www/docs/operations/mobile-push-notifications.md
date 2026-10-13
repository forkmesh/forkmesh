# Mobile push notifications (APNs)

The iOS app is woken by Apple Push Notification service when iOS has
suspended or killed it, which is the one case the account event socket cannot
cover. Everything else about account events stays as documented in
[polling elimination](polling-elimination.md): a write that moves an unread
count calls `notify_account_event`, which fans a payload-free frame to the
owner's `ForkMeshNodes` Durable Object and, for the topics below, one
payload-free alert to every phone registered for that account.

There is no Firebase and no third-party push provider. The Worker signs its
own APNs provider token and posts to Apple directly (`app/src/apns.py`). On
the phone, the iOS app uses
`native/ios/ForkMesh/Platform/ApplePushBridge.swift` and the extension
`native/ios/NotificationService`. The extension adds the portrait and badge.

## What Apple sees

Every alert names its event; there is no generic "New activity" banner, and a
push with nothing specific to say is not sent at all. A ping carries its
headline, repository, actor and body. A direct message names its sender and a
channel message its channel; their text is sealed end to end, so the body says
it is encrypted instead. The push test says it is a test. Device tokens are
stored like every other sensitive value: the row key is a blind index of the
token and the token itself is encrypted in `data`.

## What the user sees

The Worker renders a ping's alert from the notification row it just wrote
(`_apns_ping_alert` → `_push_alert_item` in `app/src/entry.py`), so the banner
reads right even if nothing on the phone runs:

| Line | Contents |
| --- | --- |
| Title | The record and its headline — `#42 Issue submitted`. |
| Subtitle | `owner/repo · by actor`, with the organization's public name when an alias fronts the repository. |
| Body | The notification body in full, up to 900 characters. |
| Image | The actor's avatar, as an attachment thumbnail (added on the phone). |
| Badge | The account's unread count (added on the phone). |

Banners are grouped per repository (`thread-id`). Every alert sets
`mutable-content`, so iOS runs the bundled notification service extension
before drawing the banner; it reads `GET /api/push/alert` with the signed-in
session and, for the row the push names (`alertId`), adds the actor's portrait
and the unread badge — neither fits Apple's 4 KB payload. If that read fails,
the banner keeps the text the Worker sent.

That route is the alert inbox rendered for a lock screen: unread rows only,
newest first, behind the same gate as `GET /api/notifications` — a session
bearer token or an account-key signature, for the caller's own account only.
Composition lives in `push_alert_handler` / `_push_alert_item` in
`app/src/entry.py`, on the same helpers the digest email uses, so a phone, an
inbox row and a digest describe one event identically.

The session travels between the app and the extension through the keychain
group `$(AppIdentifierPrefix)com.forkmesh.forkmesh`, which both targets'
entitlements list. Two things about that are load-bearing:

* The **Keychain Sharing** capability must be enabled for both App IDs
  (`com.forkmesh.forkmesh` and `com.forkmesh.forkmesh.NotificationService`)
  in Apple Developer → Identifiers, or the extension reads an empty keychain
  and every banner silently stays generic.
* The item is stored `AfterFirstUnlock`, because the common case for a push
  is a phone sitting locked; `WhenUnlocked` would fail those reads.

The app writes it on every successful device registration and clears it on
sign-out. Anything that stops the read — signed out, offline inside the
extension, an older build with no extension — falls back to the generic alert
above rather than to no banner.

Chat and direct messages are the exception: the relay stores ciphertext and
can describe a message no better than Apple can, so the phone does not even
ask — those banners read "New message" and "New direct message". It knows
which is which because the alert for those two topics names its topic beside
`aps`, and only those two: they are exactly the topics that already carry an
`apns-collapse-id` spelling the same category out to Apple in a header, so the
mark discloses nothing further.

Topics that wake a phone (`APNS_PUSH_TOPICS` in `app/src/apns.py`): `pings`
(the alert inbox), `code-push`, `direct-messages`, and `chat` (a message in a
channel or public room). `catalog` and `issues` are UI freshness hints and
never buzz: an issue event the user should hear about raises a ping of its
own, and that ping carries the text.

`code-push` is a ping (kind `code_push`, raised by `notify_code_push` in
`app/src/entry.py`) sent under its own topic so the phone can tell a push
banner from an inbox one. Every observed push raises it: an owner-signed
catalog publication whose refs moved, or a receive-pack to a hosted
repository. It reads "Pushed to owner/repo" and links to
`/owner/repo/commits/<branch>`. Recipients are the hosting account and
organization members with read access or better; an identified pusher who is
not the owner is skipped. A storm guard allows one banner per recipient per
repository branch per 60 seconds; later pushes fold into the unread ping
without buzzing. The **Code pushes** ping setting (default on) turns it off.

Every ping is its own alert. The socket frame only means "re-read your inbox",
so frames for one account coalesce inside a two-second window; the APNs alert
for `pings` and `code-push` deliberately does not, and carries no collapse id,
so two pings are two notifications. `direct-messages` and `chat` collapse on
the phone, so a conversation in full flow is one banner rather than a stack.

Every payload carries `origin` (the Worker's `APP_ORIGIN`) beside the topic,
`alertId` and `href`, so a phone signed in to several ForkMesh projects (for
example production and ForkMesh Next) routes the tap to the right one.

## Chat messages

The relay only ever sees ciphertext, so it cannot tell a message from a
retained edit, a reaction, or the key advertisement a desktop sends on every
connect. The sender says so instead: a client sets a plaintext `notify: true`
on the envelope of a `chat` or `thread-reply` frame, and `ForkMeshRoom` wakes
phones for retained frames that carry it and for nothing else. A client that
predates the field still chats normally; its messages just do not buzz anyone.

The audience comes from the room, not the message:

| Room | Who is woken |
| --- | --- |
| Direct message | The other participant (`direct-messages`). |
| Private channel | Its creator and members, minus the sender. |
| Public channel | Every registered phone, minus the sender. |
| Public room (`world-general`, repository rooms) | Every registered phone. |

A public room's socket is anonymous, so the relay cannot leave the sender's
own phone out. The alert for room chat therefore carries `"topic": "chat"`
beside `aps` (a direct message carries `"topic": "direct-messages"` the same
way), and the app does not present chat while it is in the foreground, where
the live socket has already shown the message. At most 32 phones are
contacted per message, from a device list cached for a minute per isolate.

## Secrets the Worker needs

`push_account_apns` is a silent no-op unless all three are set on the App
Worker. `./deploy.sh app` and `./deploy.sh secrets` refuse to run without
them; the deploying control node writes them into `app/.env.production`.

| Secret | Value |
| --- | --- |
| `APNS_AUTH_KEY` | The `.p8` key downloaded from Apple Developer → Certificates, Identifiers & Profiles → Keys, created with the Apple Push Notifications service enabled. Paste it with its `-----BEGIN PRIVATE KEY-----` armour; literal `\n` sequences are accepted. Apple lets you download it exactly once. |  <!-- forkmesh-secret-scan:ignore-line -->
| `APNS_KEY_ID` | The ten-character Key ID shown next to that key. |
| `APNS_TEAM_ID` | The ten-character Team ID of the developer account (`742ZANCB57` for the store build). |

The App ID `com.forkmesh.forkmesh` must have the Push Notifications
capability, and the key must belong to the same team. A key that imports but
does not match the key id answers `InvalidProviderToken`; a key from another
team answers `DeviceTokenNotForTopic`.

## Sandbox versus production

Apple runs two gateways, and a token is only valid against the one that
minted it. Sending to the wrong one is a hard `400 BadDeviceToken`, after which
the Worker prunes the token and the phone re-registers on its next launch.

| Build | Entitlement | Gateway the token belongs to |
| --- | --- | --- |
| Debug or Profile on a device | `aps-environment = development` | `api.sandbox.push.apple.com` |
| Simulator (Apple silicon, iOS 16+) | none embedded | `api.sandbox.push.apple.com` |
| Release, TestFlight, App Store | `aps-environment = production` | `api.push.apple.com` |

The app reports the environment with its token (`environment` in the
`register-device` body) and the Worker routes each token to the matching
gateway, so both kinds can be registered for one account at the same time.

## Proving it works

Settings → Notifications on the phone shows each link of the chain
(permission, token and gateway, relay credentials) and has **Send test
notification**, which calls `POST /api/accounts/push-test` with the session
and shows the result. The same route can be called by hand:

```bash
curl -s -X POST https://app.forkmesh.com/api/accounts/push-test \
  -H "Authorization: Bearer $SESSION_TOKEN" -H "User-Agent: ForkMeshMCP/0.1"
```

It answers with `configured`, `devices`, `delivered`, and one `results` entry
per registered device carrying Apple's status and reason. It sends a fixed
"ForkMesh test notification" alert, is limited to one call per account every ten
seconds, and is bound to the session's account exactly like registration.

A real ping that arrives with its text but without the actor's portrait
means the push chain is fine and the extension's read is not: check that the
build embeds the extension, that Keychain Sharing is on for both App IDs, and
that `GET /api/push/alert` answers 200 for the session.

| Answer | Meaning | Fix |
| --- | --- | --- |
| `configured: false`, `configProblem: missing_secrets` | One of the three secrets is missing on the Worker. | Set them and redeploy. |
| `configured: false`, `configProblem: key_id_malformed` or `team_id_malformed` | `APNS_KEY_ID` or `APNS_TEAM_ID` is not ten characters of `A-Z0-9`. Apple would refuse every push with `403 InvalidProviderToken`, so the Worker does not try. `./deploy.sh secrets` prints a warning for the same condition. | Copy the Key ID from Apple Developer → Keys (it is also the `XXXXXXXXXX` in the downloaded `AuthKey_XXXXXXXXXX.p8` file name) and the Team ID from Membership details, then run `./deploy.sh secrets`. |
| `devices: 0` | The phone never registered a token. | Allow notifications on the phone, or sign in again; the settings card shows why registration stopped. |
| `status: 400`, `reason: BadDeviceToken` | The token belongs to the other gateway. | Rare after the Simulator fix; reopen the app so it re-registers with the right environment. |
| `status: 400`, `reason: DeviceTokenNotForTopic` or `TopicDisallowed` | Key or bundle id mismatch. | Check the key's team and that the App ID has push enabled. |
| `status: 403`, `reason: InvalidProviderToken` or `ExpiredProviderToken` | `APNS_KEY_ID` or `APNS_TEAM_ID` does not match the key. | Correct them; the Worker re-mints its provider token on the next send. |
| `status: 410`, `reason: Unregistered` | The app was uninstalled. | Nothing; the row was pruned. |
| `status: 0` | Apple's gateway was not reached. | Retry; see the local-development note below. |
| HTTP `502`, `error: apns_provider_key` | The key is present but will not import or sign. | Re-paste the `.p8` with its armour intact. |

The register-device reply also carries `pushConfigured` and `configProblem`,
so the settings card can say "No APNs credentials configured" straight after
sign-in rather than waiting for a test.

## Android

There is no Android push transport. Android delivery would need either
Firebase Cloud Messaging, which this project has not taken on, or a foreground
service holding the account event socket. The native Android app
reports push as unsupported and receive events only while they are open.

## Limits

Delivery is best effort and runs inline on the request that produced the
event, since Python Workers have no `waitUntil` here. Apart from pings, one
account event buzzes each phone at most once per two-second coalescing
window. At most eight devices are contacted per event within a three-second
budget, at most
twenty tokens are kept per account, and a token that has not checked in for
180 days is dropped. Apple holds an alert for a phone it cannot reach for 24
hours, and only the newest one per app, so a phone that was offline overnight
gets one banner when it reconnects. Apple's provider token is cached for forty minutes per
isolate and re-minted early when Apple reports it expired.

## Local development

APNs only speaks HTTP/2. Production Workers reach it, but local `workerd` on
macOS does not negotiate HTTP/2 for outbound fetches
([cloudflare/workerd#4841](https://github.com/cloudflare/workerd/issues/4841)),
so a self-test against `wrangler dev` reports `status: 0` for every device.
Registration, routing, and the `configured` answer can still be exercised
locally; delivery has to be checked against a deployed Worker.
