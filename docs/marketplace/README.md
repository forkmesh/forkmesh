# Marketplace

The Marketplace is how optional features ship in ForkMesh. Each package lives
in `marketplace/packages/<id>/`, is listed in `marketplace/catalog.json`, and
can add a desktop page, a web page with a server module, a World module and a
phone screen through generic host sockets. These pages are the developer
documentation; the package format, lifecycle and publishing rules are in
[`marketplace/README.md`](../../marketplace/README.md), and planned plugins in
[plugins/](../plugins/README.md).

## How the Marketplace works

| Page | Covers |
| --- | --- |
| [package-system.md](package-system.md) | Code map, host sockets, desktop page contract, server module runtime, manifest fields, build, test, release, security and traps |
| [plugins.md](plugins.md) | Plugins versus switches, and which plugins a build adds to the core (`marketplace/plugins.json`) |
| [core-plugins.md](core-plugins.md) | Built-in features as account-synced switches, off by default in new projects |
| [market.md](market.md) | Packs and guides, sorting, Install all, share links, public package pages, the plugin page menu, preview images |
| [ratings.md](ratings.md) | Star ratings, required improvement reviews, maintainer replies and moderation |

## Package pages

| Page | Covers |
| --- | --- |
| [browser-extension.md](browser-extension.md) | `browser-extension`: the Chrome extension for the vault, CRM, Marketing and repository mirroring ([user guide](../browser-extension.md)) |
| [codebase-analysis.md](codebase-analysis.md) | `code-map`, `size-map` and the analysis engine shared by Strata, Ember and Synapse, on desktop and phones |
| [strata.md](strata.md) | `strata`: architecture and complexity (modules, cycles, rules, debt, quality gates) |
| [ember.md](ember.md) | `ember`: file-level hotspots |
| [synapse.md](synapse.md) | `synapse`: the import graph as a 3D brain |
| [status-page.md](status-page.md) | `status-page` and its `status-check-*` packages |
| [app-store-status.md](app-store-status.md) | `app-store-status`: the iOS app as App Store Connect sees it |
| [cleanup.md](cleanup.md) | `cleanup`: what agent runs leave on disk, and confirmed deletion |
| [computer-security.md](computer-security.md) | `computer-security`: the desktop security checkup and fixes |
| [apparmor.md](apparmor.md) | `apparmor`: AppArmor profiles, confinement and denials on Linux |
| [phone-security.md](phone-security.md) | `phone-security`: the iPhone and Android security checkup |
| [runners.md](runners.md) | `runners`: your agent runners on desktop and phones |
| [usage.md](usage.md) | `usage`: coding-agent account usage on desktop, iPhone and Android |
| [openwrt.md](openwrt.md) | `openwrt`: an OpenWrt router and its USB storage |
| [files.md](files.md) | `files`: device files and SMB network shares |
| [nexus.md](nexus.md) | `nexus`: desktop network inspection |
| [secure-drive.md](secure-drive.md) | `secure-drive`: the encrypted shared drive format (`fmsd1`) |
| [invoices.md](invoices.md) | `invoices`: CRM-linked invoices with open, view and payment tracking |
| [booking.md](booking.md) | `booking`: a personal booking page at one short link, confirmations emailed |
| [spreadsheets.md](spreadsheets.md) | `spreadsheets`: the spreadsheet editor; opens and saves `.xls`, `.xlsx`, `.ods` and CSV; AI assistant |
| [surveys.md](surveys.md) | `surveys`: surveys answered through a private link, with results and CSV export |
| [rss-reader.md](rss-reader.md) | `rss-reader`: a local-only feed reader |
| [instance-feed.md](instance-feed.md) | `instance-feed` (Feed): one instance's timeline; `feed` (Network Feed) |
| [mastodon.md](mastodon.md) | `mastodon`: the Mastodon client on the web, phones and desktop |
| [discourse-mirror.md](discourse-mirror.md) | `discourse-mirror`: a read-only copy of a public Discourse forum in a Git repository |
| [poker.md](poker.md) | `poker`: Texas Hold'em for virtual chips |
| [video-mix-dj-studio.md](video-mix-dj-studio.md) | `video-mix-dj-studio`: the DJ studio on web, desktop and iOS (user guide: [DJ Studio jams](../dj-studio.md)) |
| [video-mix-dj-studio-performance.md](video-mix-dj-studio-performance.md) | The DJ studio's iOS performance checks, read by its diagnostics agent |
| [face-changer-desktop.md](face-changer-desktop.md) | `face-changer`: the desktop virtual camera |
| [eye-control.md](eye-control.md) | `eye-control`: gaze cursor and blink to tap on the iPhone |
| [silent-speech.md](silent-speech.md) | `silent-speech`: words mouthed without a sound, read from earbud head motion on both phones |
| [tour.md](tour.md) | `tour`: the first-run welcome and guided tour |
| [people.md](people.md) | `people`: members seated around a campfire |
| [aquarium.md](aquarium.md) | `aquarium`: agents as fish over the desktop and iPhone |
| [spider-room.md](spider-room.md) | `spider-room`: the Genie spider in the room in augmented reality |

## Packages without a page here

Each has its own README in `marketplace/packages/<id>/`.

- **Productivity and data:** `notes`, `tasks`, `calendar`, `timers`, `crm`,
  `competitor`, `ledger`, `vault` (end-to-end encrypted, suite `fmv1`),
  `docs` (the `docs` library on desktop and phones), `workflows` with its
  nested `workflows-*` packs ([../app/workflows.md](../app/workflows.md)).
- **Health:** `calories`, `steps`, `water`, `sleep`, `sugar-break`,
  `stretch`, `food-guide` (account-scoped, sealed per account).
- **Operations:** `analytics`, `marketing`, `sentry`, `backups`.
- **Desktop and phone extras:** `capture` (on the iPhone, long-press a blank
  area to select, mark up and copy part of the screen;
  [../native/CAPTURE_MARKUP.md](../native/CAPTURE_MARKUP.md)), `discord`,
  `browser-extension`, `design-system`, `sign-in`, `speech`, `glasses-alert`,
  `winamp` (theme), `gemma-4` (agent model).
- **Face and voice:** `face-changer` also has a web page and a native Face
  Changer screen in the phone apps whose face, background and voice choices
  video calls use; `voice-changer` is a desktop virtual microphone plus a web
  page. Both web pages run the call engine from `app/public` and share
  `app/public/call-studio.js`
  ([../app/call-filters.md](../app/call-filters.md#plug-ins)).
- **World:** `forkmesh-sky-office`, `reddit-banner-board`, `x-banner-board`
  ([World](../world/README.md)).
- **Catalog only:** `forkmesh-ide` (VS Code extension source).

## Related

Core pages: [architecture](../architecture.md),
[encryption](../encryption.md), [self-hosting](../self-hosting.md),
[threat model](../protocol/threat-model.md). Hosts:
[desktop](../desktop/README.md), [app](../app/README.md),
[World](../world/README.md). Back to the [documentation home](../README.md).
