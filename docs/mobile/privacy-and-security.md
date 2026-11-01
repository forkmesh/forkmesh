# Privacy and security on the phone

Where your keys live on a phone, what is end-to-end encrypted, what the
ForkMesh servers can still see, and what to do if a phone is lost. The full
picture for every app is in [encryption and signatures](../encryption.md).
Back to the [phone apps](README.md).

## Your keys stay on the phone

Each phone makes its own Ed25519 key when you sign in, and that key never
leaves it.

| | Platform | Where the key lives |
| --- | --- | --- |
| 📱 | iPhone and iPad | The Keychain, in a private group no app extension can read, marked this-device-only so it is not copied to a backup or another phone |
| 🤖 | Android | The Android Keystore |

The key signs what the phone does (commits, pull requests, merges) as a node
of your account ([mobile node contract](mobile-node-contract.md)). Destructive
actions (deleting or archiving a repository, revoking devices, changing your
password or security settings) and approvals and merges ask for Face ID,
Touch ID or the device passcode first.

## What is encrypted end to end

```text
   phone ══════════ sealed ═══════════► your other devices
     │                                        ▲
     └──► ForkMesh relay ─────────────────────┘
          stores and forwards ciphertext;
          sees who, when, and how much
```

| | What | Who can read it |
| --- | --- | --- |
| 🤖 | Agent sessions and transcripts | Your own enrolled devices only; the relay stores ciphertext |
| 🎛️ | Desktop control traffic | The phone and your desktop; the relay sees only routing |
| 💬 | Direct messages, channels and group calls | The people in the conversation ([safety numbers](../chat-safety-numbers.md)) |
| 🔔 | Chat text in notifications | Decrypted on the phone |
| 🔑 | Password Vault | You; the server keeps ciphertext |

## What the servers can see

- Who talks to whom, when and how much (metadata).
- Public channels, which are not end-to-end encrypted.
- Prompts you send to an agent on a mirror or runner, which use a queue the
  relay can read, as the web app's do.
- The text of pings (alerts), which are not sealed.

The RSS Reader is local only: nothing it reads reaches the server.

## If you lose a phone

1. On the web, open the devices list in your account settings.
2. Revoke the lost phone, or use **revoke all** to remove every other device
   at once. Both ask for your password and authenticator code.
3. The phone's key stops being able to sign and its sessions end at once;
   your desktop drops it within ten minutes.

```text
   revoke ──► key refused (device_revoked) ──► desktop drops it within 10 min
```

Revocation is not retroactive: a phone can still read agent sessions that were
sealed to it before you revoked it. A new sign-in on any device sends a "new
node added" notice to your others, so check those notices.

## Permissions

The app asks for each system permission only when a feature first needs it
([getting started](getting-started.md#3-permissions-only-when-they-matter)).
The Market's phone security checkup reviews the phone's own settings
([phone security](../marketplace/phone-security.md)).

Related: [encryption and signatures](../encryption.md) ·
[agent access](mobile-agent-access.md) ·
[threat model](../protocol/threat-model.md) · [phone apps](README.md)
