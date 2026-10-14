# Mobile push notifications (APNs)

Apple Push Notification service wakes the iOS app when iOS has suspended or
killed it, the one case the account event socket cannot cover (see
[polling elimination](polling-elimination.md)). A write that changes an unread
count calls `notify_account_event`, which sends a payload-free socket frame
and, for the topics below, one alert to every phone registered to the account.

There is no third-party push provider: the server signs its own APNs provider
token and posts to Apple directly (`app/src/apns.py`). On the phone,
`native/ios/ForkMesh/Platform/ApplePushBridge.swift` registers the device and
the `native/ios/NotificationService` extension enriches each banner.

## Payloads

Every alert names its event; nothing generic is sent. Device tokens are stored
encrypted under a blind-index row key.

| Line | Contents |
| --- | --- |
| Title | Record and headline, e.g. `#42 Issue submitted` |
| Subtitle | `owner/repo · by actor` |
| Body | The notification body, up to 900 characters |
| Image, badge | Actor avatar and unread count, added on the phone |

Alerts set `mutable-content` and `thread-id` (per repository or conversation).
The extension reads `GET /api/push/alert` (unread rows, own account only) with
the session shared through the app's keychain group, stored
`AfterFirstUnlock`; Keychain Sharing must be enabled on both App IDs. If the
read fails, the banner keeps the server text.

Chat and direct-message text is end-to-end encrypted: the alert carries only
the relay `ref`, `keyVersion`, and `channelId`, and the extension fetches and
decrypts the ciphertext on the phone, falling back to "End-to-end encrypted".
Every payload includes `origin`, `alertId`, and `href` so a phone signed in to
several deployments routes taps correctly.

## Topics

`APNS_PUSH_TOPICS`: `pings`, `code-push`, `direct-messages`, and `chat`.
`catalog` and `issues` are freshness hints and never alert.

- Each ping and each direct message is its own banner (no collapse id).
- `code-push` (kind `code_push`) fires on observed pushes for readers of the
  repository, at most one banner per recipient per branch per 60 seconds, and
  can be turned off in ping settings.
- Chat alerts require the sender to set plaintext `notify: true` on a `chat` or
  `thread-reply` envelope. Audience: the other participant of a direct message,
  members of a private channel, or every registered phone for public channels
  and rooms, minus the sender where known; at most 32 phones per message and
  one push per room every two seconds.

## Configuration

`push_account_apns` is a no-op unless all three secrets are set; deploy refuses
to run without them.

| Secret | Value |
| --- | --- |
| `APNS_AUTH_KEY` | The `.p8` APNs key with its PEM armour (literal `\n` accepted). |
| `APNS_KEY_ID` | The ten-character Key ID. |
| `APNS_TEAM_ID` | The ten-character developer Team ID. |

The App ID needs the Push Notifications capability and the key must belong to
the same team. Debug builds and the Simulator register sandbox tokens; release
builds register production tokens. The app sends `environment` with its token
and the server routes each token to the matching gateway.

## Verification

Settings → Notifications on the phone shows each link of the chain and offers
**Send test notification**, which calls `POST /api/accounts/push-test` (one call
per ten seconds). It returns `configured`, `devices`, `delivered`, and Apple's
status per device:

| Answer | Fix |
| --- | --- |
| `configProblem: missing_secrets` | Set the three secrets and redeploy. |
| `key_id_malformed` / `team_id_malformed` | Copy the ten-character IDs exactly. |
| `devices: 0` | Allow notifications or sign in again on the phone. |
| `400 BadDeviceToken` | Token from the other gateway; reopen the app to re-register. |
| `DeviceTokenNotForTopic` / `TopicDisallowed` | Key team or App ID mismatch. |
| `403 InvalidProviderToken` | Key ID or Team ID does not match the key. |
| `410 Unregistered` | App uninstalled; the token is pruned. |
| HTTP `502 apns_provider_key` | The key does not import; re-paste it. |

## Limits

Delivery is best effort and inline with the triggering request: at most eight
devices per event within three seconds, twenty tokens per account, and tokens
idle for 180 days are dropped. Android has no push transport; the native
Android app reports push as unsupported. Local `workerd` cannot speak HTTP/2 to
APNs, so delivery must be tested against a deployed server.
