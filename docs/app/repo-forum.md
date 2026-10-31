# Repository forums (Discourse on ForkMesh)

Part of [the App docs](README.md). A repository can hold a Discourse-style
forum: categories with colours and subcategories, tags, Latest and Top lists,
and topic pages with posts, likes and views. The forum is kept in the
repository itself, in `.forkmesh/forum/`, so every mirror and every clone
carries it ([forum-reach](forum-reach.md)): it stays readable when a domain or
an address is blocked.

The goal (owner decision, 2026-10-31) is that ForkMesh does for Discourse what
it does for GitHub: every Discourse feature has a ForkMesh counterpart, and a
forum can be mirrored with Discourse per feature, in either direction, the way
[provider mirrors](provider-mirrors.md) work for GitHub. The decisions behind
the plan:

1. The forum's data lives in a repository, as signed records, not in the
   instance database.
2. A link with a Discourse forum is set per feature: Off, Discourse to
   ForkMesh, ForkMesh to Discourse, or both. Writing back needs the forum's
   admin API key.
3. The operators are the repository's owners (or the organization's), plus
   the moderators they appoint.
4. The web comes first. The desktop follows, then iPhone and Android
   together.

| Part | File |
| --- | --- |
| Forum tab (lazy chunk `repo-forum`) | `app/public/dashboard/js/09-repo-forum.js` |
| Tab, panel, routes, Settings switch | `08-repo-detail-network.js`, `02-helpers.js` `REPO_TAB_ROUTES`, `06-repo-content.js` `REPO_FEATURE_TABS`, `07-repo-compose-branch.js` `loadRepoFeaturePanels` |
| Per-repository switch | `REPO_FEATURE_DEFAULTS["forum"]` in `app/src/entry.py` (off by default) |
| First-view tab strip on a node | `app/host/forkmesh_host/repo_ssr.py` `TABS` |
| Package switch | `marketplace/packages/forum` (`web: builtin:forum`, installed by default) |
| Importer | `marketplace/packages/discourse-mirror` ([discourse-mirror](../marketplace/discourse-mirror.md)) |
| Tests | `app/tests/test_dashboard_repo_forum.py`, `app/browser_tests/tests/dashboard-repo-forum.spec.js`, `app/tests/host/test_repo_ssr.py`, `marketplace/packages/discourse-mirror/tests` |

## When the tab shows

- The repository's owner turns **Forum** on under Settings, Features. It is off
  for every repository until then, so the page never probes repositories that
  have no forum.
- The `forum` package is the account switch for the tab (`data-plugin-entry`).
  It is installed by default, so a reader needs nothing to read a forum.
- A deep link to `/<owner>/<repo>/forum…` opens the tab even before the
  switches load. The page then reads the switches itself (`GET …/about`) and
  says the forum is turned off when it is. On every other tab, Forum stays
  hidden until the switches say it is on.
- On with no `forum.json` yet: the page explains how to fill it.

## Routes

All of these sit under `/<owner>/<repo>/forum`:

| Path | View |
| --- | --- |
| `` (nothing), `/categories` | Top-level categories: colour bar, description, subcategory chips, topic count, the 3 latest topics |
| `/latest` | Every listed topic, newest activity first, global pins on top |
| `/top?period=all\|yearly\|quarterly\|monthly\|weekly\|daily` | Most active topics of the period |
| `/c/<slug>/<id>` | A parent category lists its subcategories; a leaf lists its topics |
| `/c/<slug>/<id>/l/latest` | All of a category's topics, its subcategories' included |
| `/tag/<key>` | Topics with a tag |
| `/t/<slug>/<id>[/<post>]` | A topic, scrolled to a post |

Topic ids are strings: imported topics keep their Discourse number, and native
topics (phase 2) will be `n<k>`, so the two never collide. The slug is only
for reading; the page rewrites it to the topic's own (`history.replaceState`).

## Format v1: `.forkmesh/forum/`

Every list is one file, so each view costs one batched `blobs` read. Every
file except `source/` and (later) `events/` is derived and rewritten whole by
its writer, so it never needs a hand merge.

```text
forum.json                 {format:1, kind:"forkmesh-forum", title, description,
                            source:{type:"discourse", origin, lastSync, pending},
                            counts:{topics, posts, categories}, pageSize:50, postsPerChunk:100, topPeriods}
categories.json            [{id, name, slug, color, text_color, description, parent_category_id, position,
                             subcategory_ids, topic_count, total_topic_count, post_count, latest:[3 rows]}]
tags.json                  [{name, key, count}]
latest/<n>.json            {page, pages, more, rows:[50]}   global pins first
c/<category id>/<n>.json   the category's and its subcategories' topics, its pins first
tag/<key>/<n>.json
top/<period>.json          {period, rows:[50]}
topics/<id>/topic.json     the topic, its stats, uploads {address: file}, chunks, posts[0..99]
topics/<id>/posts/<n>.json {posts:[…]} the next 100 posts each
uploads/<sha256>.<ext>     copied pictures (PNG, JPEG, GIF, WebP, AVIF)
source/discourse.json      the importer's cursor; readers never fetch it
events/                    reserved for phase 2 records; readers ignore it
```

**List rows** have these fields: `id, title, slug, category_id, tags,
created_at, bumped_at, last_posted_at, posts_count, reply_count, views,
like_count, pinned, pinned_globally, closed, archived, visible, excerpt,
posters [{username, name, description}], last_poster_username, origin`.
Unlisted topics (`visible: false`) are left out of every list.

