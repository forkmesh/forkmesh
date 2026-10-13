import FMBase
import Foundation
import XCTest

@testable import FMInsights

final class StatsFormatTests: XCTestCase {
    func testCompactNumbersMatchTheDesktop() {
        XCTAssertEqual(StatsFormat.compactNumber(1284), "1,284")
        XCTAssertEqual(StatsFormat.compactNumber(12900), "12.9K")
        XCTAssertEqual(StatsFormat.compactNumber(4_200_000), "4.20M")
        XCTAssertEqual(StatsFormat.compactNumber(42), "42")
        XCTAssertEqual(StatsFormat.compactNumber(2.25), "2.2")
        XCTAssertEqual(StatsFormat.compactNumber(250_000), "250K")
        XCTAssertEqual(StatsFormat.compactNumber(3_000_000_000), "3.0B")
        XCTAssertEqual(StatsFormat.compactBytes(1536), "1.5 KB")
        XCTAssertEqual(StatsFormat.compactBytes(512), "512 B")
        XCTAssertEqual(StatsFormat.compactBytes(50 * 1024 * 1024), "50 MB")
        XCTAssertEqual(StatsFormat.compactUsd(12.5), "$12.50")
        XCTAssertEqual(StatsFormat.compactUsd(-12.5), "-$12.50")
        XCTAssertEqual(StatsFormat.compactUsd(1284), "$1,284")
        XCTAssertEqual(StatsFormat.compactDuration(3 * 3600 * 1000 + 5 * 60 * 1000), "3h 5m")
        XCTAssertEqual(StatsFormat.compactDuration(59_000), "59s")
        XCTAssertEqual(StatsFormat.compactDuration(50 * 3600 * 1000), "2d 2h")
    }

    func testTrendComparesTheNewerHalfWithTheOlderHalf() {
        XCTAssertEqual(StatsFormat.trendPercent([1, 4, 2, 12])!, 180, accuracy: 0.01)
        XCTAssertEqual(StatsFormat.trendPercent([10, 10, 7, 5, 5])!, -50, accuracy: 0.01)
        XCTAssertNil(StatsFormat.trendPercent([0, 0, 3, 3]))
        XCTAssertNil(StatsFormat.trendPercent([1, 2, 3]))
        XCTAssertNil(StatsFormat.trendPercent([1, 4, 2, 12], categorical: true))
    }

    func testTrendToneFollowsPolarity() {
        let rising = StatsTileSeries(values: [1, 1, 5, 5])
        XCTAssertEqual(StatsTileData(series: rising, trend: .upIsGood).trendTone, .good)
        XCTAssertEqual(StatsTileData(series: rising, trend: .downIsGood).trendTone, .critical)
        XCTAssertEqual(StatsTileData(series: rising).trendTone, StatsTileStatus.none)
        XCTAssertNil(StatsTileData(series: StatsTileSeries(values: [100, 100, 100.5, 100.5])).shownTrend)
        XCTAssertNil(StatsTileData(series: rising, available: false).shownTrend)
    }

    func testTrendTextMatchesTheDesktopChip() {
        XCTAssertEqual(StatsFormat.trendText(4.12), "\u{25B2} 4.1%")
        XCTAssertEqual(StatsFormat.trendText(-38.6), "\u{25BC} 39%")
        XCTAssertEqual(StatsFormat.trendText(2500), "\u{25B2} >999%")
    }
}

