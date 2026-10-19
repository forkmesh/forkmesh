# Instance Feed

`marketplace/packages/instance-feed` is a Mastodon-style timeline for the
people on one ForkMesh instance (the local timeline, in Mastodon terms). Each
account installs it for itself; everyone on the instance with it installed
reads the same feed. It does not federate: the network-wide ActivityPub view
is the separate `feed` package
([../marketplace-usage-feed-mastodon.md](../marketplace-usage-feed-mastodon.md)).

## Code map

| Path | Responsibility |
| --- | --- |
| `content/worker/instance_feed.py` | Worker API at `/api/instance-feed`, schema, account data hooks. |
| `content/web/view.html`, `content/web/page.js` | Dashboard page `/dashboard/instance-feed` (page id `instance-feed`). |
| `content/desktop/InstanceFeedFeature.{h,cpp}` | Native desktop page (`builtin:instance-feed`, `comment` rail icon). |
| `content/desktop/tests/test_instance_feed_feature.cpp` | Qt test for the desktop page, run against a stub host. |
| `tests/test_instance_feed.py` | Worker suite, including install and uninstall through `plugin_data`. |

## API

All routes need a signed-in session; writes also need a same-origin request
(or a bearer token). An account without the package installed gets
`{"installed": false}` from `GET /api/instance-feed` and `409
plugin_not_installed` everywhere else.

| Route | Purpose |
| --- | --- |
| `GET /api/instance-feed?before=&limit=&author=` | Timeline, newest first, 20 per page (at most 40). Without `author` it shows top-level posts and boosts; with `author` it shows that person's posts, replies and boosts plus a `profile` summary. `next` is the cursor (`<createdAt>-<id>`) for the following page. |
| `POST /api/instance-feed/posts` | New post: `text` (1–500 characters), optional `spoiler` (content warning, 100) and `replyTo`. |
| `GET /api/instance-feed/posts/<id>` | Thread: `ancestors`, `post`, `replies` (depth-first, at most 200). A boost id resolves to its original. |
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

