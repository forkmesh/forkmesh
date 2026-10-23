# Feed

`marketplace/packages/instance-feed` is a Mastodon-style timeline for the
people on one ForkMesh instance (the local timeline, in Mastodon terms). Each
account installs it for itself; everyone on the instance with it installed
reads the same feed. It does not federate: the network-wide ActivityPub view
is the separate `feed` package
([../marketplace-usage-feed-mastodon.md](../marketplace-usage-feed-mastodon.md)).

Every surface (Market, web page, desktop rail, iOS and Android) calls it just
Feed, at the user's request on 2026-10-19; it was Instance Feed. The package
id, the `/api/instance-feed` routes, `/dashboard/instance-feed`, the tables
and the request proof keep the old name so existing installs and data carry
over. The ActivityPub `feed` package was renamed Network Feed in the same
change so the Market and the rail never show two packages called Feed.

## Code map

| Path | Responsibility |
| --- | --- |
| `content/worker/instance_feed.py` | Worker API at `/api/instance-feed`, schema, account data hooks. |
| `content/web/view.html`, `content/web/page.js` | Dashboard page `/dashboard/instance-feed` (page id `instance-feed`). |
| `content/www/post.html`, `post.js`, `post.css` | Public post page `forkmesh.com/feed?post=<id>` (`www.page` contribution `public-post`). |
| `content/desktop/InstanceFeedFeature.{h,cpp}` | Native desktop page (`builtin:instance-feed`, `comment` rail icon). |
| `content/desktop/tests/test_instance_feed_feature.cpp` | Qt test for the desktop page, run against a stub host. |
| `tests/test_instance_feed.py` | Worker suite, including install and uninstall through `plugin_data`. |

## API

All routes but `public/<id>` need a signed-in session; writes also need a
same-origin request (or a bearer token). An account without the package installed gets
`{"installed": false}` from `GET /api/instance-feed` and `409
plugin_not_installed` everywhere else.

| Route | Purpose |
| --- | --- |
| `GET /api/instance-feed?before=&limit=&author=` | Timeline, newest first, 20 per page (at most 40). Without `author` it shows top-level posts and boosts; with `author` it shows that person's posts, replies and boosts plus a `profile` summary. `next` is the cursor (`<createdAt>-<id>`) for the following page. |
| `POST /api/instance-feed/posts` | New post: `text` (1–500 characters), optional `spoiler` (content warning, 100) and `replyTo`. |
| `GET /api/instance-feed/posts/<id>` | Thread: `ancestors`, `post`, `replies` (depth-first, at most 200). A boost id resolves to its original. |
| `GET /api/instance-feed/public/<id>` | The same thread with no session and no install check, for the public post page. The viewer-only fields (`boosted`, `favourited`, `mine`, `canDelete`) are left out. |
| `DELETE /api/instance-feed/posts/<id>` | Delete your own post or boost; instance admins may delete any post. Its favourites and boosts go with it; replies stay and show "Replying to a removed post". |
| `POST`/`DELETE /api/instance-feed/posts/<id>/favourite` | Favourite or unfavourite. |
| `POST`/`DELETE /api/instance-feed/posts/<id>/boost` | Boost or undo. A boost is a row with `boost_of` set and no text. |
| `GET /api/instance-feed/export` | The caller's posts, boosts and favourites as JSON. |

## Data

Two tables, created on first use and dropped by `DATA_SCHEMA_DOWN`:
`instance_feed_posts` (posts and boosts) and `instance_feed_favourites`.
Post text, content warning and the author's chosen name capitalization are
sealed with the instance data key (`runtime.seal`), not a per-account key,
because every installed account on the instance reads them.

Install state comes from the host's account install registry
(`runtime.account_plugin_installed`). The module deliberately has no
`account_installed` hook: with `dataLifecycle.scope: "account"` and no
`installedBefore`, the registry would call that hook as its fallback, and a
hook that asked the registry back would recurse.

An account with no install row is `unchosen`, because this package has no
legacy install hook. The desktop keeps an existing local install and pushes
it to the account on its next successful account refresh. This also recovers
an install made while the desktop had Instance Feed but the instance still
ran an older catalog: that server returns `plugin_not_found` until its App
deployment includes the package. Updating the desktop alone cannot supply
the server API. An explicit account uninstall is never marked `unchosen`,
so another desktop cannot restore an intentionally removed feed.

Uninstalling (and account deletion) runs `purge_account_data`, which removes
the account's posts and boosts, its favourites, and every favourite and boost
of its posts. The page downloads the export first when there is anything in
it.

## Notifications and limits

Replies, favourites, boosts and `@name` mentions send the post's author (or
the mentioned account) a `plugin_activity` ping linking to
`/dashboard/instance-feed?post=<id>`. Mentions only notify accounts that have
the package installed, at most five per post, never the poster. Each account
may post 30 times in ten minutes (`429 posting_too_fast`).

## Public links