final class StatsHistoryTests: XCTestCase {
    func testSamplesAreSpacedCappedAndKeptPerScope() {
        let prefs = InMemoryKeyValueStore()
        let store = StatsHistoryStore(store: prefs, scope: "next")
        let minute: Int64 = 60_000
        store.record(["nodes": 3, "repos": 30], at: 0)
        store.record(["nodes": 4], at: 5 * minute)
        var history = store.load()
        XCTAssertEqual(history["nodes"], [StatsSample(at: 0, value: 4)])
        store.record(["nodes": 5], at: 11 * minute)
        history = store.load()
        XCTAssertEqual(history["nodes"]?.map(\.value), [4, 5])
        XCTAssertEqual(history["repos"]?.count, 1)
        XCTAssertTrue(StatsHistoryStore(store: prefs, scope: "other").load().isEmpty)
        for step in 0..<100 { store.record(["nodes": Double(step)], at: (20 + Int64(step) * 11) * minute) }
        XCTAssertEqual(store.load()["nodes"]?.count, StatsHistoryStore.maxSamples)
        XCTAssertEqual(store.load()["nodes"]?.last?.value, 99)
    }

    func testHistoryFillsSeriesOnlyForReadingsWithoutTheirOwn() {
        let samples = [StatsSample(at: 1, value: 2), StatsSample(at: 2, value: 4)]
        let own = StatsTileSeries(values: [1, 2, 3, 4])
        let tiles = StatsTiles.withHistory(
            ["a": StatsTileData(value: "4", reading: 4), "b": StatsTileData(series: own, reading: 9),
             "c": StatsTileData(value: "x"), "d": StatsTileData(value: "1", reading: 1)],
            ["a": samples, "b": samples, "c": samples, "d": [samples[0]]])
        XCTAssertEqual(tiles["a"]?.series.values, [2, 4])
        XCTAssertEqual(tiles["b"]?.series, own)
        XCTAssertTrue(tiles["c"]?.series.isEmpty ?? false)
        XCTAssertTrue(tiles["d"]?.series.isEmpty ?? false)
    }
}

