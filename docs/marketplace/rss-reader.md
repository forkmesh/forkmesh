# RSS Reader package

`marketplace/packages/rss-reader` is a feed reader for the native iOS app, the
native Android app and the desktop app. It is **local only**: every device
checks the feeds itself and keeps its subscriptions, articles and read state
on the device. There is no web page, no Worker API and no sync; ForkMesh's
servers never see what a person reads. The only shared thing is the package's
installed state (`install.sync: "account"`), which follows the account the
way every account-synced package does; the data does not.

The design follows [NetNewsWire](https://netnewswire.com/) (Brent Simmons,
Ranchero Software, MIT licensed): smart feeds, the timeline, the article view
and its toolbar, and the settings layout. No NetNewsWire code is used. Every
client shows the credit in its About screen; keep it there.
[rss-reader-roadmap.md](rss-reader-roadmap.md) sorts NetNewsWire's open
feature requests into what we would build next, later or not at all.

## Where it runs

| Surface | Files | Notes |
| --- | --- | --- |
| iPhone | `native/ios/ForkMeshCore/Sources/FMPackages/RSSReader*.swift`, `native/ios/ForkMesh/Features/Packages/RSSReader*.swift` | Store, fetch, parse and sanitize in `FMPackages` (tested on Linux); SwiftUI screens in the app. |
| Android | `native/android/core/.../rssreader/`, `native/android/app/.../feature/rssreader/` | Logic in `:core` (`:core:coreTest`); Compose screens in `:app`. |
| Desktop | `content/desktop/` (`builtin:rss-reader`) | Native rail page **Reader**; fetches through the window's shared network manager so the desktop firewall sees every feed host. |

Because the data is local, the manifest declares `dataLifecycle: {"scope":
"none", "localData": true}`. On the desktop that means the host keeps the
package data under `market/packages/rss-reader/data/` (subscriptions as OPML,
settings, starred articles), saves it when the package is removed and offers
to import it when the package is installed again.

### Desktop

`content/desktop/` registers `builtin:rss-reader` as the **Reader** rail page
(`RssReaderFeature`), with three panes: sources (Smart Feeds, and On This
Computer with one level of folders), the timeline, and the article.
`RssReaderModel` holds parsing, sanitizing, discovery, OPML, reader-view
extraction and the store using Qt Core only; `RssReaderFetch` fetches through
the window's shared `QNetworkAccessManager` (`NativePackageHost::network`), so
the desktop firewall decides every feed, favicon, image and reader-view
request. It allows six requests at a time and follows redirects by hand: a
301/308 chain updates the stored address, and an https→http hop is refused.
Feeds are checked when the page opens (`refresh()`, due feeds only) and on
Refresh (Shift+R, every feed); there are no timers. The rail badge is the
unread count.

- **Storage.** Articles, read state and fetch state live in
  `paths::localDataRoot()/rss-reader/` (`state.json`,
  `articles/<feedId>.json`, `icons/<feedId>.png`). The exportable part is also
  kept in the package data the host owns, `market/packages/rss-reader/data/`:
  `store` (an id matching `state.json`), `subscriptions` (OPML), `settings`
  (JSON) and `starred` (JSON, each article's HTML capped at 8 KB and about
  2 MB in total). For `scope: "none"` the host saves those keys when the
  package is removed and offers to import them on reinstall; when `store`
  does not match the files on disk, the page rebuilds from the keys and
  ignores the old files. The host does not delete the `rss-reader/` folder on
  removal; a reinstall overwrites it. `state.json` also keeps each feed's
  unread count, so the rail badge at startup does not load the articles.
  `exportData()` returns the same bundle (`subscriptionsOpml`, `settings`,
  `starred`), although the host does not call it for `scope: "none"`.
- **Article view.** A `QTextBrowser` whose `loadResource` serves only the
  feed icon and, when **Load Remote Images** is on, images fetched through
  the host network (scaled to the pane); it never loads files or other URLs.
  Links open in the browser only for `http`, `https` and `mailto`. Themes are
  Default (the app palette), Sepia, Paper and High contrast. Keyboard: J/K,
  N, Space, R, S, O, V, Shift+A, Shift+R. With **Load Remote Images** off an
  image is drawn as a link labelled with its alt text or "[Image]". The
  document's text direction follows the article's `dir`.
- **Rules and settings.** `filters`, `smartFeeds`, `foldDuplicates`,
  `markAllKeepsStarred`, `sortFeedsByUnread` and `showThumbnails` live in the
  `settings` JSON, so they leave and return with the package data. **Hide Read
  Articles** is the existing `unreadOnlyTimeline` (`hideReadArticles` is
  accepted when reading). Smart feed folder scopes are `folder:<name>`. The
  sources pane has a More (⋯) menu with New Smart Feed… and Mark Older as
  Read; Settings has a Filters tab. Filter-hidden articles are also left out
  of the app-wide search.
- **Thumbnails and `file:` feeds.** Thumbnails come through `RssReaderFetch`,
  are cropped and scaled once and kept in memory (about 300). `file:///`
  feeds are read from disk only by feed refreshes; image and reader-view
  requests still refuse files.
