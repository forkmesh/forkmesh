# Usage, Feed, and Mastodon packages

The Usage package reads only the current, already-authenticated provider usage snapshots from desktops linked to the signed-in ForkMesh account. It does not receive provider credentials or agent transcripts.

Each package has a desktop native registration that opens its authenticated dashboard route. iOS and Android discover the same dashboard contribution as a web surface and open that route from the installed package; there is no unsupported fake native destination. This keeps one complete responsive implementation across desktop, mobile, and web. Mastodon authentication remains scoped to that device browser session.

Feed projects public ForkMesh profile updates alongside the existing signature-verified ActivityPub mention stream from World. It excludes private profiles and repositories and never turns a social mention into work automatically.

Mastodon registers a client with the HTTPS instance selected by the user and completes OAuth there. Its access token and client registration are kept in browser session storage, are not synchronized to ForkMesh, and are cleared by Logout or when the browser session ends. The package uses the standard Mastodon API for timelines, posts, profile edits, notifications, relationships, lists, and preferences.
