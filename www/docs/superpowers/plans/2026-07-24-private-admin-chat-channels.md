# Private Admin Chat Channels Implementation Plan

> **Historical security note (August 28, 2026):** This plan records the first
> relay-derived private-channel design. The shipped protocol now adds automatic
> X25519 recipient encryption to private channels and direct messages inside
> that outer room cipher. Public channels remain controller-readable for
> integrations. See [`security/sealed-chat.md`](../../security/sealed-chat.md)
> for the current boundary and its active-controller limitations.

**Goal:** private encrypted chat channels that platform administrators create
and manage. Invited registered users get access immediately, everyone else is
excluded, and every chat surface can send encrypted images and documents that
survive a page refresh.

**Architecture:** a focused API module owns channel and membership policy over
encrypted database records; the relay supplies sessions, encryption, audit and
room services. Each channel and key version gets its own relay-derived
passphrase and room namespace, and a short-lived signed ticket gates the
WebSocket upgrade. The `/chat` client lists only server-authorized private
channels next to the public `#general` room.

The design, API and acceptance criteria are in the
[design spec](../specs/2026-07-24-private-admin-chat-channels-design.md).