- **Size.** At the limits (500 feeds of 400 articles, about 217 MB) the first
  open loads in about 2.2 s, saves are per feed, and a search over everything
  takes about 0.3 s (debounced).

## Feature set (every client)

- Smart feeds: **Today** (unread, published in the last 24 hours), **All
  Unread**, **Starred**, **Read Later** (shown only when it holds articles),
  then the person's own smart feeds by name, each with its unread count.
- Custom smart feeds (`smartFeeds`, at most 50): words (plain or regular
  expression, matched in the title or text), a scope (every feed, one feed or
  one folder), unread only, starred only and a maximum age in days; all must
  hold. **New Smart Feed…** creates one; the row's context menu (long press on
  phones) edits or deletes it.
- **Read Later** (`readLater` on the article) is toggled from the article and
  the timeline row; reading does not remove an article from it, and pruning
  keeps it like a starred one.
- Subscriptions ("On This iPhone", "On This Phone", "On This Computer") and
  one level of folders, sorted together by name or, with **Sort by Unread
  Count**, by unread count then name; a filter that hides feeds with nothing
  unread; **Find a Feed** (title, host or address; folders with a match open,
  smart feeds hide, the unread-only filter is ignored while searching); a
  warning mark on a feed whose last check failed ("Last check failed: …").
- Feed info: rename, move, home page, an editable feed address (normalised
  like Add Feed without discovery, refused when invalid or already followed;
  the old address goes to `previousUrls`, the fetch state is reset and the feed
  is checked at once), last check, last error, and three switches: **Pause
  Updates** (`paused`: refresh skips it, the row is muted with a pause mark),
  **Hide from Today** (`excludeFromToday`) and **Hide from Unread Counts**
  (`excludeFromUnread`: left out of Today, All Unread, folder and smart feed
  counts and the desktop rail badge; its own row keeps its count). **Check
  Now** fetches the feed without validators, ignoring pause and backoff, and
  shows "RSS 2.0 · 12 items · checked 3:04 PM" or the error. Unsubscribe.
- Add a feed from a feed address or a site address (discovery below).
- **Import Subscriptions** (OPML from a file), **Import from URL…** (OPML
  fetched with the feed fetcher, 2 MB cap) and **Export Subscriptions**
  (`htmlUrl` written when known), plus **Add the ForkMesh Blog Feed**
  (`https://forkmesh.com/blog/rss.xml`).
- Timeline: newest or oldest first, group by feed, 0 to 4 summary lines,
  search, mark all as read (with optional confirmation; **Mark All as Read
  Skips Starred** leaves starred articles unread), **Mark Older as Read…**
  (Older Than 1 Day, 3 Days, 1 Week, 2 Weeks, 1 Month, by the article's date;
  in each timeline for that timeline and in the sources menu for every feed),
  Refresh to Clear Read Articles. **Hide Read Articles** applies to every
  timeline except Starred; an article read while the timeline is open stays
  until the next refresh (with Refresh to Clear Read Articles) or until the
  timeline is left. Phone lists lead with the unread marker, headline, author
  and date; feed icons remain available as an opt-in layout setting, and
  **Show Thumbnails** adds the article image (56 pt/dp/px, cropped, rounded) at
  the trailing edge when remote images are on.
- Filters (`filters`, at most 100; Settings → **Filters**): a pattern (plain
  substring or regular expression, case-insensitive; an invalid expression
  never matches and the editor says so) on the title, the title or text (first
  20 000 characters) or the author, for every feed or one feed. **Hide**
  leaves matches stored but out of every timeline and count; **Mark as Read**
  applies only to articles as they arrive. Starred articles are exempt. **Mark
  Duplicate Links as Read** (`foldDuplicates`) marks an arriving article read
  when another feed already holds the same link (URL identity, fragment
  dropped).
- Article: feed and author above the title, feed icon, date in small capitals
  and "· N min read" (ceil(words / 230 + CJK letters / 500), at least 1),
  sanitized content with JavaScript off, item images not already in the
  content drawn at the top, enclosures (audio and video), comments link. The
  page carries the feed's or entry's language as `lang`, with `dir="rtl"` for
  `ar he iw fa ur yi ps sd ug dv ckb syr` and `dir="auto"` otherwise; `pre`
  blocks do not wrap; an image with alt text outside a captioned `figure` gets
  the alt text as a small caption.
- Toolbar: read/unread, star, next unread (moves on to the next feed with
  unread articles), reader view (on-device extraction of the original page),
  share (on phones) or copy link (desktop). The article title opens the
  original, as does the desktop's Open in Browser button and the offer shown
  when reader view finds no text. On phones the ForkMesh shell header and
  navigation stay hidden from feeds through the timeline and article detail;
  a horizontal swipe moves the full article page to the previous or next
  article.
- Settings: the timeline and sources switches above, Filters, article theme (Default, Sepia, Paper,
  High contrast), font and size, load remote images, appearance, Error Log,
  Activity Log, Stats, About.

## Parsing contract

