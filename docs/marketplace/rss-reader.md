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
  N, Space, R, S, O, V, Shift+A, Shift+R.
- **Size.** At the limits (500 feeds of 400 articles, about 217 MB) the first
  open loads in about 2.2 s, saves are per feed, and a search over everything
  takes about 0.3 s (debounced).

## Feature set (every client)

- Smart feeds: **Today** (unread, published in the last 24 hours), **All
  Unread**, **Starred**, each with its unread count.
- Subscriptions ("On This iPhone", "On This Phone", "On This Computer") and
  one level of folders, sorted together by name; a filter that hides feeds
  with nothing unread; feed info (rename, move, home page, feed address, last
  check, last error); unsubscribe.
- Add a feed from a feed address or a site address (discovery below).
- **Import Subscriptions** (OPML from a file) and **Export Subscriptions**,
  plus **Add the ForkMesh Blog Feed** (`https://forkmesh.com/blog/rss.xml`).
- Timeline: newest or oldest first, group by feed, 0 to 4 summary lines,
  feed icons, search, mark all as read (with optional confirmation), Refresh
  to Clear Read Articles.
- Article: feed and author above the title, feed icon, date in small capitals,
  sanitized content with JavaScript off, enclosures, comments link.
- Toolbar: read/unread, star, next unread (moves on to the next feed with
  unread articles), reader view (on-device extraction of the original page),
  share (on phones) or copy link (desktop). The article title opens the
  original, as does the desktop's Open in Browser button and the offer shown
  when reader view finds no text. On phones the ForkMesh shell header and
  navigation stay hidden from feeds through the timeline and article detail;
  a horizontal swipe moves the full article page to the previous or next
  article.
- Settings: the timeline switches above, article theme (Default, Sepia, Paper,
  High contrast), font and size, load remote images, appearance, Error Log,
  Activity Log, Stats, About.

## Parsing contract

The clients share one set of rules so a feed reads the same everywhere:

- **Formats**: RSS 0.9x/2.0 (`content:encoded` preferred over
  `description`, `dc:creator` / `author`, `pubDate` / `dc:date`,
  `comments`, `enclosure`), RDF/RSS 1.0, Atom 1.0 (`link rel="alternate"`,
  `content` then `summary`, `type="text|html|xhtml"`, `published` then
  `updated`), JSON Feed 1.x (`content_html`, `content_text`, `summary`,
  `external_url`, `attachments`). XML parsers run without DTDs or entity
  expansion; undefined HTML entities such as `&nbsp;` are repaired first.
- **Article identity** (de-duplication and keeping read/star when an article
  is edited): the item's id (RSS `guid`, Atom `id`, JSON Feed `id`), else its
  link, else (RDF) its `rdf:about`, else `title + "|" + published-ms` from the
  feed's own date (just the title when it has none). Whitespace collapsed and trimmed,
  cut to 600 characters.
- **Dates**: RFC 822 and ISO 8601; a missing date, or one more than a day in
  the future, becomes the time the article was first seen.
- **Discovery**: if the address answers HTML, follow `<link rel="alternate">`
  (RSS, then Atom, then JSON Feed, at most three), else try `/feed`, `/rss`,
  `/feed.xml`, `/atom.xml` on the same origin. `feed://` and `feed:` become
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
  are dropped with their content and an `iframe` becomes a link. Article
  views never run JavaScript.
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
- **Limits**: 500 feeds, 100 folders, the newest 400 articles per feed plus
  every starred one.

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
