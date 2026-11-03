# Mastodon

Part of [Marketplace](README.md). The `mastodon` package is a Mastodon client:
a native Qt page on the desktop (entry point `builtin:mastodon`, since 1.1.0),
native screens in the iPhone and Android apps ([On the phone](#on-the-phone)),
and a web page (`content/web/`) for the web dashboard. This page covers all
three.

## On the web

Mastodon registers a client with the HTTPS instance selected by the user and completes OAuth there. On the web its access token and client registration are kept in browser session storage, are not synchronized to ForkMesh, and are cleared by Logout or when the browser session ends. The desktop client keeps them in the package's desktop settings instead, also unsynchronized ([Signing in](#signing-in)). The package uses the standard Mastodon API for timelines, posts, profile edits, notifications, relationships, lists, and preferences.

## On the phone

Both phone apps draw the client natively. The dashboard page could not serve
them: it signs in through a redirect back to
`app.forkmesh.com/dashboard/mastodon` and keeps the token in that browser
tab's session storage, which does not survive the round trip from the app to
the browser and back. Both use package id `mastodon`, route `.mastodon` /
`Route.Mastodon`, `/dashboard/mastodon`, `forkmesh://mastodon` and the
Personal group, with the same labels and behaviour.

| | iPhone | Android |
| --- | --- | --- |
| Core (tested) | `FMPackages/Mastodon{Core,Models,Client,Model}.swift` | `core/.../mastodon/` |
| Screens | `Features/Packages/Mastodon{Screen,FeedViews,Pages}.swift` | `app/.../feature/mastodon/` |
| Tests | `FMPackagesTests/MastodonTests.swift` | `MastodonTest.kt` |

Signing in:

1. The server is typed as `mastodon.social`, `https://…` or `@user@server`;
   only HTTPS origins without a path are accepted.
2. The app registers itself (`POST /api/v1/apps`, scopes `read write follow`)
   with two redirect URIs, `forkmesh://mastodon/callback` and
   `urn:ietf:wg:oauth:2.0:oob`, then opens `/oauth/authorize` with a random
   `state` and a PKCE S256 challenge.
3. The iPhone uses `ASWebAuthenticationSession` (the system sign-in sheet
   that shares Safari's cookies), which hands the callback straight back.
   Android opens the browser; the `forkmesh://mastodon/callback` intent is
   caught before navigation, so the code never becomes a route. The pending
   sign-in (server, client, `state`, verifier) is saved first, so Android can
   finish it even if the system ended the app while the browser was open.
4. The code is exchanged at `/oauth/token` with the verifier. A callback with
   a different `state` is refused.
5. "Paste a code instead" reopens the authorize page with the out-of-band
   redirect for the user to paste the code (or the whole address). "Use an
   access token instead" takes a token made under Preferences → Development.

The sign-in (server, token, client id and secret, and the cached name and
picture) is kept in the phone's secure store (Keychain on the iPhone, the
encrypted store on Android) under `mastodon.session.v1`, scoped to the
project like other secrets. One account per phone; it is not synced to
ForkMesh. Signing out revokes the token on the server (`POST /oauth/revoke`).
Losing the ForkMesh session (sign-out or expiry) also forgets and revokes the
Mastodon sign-in, so the next account on the phone does not inherit it
([why](../native/ARCHITECTURE.md)). A 401 anywhere forgets the sign-in and returns to sign-in with the server
filled in.

Every request goes from the phone straight to the user's server; nothing
passes through ForkMesh.

What the phone covers: Home, Notifications, Local, Federated, Bookmarks and
Favourites; threads; profiles (follow, unfollow, follow requests shown as
Requested); hashtag timelines; search (people, hashtags, posts, resolving
remote addresses). Posts show boosts, content warnings, pictures (sensitive
ones hidden until tapped), read-only poll results, and reply, boost,
favourite and bookmark (applied at once and rolled back if the server
refuses), plus open in browser, copy link and delete for your own posts.
Mentions open the profile in the app, hashtags the tag timeline, other links
the browser. The composer has a content warning, visibility, up to the
server's picture limit (uploads wait for processing) and the server's
character limit, counted as Mastodon does: a link counts as 23 characters and
`@user@server` as `@user`. Replies mention the author and everyone they
mentioned except you, and keep the parent's visibility and warning. The
desktop's extras (lists, filters, profile editing, blocks, scheduled posts)
stay on the desktop and the web page.

## On the desktop

Source: `marketplace/packages/mastodon/content/desktop/`.

| File | What it holds |
| --- | --- |
| `MastodonCore.*` | Pure helpers: server address checks, the HTML sanitizer for post content, PKCE, Link-header paging, saved accounts, relative times. |
| `MastodonClient.*` | The REST client. Every request goes through the window's shared network manager, so the desktop firewall decides each destination. |
| `MastodonWidgets.*` | Post cards, account rows, media tiles, polls, drawn icons. |
| `MastodonDialogs.*` | Composer, media viewer, filter editor, report dialog. |
| `MastodonFeature.*`, `MastodonPages.cpp` | The page: sign-in, sidebar, history, actions and every section. |
| `tests/test_mastodon.cpp` | `forkmesh-mastodon-tests`, driven by an in-process fake Mastodon server. |

### Signing in

The user types their server (`mastodon.social`, `https://…` or a full
`@user@server` handle; only HTTPS origins without a path are accepted).

1. The page registers an app on that server (`POST /api/v1/apps`, scopes
   `read write follow`) with two redirect URIs: a loopback listener on
   `127.0.0.1:<random port>/callback` and `urn:ietf:wg:oauth:2.0:oob`.
2. It opens `/oauth/authorize` in the browser with a random `state` and a PKCE
   S256 challenge.
3. The browser returns to the loopback listener, which answers with a short
   "you can close this tab" page; the page checks `state` and exchanges the
   code (`POST /oauth/token` with the verifier).
4. If the browser cannot reach the listener (a local firewall), "Show me a
   code instead" reopens the authorize page with the out-of-band redirect and
   the user pastes the code (or the whole callback address) into the page.

"Use an access token instead" accepts a token made under Preferences →
Development on the server.

Accounts (server, token, client id and secret, and the cached name and
picture) are stored in the package's desktop settings
(`market/packages/mastodon/data/accounts`, like the Discord bot token), so
several accounts can be kept and switched from the sidebar. They are not synced
to ForkMesh. Signing out revokes the token on the server (`POST /oauth/revoke`)
and forgets it. A 401 from the server means the token was revoked elsewhere:
the page forgets that account and returns to sign-in with the server filled in.

### Network and the firewall

All calls go to the chosen server, so the firewall asks once per server. A
server cannot be listed in the endpoint catalog because the host is the user's
choice. Pictures from the server's own host, the host its avatars and headers
come from, and its thumbnail host prompt like other calls; pictures from any
other host are marked as decoration (`markFaviconProbe`) so they are skipped
silently instead of opening a firewall prompt per remote server, and a letter
avatar is shown instead.

The client holds the host's network manager and its in-flight replies through
`QPointer`. The replies are children of that manager, and when the main window
closes the manager is destroyed before the page, so `cancelAll()` from
`~MastodonFeature` used to call `disconnect()` on freed replies (SIGSEGV in
`Client::cancelAll`, report #23). `testClientOutlivesNetwork` deletes the
manager with requests in flight and then cancels.

### What the page covers

- Timelines: Home (with a compose prompt), Local, Federated, Bookmarks,
  Favourites, each list, hashtag timelines, threads, profiles (posts, posts and
  replies, media, pinned posts). Pages load 30 posts and continue through the
  `Link` header when the list is scrolled to the end.
- Posts: reply, boost, favourite, bookmark, and a menu for open in browser,
  copy link, translate, edit, pin, mute conversation, delete, delete and
  redraft, mention, mute, block, block server and report. Content warnings,
  sensitive media, polls (voting and results), link cards and server filters
  (warn shows a "Show anyway" row, hide drops the post) are honoured.
- Composer: content warning, visibility, language, up to the server's
  attachment limit with alt text (uploads wait for processing), polls,
  scheduling, the server's character limit (links count as 23), replies that
  pre-fill mentions, and editing with the post's source text.
- Notifications with type tabs, dismiss, clear all, Follow back and follow
  request answers; opening the page moves the server's read marker, and the
  rail badge counts notifications newer than that marker (checked at most once
  a minute).
- Account: profile editor (name, bio, four fields, picture, header, approval,
  bot, discoverable, indexable, hidden follower lists), featured hashtags and
  featured profiles; followers, following, requests and suggestions; blocks,
  mutes and blocked servers; lists (create, rename, replies policy, exclusive,
  members); followed hashtags; filters (v2, keywords, contexts, action,
  expiry); scheduled posts (reschedule, cancel); posting defaults; and the
  accounts on this computer.
- Search (people, hashtags, posts, resolving remote addresses) from the
  sidebar field or the window's top search box, which also finds loaded posts
  and pages.

Mastodon has no API for e-mail, password, two-factor, export, migration or
account deletion; Preferences links to those pages on the server.

The page is one layout used by both layout modes (Control Tower and Focus):
a fixed sidebar and a centred column at most 720 px wide.

### Tests

```sh
cmake --build desktop/build --target forkmesh-mastodon-tests
desktop/build/forkmesh-mastodon-tests
```

The fake server records every request, so the test checks request bodies
(post, profile form, filters, token exchange) as well as what the page shows,
and that nothing is sent anywhere but the chosen server.

```sh
cd native/ios/ForkMeshCore && FM_ONLY=FMPackages swift test --filter Mastodon
cd native/android && ./gradlew :core:coreTest -Pfilter=Mastodon
```

The phone tests check server parsing, PKCE (the RFC 7636 example), the
callback `state` check, the character count, the HTML-to-text conversion of
post content (mentions, hashtags, shortened links), Link-header paging, and,
against a fake server, every request the sign-in, timelines, toggles,
composer, uploads and follow buttons send.
