# RSS Reader

RSS Reader is an optional ForkMesh package: a full feed reader for blogs,
news sites, podcasts and anything else that publishes RSS, Atom, RDF or JSON
Feed, in the ForkMesh app on iPhone and Android and on the desktop (the
**Reader** rail page). **It is local only.** Each device checks your feeds
itself and keeps your subscriptions, articles and read state on that device.
Nothing you subscribe to or read is sent to ForkMesh.

## Credit: NetNewsWire

RSS Reader's design is inspired by [NetNewsWire](https://netnewswire.com/),
the free and open source RSS reader for Mac, iPhone and iPad created by Brent
Simmons and developed by Ranchero Software and its contributors. The Today,
All Unread and Starred smart feeds, the timeline with its unread dots, the
article view with the feed and author above the title, the article toolbar
(read, star, next unread, reader view, share) and the shape of the settings
(Feeds, Timeline, Articles, Troubleshooting) all follow NetNewsWire's lead.
Thank you to the NetNewsWire team.

RSS Reader is a separate project. It shares no code with NetNewsWire and is
not affiliated with or endorsed by it. NetNewsWire is MIT licensed:
<https://github.com/Ranchero-Software/NetNewsWire>. Every client's About
screen carries the same credit.

## What it does

- **Smart feeds.** Today (unread articles published in the last 24 hours),
  All Unread and Starred, each with its unread count.
- **Subscriptions and folders.** Feeds and folders sorted together, folders
  collapse, a filter shows only feeds with unread articles; feed info,
  rename, move, unsubscribe.
- **Adding a feed.** Paste a feed address or just a site address; the reader
  finds the site's feed. `feed:` addresses work too.
- **Import and export.** OPML from and to a file, so you can bring your
  subscriptions from another reader and take them anywhere.
- **Timeline.** Newest or oldest first, grouped by feed or not, 0 to 4
  summary lines, feed icons, search, mark all as read.
- **Article view.** Feed and author, feed icon, title, date, the article
  with JavaScript off, podcast and video attachments, comments link.
- **Toolbar.** Read / unread, star, next unread, reader view (pulls the full
  article text from the original page), open in the browser, share.
- **Settings.** Sort, grouping, Refresh to Clear Read Articles, Confirm Mark
  All as Read, timeline layout, article theme, font and size, load remote
  images, Error Log, Activity Log, Stats, About.
- **Desktop keyboard.** J/K next and previous, N next unread, Space scrolls
  then moves on, R read, S star, O open, V reader view, Shift+A mark all as
  read, Shift+R refresh.

## Where it runs

| Surface | Entry |
| --- | --- |
| iPhone | `native/ios/ForkMeshCore/Sources/FMPackages/RSSReader*.swift`, `native/ios/ForkMesh/Features/Packages/RSSReader*.swift` |
| Android | `native/android/core/.../rssreader/`, `native/android/app/.../feature/rssreader/` |
| Desktop | `content/desktop/` (`builtin:rss-reader`) |

There is no web page and no server API. The developer page, with the parsing
rules every client follows, is
[docs/marketplace/rss-reader.md](../../../docs/marketplace/rss-reader.md).

## Limits

500 feeds and 100 folders per device; the newest 400 articles per feed are
kept, plus every starred article. Feeds are checked when you open the reader
and when you refresh, never on a timer in the background.