The clients share one set of rules so a feed reads the same everywhere:

- **Formats**: RSS 0.9x/2.0 (`content:encoded` preferred over
  `description`, `dc:creator` / `author`, `pubDate` / `dc:date`,
  `comments`, `enclosure`), RDF/RSS 1.0, Atom 1.0 (`link rel="alternate"`,
  `content` then `summary`, `type="text|html|xhtml"`, `published` then
  `updated`), JSON Feed 1.x (`content_html`, `content_text`, `summary`,
  `external_url`, `attachments`, `image`, `banner_image`, `language`). XML
  parsers run without DTDs or entity expansion; undefined HTML entities such
  as `&nbsp;` are repaired first.
- **Media RSS** (RSS and Atom, also inside `media:group`): `media:thumbnail`,
  `media:content` with `medium="image"` or an `image/` type, and image
  `enclosure`s are the item's images; audio and video `media:content` are
  enclosures. `media:description` is the content when the item has none
  (plain text is escaped, blank lines make paragraphs, newlines `<br>`, bare
  URLs links); `media:title` is the title when it has none. The article image
  is JSON Feed `image`, then `banner_image`, the first thumbnail, image
  `media:content`, image enclosure, then the first `<img>` in the content.
  This is what makes YouTube and Mastodon items show their pictures.
- **Language**: RSS `<language>`, Atom `xml:lang` on the feed or entry, JSON
  Feed `language`, stored on the feed and, when it differs, on the article.
- **Article identity** (de-duplication and keeping read/star when an article
  is edited): the item's id (RSS `guid`, Atom `id`, JSON Feed `id`), else its
  link, else (RDF) its `rdf:about`, else `title + "|" + published-ms` from the
  feed's own date (just the title when it has none). Whitespace collapsed and trimmed,
  cut to 600 characters.
- **Dates**: RFC 822 and ISO 8601; a missing date, or one more than a day in
  the future, becomes the time the article was first seen.
- **Discovery**: first the known sites: YouTube `/channel/UC…`,
  `/playlist?list=…` and `/user/…` become the matching
  `youtube.com/feeds/videos.xml` address, and `/@handle`, `/c/…`, watch,
  `youtu.be` and Shorts pages are fetched for their channel id (falling back
  to normal discovery); a profile path `/@name` is tried as `/@name.rss`
  (Mastodon); `bsky.app/profile/X` becomes `…/profile/X/rss`. Then, if the
  address answers HTML, follow `<link rel="alternate">` (RSS, then Atom, then
  JSON Feed, at most three), else try `/feed`, `/rss`, `/feed.xml`,
  `/atom.xml`, `/rss/` (Ghost) on the same origin. The desktop also accepts
  `file:///` feeds, read from disk on each refresh with the same 2 MB cap;
  OPML fetched from a URL cannot add them, and the phones refuse `file:`. `feed://` and `feed:` become
  `https://`; a bare host gets `https://`. A feed already followed is matched
  by host, port, path and query.
- **Sanitizing**: only these tags survive: `a abbr b blockquote br caption
  cite code dd del details dfn div dl dt em figcaption figure h1-h6 hr i img
  ins kbd li mark ol p pre q s samp section small span strike strong sub
  summary sup table tbody td tfoot th thead time tr u ul audio video source
  wbr article aside header footer main`, with `a[href,title]`,
  `img[src,alt,title,width,height]`, `td/th[colspan,rowspan]`, `ol[start]`,
  `time[datetime]`, `abbr[title]`; URLs must be `http(s)` (`mailto:` for
  links); `script`, `style`, `object`, `embed`, `svg`, `form` and the like
  are dropped with their content, as is any element with the class
  `instapaper_ignore`, and an `iframe` becomes a link. Article views never
  run JavaScript.
- **Reader view**: prefer `<article>` / `<main>`, else the element whose
  direct `p`, `pre`, `blockquote`, `li`, `h2`, `h3` children hold the most
  text; `script`, `style`, `nav`, `header`, `footer`, `aside` and `form` are
  dropped; the result is sanitized and cached on the article.
- **Fetching**: conditional GET (ETag, Last-Modified); redirects followed,
  and a permanent one updates the stored address; at most 200 items and 2 MB
  per fetch; about six feeds at a time. A feed is due 30 minutes after its
  last check and a failing one backs off, doubling, up to a day. Clients
  check feeds when the reader opens and on refresh (pull to refresh on
  phones); there are no polling timers.
- **Limits**: 500 feeds, 100 folders, 100 filters, 50 smart feeds, the
  newest 400 articles per feed plus every starred or Read Later one.

## Tests

```sh
python3 -m pytest -q marketplace/packages/rss-reader/tests
```

The package test checks the manifest stays local-only (no Worker, no web
page, no outbound origins) and that the NetNewsWire credit is present. Each
client has its own suite: the desktop package test
(`content/desktop/tests/test_rss_reader.cpp`, target
`forkmesh-rss-reader-tests`),
`ForkMeshCore` `FMPackagesTests` (`RSSReader*`), and the Android
`:core:coreTest`.
