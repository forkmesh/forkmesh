# Safety numbers and trusted devices

Direct messages are end-to-end encrypted to each device of the two people in
the conversation. Every browser, phone and desktop you sign in on has its own
chat key. A safety number lets you check that nobody else's key has slipped
into the conversation, and new devices wait for your approval before they get
anything.

Approvals and safety numbers are for direct messages. Channels accept every
correctly signed device on their own; see
[Chat encryption and integrations](security/sealed-chat.md).

## What a safety number is

A safety number is 60 digits in 12 groups of five, plus a QR code. You and the
other person see the same number, and it covers every approved device on both
sides. To find it:

- **Web.** Press the lock button ("End-to-end") in the conversation header.
  The Chat encryption panel shows the number under **Safety number**.
- **iPhone.** Open the conversation's details and tap **Safety number**. It
  shows the digits, your QR code and **Scan their code**.
- **Android.** Open **Safety number** from the conversation's menu.

Compare the digits in person or over a call you trust, or scan the other
phone's code. Then press **Mark as verified**. A scan tells you whether it
matched, or whether you scanned the wrong code.

## When it changes

The number changes whenever the set of approved devices changes: someone
approves a new phone or browser, or a device gets a new key. The change clears
"verified", and the conversation says "Your safety number with Alex changed".
If nobody you know added a device, compare the number again before you share
anything sensitive: a key nobody expected can mean someone is trying to read
along.

## New devices wait for you

In a direct message, the first device you see for a person is trusted. Every
further device for that person, including a new device of your own, waits.
It gets nothing sealed to it and its messages are held, unseen, until you
decide. The conversation shows "A new device for Alex wants to join this chat.
Check the safety number, then approve or block." and a banner with
**Safety number**, **Approve** and **Block**.

- **Approve** lets it receive new messages and opens the ones it sent while it
  waited (the most recent ones, if it sent many).
- **Block** keeps it out.
- A new device reads only messages sealed after it was approved, never the
  older history.

When several devices are waiting at once, for example after someone signs in
on a new phone and a new browser, the iPhone shows one banner for all of them
("2 new devices for Alex want to join this chat"), and Approve and Block act
on all of them. The web shows one device at a time and says how many are
waiting. On Android the single banner is still being ported.

## Reinstalling on iPhone

Your iPhone keeps a projects folder, by default in iCloud Drive, that survives
deleting the app. It holds your identity key, encrypted with your passphrase.
When you reinstall ForkMesh and press **Restore**, the app rebuilds your chat
key from that identity key. Your safety numbers stay the same, the people you
chat with see no new device, and a conversation you had verified stays
verified.

- This works only when the folder outlives the app. If the folder sits inside
  the app (On My iPhone › ForkMesh), it is deleted with the app, and ForkMesh
  warns you at every launch with "Keep your ForkMesh data safe" until you move
  it.
- If your iPhone set up chat before this was added (October 2026), it keeps
  its older chat key until the next reinstall. That one reinstall changes your
  safety numbers once; after it they survive.
- Restoring the key does not unlock old messages: the keys that opened them
  are not part of it.
- On Android, keeping the chat key through a reinstall is still being
  ported.

Related: [Voice and video calls](calls.md) has verification codes for group
calls, and [Portable identities and recovery](portable-identities.md) covers
your identity key.
