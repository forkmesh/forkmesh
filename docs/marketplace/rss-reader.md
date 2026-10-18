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
  search, mark all as read (with optional confirmation), Refresh to Clear Read
  Articles. Phone lists lead with the unread marker, headline, author and date;
  feed icons remain available as an opt-in layout setting.
- Article: feed and author above the title, feed icon, date in small capitals,
  sanitized content with JavaScript off, enclosures, comments link. The
  comments link is added only when the content does not already link to the
  same address (compared ignoring scheme and host case, http/https, default
  ports, trailing slashes and `&amp;`; the fragment counts), so feeds such as
  Hacker News that put a Comments link in the description show it once.
- **Share to chat**: an article's title and link (with an optional note on
  phones) can be sent to a person or a channel. This is the only thing the
  reader ever sends to ForkMesh, and only when the person sends it. iOS has
  **Send to Chat** next to Share in the article toolbar
  (`RSSReaderShareSheet.swift`, the chat `ChatRecipientPicker`, sent with
  `hub.deliver`). On Android the Share button offers **Send in Chat**
  (`feature/chat/ChatShareSheet.kt`, targets built in
  `core/.../chat/ChatShareTargets.kt`). The picker lists channels, Office
  channels, direct messages, people online now and any platform user; the
  message is sent through `ChatHub.sendText`. The desktop has **Share to
  Chat…** in the toolbar and the article context menu, which lists the Office
  channels and direct conversations the signed-in account can post to. The
  package declares `chat.messages.share`; only then does the window fill
  `NativePackageHost::chatTargets` and `sendChatMessage` (backed by the Office
  channel mirror).
- Toolbar: read/unread, star, next unread (moves on to the next feed with
  unread articles), reader view (on-device extraction of the original page),
  share (on phones) or copy link (desktop). The article title opens the
  original, as does the desktop's Open in Browser button and the offer shown
  when reader view finds no text. On phones the ForkMesh shell header and
  navigation stay hidden from feeds through the timeline and article detail;
  a horizontal swipe moves the full article page to the previous or next
  article, and a tap on the right half of the page goes to the next article
  and on the left half to the previous one. A tap on a link, button or
  player, on selected text, or one that stops a scroll does not turn the
  page (iOS asks the page what sits under the finger from the app's own
  script world, since page JavaScript is off, and falls back to waiting
  0.3 s for a link to open; Android waits 200 ms and checks the WebView hit
  test and whether a link opened). On iOS a swipe that starts sideways locks
  the page's vertical scroll until the finger lifts, so the page slides one
  way only (a pan on the pager that every page's scroll view waits on). The
  iOS pager keeps the previous and next articles loaded in their own web
  views beside the current one, so the neighbouring page slides in with the
  finger already drawn instead of loading after the swipe; a release springs
  on from the finger's speed, and a new swipe or tap during the slide
  finishes it at once. Toolbar arrows and Next Unread slide the same way. The
  swipe follows the finger from the first movement and changes article past
  about a sixth of the width (at least 50 pt; Android: 72 dp) or on a flick,
  springing back otherwise and resisting at the first or last article; a
  zoomed-in page pans instead of swiping. Vertical scrolling never moves the
  page sideways. On iOS, while the page is not zoomed, horizontal bounce is
  off and the horizontal offset is pinned. Android picks the direction after
  4 dp, freezes the horizontal position while scrolling, turns zoom off and
  keeps content inside the width: code blocks wrap and tables are fixed
  layout, so a sideways drag on them changes article instead of scrolling.
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
- **Feed icons**: candidates are tried in order, and the next one is tried
  whenever a candidate gives an HTTP or network error, a body that is not an
  image (checked from its leading bytes, so an HTML page served as
  `favicon.ico` is skipped) or an image that does not decode. Android also
  skips banners more than twice as wide as tall. The desktop skips icons
  under 8 px or fully transparent, and takes the best frame of a multi-size
  `.ico`:
  1. the icon the feed declares (RSS `image/url`, Atom `icon` then `logo`,
     JSON Feed `favicon` then `icon`);
  2. the icons the home page links (`rel="icon"`, `shortcut icon`,
     `apple-touch-icon`, `apple-touch-icon-precomposed`), resolved against
     `<base>` or the page, square 32 to about 180 px first and larger before
     smaller, SVG skipped unless the platform decodes it;
  3. `/favicon.ico` on the home page's origin, then the feed's;
  4. `/apple-touch-icon.png` on the same origins.

  The home page is the feed's site link, or the feed's origin when the link is
  missing. On phones, a feed proxy (FeedBurner, feedproxy, FeedBlitz,
  FeedPress, podcast hosts) is never asked for a favicon: the site link's
  origin (Android: the latest article's) is used instead. A favicon guess is
  never stored as the feed's declared icon. Each feed records when it was
  checked and which sources it used. A change of declared icon, site link or
  feed address starts a new check. A feed with no icon shows its letter tile
  and is tried again after a day. On phones a found icon is re-checked after
  30 days, and a check that reached no server is retried at the next refresh.
  Icons are kept as PNG on the device (desktop `icons/<feedId>.png`, at most
  64 px; Android at most 192 px; iOS `icon-<feedId>`, also embedded in the
  article page). The page parsing is `discoverIconLinks` (desktop),
  `RSSReaderIcons.swift` and `RssIcons.kt`.
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
