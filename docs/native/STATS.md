# Stats board on the phone

The phone's **Stats** screen is a board of tiles, like the desktop's rail
**Stats** page ([../desktop/diagnostics.md](../desktop/diagnostics.md), "Stats
page"). Tiles can be added, removed, made wide and reordered. The layout is
kept per project.

- Android: `core/.../insights/StatsBoard.kt`, `StatsBoardModel.kt`,
  `DesktopTiles.kt`, `LiveReleases.kt`, and the screen in
  `app/.../feature/insights/InsightsScreens.kt`.
- iOS: `ForkMeshCore/Sources/FMInsights/` (the same file names) and
  `ForkMesh/Features/Insights/StatsScreen.swift`.

## Where each tile comes from

| Group | Tiles | Source |
| --- | --- | --- |
| Network | repositories, nodes, hosts, node activity | `GET /api/network/stats`, `GET /api/network/online-history` |
| Status | uptime, errors, systems down | `GET /api/status` |
| Desktop | load, memory, agents, stalls | the paired desktop's `diag.health` |
| Builds (iOS) | Codemagic minutes | `stats.codemagic` |
| Releases | latest release, Android, macOS, Windows, Linux, iOS | signed release manifests; App Store status |
| Every desktop group | every tile on the desktop's Stats page | the paired desktop's `stats.tiles` |

## Every desktop tile

The phone offers every tile in the desktop's `statsTileCatalog()`. On the
phone their ids start with `desktop.` (so `desktop.cpu`), because a few desktop
ids (`repos`, `agents`, `uptime`, `stalls`) already name a phone tile with
different data.

`stats.tiles` is a request in the owner-only `stats.` control family. The
desktop reads every catalog tile through the same `statsTileData()` its own
board uses and replies:

```json
{"sampledAt": 1791853731000,
 "tiles": [{"id": "cpu", "title": "CPU", "group": "This machine", "hint": "...",
            "value": "12%", "note": "...", "unit": "", "available": true,
            "status": "good", "trend": "down",
            "series": {"labels": [], "values": [], "categorical": false, "tones": []}}]}
```

- `status` is `none`, `good`, `warning` or `critical`. `trend` is `neutral`,
  `up` (up is good) or `down` (down is good).
- `StatsDashboard::tileToJson` builds each tile. It keeps the newest 60 points
  of a series, cuts titles and values to 80 characters and hints and notes to
  240, and sends `tones` only when there is one tone for every point.
- If the desktop's Stats page is not refreshing, the request takes one sample
  first (`sampleStatsHistory`), so the realtime tiles have a reading.
- Admin-only tiles (`users`, `users-activity`, `users-world`) are sent only by
  an admin's desktop.

How the phone shows them:

- The phone keeps a copy of the desktop catalog (`DesktopTiles.entries` on both
  platforms) for titles, groups, hints and the default layout. Every desktop
  tile is in the default layout, as on the desktop, and the long series are
  wide.
- A desktop tile is shown, and offered under **Add**, only if the desktop's
  last answer included it. The phone saves that set as
  `insights/stats/desktop-tiles.v1`, so the tiles show on the next launch
  before the desktop answers.
  - With no paired desktop, the board has no desktop tiles.
  - An older desktop answers `unsupported`. The phone then hides the desktop
    tiles and says "Update ForkMesh on your desktop to see all of its stats
    here."
  - Any other failure keeps the tiles and shows the reason in them.
- A tile id the copy does not know (sent by a newer desktop) is ignored.
- `app/tests/test_mobile_stats_desktop_parity.py` fails when the desktop
  catalog and either phone's copy differ. To add a desktop tile, add the same
  `DesktopTileEntry` to `DesktopTiles.kt` and `DesktopTiles.swift`.

## Release versions

The **Releases** tiles show what the homepage offers for download
(`www/public/homepage-downloads.js`), worked out the same way:

1. One `GET /api/repo/forkmesh/forkmesh/blobs?path=...&ref=main` reads
   `.forkmesh/releases/latest/release.json`, its `.sig` and
   `desktop/CMakeLists.txt`.
2. The manifest must verify against the homepage's Ed25519 release key (the
   last 32 bytes of the SPKI key in the homepage script). It must also have
   schema `forkmesh-release-v2`, repo `forkmesh/forkmesh`, an `assets` list and
   a string `tag`. An asset counts only with a 64-hex `blob_sha256` and a size
   above zero.
3. The Linux AppImage and Android APK come from `latest` only; if one is
   missing, its tile reads "Not in <tag>".
4. Windows and both macOS DMGs fall back, like the homepage, to the newest of
   the six earlier `candidate-x.y.(z-1…z-6)` manifests that verifies and
   carries them. Such a tile turns amber and says "behind <tag>". If no
   manifest has the asset, the tile reads "Coming soon".
5. **Latest release** shows the version in `desktop/CMakeLists.txt` if it is
   newer than `latest` and its `candidate-` manifest verifies with a build the
   homepage offers. It then notes that downloads are still the older version.
   Otherwise it shows `latest`'s tag and the number of verified downloads.

The candidate manifests come in one more `blobs` request. **iOS version** reads
`GET /api/app-store-status`
([../marketplace/app-store-status.md](../marketplace/app-store-status.md)). It
shows the newest version in `READY_FOR_SALE` or `READY_FOR_DISTRIBUTION`, plus
the newer version's state if one is waiting. That snapshot can be read only by
the account that published it, so on other accounts the tile says why it has
no data.

## Testing

- Android: `InsightsTest` and `LiveReleasesTest`
  (`./gradlew :core:coreTest -Pfilter=Insights`, `-Pfilter=LiveReleases`).
  `LiveReleasesTest` verifies the real v1.2.8 manifest in
  `core/src/test/resources/releases/` against the homepage key.
- iOS: `FMInsightsTests` (`FM_ONLY=FMInsights swift test`), including
  `LiveReleasesTests.swift`, which holds the same manifest.
- Desktop: `forkmesh-stats-dashboard-tests` covers `tileToJson`.
- `app/tests/test_mobile_stats_desktop_parity.py` checks catalog parity, the
  `stats.tiles` method on all three, the Android icons, and that both phones
  use the homepage's key and asset names.
