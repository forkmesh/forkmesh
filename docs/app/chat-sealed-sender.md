# Sealed sender for direct messages (proposed)

Status: proposed 2026-10-16, not implemented. Chat protocol v4
([chat-protocol-v4.md](chat-protocol-v4.md)) closed the first gap with Signal
(per-message forward secrecy). This page is the plan for the second: Signal's
server does not learn who sent a message, while ours knows both accounts of
every DM.

## What the server learns today

| Fact | Where |
| --- | --- |
| The two accounts of every DM | `chat_direct_conversations`, `chat_direct_participants` (`pair_bi` unique per pair) |
| Who sent each frame | the DM socket is opened with a ticket naming the account; `chat_history.msg_id` is `<ref>:<sender account_bi[:16]>` |
| Which device sent it, and to which devices | the v3/v4 frame's clear `kid`, `idk` and `keys[].kid`, and the `acct` in each bundle |
| Who to push, remind and count unread for | "the other participant" of the conversation |

Hiding the sender of one frame is worth nothing while the conversation is a
server record of two accounts: the sender is whichever one is not the
recipient. Sealed sender only means something together with removing the
pair from the server, which is what Signal does: its server knows the
recipient of each message, never the conversation.

## Design

1. **Device directory with sender certificates.** Each device registers its
   v3 bundle once per account (`POST /api/chat/devices`, authenticated). The
   server answers `GET /api/chat/devices/<username>` with that account's
   bundles, and gives each device a short-lived certificate
   `{acct, kid, idk, exp}` signed by a server chat key. The certificate rides
   inside the encryption, so a recipient knows the sender is really that
   account without the server knowing who sent.
2. **Mailboxes instead of conversation rooms.** A DM frame is posted to the
   recipient account's mailbox over a connection that carries no session and
   no ticket, authorised by an access key derived from the recipient's
   profile key (shared end to end with people they talk to). The server
   stores `(recipient, ref, ts, body)` and pushes "New message" to the
   recipient. Copies to the sender's own other devices go to the sender's
   mailbox the same way.
3. **A sealed-sender envelope around the v4 frame.** The frame (sender `kid`,
   `idk`, certificate, signature, counter) is encrypted to the recipient
   device with a fresh X25519 + ML-KEM exchange; the outside shows only the
   recipient mailbox and the recipient device ids.
4. **Conversations live on the devices.** The DM list is built from local
   history; a new device of the same account gets it from an approved device
   of that account (a sealed sync message), as Signal's linked devices do.
5. **Receipts and typing become end-to-end messages.** Delivered, read,
   view-once state and typing are sealed DM frames; the server no longer
   produces `forkmesh-delivered` / `forkmesh-read` for DMs.
6. **What stays at the server:** the recipient of each message, its time and
   size, and the connecting IP. Unread counts, reminders and the "N messages
   on the server" count become per mailbox.

## Decisions needed before building

- **New web logins.** A browser that signs in starts with no DMs until one
  of the account's approved devices syncs the list (Signal's model), or the
  server keeps the list encrypted to the account's devices. The second needs
  an account key the web does not have yet.
- **Who may message you anonymously.** Only people holding your access key
  (Signal's default; a first message from a stranger is sent with the
  sender's identity visible), or anyone (more spam exposure).
- **The certificate trust root.** Clients pin the server chat key on first
  use; a change shows a warning, like a safety-number change.
- **Calls, view-once media and reminder emails** move to mailbox
  addressing in the same release; every client and the server ship
  together, as v3 did.

The size is that of v3 again: the server DM routes and tables, and the DM
transport of the web, desktop, iOS and Android clients.
