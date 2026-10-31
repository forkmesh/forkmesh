# Mastodon on the desktop

The `mastodon` package (entry point `builtin:mastodon`) is a native Qt Mastodon
client on the desktop since 1.1.0. Before that the desktop entry only opened
the web dashboard page, which keeps its token in one browser session. The web
page (`content/web/`) is unchanged and still serves the web and phone apps.

Source: `marketplace/packages/mastodon/content/desktop/`.

| File | What it holds |
| --- | --- |
| `MastodonCore.*` | Pure helpers: server address checks, the HTML sanitizer for post content, PKCE, Link-header paging, saved accounts, relative times. |
| `MastodonClient.*` | The REST client. Every request goes through the window's shared network manager, so the desktop firewall decides each destination. |
| `MastodonWidgets.*` | Post cards, account rows, media tiles, polls, drawn icons. |
| `MastodonDialogs.*` | Composer, media viewer, filter editor, report dialog. |
| `MastodonFeature.*`, `MastodonPages.cpp` | The page: sign-in, sidebar, history, actions and every section. |
| `tests/test_mastodon.cpp` | `forkmesh-mastodon-tests`, driven by an in-process fake Mastodon server. |

## Signing in

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

## Network and the firewall

All calls go to the chosen server, so the firewall asks once per server. A
server cannot be listed in the endpoint catalog because the host is the user's
choice. Pictures from the server's own host, the host its avatars and headers
come from, and its thumbnail host prompt like other calls; pictures from any
other host are marked as decoration (`markFaviconProbe`) so they are skipped
silently instead of opening a firewall prompt per remote server, and a letter
avatar is shown instead.

## What the page covers

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

## Tests

```sh
cmake --build desktop/build --target forkmesh-mastodon-tests
desktop/build/forkmesh-mastodon-tests
```

The fake server records every request, so the test checks request bodies
(post, profile form, filters, token exchange) as well as what the page shows,
and that nothing is sent anywhere but the chosen server.
