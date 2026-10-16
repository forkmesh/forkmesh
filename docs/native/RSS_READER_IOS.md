# RSS Reader on iPhone

The iPhone client of the [RSS Reader package](../marketplace/rss-reader.md)
(`rss-reader`, route `.rssReader`). It is local only: feeds are fetched,
parsed and stored on the phone, and nothing goes to a ForkMesh server. The
layout follows NetNewsWire for iPhone; the About screen carries the credit.

## Files

| Part | Files |
| --- | --- |
| Text, URL rules | `ForkMeshCore/Sources/FMPackages/RSSReaderText.swift` |
| HTML tokenizer, entities, sanitizer, summaries, feed discovery, reader-view extraction | `RSSReaderHTML.swift` |
| XML tree over `XMLParser`, repair, charset decoding | `RSSReaderXML.swift` |
| RSS / RDF / Atom / JSON Feed parser, dates, guid rule, OPML | `RSSReaderParser.swift` |
| Stored models, settings, limits, filter matcher | `RSSReaderModels.swift` |
| On-device store and persistence | `RSSReaderStore.swift` |
| Fetching, redirects, discovery, reader view, bounded refresh | `RSSReaderFetcher.swift` |
| Site rules for discovery (YouTube, Mastodon, Bluesky) | `RSSReaderDiscovery.swift` |
| Article page HTML, dates, error texts, the NetNewsWire credit | `RSSReaderPage.swift` |
| `@Observable` model the screens use | `RSSReaderModel.swift` |
| Screens | `ForkMesh/Features/Packages/RSSReaderScreen.swift` (Feeds, Add Feed, Feed Info), `RSSReaderTimelineScreen.swift`, `RSSReaderArticleScreen.swift`, `RSSReaderSettingsScreen.swift`, `RSSReaderEditors.swift` (smart feed editor, Filters) |
| Tests | `ForkMeshCore/Tests/FMPackagesTests/RSSReader*Tests.swift` |

## Storage

The store keeps JSON files in `Application Support/projects/<project
slug>/rss-reader/` (the legacy project uses `Application
Support/rss-reader/`), so removing a project removes its reader too:

- `library.json`: folders, feeds (with ETag, Last-Modified, previous
  addresses, error state, language and the `paused`, `excludeFromToday`,
  `excludeFromUnread` switches), settings, `filters`, `smartFeeds`, the
  activity log (200 entries), collapsed sections and the last refresh time.
- `articles-<feed id>.json`: the article list of one feed (title, links,
  author, 300-character summary, image, language, dates, read, starred, read
  later).
- `bodies-<feed id>.json`: sanitized content, comments link, enclosures, the
  item's images and the cached reader view. Bodies load per feed when an article opens and only
  the eight most recently used feeds stay in memory. A "Title or Text" filter
or a smart feed with words reads each feed's bodies the first time it needs
them and keeps their plain text in memory, so the first count after launch
can take longer with many feeds.

Writes are atomic with `completeFileProtectionUntilFirstUserAuthentication`,
run on a background queue, and are debounced (0.8 s) after each change; the
reader also saves when it leaves the screen or the app goes to the
background. Unsubscribing deletes that feed's two files with `unlink`, a
single-file call: there is no recursive delete in the reader.

## Refresh

- Opening the reader checks the feeds that are due (30 minutes after their
  last check, longer after failures: 30 min doubling up to a day). Pull to
  refresh checks every feed not checked in the last minute.
- At most six feeds are fetched at once. Requests carry `If-None-Match` /
  `If-Modified-Since`, an `Accept` for feed types and a `ForkMesh-Reader`
  user agent; the HTTP client is ephemeral (no cookies, no cache). Bodies
  over 2 MB are refused.
- Redirects are followed by hand, at most five. When every hop is permanent
  (301/308) the feed takes the new address and remembers the old one in
  `previousUrls`, unless another feed already has that address.
- There is no background refresh and no timer.

## App Transport Security

`Config/ForkMesh-Info.plist` has no `NSAppTransportSecurity` entry, so iOS
refuses plain `http://` loads. The reader does not weaken that: it asks for
the `https://` form of an `http://` feed (and of an `http://` redirect
target) and stores the https address once it works. When the https request
fails, the feed shows "This feed is only served over plain HTTP, which iOS
does not allow" (`insecure_http`) in Feed Info and the Error Log. Images,
players and feed icons in an article are upgraded to https when the page is
drawn; links keep their address, because they open in Safari or
`SFSafariViewController`, which ATS does not restrict.

## Parsing

The parser and sanitizer follow the rules in the package page, including the
guid rule (item id, else link, else `title|published-ms` from the feed's own
date, else the title; whitespace collapsed, cut to 600 code points), which
the store uses to recognise an article it already has. XML goes through
`XMLParser` (FoundationXML on Linux); a document that fails is repaired once
(control characters dropped, bare `&` escaped, HTML named entities turned
into numeric ones) and parsed again. The HTML entity table covers the HTML 4
set; rarer HTML5 names stay literal.

## Article view

The article (header, enclosures, body, comments link) is one HTML page in a
`WKWebView` with JavaScript off (`allowsContentJavaScript = false`), a
non-persistent website data store, a Content-Security-Policy that allows no
scripts, frames or forms, and no referrer. The body is sanitized again with
the allowlist when drawn; with **Load Remote Images** off, images and players
become links and the policy blocks remote images. Every link tap is caught
and opened in the in-app browser or Safari, as the setting says. Reader view
fetches the original page, extracts the main text on the phone and caches it
with the article.

## Choices worth knowing

- Deleting a folder moves its feeds to the top level instead of deleting
  them.
- The Starred smart feed's count is starred articles that are still unread,
  as NetNewsWire shows it; the Starred timeline lists every starred article.
- Today and All Unread keep an article that was read while the timeline is
  open until the next refresh when **Refresh to Clear Read Articles** is on,
  and until the timeline is left otherwise.