**Posts** have these fields: `id, post_number, post_type, username, name,
created_at, updated_at, reply_to_post_number, action_code, like_count, reads,
reply_count, hidden, origin`. The body depends on the format: `format: "html"`
with `cooked` for imported posts; `format: "markdown"` with `raw` for native
posts (phase 2).

**Tag keys** are the tag itself when it is safe as a path segment (letters in
any script, digits, `.`, `_`, `-`). Otherwise the key is `h-` followed by 16
hex characters of its SHA-256.

**Top** ranks topics by `likes × 15 + posts × 5 + views × 0.05`, counting
topics created within the period.

## Rendering imported posts safely

Imported posts are Discourse's cooked HTML, written by somebody else and
passed through another server. `forumCookedHtml` never inserts that HTML
as it is:

- It parses the HTML with `DOMParser`. That document is inert: nothing runs
  and no image loads.
- It then writes new markup from an allowlist that matches the importer's own
  `Sanitizer`, escaping every text and attribute.
- These are dropped with everything inside them: `script`, `style`, `iframe`,
  `object`, `embed`, `svg`, `math`, forms, media, `picture`, `source`.
- Only `href`, `src`, `alt`, `title`, the image size, the table spans, `start`
  and `open` survive. `class` survives only for `quote`, `title`, `onebox`,
  `lightbox-wrapper` and `spoiler`. `style`, `srcset` and every `on…`
  attribute are dropped.
- **Pictures:**
  - A picture the importer copied loads from the repository
    (`…/raw?path=.forkmesh/forum/uploads/<file>`, `referrerpolicy=no-referrer`).
  - An emoji becomes its alt text.
  - A quoted avatar is dropped.
  - Any other picture becomes the text link `[image: alt]`.
  - **The page never loads anything from another host.** The browser test
    fails on any request that leaves the test server.
- **Links:**
  - A link to a topic on the same forum becomes a link inside the copy.
  - Profile, category, tag, badge and group links on the forum become plain
    text (`@name`).
  - `javascript:`, `data:`, `vbscript:` and `file:` links lose their anchor
    and keep only their text.
  - Every other link opens in a new tab with `rel="nofollow noopener noreferrer"`.
- Category colours are forum data and are drawn only after they pass
  `^#?([0-9a-f]{3}|[0-9a-f]{6})$`. Everything else uses the theme roles:
  links and the current list are blue, and nothing is green.

Avatars are letters (`forumAvatar`), never pictures from the forum. Times
follow Discourse: `5m`, `3h`, `6d`, then `Aug 17` within the year and
`Jun 2025` before it, with the full date on hover. Counts read `3.0k` and
`1.3m`.

**Read state** is kept per browser in `localStorage`
(`forkmesh.forum.seen.<repo>`, up to 2000 topics). It drives the blue dot on
topics with new posts and the order of "New & Unread Topics" under a topic.

**Reply** links to the topic on the source forum until ForkMesh accepts posts
(phase 2). The heart shows the imported like count but is not a button yet.

## Roadmap

- **Phase 1 (done, 2026-10-31).** Format v1, the importer's `forum` layout,
  and the web Forum tab, read-only.
- **Phase 2: native signed records.** A new collaboration kind, `forum`. It is
  not an extension of discussions: their one-file-per-comment layout and fixed
  categories would break every existing reader of `discussion.md`.
  - Event schema `forkmesh-forum-event-v1`. Authors can create topics and
    posts, edit and delete them, and like or unlike a post.
  - Operators (`moderation.can_moderate_repo`) can:
    - pin and unpin, globally or in a category, with an end date;
    - close and open, archive, list and unlist, and move topics;
    - hide and unhide posts, and delete them as staff;
    - create, update, delete and reorder categories;
    - manage tags and change site settings.
  - Flags and silencing or suspending people stay in the moderation tables in
    the database: who flagged is private, and silencing controls access.
  - Each event is an immutable file, `events/<topic>/<ts>-<sha8>.json`, so a
    merge is a union. Writers fold the events into the format v1 views.
  - Touches:
    - `app/src/events.py`, a `forum_handler` and a `forum_inbox` table
      (migration, schema fingerprint, rekey, `_REPO_SCOPED_TABLES`);
    - `tools/mirror_gateway.py`, `server/intake.go` and a new `server/forum.go`;
    - the catalog topic count;
    - the composer and operator menus in `09-repo-forum.js`;
    - no-JS pages on nodes (`read_surfaces.py`);
    - the desktop drain.
- **Phase 3: Discourse as a provider mirror.**
  - Add `discourse` to `PROVIDERS` in `repository_provider_mirror.py`. Its
    origin is any https address, guarded by `repository_webhooks.clean_url`.
  - Credentials: an admin API key and `Api-Username`.
  - Directions are set per feature for categories, topics, posts, moderation
    and tags. Likes only come in.
  - A cursor-paged cron within the subrequest budget, with optional signed
    Discourse webhooks.
  - Settings on the web, iPhone and Android together.
  - The importer stays for anonymous copies and very large first copies.
- **Phase 4: the rest of the admin tools.**
  - Review queue, users and trust levels, groups (organization teams), badges,
    tag groups and site settings.
  - Private categories (in a private repository), search, notifications and
    watching, read state across devices.
  - Bookmarks, polls, solved, the staff action log, webhooks.
- **Phase 5: the other apps.** A desktop page in both layout modes, Control
  Tower and Focus. Then iPhone and Android together, with any differences
  listed in `docs/native/ANDROID.md`.