final class CodemagicUsageTests: XCTestCase {
    func testUsageParsesTheDesktopAnswerAndColoursTheTile() throws {
        let usage = try XCTUnwrap(CodemagicUsage(json: JSON.parseObject(
            #"{"limitSeconds":30000,"usedSeconds":3815,"remainingSeconds":26185,"previousSeconds":31787,"paidSeconds":0,"resetsAt":1793491200000}"#)))
        XCTAssertEqual(usage.limitMinutes, 500)
        XCTAssertEqual(usage.remainingMinutes, 436)
        XCTAssertEqual(usage.usedMinutes, 64)
        XCTAssertEqual(usage.tone, .good)
        var calendar = Calendar(identifier: .gregorian)
        calendar.locale = Locale(identifier: "en_US_POSIX")
        calendar.timeZone = TimeZone(identifier: "UTC")!
        let tile = try XCTUnwrap(StatsTiles.codemagic(usage, calendar: calendar)["codemagic"])
        XCTAssertEqual(tile.value, "436 min")
        XCTAssertEqual(tile.note, "of 500 left \u{00B7} resets Nov 1")
        XCTAssertEqual(tile.status, .good)
        XCTAssertEqual(tile.reading, 436)

        let low = try XCTUnwrap(CodemagicUsage(json: JSON.parseObject(#"{"limitSeconds":30000,"usedSeconds":25000}"#)))
        XCTAssertEqual(low.tone, .warning)
        let out = try XCTUnwrap(CodemagicUsage(json: JSON.parseObject(#"{"limitSeconds":30000,"usedSeconds":31000}"#)))
        XCTAssertEqual(out.remainingMinutes, 0)
        XCTAssertEqual(out.tone, .critical)
        XCTAssertNil(CodemagicUsage(json: JSON.parseObject(#"{"usedSeconds":10}"#)))
    }
}

final class StatsLayoutTests: XCTestCase {
    let catalog = ["repos", "issues", "tokens", "cost", "peers", "agents"].map {
        StatsTileCatalogEntry(id: $0, title: $0, group: "g", hint: "", symbol: "circle")
    }
    let defaults = [
        StatsTilePlacement(id: "repos", width: 2), StatsTilePlacement(id: "issues"), StatsTilePlacement(id: "tokens"),
        StatsTilePlacement(id: "cost"), StatsTilePlacement(id: "peers"),
    ]

    func testJsonRoundTripClampsSpansAndDropsIdlessEntries() throws {
        let layout = [StatsTilePlacement(id: "repos", width: 2), StatsTilePlacement(id: "issues", height: 2)]
        let back = StatsLayout.fromJSON(StatsLayout.toJSON(layout))
        XCTAssertEqual(back, layout)
        let odd = try JSON.parse(#"[{"id":"x","w":9,"h":0},{"w":1,"h":1}]"#)
        let clamped = StatsLayout.fromJSON(odd)
        XCTAssertEqual(clamped, [StatsTilePlacement(id: "x", width: 2, height: 1)])
    }

    func testFirstLaunchStartsFromTheDefaultsAndOffersEverything() {
        let first = StatsLayout.merge(saved: [], hasSaved: false, defaults: defaults, catalog: catalog, offered: [])
        XCTAssertEqual(first.layout.map(\.id), defaults.map(\.id))
        XCTAssertTrue(first.changed)
        XCTAssertEqual(first.offered.count, catalog.count)
    }

    func testAnOldBoardKeepsItsOrderAndGainsNewTilesOnce() {
        let old = [StatsTilePlacement(id: "cost", width: 2), StatsTilePlacement(id: "repos")]
        let grown = StatsLayout.merge(saved: old, hasSaved: true, defaults: defaults, catalog: catalog, offered: [])
        XCTAssertEqual(grown.layout.map(\.id), ["cost", "repos", "issues", "tokens", "peers"])
        XCTAssertEqual(grown.layout[0].width, 2)
        XCTAssertTrue(grown.changed)

        let again = StatsLayout.merge(saved: grown.layout, hasSaved: true, defaults: defaults, catalog: catalog, offered: grown.offered)
        XCTAssertEqual(again.layout, grown.layout)
        XCTAssertFalse(again.changed)

        let trimmed = grown.layout.filter { $0.id != "tokens" }
        let kept = StatsLayout.merge(saved: trimmed, hasSaved: true, defaults: defaults, catalog: catalog, offered: grown.offered)
        XCTAssertFalse(kept.layout.map(\.id).contains("tokens"))
        XCTAssertFalse(kept.changed)

        let emptied = StatsLayout.merge(saved: [], hasSaved: true, defaults: defaults, catalog: catalog, offered: grown.offered)
        XCTAssertTrue(emptied.layout.isEmpty)
        XCTAssertFalse(emptied.changed)

        let foreign = grown.layout + [StatsTilePlacement(id: "from-a-newer-build", height: 2)]
        let preserved = StatsLayout.merge(saved: foreign, hasSaved: true, defaults: defaults, catalog: catalog, offered: grown.offered)
        XCTAssertEqual(preserved.layout.last?.id, "from-a-newer-build")
        XCTAssertEqual(StatsLayout.visible(preserved.layout, catalog: catalog).count, grown.layout.count)
    }

    func testStoreKeepsTheBoardPerScopeAndResetRestoresDefaults() {
        let prefs = InMemoryKeyValueStore()
        let store = StatsLayoutStore(store: prefs, scope: "next")
        let first = store.load()
        XCTAssertEqual(first.layout, StatsCatalog.defaults)
        store.save(Array(first.layout.dropFirst()), offered: first.offered)
        XCTAssertEqual(store.load().layout, Array(StatsCatalog.defaults.dropFirst()))
        XCTAssertEqual(StatsLayoutStore(store: prefs, scope: "other").load().layout, StatsCatalog.defaults)
        store.reset()
        XCTAssertEqual(store.load().layout, StatsCatalog.defaults)
    }

    func testCatalogDefaultsAreAllKnownTiles() {
        let ids = Set(StatsCatalog.entries.map(\.id))
        XCTAssertEqual(ids.count, StatsCatalog.entries.count)
        for placement in StatsCatalog.defaults { XCTAssertTrue(ids.contains(placement.id), placement.id) }
    }
}