Every post can be read without signing in at `forkmesh.com/feed?post=<id>`
(the instance's own origin on a self-hosted install), added at the user's
request on 2026-10-23 so a post can be shared outside the dashboard. Posts
were already readable by anyone who signed up and installed the package, so
the link does not reveal anything new; the install note and the
`instance.feed.write` permission now say posts are readable by anyone with the
link. The page is `noindex`, so search engines do not list posts.

On the dashboard each post has a globe link ("View on the website", new tab)
and a Copy link button, both using that URL. The page gets the site origin
from `publicSiteOrigin` in the page API (`10-boot.js`), which names
forkmesh.com when the dashboard runs on app.forkmesh.com. The public page
shows the thread (ancestors, the post, replies), keeps a content warning
collapsed behind Show more, links authors and mentions to `/@name`, and has
Copy link and Reply in Feed (`/dashboard/instance-feed?post=<id>`). The desktop,
iOS and Android pages do not have these buttons yet.

## Page

The page keeps its view in the query string (`?post=<id>` for a thread,
`?author=<name>` for a profile) so notification links and the back button
work. It uses only utilities already present in the prebuilt
`app/public/dashboard/tailwind.css`; rebuilding that file from this worktree
drops classes other pages rely on, so check before adding new ones. Colours
follow `theme.json`: `Post` is the page's one `cta` button, an active boost
is `accent` and an active favourite is `attention`.

## Desktop

`InstanceFeedFeature` is a native Qt page, not a link to the dashboard. It
calls the Worker through `NativePackageHost::request` with the request proof
`forkmesh-instance-feed-request-v1`, so the host signs every call with the
desktop's session. It has the same views as the web page (timeline, thread,
profile), the same compose box (Ctrl+Enter posts) and the same actions, and
it styles itself with the app stylesheet's existing object names
(`issueAvatar`, `issueDivider`, `issueComposerSm`, `ghostButton`,
`primaryButton`). Rich-text links use `fm-author:<name>` and
`fm-thread:<id>` internally; `https` links open in the browser.

A native page may only call its own API prefix and organization lifecycle
paths, so it cannot install the package itself. Installing from the desktop
Market already pushes the install to the account
(`MainWindow::pushAccountPackageChange`), so an account without the package
only sees a note pointing to the Market. `openRailTile("post-<id>")` and
`openRailTile("author-<name>")` open a thread or a profile. The Worker's
notification links are dashboard paths (`/dashboard/instance-feed?post=<id>`),
which the desktop's notification router does not map to native pages yet, so
clicking one raises the window without opening the post.

## Android

`native/android/app/src/main/kotlin/com/forkmesh/nativeapp/feature/instancefeed/InstanceFeedScreen.kt`
is the native Compose page, registered for `Route.Kind.InstanceFeed` by
`InstanceFeedFeature`. The hub, the top bar and the route catalog call it
"Feed"; the package id, route kind and class names keep `instance-feed` /
`InstanceFeed`. Its icon is `comment`, as on the desktop rail
(`dynamic_feed` is not in the bundled Material Icons subset).

It talks to the Worker through `market/InstanceFeed.kt` in `:core` (the
account's `PackagesClient` from `MarketFeature.client()`), always off the main
thread. The timeline, a profile (`@name`) and a thread (`Post`) are separate
screens on the navigator stack, so the system back button walks back through
them; they share one `FeedSession` that carries favourite, boost and reply
count changes and deletions across them without a reload. A route with a post
id (`forkmesh://instance-feed?post=<id>` or the dashboard link) opens the
timeline and pushes that thread on top.

The timeline has the compose box first: text with a remaining-character
counter against the server's `maxLength`, an optional content warning (`CW`)
and the `Post` button. Reply puts the composer in reply mode
("Replying to @name", prefilled mention, Cancel) and scrolls to it; after a
reply the thread opens, or reloads if it is the current view. Posts show the
author, `@name`, relative time, "X boosted" and "Replying to @y" headers, a
collapsed content warning behind Show more, and text from `InstanceFeed.spans`
with tappable `https` links (browser) and mentions (profile). Actions are
reply, boost (active in `accent`), favourite (active in `warning`) and, when
`canDelete`, delete after a confirmation. Older pages load when the end of the
list comes into view, or from "Load older posts"; pull down or the refresh
button reloads. An account without the package sees a note with an "Open in
Market" button to the package's Market page.

## iOS

`native/ios/ForkMesh/Features/Packages/FeedScreen.swift` is the native SwiftUI
page, registered for `.instanceFeed(post:)` in
`Navigation/RouteFallbacks.swift`. It drives `InstanceFeedModel` from
`ForkMeshCore/Sources/FMTools/InstanceFeedModels.swift` (the account's
`ApiService.instanceFeedApi`). The route catalog, the hub and the screen call it
"Feed"; the package id, route case and type names keep `instance-feed` /
`InstanceFeed`.

The timeline has the compose box first (text, remaining-character counter
against `maxLength`, optional content warning, `Post`). Reply opens a sheet in
reply mode ("Replying to @name", prefilled mention). Posts show the author,
`@name`, relative time, "X boosted" and "Replying to @y" lines, a content
warning collapsed behind Show more, and text from `InstanceFeed.segments` with
`https` links (browser) and mentions (profile). Actions are reply, boost
(active in `accent`), favourite (active in `warning`) and, when `canDelete`,
delete after a confirmation. Tapping a post pushes its thread and tapping an
author pushes their profile (summary header first); each pushed screen has its
own model. Older pages load when the end of the list appears or from "Load
older posts"; pull down or the refresh button reloads. A route with a post id
opens that thread with "Back to the feed". An account without the package sees
the install note and "Open in the Market", which opens the package's Market
page.
