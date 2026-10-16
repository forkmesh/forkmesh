# RSS Reader: what to build next, from NetNewsWire's feature requests

On 2026-08-26 NetNewsWire announced that its bug list was nearly empty and it
was moving on to feature requests
([netnewswire.blog](https://netnewswire.blog/2026/08/26/time-to-start-doing-feature.html),
[Brent Simmons, "After Fixin' the Bugs"](https://inessential.com/2026/08/26/after-fixin-the-bugs.html)).
Our [RSS Reader package](rss-reader.md) follows NetNewsWire's design, so its
open feature requests are a ready-made list of what readers ask for. This page
is that list, sorted by what we would take.

Snapshot: 2026-10-16, **560 open issues of type Feature** in
[Ranchero-Software/NetNewsWire](https://github.com/Ranchero-Software/NetNewsWire/issues?q=is%3Aissue%20state%3Aopen%20type%3AFeature)
(the post counted 556). Numbers in brackets are GitHub reactions, then the
NetNewsWire milestone where there is one. To refresh the list, query the
GitHub search API for
`repo:Ranchero-Software/NetNewsWire is:issue state:open type:Feature`.

## Points from the post

- Most popular: custom smart feeds, content filtering, timeline thumbnails,
  and the traditional layout on the Mac.
- Wanted by fewer people: more sync systems, appearance settings and sorting
  options.
- Small and easy: for example, dropping elements with the class
  `.instapaper_ignore`.
- **No AI features**, not even on-device ones, for now.
- Order follows dependencies. Thumbnails need enclosure and Media RSS
  parsing first, and Mastodon feeds carry their images as Media RSS
  enclosures. NetNewsWire also weighs the open web, how much work and risk a
  feature is, testing, and how much it adds for users to think about.
- Future releases are planned in GitHub milestones (7.3, 7.4 and 7.5 at
  the time of the snapshot).

## How we choose

The rules come from [rss-reader.md](rss-reader.md) and the rest of the
platform:

1. **Local only.** No ForkMesh server, Worker or third-party service sees
   what a person reads. Sync services, read-later services and per-device
   settings sync are out.
2. **No page JavaScript.** Article views never run scripts or embed frames.
   Requests to show the live web page or embed players are out.
3. **No timers.** Feeds are checked when the reader opens and on Refresh.
   Background refresh, notifications and widgets would need polling, so they
   are out for now.
4. **No AI in the reader.** This matches NetNewsWire. Summaries,
   translation and AI filtering would send article text to a model.
5. **One feature on three clients.** iPhone, Android and the desktop share
   one parsing contract. A feature counts only if all three can have it, so
   Apple-only integrations (Shortcuts, Spotlight, widgets, Handoff, visionOS,
   app icons) do not apply.
6. **Favour parsing and the open web.** Better feeds for everyone, including
   Mastodon, Bluesky, YouTube and Ghost, matter more than extra settings.
7. **Few, compact controls.** A new setting has to be worth what it adds to
   Settings.

## Take next (built)

These are ordered so that each one unblocks the next. Every item is small or
medium and fits the rules above. All ten were built on 2026-10-16 on the
iPhone, Android and the desktop; [rss-reader.md](rss-reader.md) describes how
they behave.

1. **Media RSS images and descriptions.** Today we read `media:content` only
   for audio and video. Also read images from `media:content`,
   `media:thumbnail` and `media:group`, and use `media:description` as the
   summary when nothing else is there. Show the images in the article, which
   makes Mastodon posts and YouTube entries display properly.
   ([#4132](https://github.com/Ranchero-Software/NetNewsWire/issues/4132) (18, 7.3),
   [#2538](https://github.com/Ranchero-Software/NetNewsWire/issues/2538) (17),
   [#3131](https://github.com/Ranchero-Software/NetNewsWire/issues/3131) (7.3),
   [#4331](https://github.com/Ranchero-Software/NetNewsWire/issues/4331) (7.3),
   [#4189](https://github.com/Ranchero-Software/NetNewsWire/issues/4189),
   [#4065](https://github.com/Ranchero-Software/NetNewsWire/issues/4065) (18),
   [#127](https://github.com/Ranchero-Software/NetNewsWire/issues/127))
2. **Timeline thumbnails** as an opt-in layout, alongside the existing feed
   icon option. The image is the article's first Media RSS or enclosure image
   and loads only when **Load Remote Images** is on.
   ([#1551](https://github.com/Ranchero-Software/NetNewsWire/issues/1551) (52),
   [#4186](https://github.com/Ranchero-Software/NetNewsWire/issues/4186),
   [#3098](https://github.com/Ranchero-Software/NetNewsWire/issues/3098))
3. **Hide read articles** in every timeline, as one global switch that works
   with Mark All as Read and Refresh to Clear Read Articles.
   ([#3030](https://github.com/Ranchero-Software/NetNewsWire/issues/3030) (13),
   [#3766](https://github.com/Ranchero-Software/NetNewsWire/issues/3766),
   [#4184](https://github.com/Ranchero-Software/NetNewsWire/issues/4184),
   [#4374](https://github.com/Ranchero-Software/NetNewsWire/issues/4374))
4. **Per-feed switches in Feed Info:** pause a feed (refresh skips it but
   keeps its articles), leave it out of Today, leave it out of unread counts,
   and edit the feed address. The address is read-only today.
   ([#3401](https://github.com/Ranchero-Software/NetNewsWire/issues/3401) (20),
   [#4708](https://github.com/Ranchero-Software/NetNewsWire/issues/4708),
   [#2388](https://github.com/Ranchero-Software/NetNewsWire/issues/2388),
   [#1962](https://github.com/Ranchero-Software/NetNewsWire/issues/1962),
   [#3247](https://github.com/Ranchero-Software/NetNewsWire/issues/3247) (11),
   [#5394](https://github.com/Ranchero-Software/NetNewsWire/issues/5394))
5. **Content filters.** Per-feed or global rules on title, author or text
   (plain words or a regular expression) that either hide matches or mark
   them read. Repeats of the same link in several feeds can be folded into
   one. Everything is checked on the device.
   ([#1864](https://github.com/Ranchero-Software/NetNewsWire/issues/1864) (64),
   [#4406](https://github.com/Ranchero-Software/NetNewsWire/issues/4406) (19),
   [#847](https://github.com/Ranchero-Software/NetNewsWire/issues/847),
   [#3975](https://github.com/Ranchero-Software/NetNewsWire/issues/3975),
   [#3777](https://github.com/Ranchero-Software/NetNewsWire/issues/3777))
6. **Custom smart feeds.** Saved searches over feed, folder, author, words,
   unread, starred and age, built on the same matcher as the filters. One
   built-in addition is a **Read Later** feed that is separate from Starred.
   ([#217](https://github.com/Ranchero-Software/NetNewsWire/issues/217) (18),
   [#3023](https://github.com/Ranchero-Software/NetNewsWire/issues/3023) (20),
   [#4888](https://github.com/Ranchero-Software/NetNewsWire/issues/4888),
   [#4079](https://github.com/Ranchero-Software/NetNewsWire/issues/4079))
7. **Better discovery for the open web.** Build the feed address from a
   YouTube channel or video page, a Mastodon profile (`/@user.rss`), a
   Bluesky profile (`/rss`) and Ghost sites. Accept `file:` feeds on the
   desktop, import OPML from a URL, and keep home pages when importing OPML.
   ([#902](https://github.com/Ranchero-Software/NetNewsWire/issues/902) (9),
   [#4362](https://github.com/Ranchero-Software/NetNewsWire/issues/4362),
   [#3748](https://github.com/Ranchero-Software/NetNewsWire/issues/3748),
   [#4869](https://github.com/Ranchero-Software/NetNewsWire/issues/4869) (8),
   [#5375](https://github.com/Ranchero-Software/NetNewsWire/issues/5375) (7.4),
   [#985](https://github.com/Ranchero-Software/NetNewsWire/issues/985),
   [#136](https://github.com/Ranchero-Software/NetNewsWire/issues/136),
   [#1857](https://github.com/Ranchero-Software/NetNewsWire/issues/1857))
8. **Small sanitizer and article wins**, each a few lines plus a test on
   every client:
   - drop `.instapaper_ignore` elements
     ([#3501](https://github.com/Ranchero-Software/NetNewsWire/issues/3501),
     already closed in NetNewsWire);
   - set `lang` and `dir` on the article from the feed's `xml:lang` /
     `language` so that right-to-left text reads correctly
     ([#4688](https://github.com/Ranchero-Software/NetNewsWire/issues/4688),
     [#1851](https://github.com/Ranchero-Software/NetNewsWire/issues/1851),
     [#2148](https://github.com/Ranchero-Software/NetNewsWire/issues/2148));
   - do not wrap code blocks, and scroll them sideways instead
     ([#542](https://github.com/Ranchero-Software/NetNewsWire/issues/542),
     [#1986](https://github.com/Ranchero-Software/NetNewsWire/issues/1986),
     [#4059](https://github.com/Ranchero-Software/NetNewsWire/issues/4059));
   - show image alt text as a caption
     ([#3968](https://github.com/Ranchero-Software/NetNewsWire/issues/3968));
   - show an estimated reading time in the article header
     ([#4818](https://github.com/Ranchero-Software/NetNewsWire/issues/4818)).
9. **Marking read in bulk:** Mark All as Read can skip starred articles, and
   Catch Up marks everything older than a chosen age as read.
   ([#4548](https://github.com/Ranchero-Software/NetNewsWire/issues/4548) (7.4),
   [#3976](https://github.com/Ranchero-Software/NetNewsWire/issues/3976),
   [#662](https://github.com/Ranchero-Software/NetNewsWire/issues/662),
   [#74](https://github.com/Ranchero-Software/NetNewsWire/issues/74))
10. **Sources list tidy-ups:** sort feeds by unread count, find a feed by
    name, show a feed's error mark in the list, and add a Validate Feed
    command to Feed Info.
    ([#853](https://github.com/Ranchero-Software/NetNewsWire/issues/853),
    [#5285](https://github.com/Ranchero-Software/NetNewsWire/issues/5285),
    [#5297](https://github.com/Ranchero-Software/NetNewsWire/issues/5297),
    [#952](https://github.com/Ranchero-Software/NetNewsWire/issues/952),
    [#555](https://github.com/Ranchero-Software/NetNewsWire/issues/555),
    [#4673](https://github.com/Ranchero-Software/NetNewsWire/issues/4673),
    [#49](https://github.com/Ranchero-Software/NetNewsWire/issues/49) (7.3))

## Later

These fit our rules but cost more or help fewer people:

- **Mark read while scrolling**, as an option, with an optional delay
  ([#4369](https://github.com/Ranchero-Software/NetNewsWire/issues/4369) (16),
  [#3924](https://github.com/Ranchero-Software/NetNewsWire/issues/3924),
  [#4525](https://github.com/Ranchero-Software/NetNewsWire/issues/4525),
  [#4149](https://github.com/Ranchero-Software/NetNewsWire/issues/4149)).
- **Feeds behind HTTP authentication**, with credentials kept in the device's
  keychain or keystore and never in the package data
  ([#502](https://github.com/Ranchero-Software/NetNewsWire/issues/502) (36),
  [#4068](https://github.com/Ranchero-Software/NetNewsWire/issues/4068)).
- **Subfolders**. Today there is one level of folders
  ([#503](https://github.com/Ranchero-Software/NetNewsWire/issues/503) (15),
  [#4637](https://github.com/Ranchero-Software/NetNewsWire/issues/4637)).
- **Reader view per feed:** always use it for chosen feeds and fetch it ahead
  on refresh
  ([#4272](https://github.com/Ranchero-Software/NetNewsWire/issues/4272),
  [#4646](https://github.com/Ranchero-Software/NetNewsWire/issues/4646),
  [#2432](https://github.com/Ranchero-Software/NetNewsWire/issues/2432),
  [#5230](https://github.com/Ranchero-Software/NetNewsWire/issues/5230)).
- **Offline images** for starred or chosen feeds, within a size cap
  ([#1873](https://github.com/Ranchero-Software/NetNewsWire/issues/1873) (14),
  [#5284](https://github.com/Ranchero-Software/NetNewsWire/issues/5284)).
- **Open in the browser per feed**, for feeds that carry only links
  ([#2268](https://github.com/Ranchero-Software/NetNewsWire/issues/2268) (13),
  [#1982](https://github.com/Ranchero-Software/NetNewsWire/issues/1982)).
- **Retention:** how long to keep read articles, and a per-feed cleanup
  ([#2289](https://github.com/Ranchero-Software/NetNewsWire/issues/2289),
  [#4163](https://github.com/Ranchero-Software/NetNewsWire/issues/4163),
  [#4355](https://github.com/Ranchero-Software/NetNewsWire/issues/4355)).
- **Export starred articles** (HTML or JSON Feed) and import them back
  ([#978](https://github.com/Ranchero-Software/NetNewsWire/issues/978),
  [#4592](https://github.com/Ranchero-Software/NetNewsWire/issues/4592)).
- **Custom feed icons** and better small-icon fallbacks
  ([#3613](https://github.com/Ranchero-Software/NetNewsWire/issues/3613) (11),
  [#5357](https://github.com/Ranchero-Software/NetNewsWire/issues/5357),
  [#5393](https://github.com/Ranchero-Software/NetNewsWire/issues/5393)).
- **Desktop keyboard extras:** custom shortcuts, and keys for previous article
  and font size
  ([#508](https://github.com/Ranchero-Software/NetNewsWire/issues/508) (37),
  [#3399](https://github.com/Ranchero-Software/NetNewsWire/issues/3399) (13),
  [#3984](https://github.com/Ranchero-Software/NetNewsWire/issues/3984)).
- **Article themes:** separate light and dark themes, a theme per feed, and
  footnote handling
  ([#4473](https://github.com/Ranchero-Software/NetNewsWire/issues/4473),
  [#4735](https://github.com/Ranchero-Software/NetNewsWire/issues/4735),
  [#2720](https://github.com/Ranchero-Software/NetNewsWire/issues/2720)).
- **Timeline polish:** date section headers, and a sort order per smart feed
  or folder
  ([#866](https://github.com/Ranchero-Software/NetNewsWire/issues/866),
  [#2997](https://github.com/Ranchero-Software/NetNewsWire/issues/2997)).
- **Recently read** smart feed
  ([#1559](https://github.com/Ranchero-Software/NetNewsWire/issues/1559),
  [#3957](https://github.com/Ranchero-Software/NetNewsWire/issues/3957)).

## Already in ForkMesh

These are open in NetNewsWire but our reader already does them, so there is
nothing to do:

- Refresh to Clear Read Articles on the desktop
  ([#4409](https://github.com/Ranchero-Software/NetNewsWire/issues/4409)).
- Mark All as Read confirmation
  ([#4232](https://github.com/Ranchero-Software/NetNewsWire/issues/4232),
  [#3804](https://github.com/Ranchero-Software/NetNewsWire/issues/3804)).
- Feed icons in the timeline are optional
  ([#3177](https://github.com/Ranchero-Software/NetNewsWire/issues/3177),
  [#1877](https://github.com/Ranchero-Software/NetNewsWire/issues/1877),
  [#5109](https://github.com/Ranchero-Software/NetNewsWire/issues/5109)).
- Summary lines can be set from 0 to 4
  ([#2999](https://github.com/Ranchero-Software/NetNewsWire/issues/2999),
  [#4496](https://github.com/Ranchero-Software/NetNewsWire/issues/4496),
  [#4683](https://github.com/Ranchero-Software/NetNewsWire/issues/4683)).
- A permanent redirect updates the stored feed address
  ([#953](https://github.com/Ranchero-Software/NetNewsWire/issues/953)).
- Feed Info shows the last check and the last error
  ([#4211](https://github.com/Ranchero-Software/NetNewsWire/issues/4211)).
- Remote images can be turned off, and video never autoplays because
  `autoplay` is not an allowed attribute
  ([#3167](https://github.com/Ranchero-Software/NetNewsWire/issues/3167),
  [#4821](https://github.com/Ranchero-Software/NetNewsWire/issues/4821),
  [#4143](https://github.com/Ranchero-Software/NetNewsWire/issues/4143)).
- The appearance setting is separate from the system's
  ([#3165](https://github.com/Ranchero-Software/NetNewsWire/issues/3165)).
- The comments link is shown
  ([#4278](https://github.com/Ranchero-Software/NetNewsWire/issues/4278)).
- JSON Feed 1.1 `authors` is read
  ([#2758](https://github.com/Ranchero-Software/NetNewsWire/issues/2758)).
- A filter hides feeds with nothing unread
  ([#3553](https://github.com/Ranchero-Software/NetNewsWire/issues/3553)).
- Proxy settings: the desktop fetches through the window's network manager,
  so the ForkMesh firewall and network settings apply
  ([#478](https://github.com/Ranchero-Software/NetNewsWire/issues/478)).

## Not for us

| Group | Why |
| --- | --- |
| Third-party sync and accounts: Feedbin, Feedly, FreshRSS, Inoreader, Miniflux, Nextcloud, tt-rss, CommaFeed, NewsBlur, iCloud, WebDAV, Microsub, settings sync | Local only (rule 1). |
| AI summaries, translation, AI filtering and curation | Rule 4. NetNewsWire has declined them too. |
| Embedded or live web views, a JavaScript switch per feed, inline YouTube players, Picture in Picture | Rule 2. The YouTube thumbnail and description come in through Media RSS (take-next item 1). |
| Background refresh, notifications, widgets, refreshing only on Wi-Fi | Rule 3. |
| Apple-only integrations: Shortcuts, Spotlight, Handoff, Safari extension, visionOS, app icon pickers, Dock and menu-bar items, Focus filters, Raycast, IINA | Rule 5. |
| Read-later and bookmarking services: Pinboard, Instapaper, Readwise, Apple News | The phones' share sheet and the desktop's copy link already cover them, and an in-app connection would send data off the device. |
| Routing YouTube through Invidious or DeArrow | Sends what a person watches to a third party. |
| Interface localization | This is handled for the whole app, not per package. |

## Groups by count

How all 560 requests sort into groups, with the decision for each group. A
group can hold a few items that are decided differently above.

| Group | Requests | Decision |
| --- | ---: | --- |
| Article view, reader view, images and themes | 80 | Take items 8 and later; no live web views. |
| Sidebar, folders and per-feed settings | 73 | Take items 4 and 10; subfolders and icons later. |
| Marking read, navigation and keyboard | 57 | Take item 9; scroll-to-read and keys later. |
| Third-party sync services and accounts | 48 | Not for us. |
| Timeline layout and thumbnails | 37 | Take items 2 and 3. |
| Other app chrome, settings and documentation | 37 | Case by case; mostly Mac and iOS chrome. |
| Smart feeds, filtering and search | 35 | Take items 5 and 6. |
| Apple-platform and system integrations | 34 | Not for us. |
| Adding feeds, discovery, OPML and export | 30 | Take item 7; starred export later. |
| Notifications, refresh and networking | 23 | HTTP auth later; the rest not for us. |
| YouTube, video and audio | 20 | Thumbnails and feed URLs yes; players no. |
| Read-later and sharing services | 19 | Not for us. |
| Stars, saving, tags and history | 18 | Read Later (item 6); recently read later. |
| Parsing, enclosures and Media RSS | 16 | Take item 1 first. |
| Storage, retention and performance | 13 | Retention later. |
| AI, summaries and translation | 12 | Not for us. |
| Localization and languages | 8 | `lang`/`dir` in item 8; the rest belongs to the whole app. |

## Every open request

Generated from the snapshot above. Within each group, requests are sorted by
reactions. Each request is listed once, under the first group that matches
its NetNewsWire labels or title.

### AI, summaries and translation (12)

- [#4665](https://github.com/Ranchero-Software/NetNewsWire/issues/4665) Implement Apple's Foundation Model (Local AI Summary) (21)
- [#3628](https://github.com/Ranchero-Software/NetNewsWire/issues/3628) Ai summarizing tool (6)
- [#4586](https://github.com/Ranchero-Software/NetNewsWire/issues/4586) Translation support (6)
- [#3107](https://github.com/Ranchero-Software/NetNewsWire/issues/3107) Idea: Translate options (5)
- [#4094](https://github.com/Ranchero-Software/NetNewsWire/issues/4094) Translate article (4)
- [#2716](https://github.com/Ranchero-Software/NetNewsWire/issues/2716) Supporting Apple's translation service (2)
- [#5334](https://github.com/Ranchero-Software/NetNewsWire/issues/5334) AI-based content filtering (2)
- [#3981](https://github.com/Ranchero-Software/NetNewsWire/issues/3981) Adding Translation Functionality to RSS Reader
- [#4811](https://github.com/Ranchero-Software/NetNewsWire/issues/4811) Option to translate certain feeds and/or folder contents
- [#4853](https://github.com/Ranchero-Software/NetNewsWire/issues/4853) Hope for Supporting integrating OpenAI API for automatic RSS title translation and content summarization
- [#5475](https://github.com/Ranchero-Software/NetNewsWire/issues/5475) Can AI translation and summarization capabilities be integrated?
- [#5488](https://github.com/Ranchero-Software/NetNewsWire/issues/5488) Auto Curation

### Third-party sync services and accounts (48)

- [#3731](https://github.com/Ranchero-Software/NetNewsWire/issues/3731) Micro guide: NetNewsWire ❤️ FreshRSS setup (57)
- [#2247](https://github.com/Ranchero-Software/NetNewsWire/issues/2247) NextCloud News integration possible? (55)
- [#388](https://github.com/Ranchero-Software/NetNewsWire/issues/388) Tt-rss syncing (49)
- [#3593](https://github.com/Ranchero-Software/NetNewsWire/issues/3593) Add Miniflux to available account types (18)
- [#944](https://github.com/Ranchero-Software/NetNewsWire/issues/944) Feedbin custom IP/host (13)
- [#2739](https://github.com/Ranchero-Software/NetNewsWire/issues/2739) Sync Feed Metadata when using iCloud (13)
- [#495](https://github.com/Ranchero-Software/NetNewsWire/issues/495) Support for Microsub servers (11)
- [#2846](https://github.com/Ranchero-Software/NetNewsWire/issues/2846) Sync via WebDAV (11)
- [#4743](https://github.com/Ranchero-Software/NetNewsWire/issues/4743) Add support for Miniflux API (11)
- [#3908](https://github.com/Ranchero-Software/NetNewsWire/issues/3908) Supporting labels/tags from FreshRSS (10)
- [#1868](https://github.com/Ranchero-Software/NetNewsWire/issues/1868) Persist settings in iCloud so multiple devices can share (8)
- [#4386](https://github.com/Ranchero-Software/NetNewsWire/issues/4386) CommaFeed support (8)
- [#3920](https://github.com/Ranchero-Software/NetNewsWire/issues/3920) Allow custom HTTP headers when connecting to FreshRSS (7)
- [#2156](https://github.com/Ranchero-Software/NetNewsWire/issues/2156) Support for reading Pinboard.in unread items (4)
- [#4353](https://github.com/Ranchero-Software/NetNewsWire/issues/4353) Add support for 3rd-party login for Inoreader (3)
- [#809](https://github.com/Ranchero-Software/NetNewsWire/issues/809) Feedbin folders (2)
- [#4160](https://github.com/Ranchero-Software/NetNewsWire/issues/4160) Updated Articles (2)
- [#4256](https://github.com/Ranchero-Software/NetNewsWire/issues/4256) Share button for NewsBlur's BlurBlog (2)
- [#788](https://github.com/Ranchero-Software/NetNewsWire/issues/788) Feedbin: use image proxy to serve all included images from Feedbin (1)
- [#2745](https://github.com/Ranchero-Software/NetNewsWire/issues/2745) Support Feedbin’s “rich text” mode for newsletters (1)
- [#2806](https://github.com/Ranchero-Software/NetNewsWire/issues/2806) Add  self-hosted newsblur (1)
- [#4230](https://github.com/Ranchero-Software/NetNewsWire/issues/4230) Move to icloud (1)
- [#699](https://github.com/Ranchero-Software/NetNewsWire/issues/699) Feedbin: Recently Read Entries
- [#1030](https://github.com/Ranchero-Software/NetNewsWire/issues/1030) Feedly: improve error handling during authentication
- [#1056](https://github.com/Ranchero-Software/NetNewsWire/issues/1056) Show credentials dialog when FeedBin password is invalid (because it's been changed) (0, 7.4)
- [#1080](https://github.com/Ranchero-Software/NetNewsWire/issues/1080) Feedly: improve error handling when creating or updating collections (0, 7.5)
- [#1299](https://github.com/Ranchero-Software/NetNewsWire/issues/1299) Feedly: Starred articles from feeds not followed
- [#2107](https://github.com/Ranchero-Software/NetNewsWire/issues/2107) Concerning Feedbin Pages feed
- [#2144](https://github.com/Ranchero-Software/NetNewsWire/issues/2144) Read Later Pages from Feedbin are displayed weird
- [#2531](https://github.com/Ranchero-Software/NetNewsWire/issues/2531) Feedly: Imported newsletters not showing up
- [#2550](https://github.com/Ranchero-Software/NetNewsWire/issues/2550) Feedly board
- [#2679](https://github.com/Ranchero-Software/NetNewsWire/issues/2679) Keep (un)read status when moving feed from "On My Mac" to Feedly
- [#2974](https://github.com/Ranchero-Software/NetNewsWire/issues/2974) Proxy doesn't seem to work when using Inoreader
- [#3173](https://github.com/Ranchero-Software/NetNewsWire/issues/3173) EshRSS selfsigned certificate
- [#3294](https://github.com/Ranchero-Software/NetNewsWire/issues/3294) Feedly: Leo Web Alerts are missing
- [#3530](https://github.com/Ranchero-Software/NetNewsWire/issues/3530) Add Alternative API Base URLs to Inoreader
- [#3900](https://github.com/Ranchero-Software/NetNewsWire/issues/3900) Show updated articles
- [#4053](https://github.com/Ranchero-Software/NetNewsWire/issues/4053) Can’t log in to Inoreader when I signed up for it via Apple
- [#4071](https://github.com/Ranchero-Software/NetNewsWire/issues/4071) Feedbin saved search
- [#4093](https://github.com/Ranchero-Software/NetNewsWire/issues/4093) Allow multiple different iCloud accounts
- [#4168](https://github.com/Ranchero-Software/NetNewsWire/issues/4168) Regarding the issue of third-party account login.
- [#4397](https://github.com/Ranchero-Software/NetNewsWire/issues/4397) Allow for custom OAuth authorisation with GReader based API
- [#4433](https://github.com/Ranchero-Software/NetNewsWire/issues/4433) Facebook feed item titles are the entire post itself
- [#4907](https://github.com/Ranchero-Software/NetNewsWire/issues/4907) No automatic notification when account needs reauthorization
- [#4923](https://github.com/Ranchero-Software/NetNewsWire/issues/4923) EshRSS: Improve “Uncategorized” Handing in NNW (0, 7.4)
- [#5225](https://github.com/Ranchero-Software/NetNewsWire/issues/5225) ICloud: create new zone for article content, and use old zone just for statuses (0, 7.5)
- [#5397](https://github.com/Ranchero-Software/NetNewsWire/issues/5397) S: Hide Empty “Uncategorized” Group and Add Starred Article Filter
- [#5446](https://github.com/Ranchero-Software/NetNewsWire/issues/5446) Relax HTTP 504 gateway timeout times?

### YouTube, video and audio (20)

- [#3361](https://github.com/Ranchero-Software/NetNewsWire/issues/3361) Convert YouTube embeds to html5 video, like Vinegar in Safari (18)
- [#4065](https://github.com/Ranchero-Software/NetNewsWire/issues/4065) Add YouTube thumbnail and description (18)
- [#902](https://github.com/Ranchero-Software/NetNewsWire/issues/902) Construct feed URLs for YouTube channels (9)
- [#3683](https://github.com/Ranchero-Software/NetNewsWire/issues/3683) YouTube Inline Video Support (9)
- [#1167](https://github.com/Ranchero-Software/NetNewsWire/issues/1167) Open YouTube feed in IINA (8)
- [#2820](https://github.com/Ranchero-Software/NetNewsWire/issues/2820) ✨ Feature Request: Reader (5)
- [#3813](https://github.com/Ranchero-Software/NetNewsWire/issues/3813) Enhancement: Redirect embedded YouTube links to Invidious (5)
- [#1426](https://github.com/Ranchero-Software/NetNewsWire/issues/1426) Picture in Picture (4)
- [#3206](https://github.com/Ranchero-Software/NetNewsWire/issues/3206) MacOS - Easy way to go full screen for embedded YouTube content (4)
- [#128](https://github.com/Ranchero-Software/NetNewsWire/issues/128) Attachments: show download links for audio, etc. in detail view (2)
- [#4143](https://github.com/Ranchero-Software/NetNewsWire/issues/4143) Disable Video Autoplay (2)
- [#4214](https://github.com/Ranchero-Software/NetNewsWire/issues/4214) Read aloud or Speak selected article (2)
- [#3975](https://github.com/Ranchero-Software/NetNewsWire/issues/3975) Req: filter YouTube videos with duration under 1min (ie Shorts) (1)
- [#4193](https://github.com/Ranchero-Software/NetNewsWire/issues/4193) Integrate DeArrow (1)
- [#4200](https://github.com/Ranchero-Software/NetNewsWire/issues/4200) YouTube Regular vs Shorts? (1)
- [#4703](https://github.com/Ranchero-Software/NetNewsWire/issues/4703) Allow the playing of all unread as audio on iOS app (1)
- [#844](https://github.com/Ranchero-Software/NetNewsWire/issues/844) Play audio links within app
- [#4140](https://github.com/Ranchero-Software/NetNewsWire/issues/4140) Improve/Align media playback modes
- [#4362](https://github.com/Ranchero-Software/NetNewsWire/issues/4362) Can NNW deduce an RSS feed from a youtube video URL?
- [#4366](https://github.com/Ranchero-Software/NetNewsWire/issues/4366) Audiofunction to listen/hear the Articles like an Audiobook

### Apple-platform and system integrations (34)

- [#4376](https://github.com/Ranchero-Software/NetNewsWire/issues/4376) Allow "Use Reader View" to use Safari's native Reader View (11)
- [#3375](https://github.com/Ranchero-Software/NetNewsWire/issues/3375) Adding NNW Widget in macOS (10)
- [#4666](https://github.com/Ranchero-Software/NetNewsWire/issues/4666) UI Update: Dark Mode (And Tinted) App Icon On Mac (9)
- [#4060](https://github.com/Ranchero-Software/NetNewsWire/issues/4060) Focus filters (6)
- [#4832](https://github.com/Ranchero-Software/NetNewsWire/issues/4832) VisionOS Checkbox (6)
- [#5427](https://github.com/Ranchero-Software/NetNewsWire/issues/5427) Support iPhone Duo Layout (6)
- [#2128](https://github.com/Ranchero-Software/NetNewsWire/issues/2128) MacOS menu bar icon (5)
- [#4058](https://github.com/Ranchero-Software/NetNewsWire/issues/4058) App Icon chooser (5)
- [#4283](https://github.com/Ranchero-Software/NetNewsWire/issues/4283) Widgets per feed (5)
- [#4668](https://github.com/Ranchero-Software/NetNewsWire/issues/4668) IOS Shortcut to Retrieve Unread Headlines (4)
- [#3589](https://github.com/Ranchero-Software/NetNewsWire/issues/3589) IOS 16 / Lock Screen Widget (3)
- [#4393](https://github.com/Ranchero-Software/NetNewsWire/issues/4393) Re-register as default RSS reader on macOS (3)
- [#5296](https://github.com/Ranchero-Software/NetNewsWire/issues/5296) Small Widget on iOS/iPadOS (3)
- [#4562](https://github.com/Ranchero-Software/NetNewsWire/issues/4562) Create Raycast extension (2)
- [#4789](https://github.com/Ranchero-Software/NetNewsWire/issues/4789) Bug/Feature Request: Share to Apple News (2)
- [#1895](https://github.com/Ranchero-Software/NetNewsWire/issues/1895) Allow searching of articles with Spotlight on macOS (1)
- [#2082](https://github.com/Ranchero-Software/NetNewsWire/issues/2082) Edit feed url from shortcuts app (1)
- [#3674](https://github.com/Ranchero-Software/NetNewsWire/issues/3674) We should index unread articles so that Spotlight search can find them (1)
- [#4201](https://github.com/Ranchero-Software/NetNewsWire/issues/4201) Ability to open all links in specified Safari profile (1)
- [#4626](https://github.com/Ranchero-Software/NetNewsWire/issues/4626) Support for the “Get what’s onscreen” Shortcuts Action (1, 7.5)
- [#5436](https://github.com/Ranchero-Software/NetNewsWire/issues/5436) IOS Dark Mode Icon (1)
- [#83](https://github.com/Ranchero-Software/NetNewsWire/issues/83) Add Dock menu of unread items (like NNW had)
- [#874](https://github.com/Ranchero-Software/NetNewsWire/issues/874) Support Handoff
- [#2266](https://github.com/Ranchero-Software/NetNewsWire/issues/2266) IOS: Shortcuts - Export/import subscriptions (feature)
- [#3006](https://github.com/Ranchero-Software/NetNewsWire/issues/3006) Widget: Future Development
- [#3185](https://github.com/Ranchero-Software/NetNewsWire/issues/3185) Shortcut for Launch Center Pro
- [#3682](https://github.com/Ranchero-Software/NetNewsWire/issues/3682) Widget design comments in 6.1 beta
- [#3697](https://github.com/Ranchero-Software/NetNewsWire/issues/3697) Add Feed Name parameter to Add Web Feed Shortcut
- [#3844](https://github.com/Ranchero-Software/NetNewsWire/issues/3844) Detecting if the current URL is a feed
- [#3883](https://github.com/Ranchero-Software/NetNewsWire/issues/3883) Support for Shortcuts?
- [#3961](https://github.com/Ranchero-Software/NetNewsWire/issues/3961) Safari Extension Icon via SF Symbols
- [#4642](https://github.com/Ranchero-Software/NetNewsWire/issues/4642) ‘Refresh feeds’ as an App Intent for Shortcuts
- [#4812](https://github.com/Ranchero-Software/NetNewsWire/issues/4812) Expose the "Show Reader View" action to Shortcuts
- [#4935](https://github.com/Ranchero-Software/NetNewsWire/issues/4935) Provide Option to Suppress Feeds from Widget Timelines

### Read-later and sharing services (19)

- [#881](https://github.com/Ranchero-Software/NetNewsWire/issues/881) Save to Pinboard (7)
- [#765](https://github.com/Ranchero-Software/NetNewsWire/issues/765) "Send to Instapaper" feature (6)
- [#1914](https://github.com/Ranchero-Software/NetNewsWire/issues/1914) Replace share extension with action extension (4)
- [#3157](https://github.com/Ranchero-Software/NetNewsWire/issues/3157) Include more metadata when sharing a post with the share sheet (iOS) - feature request (3)
- [#3898](https://github.com/Ranchero-Software/NetNewsWire/issues/3898) Readwise highlighting (3)
- [#3715](https://github.com/Ranchero-Software/NetNewsWire/issues/3715) Print/Export to PDF (2)
- [#4674](https://github.com/Ranchero-Software/NetNewsWire/issues/4674) Add an "email" button if a feed entry includes the author's email address (2)
- [#4012](https://github.com/Ranchero-Software/NetNewsWire/issues/4012) Include article title in share on iOS (1, 7.5)
- [#4793](https://github.com/Ranchero-Software/NetNewsWire/issues/4793) Copy link/share with highlight (1)
- [#5202](https://github.com/Ranchero-Software/NetNewsWire/issues/5202) Share sheet access (1)
- [#381](https://github.com/Ranchero-Software/NetNewsWire/issues/381) Minor thoughts
- [#852](https://github.com/Ranchero-Software/NetNewsWire/issues/852) Keyboard shortcut for send to Mail
- [#1257](https://github.com/Ranchero-Software/NetNewsWire/issues/1257) Mac: investigate integration with Pinboard for Starred Items
- [#2070](https://github.com/Ranchero-Software/NetNewsWire/issues/2070) Add keyboard shortcut for “Add to Reading List”
- [#3442](https://github.com/Ranchero-Software/NetNewsWire/issues/3442) MacOS: feature request - printing
- [#3489](https://github.com/Ranchero-Software/NetNewsWire/issues/3489) Keyboard shortcut for reader view and share sheet. (0, 7.3)
- [#3582](https://github.com/Ranchero-Software/NetNewsWire/issues/3582) MacOS: make copy and paste of items in the Source Pane (outline view) work to move/copy feeds
- [#3692](https://github.com/Ranchero-Software/NetNewsWire/issues/3692) Add "Share" menu to the File menu (0, 7.3)
- [#4359](https://github.com/Ranchero-Software/NetNewsWire/issues/4359) Share feature in NetNewsWire

### Notifications, refresh and networking (23)

- [#502](https://github.com/Ranchero-Software/NetNewsWire/issues/502) Support for feeds behind HTTP auth (36)
- [#478](https://github.com/Ranchero-Software/NetNewsWire/issues/478) Add proxy server support (15)
- [#4142](https://github.com/Ranchero-Software/NetNewsWire/issues/4142) Add option to prevent automatic refresh when launching NNW (4)
- [#4212](https://github.com/Ranchero-Software/NetNewsWire/issues/4212) "Show Notifications~" on by default or at least prompt when adding feed (3)
- [#162](https://github.com/Ranchero-Software/NetNewsWire/issues/162) Status bar: show last refresh time, # feeds (2, 7.3)
- [#953](https://github.com/Ranchero-Software/NetNewsWire/issues/953) Should update feed URL for 301 Moved Permanently (2)
- [#3493](https://github.com/Ranchero-Software/NetNewsWire/issues/3493) No way to clear cookies? (2)
- [#4211](https://github.com/Ranchero-Software/NetNewsWire/issues/4211) Show last request time or time since (2)
- [#136](https://github.com/Ranchero-Software/NetNewsWire/issues/136) Make Import OPML from URL work (1)
- [#2078](https://github.com/Ranchero-Software/NetNewsWire/issues/2078) Notification for all feeds in a folder (1)
- [#2675](https://github.com/Ranchero-Software/NetNewsWire/issues/2675) Notify About New Articles Behaviour (1)
- [#3690](https://github.com/Ranchero-Software/NetNewsWire/issues/3690) Rich notifications (1)
- [#4068](https://github.com/Ranchero-Software/NetNewsWire/issues/4068) Password protected RSS Feeds - works in NetNewsWire 4.1, not 6.1 (1)
- [#4560](https://github.com/Ranchero-Software/NetNewsWire/issues/4560) Policies around refresh times (1)
- [#970](https://github.com/Ranchero-Software/NetNewsWire/issues/970) Allow Refresh options for individual items and folders
- [#3022](https://github.com/Ranchero-Software/NetNewsWire/issues/3022) IOS: add “Refresh” command to App Context Menu
- [#3167](https://github.com/Ranchero-Software/NetNewsWire/issues/3167) Disable auto downloading remote content
- [#3594](https://github.com/Ranchero-Software/NetNewsWire/issues/3594) Play sound for notification
- [#4006](https://github.com/Ranchero-Software/NetNewsWire/issues/4006) Localize notificationDisplayName
- [#4083](https://github.com/Ranchero-Software/NetNewsWire/issues/4083) Ios - Option to disable all forms automatic feed refreshing or updating.
- [#4194](https://github.com/Ranchero-Software/NetNewsWire/issues/4194) At refresh time, the list on the left shows the refresh status of each Feed
- [#4374](https://github.com/Ranchero-Software/NetNewsWire/issues/4374) Refresh should also hide the read feed
- [#4539](https://github.com/Ranchero-Software/NetNewsWire/issues/4539) Postpone automatic refresh until WiFi has connected

### Parsing, enclosures and Media RSS (16)

- [#4132](https://github.com/Ranchero-Software/NetNewsWire/issues/4132) Support Media RSS (18, 7.3)
- [#2538](https://github.com/Ranchero-Software/NetNewsWire/issues/2538) Add spport for media:content element (17)
- [#127](https://github.com/Ranchero-Software/NetNewsWire/issues/127) Attachments: show some kinds inline in detail (2)
- [#4114](https://github.com/Ranchero-Software/NetNewsWire/issues/4114) Accept also Gemini logs (1)
- [#4278](https://github.com/Ranchero-Software/NetNewsWire/issues/4278) Add button for comments url in feed (if exists) (1)
- [#4688](https://github.com/Ranchero-Software/NetNewsWire/issues/4688) Add lang attribute equal to value of ‹feed›'s xml:lang (1)
- [#5284](https://github.com/Ranchero-Software/NetNewsWire/issues/5284) Enclosure caching (1)
- [#1851](https://github.com/Ranchero-Software/NetNewsWire/issues/1851) Render articles with HTML language matching feed language, if present
- [#2573](https://github.com/Ranchero-Software/NetNewsWire/issues/2573) Data within category tags not visible?
- [#2758](https://github.com/Ranchero-Software/NetNewsWire/issues/2758) Jsonfeed 1.1 "authors" not supported? (0, 7.4)
- [#3131](https://github.com/Ranchero-Software/NetNewsWire/issues/3131) Display image enclosures (0, 7.3)
- [#4189](https://github.com/Ranchero-Software/NetNewsWire/issues/4189) "‹media:description›" as a summary for the feed
- [#4331](https://github.com/Ranchero-Software/NetNewsWire/issues/4331) Handle enclosures including media:content in Parser (0, 7.3)
- [#4492](https://github.com/Ranchero-Software/NetNewsWire/issues/4492) Could conform to Atom spec better
- [#4685](https://github.com/Ranchero-Software/NetNewsWire/issues/4685) Twtxt
- [#5217](https://github.com/Ranchero-Software/NetNewsWire/issues/5217) BUG: Sort order for events-as-entries doesn't work correctly

### Adding feeds, discovery, OPML and export (30)

- [#3247](https://github.com/Ranchero-Software/NetNewsWire/issues/3247) Allow editing of feed URLs (11)
- [#4869](https://github.com/Ranchero-Software/NetNewsWire/issues/4869) Add web account AT Protocol (8)
- [#978](https://github.com/Ranchero-Software/NetNewsWire/issues/978) Import/Export "Starred" List (5)
- [#985](https://github.com/Ranchero-Software/NetNewsWire/issues/985) Support "file:" feeds (4)
- [#626](https://github.com/Ranchero-Software/NetNewsWire/issues/626) OPML Subscription Lists (3, 7.3)
- [#3166](https://github.com/Ranchero-Software/NetNewsWire/issues/3166) Exporting full-text articles as .ePub files on macOS (3)
- [#3049](https://github.com/Ranchero-Software/NetNewsWire/issues/3049) No feedback on adding feed from share sheet (2)
- [#3748](https://github.com/Ranchero-Software/NetNewsWire/issues/3748) Add Ability to Follow Mastodon Users (2)
- [#3807](https://github.com/Ranchero-Software/NetNewsWire/issues/3807) Enhancement req: Validate feed when adding via share sheet (2)
- [#5394](https://github.com/Ranchero-Software/NetNewsWire/issues/5394) Allow editing subscription URL for existing feeds (2)
- [#3646](https://github.com/Ranchero-Software/NetNewsWire/issues/3646) Add feed preview (1)
- [#282](https://github.com/Ranchero-Software/NetNewsWire/issues/282) Technote: how the feed finder works
- [#700](https://github.com/Ranchero-Software/NetNewsWire/issues/700) Feed name on adding new feed
- [#841](https://github.com/Ranchero-Software/NetNewsWire/issues/841) Different feeds from same base URL not allowed
- [#1006](https://github.com/Ranchero-Software/NetNewsWire/issues/1006) Use Google search as a feed 'registry'
- [#1857](https://github.com/Ranchero-Software/NetNewsWire/issues/1857) Utility feature request: OPML import to preserve home page URLs
- [#1993](https://github.com/Ranchero-Software/NetNewsWire/issues/1993) Permission to add the same RSS feed several times
- [#2441](https://github.com/Ranchero-Software/NetNewsWire/issues/2441) Prefer HTTPS feeds
- [#3385](https://github.com/Ranchero-Software/NetNewsWire/issues/3385) Feed Builder
- [#3482](https://github.com/Ranchero-Software/NetNewsWire/issues/3482) Share Issues from NetNewsWire v Browser
- [#3669](https://github.com/Ranchero-Software/NetNewsWire/issues/3669) Can't Add a New Feed With Unencoded Spaces in the URL
- [#3917](https://github.com/Ranchero-Software/NetNewsWire/issues/3917) Better Fediverse/RSS handling
- [#3945](https://github.com/Ranchero-Software/NetNewsWire/issues/3945) Not detecting duplicate url early
- [#3958](https://github.com/Ranchero-Software/NetNewsWire/issues/3958) Ability to open Mastodon/ActivityPub links in the instance of choice / rewrite Mastodon URLs
- [#4175](https://github.com/Ranchero-Software/NetNewsWire/issues/4175) Option to export subscriptions in OPML 2.0
- [#4341](https://github.com/Ranchero-Software/NetNewsWire/issues/4341) Article export
- [#4592](https://github.com/Ranchero-Software/NetNewsWire/issues/4592) IPad: how to export starred content in smart feeds
- [#4920](https://github.com/Ranchero-Software/NetNewsWire/issues/4920) Move feed from one account to another
- [#5375](https://github.com/Ranchero-Software/NetNewsWire/issues/5375) Autodiscovery for Ghost platform (0, 7.4)
- [#5398](https://github.com/Ranchero-Software/NetNewsWire/issues/5398) Allow "New Feed..." option when right-clicking on Folder (opens Add modal with folder pre-filled for convenience)

### Smart feeds, filtering and search (35)

- [#1864](https://github.com/Ranchero-Software/NetNewsWire/issues/1864) Regex filters (64)
- [#3023](https://github.com/Ranchero-Software/NetNewsWire/issues/3023) Can we add "Read Later" smart feed? (20)
- [#4406](https://github.com/Ranchero-Software/NetNewsWire/issues/4406) Content-based filtering (19)
- [#217](https://github.com/Ranchero-Software/NetNewsWire/issues/217) Design needed: create and edit smart feeds (18)
- [#3030](https://github.com/Ranchero-Software/NetNewsWire/issues/3030) Hide read articles should be available as a global option (13)
- [#870](https://github.com/Ranchero-Software/NetNewsWire/issues/870) All Unread Smart Feed should be cleared after marking all articles as read (9)
- [#2997](https://github.com/Ranchero-Software/NetNewsWire/issues/2997) Sort button per Smart Feed/ Folder (9)
- [#3449](https://github.com/Ranchero-Software/NetNewsWire/issues/3449) Tags for starred items (8)
- [#3423](https://github.com/Ranchero-Software/NetNewsWire/issues/3423) Can't search using Chinese 😇 (6, 7.5)
- [#4522](https://github.com/Ranchero-Software/NetNewsWire/issues/4522) / filtering (4)
- [#560](https://github.com/Ranchero-Software/NetNewsWire/issues/560) Next Unread should stay in list / Smart Feeds (3)
- [#847](https://github.com/Ranchero-Software/NetNewsWire/issues/847) Filter duplicate articles when they are on multiple feeds. (3)
- [#996](https://github.com/Ranchero-Software/NetNewsWire/issues/996) Narrow down search (3)
- [#3862](https://github.com/Ranchero-Software/NetNewsWire/issues/3862) Filter starred articles (3)
- [#2046](https://github.com/Ranchero-Software/NetNewsWire/issues/2046) Searching article text (2)
- [#4184](https://github.com/Ranchero-Software/NetNewsWire/issues/4184) MacOS: Mark All as Read should clear the list when the Hide Read Articles filter is active (2)
- [#2136](https://github.com/Ranchero-Software/NetNewsWire/issues/2136) Option to show or hide Smart Feeds group (1, 7.4)
- [#2216](https://github.com/Ranchero-Software/NetNewsWire/issues/2216) Implement Article inline text search (1)
- [#3766](https://github.com/Ranchero-Software/NetNewsWire/issues/3766) Add an option to make the Unread filter an scene-wide setting. (1)
- [#3777](https://github.com/Ranchero-Software/NetNewsWire/issues/3777) 'ArticleFilter' Proposal (1)
- [#4079](https://github.com/Ranchero-Software/NetNewsWire/issues/4079) “Morning Edition” smart feed (1)
- [#4162](https://github.com/Ranchero-Software/NetNewsWire/issues/4162) Highlight matching article search text (1)
- [#4888](https://github.com/Ranchero-Software/NetNewsWire/issues/4888) Ability to change "today's articles" length (1)
- [#5281](https://github.com/Ranchero-Software/NetNewsWire/issues/5281) Hope the search function will support more languages (1)
- [#118](https://github.com/Ranchero-Software/NetNewsWire/issues/118) View menu or prefs to choose smart feeds in sidebar
- [#952](https://github.com/Ranchero-Software/NetNewsWire/issues/952) "Find" should match on feed titles and other metadata
- [#2027](https://github.com/Ranchero-Software/NetNewsWire/issues/2027) Find should be limited to selected Newsgroups/Folders only as option
- [#3082](https://github.com/Ranchero-Software/NetNewsWire/issues/3082) Handle (e.g. delete) RSS messages by blacklisting Unicode character sets in multilingual feeds
- [#3553](https://github.com/Ranchero-Software/NetNewsWire/issues/3553) Read Feeds Filter toolbar icon for Mac
- [#3630](https://github.com/Ranchero-Software/NetNewsWire/issues/3630) Animate find bar in article search
- [#3926](https://github.com/Ranchero-Software/NetNewsWire/issues/3926) Bulk Updates to Per Feed Settings
- [#3947](https://github.com/Ranchero-Software/NetNewsWire/issues/3947) Option to treat a feed as a "folder" with contents grouped by author
- [#4003](https://github.com/Ranchero-Software/NetNewsWire/issues/4003) Search by author field not working
- [#4733](https://github.com/Ranchero-Software/NetNewsWire/issues/4733) Unable to search articles with ★ character
- [#4807](https://github.com/Ranchero-Software/NetNewsWire/issues/4807) "Feature request" - starred list always ordered new to old

### Marking read, navigation and keyboard (57)

- [#508](https://github.com/Ranchero-Software/NetNewsWire/issues/508) Support for custom keyboard shortcuts (37)
- [#4369](https://github.com/Ranchero-Software/NetNewsWire/issues/4369) Mark as read while scrolling (16)
- [#3399](https://github.com/Ranchero-Software/NetNewsWire/issues/3399) Add Previous/Next Item to Go menu (13)
- [#2449](https://github.com/Ranchero-Software/NetNewsWire/issues/2449) Customize swipe actions (8)
- [#3924](https://github.com/Ranchero-Software/NetNewsWire/issues/3924) Add a button to toggle Mark Articles as Read on Scroll (5)
- [#3491](https://github.com/Ranchero-Software/NetNewsWire/issues/3491) Add Swipe right to mark all as Read (4)
- [#74](https://github.com/Ranchero-Software/NetNewsWire/issues/74) Add pref for automatically marking items as read after n days (3)
- [#1960](https://github.com/Ranchero-Software/NetNewsWire/issues/1960) Swipe left/right on trackpad to navigate to next/previous article on Mac (3)
- [#43](https://github.com/Ranchero-Software/NetNewsWire/issues/43) Make Previous Unread work (2)
- [#869](https://github.com/Ranchero-Software/NetNewsWire/issues/869) Next Unread should skip expanded folders (2)
- [#1113](https://github.com/Ranchero-Software/NetNewsWire/issues/1113) SH+spacebar should trigger page up on detail view (2)
- [#2409](https://github.com/Ranchero-Software/NetNewsWire/issues/2409) Add single key shortcut for Reader View (2)
- [#2824](https://github.com/Ranchero-Software/NetNewsWire/issues/2824) MacOS Feature: Add keyboard shortcut for folders (2)
- [#4149](https://github.com/Ranchero-Software/NetNewsWire/issues/4149) Add a delay before marking items as read (2)
- [#4232](https://github.com/Ranchero-Software/NetNewsWire/issues/4232) Add confirmation option to Mark All as Read on MacOS (2)
- [#4641](https://github.com/Ranchero-Software/NetNewsWire/issues/4641) IOS: please add support for swipes for bottom toolbar buttons (2)
- [#44](https://github.com/Ranchero-Software/NetNewsWire/issues/44) Make Go › Back, Forward work (1)
- [#567](https://github.com/Ranchero-Software/NetNewsWire/issues/567) Hitting space on last in feed should go to next unread in next feed, not folder (regression from NNW3) (1)
- [#3145](https://github.com/Ranchero-Software/NetNewsWire/issues/3145) Option about Mark All as Read button position - iOS (1)
- [#3743](https://github.com/Ranchero-Software/NetNewsWire/issues/3743) Add a toggle for Mark Read Automatically (1)
- [#3804](https://github.com/Ranchero-Software/NetNewsWire/issues/3804) Mark All as Read in Confirmation (1, 7.4)
- [#3976](https://github.com/Ranchero-Software/NetNewsWire/issues/3976) 'Mark All As Read' older than a certain time period (1)
- [#4548](https://github.com/Ranchero-Software/NetNewsWire/issues/4548) Mark All As Read, Except Starred (1, 7.4)
- [#4549](https://github.com/Ranchero-Software/NetNewsWire/issues/4549) Mark as read manually (1)
- [#4709](https://github.com/Ranchero-Software/NetNewsWire/issues/4709) Pull down to refresh, push up to Mark as Read (1)
- [#5441](https://github.com/Ranchero-Software/NetNewsWire/issues/5441) Suggestion - swipe to open article website (1)
- [#490](https://github.com/Ranchero-Software/NetNewsWire/issues/490) Expand/contract keyboard shortcut behavior
- [#662](https://github.com/Ranchero-Software/NetNewsWire/issues/662) Catch Up command
- [#738](https://github.com/Ranchero-Software/NetNewsWire/issues/738) "Mark Read" toolbar item label should be "Toggle Read"
- [#1019](https://github.com/Ranchero-Software/NetNewsWire/issues/1019) When moving from Today to All Unreads, Next Unread or Spacebar should skip past unread posts from today
- [#1164](https://github.com/Ranchero-Software/NetNewsWire/issues/1164) Help user with locating the next line to read when spacebar-based scrolling presents the last page
- [#1214](https://github.com/Ranchero-Software/NetNewsWire/issues/1214) Maintain Focus in Reader Pane When Using Next Unread
- [#1456](https://github.com/Ranchero-Software/NetNewsWire/issues/1456) IOS: Swipe Animation Blocks Selecting Another Article
- [#1971](https://github.com/Ranchero-Software/NetNewsWire/issues/1971) Ios: swipe right to view comments
- [#2415](https://github.com/Ranchero-Software/NetNewsWire/issues/2415) Unread articles are incorrectly marked as read
- [#2529](https://github.com/Ranchero-Software/NetNewsWire/issues/2529) Control-click "Feedly' to Mark All (folders) as read (macOS)
- [#3136](https://github.com/Ranchero-Software/NetNewsWire/issues/3136) Add emacs key to navigate.
- [#3226](https://github.com/Ranchero-Software/NetNewsWire/issues/3226) Articles are marked as read when navigated to with VoiceOver on the Mac
- [#3546](https://github.com/Ranchero-Software/NetNewsWire/issues/3546) Editing Mode for All Unread iOS
- [#3601](https://github.com/Ranchero-Software/NetNewsWire/issues/3601) Key command for marking article unread doesn't work when caps-lock is down
- [#3734](https://github.com/Ranchero-Software/NetNewsWire/issues/3734) Mark All as Read Behavior
- [#3869](https://github.com/Ranchero-Software/NetNewsWire/issues/3869) Could we make the space bar being used for "go to next unread" as an config?
- [#3984](https://github.com/Ranchero-Software/NetNewsWire/issues/3984) Please add a keyboard shortcut for increase/decrease font size (Mac)
- [#4016](https://github.com/Ranchero-Software/NetNewsWire/issues/4016) Internationalise macOS Keyboard Shortcuts
- [#4237](https://github.com/Ranchero-Software/NetNewsWire/issues/4237) Option to move "Mark All As Read" button to the right
- [#4270](https://github.com/Ranchero-Software/NetNewsWire/issues/4270) Setting to disable single-key shortcuts
- [#4525](https://github.com/Ranchero-Software/NetNewsWire/issues/4525) Option for not marking as read until scrolled to the bottom
- [#4805](https://github.com/Ranchero-Software/NetNewsWire/issues/4805) Menu position of "Mark All as Read" inconsistent between iOS and macOS
- [#4839](https://github.com/Ranchero-Software/NetNewsWire/issues/4839) Tap to close article
- [#4887](https://github.com/Ranchero-Software/NetNewsWire/issues/4887) Turning off read receipts
- [#5120](https://github.com/Ranchero-Software/NetNewsWire/issues/5120) Scroll to next unread article
- [#5337](https://github.com/Ranchero-Software/NetNewsWire/issues/5337) Keyboard shortcut to go to the previous article/entry
- [#5354](https://github.com/Ranchero-Software/NetNewsWire/issues/5354) Star & Mark As Read
- [#5377](https://github.com/Ranchero-Software/NetNewsWire/issues/5377) Bring back "jump to last item" from the old NNW
- [#5388](https://github.com/Ranchero-Software/NetNewsWire/issues/5388) (iPadOS): Esc key for closing article preview
- [#5425](https://github.com/Ranchero-Software/NetNewsWire/issues/5425) IOS When all articles in a folder are read, don't move automatically to the next folder
- [#5433](https://github.com/Ranchero-Software/NetNewsWire/issues/5433) Suggestion: Ctrl+D/U for half-page scrolling

### Stars, saving, tags and history (18)

- [#4396](https://github.com/Ranchero-Software/NetNewsWire/issues/4396) Custom Bookmarks/Lists (8)
- [#4271](https://github.com/Ranchero-Software/NetNewsWire/issues/4271) Tags? (5)
- [#4826](https://github.com/Ranchero-Software/NetNewsWire/issues/4826) Support for ‹summary› tags? (3)
- [#512](https://github.com/Ranchero-Software/NetNewsWire/issues/512) Allow to favour feeds (1)
- [#1559](https://github.com/Ranchero-Software/NetNewsWire/issues/1559) Recently read list (1)
- [#1674](https://github.com/Ranchero-Software/NetNewsWire/issues/1674) Preserve starred status when moving feed between accounts (1)
- [#3386](https://github.com/Ranchero-Software/NetNewsWire/issues/3386) MacOS Request: Contextual menu option to save images (1)
- [#3931](https://github.com/Ranchero-Software/NetNewsWire/issues/3931) Highlight management (1)
- [#3946](https://github.com/Ranchero-Software/NetNewsWire/issues/3946) Tag people, places and other names (1)
- [#3957](https://github.com/Ranchero-Software/NetNewsWire/issues/3957) Reading history (1)
- [#4169](https://github.com/Ranchero-Software/NetNewsWire/issues/4169) Count starred articles (1)
- [#4787](https://github.com/Ranchero-Software/NetNewsWire/issues/4787) Deferred Stars (1)
- [#1588](https://github.com/Ranchero-Software/NetNewsWire/issues/1588) Add a number indicator to show how many items are in the Starred smart playlist
- [#2982](https://github.com/Ranchero-Software/NetNewsWire/issues/2982) Ability to pull down to refresh/clear unstarred items in Starred list
- [#3028](https://github.com/Ranchero-Software/NetNewsWire/issues/3028) Long press to clear starred items
- [#4097](https://github.com/Ranchero-Software/NetNewsWire/issues/4097) Save article from link and sync starred
- [#4098](https://github.com/Ranchero-Software/NetNewsWire/issues/4098) Starred items menu: reword "Mark as Unstarred"
- [#5413](https://github.com/Ranchero-Software/NetNewsWire/issues/5413) Bookmarks within Articles

### Sidebar, folders and per-feed settings (73)

- [#1551](https://github.com/Ranchero-Software/NetNewsWire/issues/1551) Image thumbnail on article list instead of feed icon (52)
- [#3401](https://github.com/Ranchero-Software/NetNewsWire/issues/3401) Disable feed (20)
- [#503](https://github.com/Ranchero-Software/NetNewsWire/issues/503) Allow subfolders (15)
- [#3613](https://github.com/Ranchero-Software/NetNewsWire/issues/3613) Allow the user to change feed icons (11)
- [#888](https://github.com/Ranchero-Software/NetNewsWire/issues/888) Choose typeface for list mode and preview (5)
- [#1944](https://github.com/Ranchero-Software/NetNewsWire/issues/1944) Persist feeds sidebar open/close state (5, 7.3)
- [#3177](https://github.com/Ranchero-Software/NetNewsWire/issues/3177) Option to hide site icons in timeline (5)
- [#3679](https://github.com/Ranchero-Software/NetNewsWire/issues/3679) Add option to hide unread labels (5)
- [#356](https://github.com/Ranchero-Software/NetNewsWire/issues/356) Sidebar: allow list of feeds to be sorted (4)
- [#555](https://github.com/Ranchero-Software/NetNewsWire/issues/555) If a feed is unreachable, display an error icon (4)
- [#2388](https://github.com/Ranchero-Software/NetNewsWire/issues/2388) Exclude feed from the Today View (4)
- [#3149](https://github.com/Ranchero-Software/NetNewsWire/issues/3149) Unread count in sidebar is very distracting and demands unwarranted attention (UI suggestion) (3)
- [#3888](https://github.com/Ranchero-Software/NetNewsWire/issues/3888) Allow option to choose icon size of Article list on Mac similar to iOS (3)
- [#4196](https://github.com/Ranchero-Software/NetNewsWire/issues/4196) Add setting to start app in chosen view (3)
- [#911](https://github.com/Ranchero-Software/NetNewsWire/issues/911) NNW should use the first supported favicon (2)
- [#2854](https://github.com/Ranchero-Software/NetNewsWire/issues/2854) Support ⌘-click to toggle all folders (2)
- [#5030](https://github.com/Ranchero-Software/NetNewsWire/issues/5030) IOS: Show unread count in the back button from article view (Mail-style) (2)
- [#5109](https://github.com/Ranchero-Software/NetNewsWire/issues/5109) Disable icons? (2)
- [#973](https://github.com/Ranchero-Software/NetNewsWire/issues/973) Is there any way to hide both sidebars? (1)
- [#1737](https://github.com/Ranchero-Software/NetNewsWire/issues/1737) Mac Feature request: Add 'Move/copy feed to...' item(s) to feed(s) context menu (1)
- [#1877](https://github.com/Ranchero-Software/NetNewsWire/issues/1877) IOS: Allow the Feed Icon to be hidden completely (1)
- [#1962](https://github.com/Ranchero-Software/NetNewsWire/issues/1962) “Silent” Feeds (1)
- [#1982](https://github.com/Ranchero-Software/NetNewsWire/issues/1982) Feed-by-feed option to just display the web page? (iOS) (1)
- [#2148](https://github.com/Ranchero-Software/NetNewsWire/issues/2148) Support for Right to Left languages for IOS (1)
- [#3923](https://github.com/Ranchero-Software/NetNewsWire/issues/3923) Add a "Per Feed Defaults" section to the Settings (1)
- [#4410](https://github.com/Ranchero-Software/NetNewsWire/issues/4410) Manual folder sorting on Mac (1)
- [#4708](https://github.com/Ranchero-Software/NetNewsWire/issues/4708) Pause feed setting (1)
- [#4788](https://github.com/Ranchero-Software/NetNewsWire/issues/4788) Per-Feed Javascript Toggle (1)
- [#5093](https://github.com/Ranchero-Software/NetNewsWire/issues/5093) Hopefully, the folder unread display will be restored (1)
- [#5297](https://github.com/Ranchero-Software/NetNewsWire/issues/5297) Find feeds in sidebar based on feed name (1)
- [#116](https://github.com/Ranchero-Software/NetNewsWire/issues/116) Sidebar: animate undo delete
- [#117](https://github.com/Ranchero-Software/NetNewsWire/issues/117) Sidebar: animate redo delete
- [#506](https://github.com/Ranchero-Software/NetNewsWire/issues/506) Set sort order (ascending, descending) for shared list of items
- [#533](https://github.com/Ranchero-Software/NetNewsWire/issues/533) Sidebar: should be able to drag/drop directly on closed folder and leave it closed
- [#570](https://github.com/Ranchero-Software/NetNewsWire/issues/570) Accessibility: Drag and drop required to rearrange feeds in sidebar menu
- [#826](https://github.com/Ranchero-Software/NetNewsWire/issues/826) Add a ‘Get Info’ contextual menu item to feeds in sidebar
- [#853](https://github.com/Ranchero-Software/NetNewsWire/issues/853) Sort subscriptions by unread count
- [#933](https://github.com/Ranchero-Software/NetNewsWire/issues/933) Collapsible sections in sidebar can't be expanded/collapsed by VoiceOver
- [#1248](https://github.com/Ranchero-Software/NetNewsWire/issues/1248) Mac: don't expand folder when feed dropped on it
- [#1261](https://github.com/Ranchero-Software/NetNewsWire/issues/1261) New Unread articles appear below old Read ones
- [#1907](https://github.com/Ranchero-Software/NetNewsWire/issues/1907) Timeline image doesn’t display after adding Feedly
- [#2522](https://github.com/Ranchero-Software/NetNewsWire/issues/2522) ‹macOS› ‹iOS›  Change Folders and Feed views
- [#2705](https://github.com/Ranchero-Software/NetNewsWire/issues/2705) Feeds disregard their inspector settings when moved between accounts.
- [#2837](https://github.com/Ranchero-Software/NetNewsWire/issues/2837) Add option to deactivate a single web feed
- [#2843](https://github.com/Ranchero-Software/NetNewsWire/issues/2843) IOS Request: Enable multiple web feed selection
- [#2907](https://github.com/Ranchero-Software/NetNewsWire/issues/2907) Move feeds via menu or picker (since dragging can be difficult)
- [#2990](https://github.com/Ranchero-Software/NetNewsWire/issues/2990) Improve account picker when adding a folder (0, 7.4)
- [#3098](https://github.com/Ranchero-Software/NetNewsWire/issues/3098) (mac): adjust thumbnails size
- [#3388](https://github.com/Ranchero-Software/NetNewsWire/issues/3388) IPadOS feature request: Support ctrl+cmd+s to show/hide the sidebar (0, 7.3)
- [#3434](https://github.com/Ranchero-Software/NetNewsWire/issues/3434) New Folder with Selection
- [#3513](https://github.com/Ranchero-Software/NetNewsWire/issues/3513) Access feed settings/delete from Article view
- [#3514](https://github.com/Ranchero-Software/NetNewsWire/issues/3514) Unread Count within articles
- [#3651](https://github.com/Ranchero-Software/NetNewsWire/issues/3651) Can't drag folders on iOS
- [#3757](https://github.com/Ranchero-Software/NetNewsWire/issues/3757) Is there anyway to sign different theme to different rss sources
- [#3937](https://github.com/Ranchero-Software/NetNewsWire/issues/3937) More flexible sidebar (rearranging feeds, easier adding of new feeds)
- [#4219](https://github.com/Ranchero-Software/NetNewsWire/issues/4219) Update sorted feeds, back from the past
- [#4231](https://github.com/Ranchero-Software/NetNewsWire/issues/4231) Option to hide/shrink Feeds section on startup
- [#4279](https://github.com/Ranchero-Software/NetNewsWire/issues/4279) Customizable Icon for Feeds?
- [#4281](https://github.com/Ranchero-Software/NetNewsWire/issues/4281) Allow to create a new folder when adding a new feed via the iOS share function
- [#4332](https://github.com/Ranchero-Software/NetNewsWire/issues/4332) Mac - indicator in sidebar for when hiding read feeds
- [#4336](https://github.com/Ranchero-Software/NetNewsWire/issues/4336) Mac add folder toolbar item
- [#4338](https://github.com/Ranchero-Software/NetNewsWire/issues/4338) Make it possible to reorder accounts in sidebar and in Feeds list
- [#4339](https://github.com/Ranchero-Software/NetNewsWire/issues/4339) Consider minimum 3 digit width for pill shapes in sidebar
- [#4532](https://github.com/Ranchero-Software/NetNewsWire/issues/4532) Selective Feed Syncing (at least on mobile)
- [#4637](https://github.com/Ranchero-Software/NetNewsWire/issues/4637) Handle feed categories with slash as nested or tree
- [#4673](https://github.com/Ranchero-Software/NetNewsWire/issues/4673) Broken feed reports no error?
- [#4859](https://github.com/Ranchero-Software/NetNewsWire/issues/4859) Mac: Adjust favicon size in timeline
- [#4889](https://github.com/Ranchero-Software/NetNewsWire/issues/4889) Ability to limit posts per day from a feed
- [#5166](https://github.com/Ranchero-Software/NetNewsWire/issues/5166) Focused window's sidebar in dark mode has a poor contrast ratio for unread counters [7.0.1b4 macOS 15] (0, 7.5)
- [#5285](https://github.com/Ranchero-Software/NetNewsWire/issues/5285) Restore "Sort Feeds By Unread Count" option (lost in the rewrite)
- [#5357](https://github.com/Ranchero-Software/NetNewsWire/issues/5357) Scale up small feed icons
- [#5393](https://github.com/Ranchero-Software/NetNewsWire/issues/5393) Check home page feed images when feed-specified image is small
- [#5395](https://github.com/Ranchero-Software/NetNewsWire/issues/5395) Flexible Sidebar & Feed Positioning

### Timeline layout and thumbnails (37)

- [#4981](https://github.com/Ranchero-Software/NetNewsWire/issues/4981) Modern UI Refresh for Lists (5)
- [#500](https://github.com/Ranchero-Software/NetNewsWire/issues/500) Add visual de-emphasis for read articles (4)
- [#3302](https://github.com/Ranchero-Software/NetNewsWire/issues/3302) Newspaper view for article list (4)
- [#2432](https://github.com/Ranchero-Software/NetNewsWire/issues/2432) Pre-fetch the next reader view in article list (3)
- [#2999](https://github.com/Ranchero-Software/NetNewsWire/issues/2999) Option to turn off feed summary (3)
- [#866](https://github.com/Ranchero-Software/NetNewsWire/issues/866) Date section headers in article lists (2)
- [#1589](https://github.com/Ranchero-Software/NetNewsWire/issues/1589) Items that are unread and starred only show the star icon (2)
- [#437](https://github.com/Ranchero-Software/NetNewsWire/issues/437) Article display options (1)
- [#3146](https://github.com/Ranchero-Software/NetNewsWire/issues/3146) More difference between title and preview text in article list - iOS (1)
- [#3510](https://github.com/Ranchero-Software/NetNewsWire/issues/3510) Sort feed alphabetically (1)
- [#3522](https://github.com/Ranchero-Software/NetNewsWire/issues/3522) Add option to show full title (1)
- [#4409](https://github.com/Ranchero-Software/NetNewsWire/issues/4409) "Refresh to Clear Read Articles" setting on macOS (1)
- [#4496](https://github.com/Ranchero-Software/NetNewsWire/issues/4496) Turn off article preview in list preview (1)
- [#4683](https://github.com/Ranchero-Software/NetNewsWire/issues/4683) Is it possible to hide the snippet text in the middle feed list to make it more compact? (1)
- [#4925](https://github.com/Ranchero-Software/NetNewsWire/issues/4925) Add timeline text size setting (1)
- [#5363](https://github.com/Ranchero-Software/NetNewsWire/issues/5363) More View Options for the List Page (1)
- [#24](https://github.com/Ranchero-Software/NetNewsWire/issues/24) Show microblog posts in entirety in timeline
- [#1000](https://github.com/Ranchero-Software/NetNewsWire/issues/1000) Line separator between posts
- [#1008](https://github.com/Ranchero-Software/NetNewsWire/issues/1008) Change behavior when clicking blank space in leftmost columns
- [#1594](https://github.com/Ranchero-Software/NetNewsWire/issues/1594) Cosmetic: Empty feed has strange line spacing
- [#2340](https://github.com/Ranchero-Software/NetNewsWire/issues/2340) Display feed articles by time seen by netnewswire
- [#2734](https://github.com/Ranchero-Software/NetNewsWire/issues/2734) (iOS): Option to change view based on feeds
- [#3081](https://github.com/Ranchero-Software/NetNewsWire/issues/3081) List bar font size too big!  can not be little?
- [#4073](https://github.com/Ranchero-Software/NetNewsWire/issues/4073) Distinguish between read and unread title font colors in the Timeline
- [#4186](https://github.com/Ranchero-Software/NetNewsWire/issues/4186) I want to increase the display of 'item › image' thumbnails
- [#4440](https://github.com/Ranchero-Software/NetNewsWire/issues/4440) Text Size options for article list view on macOS
- [#4559](https://github.com/Ranchero-Software/NetNewsWire/issues/4559) Viewing Reads feeds
- [#4739](https://github.com/Ranchero-Software/NetNewsWire/issues/4739) Stronger visual differentiation between title and body in timeline
- [#4964](https://github.com/Ranchero-Software/NetNewsWire/issues/4964) IOS - NNW 7.0: display last updated when showing articles list
- [#4977](https://github.com/Ranchero-Software/NetNewsWire/issues/4977) IOS: Large title navigation bars within feeds lists
- [#5016](https://github.com/Ranchero-Software/NetNewsWire/issues/5016) Empty state
- [#5078](https://github.com/Ranchero-Software/NetNewsWire/issues/5078) Implement Posh Context Menus (0, 7.3)
- [#5139](https://github.com/Ranchero-Software/NetNewsWire/issues/5139) IOS: show last-update date on timeline
- [#5173](https://github.com/Ranchero-Software/NetNewsWire/issues/5173) IOS: Tweaking timeline density
- [#5174](https://github.com/Ranchero-Software/NetNewsWire/issues/5174) Unread count in timeline view uses wrong font
- [#5201](https://github.com/Ranchero-Software/NetNewsWire/issues/5201) Feed name visibility
- [#5448](https://github.com/Ranchero-Software/NetNewsWire/issues/5448) When swiping to mark read/unread, indicator does not update until animation completes

### Storage, retention and performance (13)

- [#1873](https://github.com/Ranchero-Software/NetNewsWire/issues/1873) For iOS: Cache images for off-line reading (14)
- [#4355](https://github.com/Ranchero-Software/NetNewsWire/issues/4355) Clear cache per channel (5)
- [#4272](https://github.com/Ranchero-Software/NetNewsWire/issues/4272) Cache Reader View versions of selected feeds (4)
- [#2289](https://github.com/Ranchero-Software/NetNewsWire/issues/2289) Make retention configurable (2)
- [#4493](https://github.com/Ranchero-Software/NetNewsWire/issues/4493) Cache Cleanup (2)
- [#4163](https://github.com/Ranchero-Software/NetNewsWire/issues/4163) Delete articles manually or in settings, set x days to automatically delete articles (1)
- [#861](https://github.com/Ranchero-Software/NetNewsWire/issues/861) Stats for feeds
- [#2858](https://github.com/Ranchero-Software/NetNewsWire/issues/2858) Version 5.1.3 (3018) (Mac) : cannot delete individual articles
- [#3354](https://github.com/Ranchero-Software/NetNewsWire/issues/3354) UI nonresponsiveness for image previews
- [#4123](https://github.com/Ranchero-Software/NetNewsWire/issues/4123) Automatic deletion of (local) articles
- [#4138](https://github.com/Ranchero-Software/NetNewsWire/issues/4138) Feeds stats
- [#4273](https://github.com/Ranchero-Software/NetNewsWire/issues/4273) Unread articles disappearing unreasonably often
- [#4580](https://github.com/Ranchero-Software/NetNewsWire/issues/4580) Too much refreshing links

### Article view, reader view, images and themes (80)

- [#3165](https://github.com/Ranchero-Software/NetNewsWire/issues/3165) Ability to control dark mode independently from the system (14)
- [#2268](https://github.com/Ranchero-Software/NetNewsWire/issues/2268) Immediately open articles from a feed in the browser (13)
- [#2793](https://github.com/Ranchero-Software/NetNewsWire/issues/2793) Always show web view (iOS) (12)
- [#4438](https://github.com/Ranchero-Software/NetNewsWire/issues/4438) Font size, typeface, and layout customization options. (5)
- [#4818](https://github.com/Ranchero-Software/NetNewsWire/issues/4818) See Approximate Reading Time for Articles (5)
- [#5164](https://github.com/Ranchero-Software/NetNewsWire/issues/5164) Embedded Webview (5)
- [#1986](https://github.com/Ranchero-Software/NetNewsWire/issues/1986) Code Block enhancements (3)
- [#3460](https://github.com/Ranchero-Software/NetNewsWire/issues/3460) Unified Image Handling (3)
- [#4517](https://github.com/Ranchero-Software/NetNewsWire/issues/4517) Catppuccin themes (3)
- [#72](https://github.com/Ranchero-Software/NetNewsWire/issues/72) On open-in-browser, do some kind of feedback or animation (2)
- [#2893](https://github.com/Ranchero-Software/NetNewsWire/issues/2893) Article only view (2)
- [#3426](https://github.com/Ranchero-Software/NetNewsWire/issues/3426) Article Theme Previews (2)
- [#3657](https://github.com/Ranchero-Software/NetNewsWire/issues/3657) Feeds from kill-the-newsletter.com (which converts email newsletters -› RSS) do not display properly (2, 7.5)
- [#3720](https://github.com/Ranchero-Software/NetNewsWire/issues/3720) Future Request: Support change to any installed font on iOS (2)
- [#3874](https://github.com/Ranchero-Software/NetNewsWire/issues/3874) Theme documentation (2, 7.3)
- [#3977](https://github.com/Ranchero-Software/NetNewsWire/issues/3977) Make RSS link element available offline (2)
- [#5240](https://github.com/Ranchero-Software/NetNewsWire/issues/5240) Open Article in New Window (2, 7.5)
- [#5365](https://github.com/Ranchero-Software/NetNewsWire/issues/5365) Add ability to zoom-in on Mac (2)
- [#542](https://github.com/Ranchero-Software/NetNewsWire/issues/542) Code block wrapping shouldn’t wrap (1)
- [#1786](https://github.com/Ranchero-Software/NetNewsWire/issues/1786) Make links that are already known to the app not open a web browser (1)
- [#2720](https://github.com/Ranchero-Software/NetNewsWire/issues/2720) Heuristic for detecting footnotes (1)
- [#2788](https://github.com/Ranchero-Software/NetNewsWire/issues/2788) Add feed title, author, and maybe article title to navigation bar (1)
- [#3381](https://github.com/Ranchero-Software/NetNewsWire/issues/3381) (macOS): Right click on link to Open With (Browser)... (1)
- [#3647](https://github.com/Ranchero-Software/NetNewsWire/issues/3647) Add dark theme to complement current black amoled theme (1)
- [#3650](https://github.com/Ranchero-Software/NetNewsWire/issues/3650) Option to clear browser data (1)
- [#3769](https://github.com/Ranchero-Software/NetNewsWire/issues/3769) Hiding the viewing pane (1)
- [#3847](https://github.com/Ranchero-Software/NetNewsWire/issues/3847) Add Open Article command (1)
- [#3968](https://github.com/Ranchero-Software/NetNewsWire/issues/3968) Allow display of image alt text by default (1)
- [#4484](https://github.com/Ranchero-Software/NetNewsWire/issues/4484) Links in BlueSky posts not recognized as links in NNW (1)
- [#4646](https://github.com/Ranchero-Software/NetNewsWire/issues/4646) Auto download content of reader view mode (1)
- [#4866](https://github.com/Ranchero-Software/NetNewsWire/issues/4866) Links containing URI schemes to external applications do not work on iOS (1, 7.3)
- [#38](https://github.com/Ranchero-Software/NetNewsWire/issues/38) Make View › Zoom commands work
- [#154](https://github.com/Ranchero-Software/NetNewsWire/issues/154) Technote: article UTI and format
- [#518](https://github.com/Ranchero-Software/NetNewsWire/issues/518) Add ARIA support to DetailView web content
- [#582](https://github.com/Ranchero-Software/NetNewsWire/issues/582) Some mechanism for making it easy to find and see updated articles that are already read
- [#647](https://github.com/Ranchero-Software/NetNewsWire/issues/647) Image links should show image in separate window
- [#1680](https://github.com/Ranchero-Software/NetNewsWire/issues/1680) MacOS : Toolbar Browser icon to represent default browser in use
- [#1762](https://github.com/Ranchero-Software/NetNewsWire/issues/1762) In article view, add extra space to solve partial-page scrolling when near the end
- [#2033](https://github.com/Ranchero-Software/NetNewsWire/issues/2033) IOS: Use high resolution image for image zooming if available
- [#2227](https://github.com/Ranchero-Software/NetNewsWire/issues/2227) Text layout with images
- [#2282](https://github.com/Ranchero-Software/NetNewsWire/issues/2282) Code blocks as link text are not obvious
- [#2809](https://github.com/Ranchero-Software/NetNewsWire/issues/2809) No context actions
- [#2954](https://github.com/Ranchero-Software/NetNewsWire/issues/2954) IOS Feature Request: Keyboard shortcut and gesture to close image
- [#2970](https://github.com/Ranchero-Software/NetNewsWire/issues/2970) Link previews that show destination link?
- [#2981](https://github.com/Ranchero-Software/NetNewsWire/issues/2981) Support rel-payment data within atom feeds
- [#3272](https://github.com/Ranchero-Software/NetNewsWire/issues/3272) To make semi-transparent PNG and SVG images more visible in dark mode
- [#3349](https://github.com/Ranchero-Software/NetNewsWire/issues/3349) I want that the experience of clicking images is like Photos, Telegram or iMessage
- [#3684](https://github.com/Ranchero-Software/NetNewsWire/issues/3684) Open link with reader mode on in iOS
- [#3695](https://github.com/Ranchero-Software/NetNewsWire/issues/3695) Add right-click action to the Article Extractor Button to toggle Always User Reader View
- [#3696](https://github.com/Ranchero-Software/NetNewsWire/issues/3696) The Privacy Policy button in the Preferences should be changed to a link
- [#3759](https://github.com/Ranchero-Software/NetNewsWire/issues/3759) Option to preload article URL
- [#3895](https://github.com/Ranchero-Software/NetNewsWire/issues/3895) Moving the reader's view to the centre (macOS)
- [#3932](https://github.com/Ranchero-Software/NetNewsWire/issues/3932) Reader View doesn’t display full articles
- [#3964](https://github.com/Ranchero-Software/NetNewsWire/issues/3964) Show article header and title for the article section while full content is fetching.
- [#3967](https://github.com/Ranchero-Software/NetNewsWire/issues/3967) Theme designer utility?
- [#4001](https://github.com/Ranchero-Software/NetNewsWire/issues/4001) May I ask if the iOS can support Chinese font changes? (0, 7.5)
- [#4054](https://github.com/Ranchero-Software/NetNewsWire/issues/4054) Can't use an alias in Themes
- [#4059](https://github.com/Ranchero-Software/NetNewsWire/issues/4059) Large SVGs should scroll horizontally
- [#4100](https://github.com/Ranchero-Software/NetNewsWire/issues/4100) Specify Browser To Open Links
- [#4197](https://github.com/Ranchero-Software/NetNewsWire/issues/4197) Footnotes cause wrong line breaks
- [#4209](https://github.com/Ranchero-Software/NetNewsWire/issues/4209) MacOS “Clean Up Articles” function unclear
- [#4255](https://github.com/Ranchero-Software/NetNewsWire/issues/4255) Reading articles with VoiceOver
- [#4268](https://github.com/Ranchero-Software/NetNewsWire/issues/4268) IPadOS option to open web links in background
- [#4473](https://github.com/Ranchero-Software/NetNewsWire/issues/4473) Assign different themes for light and dark mode
- [#4561](https://github.com/Ranchero-Software/NetNewsWire/issues/4561) Add Smart Zoom Support
- [#4581](https://github.com/Ranchero-Software/NetNewsWire/issues/4581) JSON feed images not shown
- [#4735](https://github.com/Ranchero-Software/NetNewsWire/issues/4735) Set custom theme for a feed
- [#4738](https://github.com/Ranchero-Software/NetNewsWire/issues/4738) Option to not play animations in feed automatically
- [#4821](https://github.com/Ranchero-Software/NetNewsWire/issues/4821) Option to disable images/Text only mode
- [#4838](https://github.com/Ranchero-Software/NetNewsWire/issues/4838) Way to tell if article view is showing rendered Markdown
- [#4885](https://github.com/Ranchero-Software/NetNewsWire/issues/4885) Waspline Reader plugin
- [#5111](https://github.com/Ranchero-Software/NetNewsWire/issues/5111) Monochrome mode
- [#5194](https://github.com/Ranchero-Software/NetNewsWire/issues/5194) IPad: Reloading the unread articles list
- [#5199](https://github.com/Ranchero-Software/NetNewsWire/issues/5199) Unable to render WeChat official account web page properly
- [#5230](https://github.com/Ranchero-Software/NetNewsWire/issues/5230) 'Always use reader view' requires too many taps on iOS
- [#5291](https://github.com/Ranchero-Software/NetNewsWire/issues/5291) Support for MathJax in reader
- [#5350](https://github.com/Ranchero-Software/NetNewsWire/issues/5350) Open the website instead of detail view
- [#5410](https://github.com/Ranchero-Software/NetNewsWire/issues/5410) Proposal: Chinese/CJK-focused Quiet Reader article theme
- [#5431](https://github.com/Ranchero-Software/NetNewsWire/issues/5431) IPad: option to hide toolbars in full screen article view
- [#5470](https://github.com/Ranchero-Software/NetNewsWire/issues/5470) Open Magnet links (0, 7.2.1)

### Localization and languages (8)

- [#78](https://github.com/Ranchero-Software/NetNewsWire/issues/78) Localizations (7)
- [#851](https://github.com/Ranchero-Software/NetNewsWire/issues/851) Italian localization (2)
- [#1858](https://github.com/Ranchero-Software/NetNewsWire/issues/1858) Localization proposal (2)
- [#850](https://github.com/Ranchero-Software/NetNewsWire/issues/850) Ench localization (1)
- [#5356](https://github.com/Ranchero-Software/NetNewsWire/issues/5356) Any chance Chinese localization could be added in a future release? (1)
- [#849](https://github.com/Ranchero-Software/NetNewsWire/issues/849) German localization
- [#4018](https://github.com/Ranchero-Software/NetNewsWire/issues/4018) Internationalise Account Package
- [#4239](https://github.com/Ranchero-Software/NetNewsWire/issues/4239) Today and list of words to exclude

### Other app chrome, settings and documentation (37)

- [#857](https://github.com/Ranchero-Software/NetNewsWire/issues/857) Open in browser should work on multiple selection (3, 7.4)
- [#3214](https://github.com/Ranchero-Software/NetNewsWire/issues/3214) On iPad customize text size and/or column width (3)
- [#4640](https://github.com/Ranchero-Software/NetNewsWire/issues/4640) IOS: please add an ability to fully customize the bottom toolbar (3)
- [#4660](https://github.com/Ranchero-Software/NetNewsWire/issues/4660) Layered App Icons (3, 7.5)
- [#1478](https://github.com/Ranchero-Software/NetNewsWire/issues/1478) IOS: feed items don’t remember their scroll position (2)
- [#2566](https://github.com/Ranchero-Software/NetNewsWire/issues/2566) Add support to control text size (independently from system settings) (2)
- [#158](https://github.com/Ranchero-Software/NetNewsWire/issues/158) Show feed source command (1)
- [#174](https://github.com/Ranchero-Software/NetNewsWire/issues/174) Cascade new reader windows (1)
- [#3161](https://github.com/Ranchero-Software/NetNewsWire/issues/3161) Surface an "Add Account" option in the "+" Menu (1)
- [#3686](https://github.com/Ranchero-Software/NetNewsWire/issues/3686) Add Mark Above as Read and Mark Below as Read to the Mac toolbar (1)
- [#3738](https://github.com/Ranchero-Software/NetNewsWire/issues/3738) IOS Settings Revamp (1)
- [#4639](https://github.com/Ranchero-Software/NetNewsWire/issues/4639) IOS: please add a config option to disable rubberband effect (1)
- [#5006](https://github.com/Ranchero-Software/NetNewsWire/issues/5006) Move settings to the top (1)
- [#49](https://github.com/Ranchero-Software/NetNewsWire/issues/49) Add Validate Feed command (probably in sidebar contextual menu and gear menu) (0, 7.3)
- [#159](https://github.com/Ranchero-Software/NetNewsWire/issues/159) Cmd-. to cancel refresh
- [#160](https://github.com/Ranchero-Software/NetNewsWire/issues/160) Cmd-. to cancel find-feed
- [#206](https://github.com/Ranchero-Software/NetNewsWire/issues/206) Command to go to author’s home page
- [#937](https://github.com/Ranchero-Software/NetNewsWire/issues/937) Use system appearance for indeterminate progress window (0, 7.5)
- [#2064](https://github.com/Ranchero-Software/NetNewsWire/issues/2064) Version Number Misplaced (0, 7.3)
- [#2621](https://github.com/Ranchero-Software/NetNewsWire/issues/2621) Interface idea: Favorites bar, like in Mail
- [#3237](https://github.com/Ranchero-Software/NetNewsWire/issues/3237) Feed Format in Info Window
- [#3362](https://github.com/Ranchero-Software/NetNewsWire/issues/3362) MacOS: colored "toggle-able" toolbar icons
- [#3402](https://github.com/Ranchero-Software/NetNewsWire/issues/3402) Cannot fully reset all preferences
- [#3547](https://github.com/Ranchero-Software/NetNewsWire/issues/3547) Add Install Section To README
- [#3608](https://github.com/Ranchero-Software/NetNewsWire/issues/3608) MacOS: Feed rename in Sidebar isn't an undoable command
- [#4070](https://github.com/Ranchero-Software/NetNewsWire/issues/4070) Advanced features
- [#4257](https://github.com/Ranchero-Software/NetNewsWire/issues/4257) Clicking in empty space of middle column should move keyboard focus
- [#4335](https://github.com/Ranchero-Software/NetNewsWire/issues/4335) Mac add feed toolbar item
- [#4741](https://github.com/Ranchero-Software/NetNewsWire/issues/4741) How to transfer Netnewswire settings?
- [#4801](https://github.com/Ranchero-Software/NetNewsWire/issues/4801) Surface Hidden Preferences as "Advanced Preferences" in Preferences Pane
- [#4825](https://github.com/Ranchero-Software/NetNewsWire/issues/4825) WordLand's "shadow" feed has source:markdown elements
- [#5085](https://github.com/Ranchero-Software/NetNewsWire/issues/5085) Properly categorize keyboard shortcuts in Menu Bar on iPad (0, 7.4)
- [#5086](https://github.com/Ranchero-Software/NetNewsWire/issues/5086) Add support for copying the build number from NetNewsWire settings (0, 7.3)
- [#5157](https://github.com/Ranchero-Software/NetNewsWire/issues/5157) IOS RFE — color contrast for “next” button
- [#5324](https://github.com/Ranchero-Software/NetNewsWire/issues/5324) Add optional single-column navigation stack reading mode
- [#5374](https://github.com/Ranchero-Software/NetNewsWire/issues/5374) (re-) allow persistent feed list panel beside content on vertical iPad mini
- [#5407](https://github.com/Ranchero-Software/NetNewsWire/issues/5407) Make feeds editable from their title on reader view
