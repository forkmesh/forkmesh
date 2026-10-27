import FMApi
import FMBase
import FMChat
import FMControl
import Observation
import XCTest
@testable import FMAgents

final class AgentLinesTests: XCTestCase {
    private let repo = Repository(owner: "forkmesh", name: "forkmesh")

    func testShortAgeMatchesDesktop() {
        let now = 1_000_000_000_000
        XCTAssertEqual(AgentLines.shortAge(ms: 0, nowMs: now), "")
        XCTAssertEqual(AgentLines.shortAge(ms: now + 5_000, nowMs: now), "now")
        XCTAssertEqual(AgentLines.shortAge(ms: now - 42_000, nowMs: now), "42s")
        XCTAssertEqual(AgentLines.shortAge(ms: now - 5 * 60_000, nowMs: now), "5m")
        XCTAssertEqual(AgentLines.shortAge(ms: now - 3 * 3_600_000, nowMs: now), "3h")
        XCTAssertEqual(AgentLines.shortAge(ms: now - 2 * 86_400_000, nowMs: now), "2d")
        XCTAssertEqual(AgentLines.shortAge(ms: now - 14 * 86_400_000, nowMs: now), "2w")
        XCTAssertEqual(AgentLines.shortAge(ms: now - 60 * 86_400_000, nowMs: now), "2mo")
        XCTAssertEqual(AgentLines.shortAge(ms: now - 800 * 86_400_000, nowMs: now), "2y")
        XCTAssertEqual(AgentLines.count(120), "120")
        XCTAssertEqual(AgentLines.count(1200), "999+")
    }

    func testListViewCyclesCompactZenFullEagleAndReadsWhatWasStored() {
        XCTAssertEqual(AgentListView.compact.next, .zen)
        XCTAssertEqual(AgentListView.zen.next, .full)
        XCTAssertEqual(AgentListView.full.next, .eagle)
        XCTAssertEqual(AgentListView.eagle.next, .compact)
        XCTAssertEqual(AgentListView.eagle.caption, "Eagle")
        XCTAssertEqual(AgentListView(stored: "full"), .full)
        XCTAssertEqual(AgentListView(stored: "eagle"), .eagle)
        XCTAssertEqual(AgentListView(stored: nil), .eagle)
        XCTAssertEqual(AgentListView(stored: "grid"), .eagle)
        XCTAssertEqual(AgentListView(stored: "compact"), .compact)
        XCTAssertEqual(AgentListView.storageKey, "agents/listView")
    }

    func testListViewSwitchesToEagleOnceThenKeepsTheChoice() {
        XCTAssertEqual(AgentListView(stored: "compact", eagleDefaulted: false), .eagle)
        XCTAssertEqual(AgentListView(stored: nil, eagleDefaulted: false), .eagle)
        XCTAssertEqual(AgentListView(stored: "compact", eagleDefaulted: true), .compact)
        XCTAssertEqual(AgentListView(stored: "zen", eagleDefaulted: true), .zen)
        XCTAssertEqual(AgentListView(stored: nil, eagleDefaulted: true), .eagle)
        XCTAssertEqual(AgentListView.eagleDefaultKey, "agents/listViewEagleDefault")
    }

    func testListSortCyclesAndDefaultsToMostRecent() {
        XCTAssertEqual(AgentLineSort(stored: nil), .recent)
        XCTAssertEqual(AgentLineSort(stored: "bogus"), .recent)
        XCTAssertEqual(AgentLineSort(stored: "repo"), .repo)
        XCTAssertEqual(AgentLineSort.storageKey, "agents/listSort")
        var seen: [AgentLineSort] = []
        var sort = AgentLineSort.recent
        repeat {
            seen.append(sort)
            sort = sort.next
        } while sort != .recent
        XCTAssertEqual(seen, [.recent, .running, .status, .priority, .name, .repo, .changes])
        XCTAssertEqual(seen.map(\.caption), ["Recent", "Running", "Status", "Priority", "Name", "Repo", "Changes"])
    }

    func testListSortOrdersLines() {
        func line(_ id: String, _ title: String, _ status: AgentLineStatus, active: Bool, at: Int, repo: String, added: Int)
            -> AgentLine {
            AgentLine(
                id: id, source: .control(UnifiedAgentRow(
                    id: id, badge: .desktopLan, title: title, repoOwner: "forkmesh", repoName: repo, node: "studio",
                    status: status.rawValue, statusLabel: title, isActive: active)),
                title: title, face: "feather", status: status, statusLabel: title, isActive: active,
                updatedAtMs: at, repoName: repo, linesAdded: added)
        }
        let old = line("a", "Bravo", .running, active: true, at: 100, repo: "zeta", added: 10)
        let new = line("b", "alpha", .success, active: false, at: 300, repo: "alpha", added: 500)
        var mid = line("c", "Charlie", .waiting, active: true, at: 200, repo: "Mid", added: -1)
        mid.priority = 3
        let lines = [old, new, mid]
        XCTAssertEqual(AgentLineSort.recent.sorted(lines).map(\.id), ["b", "c", "a"])
        XCTAssertEqual(AgentLineSort.running.sorted(lines).map(\.id), ["c", "a", "b"])
        XCTAssertEqual(AgentLineSort.status.sorted(lines).map(\.id), ["c", "a", "b"])
        XCTAssertEqual(AgentLineSort.name.sorted(lines).map(\.id), ["b", "a", "c"])
        XCTAssertEqual(AgentLineSort.repo.sorted(lines).map(\.id), ["b", "c", "a"])
        XCTAssertEqual(AgentLineSort.changes.sorted(lines).map(\.id), ["b", "a", "c"])
        XCTAssertEqual(AgentLineSort.priority.sorted(lines).map(\.id), ["c", "b", "a"])
    }

    func testFullChipsNameEachDetailInOneWordInColumnOrder() {
        let now = 1_000_000_000_000
        let line = AgentLine(
            id: "a", source: .control(UnifiedAgentRow(
                id: "a", badge: .desktopLan, title: "t", repoOwner: "forkmesh", repoName: "forkmesh", node: "studio",
                status: "running", statusLabel: "Running", isActive: true)),
            title: "Tidy", face: "feather", status: .running, statusLabel: "Running", isActive: true,
            updatedAtMs: now - 8 * 60_000, repoName: "forkmesh", branch: "agent/1", hasWorktree: true,
            behind: 3, filesChanged: 4, linesAdded: 5285, linesRemoved: 74, provider: "claude-code")
        let chips = AgentLines.chips(line, nowMs: now)
        XCTAssertEqual(chips.map(\.slot), [.status, .provider, .host, .age, .sync, .files, .churn, .branch])
        XCTAssertEqual(
            chips.map(\.word), ["Running", "Claude", "Desktop", "8m", "Behind", "4", "+5.2k\u{2212}74", "Worktree"])
        for chip in chips {
            XCTAssertFalse(chip.word.contains(" "))
        }

        var quiet = line
        quiet.status = .success
        quiet.branch = ""
        quiet.cloud = true
        quiet.provider = "codex"
        quiet.conflicted = true
        XCTAssertEqual(
            AgentLines.chips(quiet, nowMs: now).map(\.word), ["Success", "Codex", "Cloud", "8m", "Conflicts", "4",
                                                               "+5.2k\u{2212}74"])

        var onRunner = quiet
        onRunner.runner = "fmnext-r1"
        XCTAssertEqual(AgentLines.chips(onRunner, nowMs: now).first { $0.slot == .host }?.word, "fmnext-r1")
        XCTAssertEqual(AgentLines.runnerWord("very-long-runner-name"), "very-long\u{2026}")
        XCTAssertEqual(AgentLines.runnerWord(" "), "Runner")
        var second = onRunner
        second.runner = "FMNext-R1"
        var finished = onRunner
        finished.isActive = false
        var elsewhere = onRunner
        elsewhere.runner = "fmnext-r2"
        var local = onRunner
        local.runner = ""
        XCTAssertEqual(
            AgentLines.runnerCounts([onRunner, second, finished, elsewhere, local]), ["fmnext-r1": 2, "fmnext-r2": 1])
        XCTAssertEqual(AgentLines.compactCount(12_345), "12k")
        XCTAssertEqual(AgentLines.compactCount(999), "999")
    }

    func testStatusKinds() {
        XCTAssertEqual(AgentLines.status("running", merged: false, active: true), .running)
        XCTAssertEqual(AgentLines.status("security-pending", merged: false, active: true), .queued)
        XCTAssertEqual(AgentLines.status("waiting-for-input", merged: false, active: true), .waiting)
        XCTAssertEqual(AgentLines.status("completed", merged: false, active: false), .success)
        XCTAssertEqual(AgentLines.status("completed", merged: true, active: false), .merged)
        XCTAssertEqual(AgentLines.status("cancelled", merged: false, active: false), .stopped)
        XCTAssertEqual(AgentLines.status("rejected", merged: false, active: false), .failed)
        XCTAssertEqual(AgentLines.status("", merged: false, active: false), .other)
    }

    func testDesktopListJSONCarriesDiffWorktreeAndImage() {
        let json: JSONObject = [
            "id": .int(7), "repositoryOwner": .string("forkmesh"), "repositoryName": .string("forkmesh"),
            "status": .string("running"), "provider": .string("claude-code"), "model": .string("claude-opus-5-5"),
            "branchName": .string("agent/7"), "worktree": .bool(true), "priority": .int(12),
            "backgroundTasks": .int(2), "tokensIn": .int(48_000), "tokensOut": .int(900),
            "promptTokens": .int(10), "completionTokens": .int(20),
            "diffStats": .object([
                "files": .int(4), "ahead": .int(2), "behind": .int(0), "added": .int(30), "removed": .int(5),
            ]),
            "attachments": .array([
                .object([
                    "id": .string("0123456789abcdef0123456789abcdef"), "name": .string("shot.png"),
                    "mediaType": .string("image/png"), "size": .int(100),
                ]),
            ]),
        ]
        let row = UnifiedAgentRow.fromDesktop(LocalAgentRow.fromJSON(json), transport: .lan, desktop: "studio")
        let line = AgentLines.line(row)
        XCTAssertEqual(line.face, "opus")
        XCTAssertEqual(line.status, .running)
        XCTAssertEqual(line.branch, "agent/7")
        XCTAssertTrue(line.hasWorktree)
        XCTAssertEqual(line.ahead, 2)
        XCTAssertEqual(line.filesChanged, 4)
        XCTAssertEqual(line.linesAdded, 30)
        XCTAssertEqual(line.linesRemoved, 5)
        XCTAssertEqual(line.priority, 12)
        XCTAssertEqual(line.backgroundTasks, 2)
        XCTAssertEqual(line.tokensIn, 48_000)
        XCTAssertEqual(line.tokensOut, 900)
        XCTAssertTrue(line.accessibilitySummary.contains("2 background tasks"))
        XCTAssertEqual(line.thumbnail?.cacheKey, "desktop:7:0123456789abcdef0123456789abcdef")
        let quiet = LocalAgentRow.fromJSON(["id": .int(8), "status": .string("done")])
        XCTAssertEqual(AgentLines.line(UnifiedAgentRow.fromDesktop(quiet, transport: .lan, desktop: "studio")).backgroundTasks, 0)
        let older = LocalAgentRow.fromJSON([
            "id": .int(9), "status": .string("done"), "promptTokens": .int(70), "completionTokens": .int(30),
        ])
        let olderLine = AgentLines.line(UnifiedAgentRow.fromDesktop(older, transport: .lan, desktop: "studio"))
        XCTAssertEqual(olderLine.tokensIn, 70)
        XCTAssertEqual(olderLine.tokensOut, 30)
    }

    func testTileBadgesFollowTheListIconsMostUrgentFirst() {
        var line = AgentLines.line(UnifiedAgentRow.fromDesktop(
            LocalAgentRow.fromJSON(["id": .int(1), "status": .string("running")]), transport: .lan, desktop: "studio"))
        XCTAssertEqual(AgentLineBadge.of(line), [])
        line.branch = "agent/1"
        line.hasWorktree = true
        line.ahead = 2
        line.behind = 3
        line.prNumber = 9
        line.cloud = true
        XCTAssertEqual(AgentLineBadge.of(line), [.behind, .worktree, .branch, .ahead, .pull, .cloud])
        line.conflicted = true
        line.runner = "box"
        XCTAssertEqual(AgentLineBadge.of(line), [.conflicted, .worktree, .branch, .ahead, .pull, .runner])
    }

    func testTokenPaceRisesWithSpeedAndInputCountsOnATenfoldScale() {
        XCTAssertEqual(AgentTokenPace.of(perSecond: 0, input: false), .idle)
        XCTAssertEqual(AgentTokenPace.of(perSecond: 5, input: false), .slow)
        XCTAssertEqual(AgentTokenPace.of(perSecond: 30, input: false), .steady)
        XCTAssertEqual(AgentTokenPace.of(perSecond: 100, input: false), .fast)
        XCTAssertEqual(AgentTokenPace.of(perSecond: 500, input: false), .blazing)
        XCTAssertEqual(AgentTokenPace.of(perSecond: 100, input: true), .slow)
        XCTAssertEqual(AgentTokenPace.of(perSecond: 5_000, input: true), .blazing)
        XCTAssertEqual(AgentTokenPace.allCases.map(\.travel), AgentTokenPace.allCases.map(\.travel).sorted())
    }

    func testTokenSamplerMeasuresGrowthFadesWhenItStopsAndCapsLongGaps() {
        var sampler = AgentTokenSampler()
        sampler.sample(tokensIn: 1_000, tokensOut: 100, atMs: 10_000)
        XCTAssertEqual(sampler.outPace, .idle)
        sampler.sample(tokensIn: 3_000, tokensOut: 200, atMs: 12_000)
        XCTAssertEqual(sampler.inPerSecond, 1_000, accuracy: 0.001)
        XCTAssertEqual(sampler.outPerSecond, 50, accuracy: 0.001)
        XCTAssertEqual(sampler.outPace, .steady)
        sampler.sample(tokensIn: 3_000, tokensOut: 200, atMs: 13_000)
        let faded = 50 * pow(0.5, 1 / AgentTokenSampler.halfLifeSeconds)
        XCTAssertEqual(sampler.outPerSecond, faded, accuracy: 0.001)
        XCTAssertEqual(sampler.outPace, .steady)
        sampler.sample(tokensIn: 3_000, tokensOut: 1_000, atMs: 73_000)
        XCTAssertEqual(sampler.outPerSecond, (faded + 100) / 2, accuracy: 0.001)
        for second in 1...20 {
            sampler.sample(tokensIn: 3_000, tokensOut: 1_000, atMs: 73_000 + second * 1_000)
        }
        XCTAssertEqual(sampler.outPerSecond, 0)
        XCTAssertEqual(sampler.outPace, .idle)
        XCTAssertEqual(sampler.outTravel, 0)
        sampler.sample(tokensIn: 3_000, tokensOut: 1_200, atMs: 95_000)
        XCTAssertEqual(sampler.outPerSecond, 100, accuracy: 0.001)
        XCTAssertEqual(sampler.outTravel, 12, accuracy: 0.001)
        sampler.sample(tokensIn: 0, tokensOut: 0, atMs: 96_000)
        XCTAssertEqual(sampler.inPerSecond, 0)
        XCTAssertEqual(sampler.outPerSecond, 0)
        sampler.sample(tokensIn: 10, tokensOut: 400, atMs: 97_000)
        XCTAssertGreaterThan(sampler.outPerSecond, 0)
        sampler.settle()
        XCTAssertEqual(sampler.outPerSecond, 0)
    }

    func testTokenSamplerHoldFadesTheRateWithoutNewTotals() {
        var sampler = AgentTokenSampler()
        sampler.hold(atMs: 1_000)
        XCTAssertEqual(sampler.outPerSecond, 0)
        sampler.sample(tokensIn: 0, tokensOut: 0, atMs: 1_000)
        sampler.sample(tokensIn: 0, tokensOut: 100, atMs: 2_000)
        sampler.hold(atMs: 3_500)
        XCTAssertEqual(sampler.outPerSecond, 50, accuracy: 0.001)
        sampler.sample(tokensIn: 0, tokensOut: 150, atMs: 4_500)
        XCTAssertEqual(sampler.outPerSecond, 50, accuracy: 0.001)
    }

    func testTokenTravelGrowsWithTheRateUpToACap() {
        XCTAssertEqual(AgentTokenPace.travel(perSecond: 0.5, input: false), 0)
        XCTAssertEqual(AgentTokenPace.travel(perSecond: 1, input: false), 4, accuracy: 0.001)
        XCTAssertEqual(AgentTokenPace.travel(perSecond: 100, input: false), 12, accuracy: 0.001)
        XCTAssertEqual(AgentTokenPace.travel(perSecond: 1_000, input: true), 12, accuracy: 0.001)
        XCTAssertEqual(AgentTokenPace.travel(perSecond: 1e9, input: false), AgentTokenPace.maxTravel)
    }

    func testTokenSeriesChartsPerSecondGrowthAcrossSources() {
        var series = AgentTokenSeries()
        series.tick(["desktop": [1_000, 50]])
        XCTAssertEqual(series.input, [0])
        XCTAssertEqual(series.output, [0])
        series.tick(["desktop": [4_000, 80], "row:r": [500, 5]])
        XCTAssertEqual(series.input, [0, 3_000])
        XCTAssertEqual(series.output, [0, 30])
        series.tick(["desktop": [3_000, 90], "row:r": [600, 9]])
        XCTAssertEqual(series.input.last, 100)
        XCTAssertEqual(series.output.last, 14)
        for _ in 0..<(AgentTokenSeries.seconds + 3) { series.tick(["desktop": [3_000, 90]]) }
        XCTAssertEqual(series.output.count, AgentTokenSeries.seconds)
        XCTAssertEqual(series.output.last, 0)
    }

    func testTokenSeriesTotalsPreferTheDesktopFeedOverItsRows() {
        var desk = UnifiedAgentRow(
            id: "desktop:7", badge: .desktopLan, title: "t", repoOwner: "o", repoName: "r", node: "studio",
            status: "running", statusLabel: "Running", isActive: true, localId: 7)
        desk.tokensIn = 10
        desk.tokensOut = 2
        var cloud = UnifiedAgentRow(
            id: "run:1", badge: .node, title: "c", repoOwner: "o", repoName: "r", node: "runner",
            status: "running", statusLabel: "Running", isActive: true)
        cloud.tokensIn = 30
        cloud.tokensOut = 4
        XCTAssertEqual(
            AgentTokenSeries.totals(desktop: [], rows: [desk, cloud]),
            ["row:desktop:7": [10, 2], "row:run:1": [30, 4]])
        XCTAssertEqual(
            AgentTokenSeries.totals(desktop: [900, 60], rows: [desk, cloud]),
            ["desktop": [900, 60], "row:run:1": [30, 4]])
    }

    func testEagleDialogIsTheLatestFinishedProseTheAgentSaid() {
        let transcript = "Looking at the **list** view.\n\n[Read AgentLineRow.swift]\n\nNow wiring the `ticker`\nacross the card.\n\nhalf a li"
        XCTAssertEqual(AgentLines.dialog(transcript), "Now wiring the ticker across the card.")
        XCTAssertEqual(AgentLines.dialog("Done.\n\n[Read a.swift]\n"), "Done.")
        XCTAssertEqual(AgentLines.dialog("- Checked the **list**\n\n[Bash swift test]\n!! exit 1\n==> Task done: build\n"), "Checked the list")
        XCTAssertEqual(AgentLines.dialog("> fix it\n\n[Bash swift test]\n\n```\nlet a = 1\n```\n"), "")
        XCTAssertEqual(AgentLines.dialog("still typing"), "")
        XCTAssertEqual(AgentLines.dialog(""), "")
        XCTAssertEqual(AgentLines.dialog(String(repeating: "word ", count: 60) + "\n", limit: 20), "word word word word\u{2026}")
    }

    func testEagleDialogOfACloudRunPrefersItsLiveOutput() {
        let said = AgentLaunchMessage(role: "assistant", text: "Opened the pull request.")
        var session = AgentLaunchSession(
            sessionId: "s1", status: "running", history: [AgentLaunchMessage(role: "user", text: "fix it"), said])
        XCTAssertEqual(AgentLines.dialog(session), "Opened the pull request.")
        session.liveText = "Running the tests\n"
        XCTAssertEqual(AgentLines.dialog(session), "Running the tests")
        session.status = "completed"
        XCTAssertEqual(AgentLines.dialog(session), "Opened the pull request.")
    }

    @MainActor
    func testEagleFeedHandsEachRunningDesktopRowItsLatestLine() {
        var deltas: [String: @MainActor (AgentTranscriptDelta) -> Void] = [:]
        let feeds = ControlWatchFeeds { row, _, onDelta in
            deltas[row.id] = onDelta
            return nil
        }
        let live = UnifiedAgentRow(
            id: "desktop:7", badge: .desktopLan, title: "t", repoOwner: "forkmesh", repoName: "ForkMesh", node: "studio",
            status: "running", statusLabel: "Running", isActive: true, localId: 7)
        let idle = UnifiedAgentRow(
            id: "desktop:9", badge: .desktopLan, title: "o", repoOwner: "forkmesh", repoName: "forkmesh", node: "studio",
            status: "done", statusLabel: "Done", isActive: false, localId: 9)
        let feed = AgentEagleFeed()
        feed.follow([live, idle]) { feeds }
        XCTAssertEqual(deltas.keys.sorted(), ["desktop:7"])
        deltas["desktop:7"]?(AgentTranscriptDelta(text: "Running the tests.\n\n[Bash swift test]\n", next: 38, reset: true, active: true, status: "running"))
        let entry = AgentLines.line(.desktop(repo, AgentSession(id: 7, status: "running")))
        XCTAssertEqual(feed.dialog(entry), "Running the tests.")
        XCTAssertEqual(feed.dialog(AgentLines.line(live)), "Running the tests.")
        XCTAssertEqual(feed.dialog(AgentLines.line(idle)), "")
        let tools = String(repeating: "[Bash swift test]\n\n", count: 600)
        deltas["desktop:7"]?(AgentTranscriptDelta(text: tools, next: 38 + tools.utf8.count, reset: false, active: true, status: "running"))
        XCTAssertEqual(feed.dialog(entry), "Running the tests.")
        feed.stop()
        XCTAssertEqual(feed.dialog(entry), "")
    }

    @MainActor
    func testEagleFeedKeepsTheLastLineOfAStoppedRun() async {
        var deltas: [String: @MainActor (AgentTranscriptDelta) -> Void] = [:]
        let feeds = ControlWatchFeeds { row, _, onDelta in
            deltas[row.id] = onDelta
            return nil
        }
        let live = UnifiedAgentRow(
            id: "desktop:7", badge: .desktopLan, title: "t", repoOwner: "forkmesh", repoName: "forkmesh", node: "studio",
            status: "running", statusLabel: "Running", isActive: true, localId: 7)
        var done = live
        done.isActive = false
        done.status = "done"
        let idle = UnifiedAgentRow(
            id: "desktop:9", badge: .desktopLan, title: "o", repoOwner: "forkmesh", repoName: "forkmesh", node: "studio",
            status: "done", statusLabel: "Done", isActive: false, localId: 9)
        var fetched: [String] = []
        let feed = AgentEagleFeed()
        feed.follow([live, idle], feeds: { feeds }) { row in
            fetched.append(row.id)
            return "> fix it\n\nShipped the fix.\n\n[Bash swift test]\n"
        }
        deltas["desktop:7"]?(AgentTranscriptDelta(text: "Running the tests.\n\n[Bash swift test]\n", next: 38, reset: true, active: true, status: "running"))
        feed.follow([done, idle], feeds: { feeds })
        XCTAssertEqual(feed.dialog(AgentLines.line(done)), "Running the tests.")
        feed.recall(AgentLines.line(done))
        let entry = AgentLines.line(.desktop(repo, AgentSession(id: 9, status: "done")))
        XCTAssertEqual(feed.dialog(entry), "")
        feed.recall(entry)
        feed.recall(AgentLines.line(idle))
        for _ in 0..<20 where feed.dialog(entry).isEmpty { await Task.yield() }
        XCTAssertEqual(feed.dialog(entry), "Shipped the fix.")
        XCTAssertEqual(feed.dialog(AgentLines.line(idle)), "Shipped the fix.")
        XCTAssertEqual(fetched, ["desktop:9"])
        feed.stop()
    }

    @MainActor
    func testEagleFeedRedrawsOnlyTheRowWhoseLineChanged() {
        var deltas: [String: @MainActor (AgentTranscriptDelta) -> Void] = [:]
        let feeds = ControlWatchFeeds { row, _, onDelta in
            deltas[row.id] = onDelta
            return nil
        }
        let first = UnifiedAgentRow(
            id: "desktop:7", badge: .desktopLan, title: "t", repoOwner: "forkmesh", repoName: "forkmesh", node: "studio",
            status: "running", statusLabel: "Running", isActive: true, localId: 7)
        let second = UnifiedAgentRow(
            id: "desktop:8", badge: .desktopLan, title: "u", repoOwner: "forkmesh", repoName: "forkmesh", node: "studio",
            status: "running", statusLabel: "Running", isActive: true, localId: 8)
        let feed = AgentEagleFeed()
        feed.follow([first, second]) { feeds }
        deltas["desktop:7"]?(AgentTranscriptDelta(text: "Reading.\n\n[Read a.swift]\n", next: 10, reset: true, active: true, status: "running"))
        deltas["desktop:8"]?(AgentTranscriptDelta(text: "Testing.\n\n[Bash swift test]\n", next: 10, reset: true, active: true, status: "running"))
        var firstRedraws = 0
        var secondRedraws = 0
        withObservationTracking { _ = feed.dialog(AgentLines.line(first)) } onChange: { firstRedraws += 1 }
        withObservationTracking { _ = feed.dialog(AgentLines.line(second)) } onChange: { secondRedraws += 1 }
        deltas["desktop:8"]?(AgentTranscriptDelta(text: "Fixed it.\n\n[Edit a.swift]\n", next: 30, reset: true, active: true, status: "running"))
        XCTAssertEqual(feed.dialog(AgentLines.line(second)), "Fixed it.")
        XCTAssertEqual(firstRedraws, 0, "another run's line does not redraw this row")
        XCTAssertEqual(secondRedraws, 1)
        feed.stop()
        XCTAssertEqual(feed.dialog(AgentLines.line(first)), "")
    }

    @MainActor
    func testEagleFeedReadsOnlyTheTranscriptThatMoved() {
        var deltas: [String: @MainActor (AgentTranscriptDelta) -> Void] = [:]
        let feeds = ControlWatchFeeds { row, _, onDelta in
            deltas[row.id] = onDelta
            return nil
        }
        let rows = (1...6).map { (n: Int) -> UnifiedAgentRow in
            UnifiedAgentRow(
                id: "desktop:\(n)", badge: .desktopLan, title: "t\(n)", repoOwner: "forkmesh", repoName: "forkmesh",
                node: "studio", status: "running", statusLabel: "Running", isActive: true, localId: n)
        }
        var read: [String] = []
        let feed = AgentEagleFeed { (text: String) -> String in
            read.append(text)
            return AgentLines.dialog(text)
        }
        feed.follow(rows) { feeds }
        for row in rows {
            deltas[row.id]?(AgentTranscriptDelta(
                text: "Line \(row.localId).\n\n", next: 9, reset: true, active: true, status: "running"))
        }
        read.removeAll()
        for step in 0..<10 {
            deltas["desktop:3"]?(AgentTranscriptDelta(
                text: "Step \(step).\n\n", next: 18 + step * 9, reset: false, active: true, status: "running"))
        }
        XCTAssertEqual(read.count, 10, "a delta re-reads its own run's transcript, not every followed one")
        XCTAssertTrue(read.allSatisfy { $0.hasPrefix("Line 3.") })
        XCTAssertEqual(feed.dialog(AgentLines.line(rows[2])), "Step 9.")
        XCTAssertEqual(feed.dialog(AgentLines.line(rows[0])), "Line 1.")
        feed.stop()
    }

    func testMergeShowsEachRunOnceAcrossRepositories() {
        let pushed = AgentSession(id: 7, status: "running", branchName: "agent/7", createdAtMs: 100)
        let queued = AgentLaunchSession(sessionId: "s1", owner: "forkmesh", repo: "forkmesh", status: "completed", updatedAtMs: 50)
        let entries: [AgentEntry] = [
            .desktop(repo, pushed),
            .launched(repo, queued, [], cloud: true),
        ]
        let live = UnifiedAgentRow(
            id: "desktop:7", badge: .desktopLan, title: "t", repoOwner: "forkmesh", repoName: "forkmesh", node: "studio",
            status: "running", statusLabel: "Running", isActive: true, localId: 7, filesChanged: 3, hasWorktree: true)
        let other = UnifiedAgentRow(
            id: "desktop:9", badge: .desktopLan, title: "other repo", repoOwner: "me", repoName: "site", node: "studio",
            status: "done", statusLabel: "Done", isActive: false, updatedAtMs: 500, localId: 9)
        let node = UnifiedAgentRow(
            id: "node:s1", badge: .node, title: "q", repoOwner: "forkmesh", repoName: "forkmesh", node: "m1",
            status: "completed", statusLabel: "Completed", isActive: false, cloudSessionId: "s1")
        let lines = AgentLines.merge(entries: entries, rows: [live, other, node])
        XCTAssertEqual(lines.map(\.id), ["desktop:forkmesh/forkmesh:7", "control:desktop:9", "queued:forkmesh/forkmesh:s1"])
        XCTAssertEqual(lines[0].filesChanged, 3)
        XCTAssertTrue(lines[0].hasWorktree)
        XCTAssertEqual(lines[1].repoName, "site")
        XCTAssertTrue(lines[2].cloud)
    }

    func testQueuedRunTakesItsStateFromTheNewerAccountRead() {
        let stale = AgentLaunchSession(sessionId: "s1", owner: "forkmesh", repo: "forkmesh", status: "running", updatedAtMs: 50)
        var ended = UnifiedAgentRow(
            id: "node:s1", badge: .node, title: "q", repoOwner: "forkmesh", repoName: "forkmesh", node: "m1",
            status: "failed", statusLabel: "stopped", isActive: false, updatedAtMs: 90, cloudSessionId: "s1")
        ended.finishedAtMs = 90
        let settled = AgentLines.merge(entries: [.launched(repo, stale, [], cloud: true)], rows: [ended])
        XCTAssertEqual(settled.map(\.id), ["queued:forkmesh/forkmesh:s1"])
        XCTAssertFalse(settled[0].isActive)
        XCTAssertEqual(settled[0].status, .failed)
        XCTAssertEqual(settled[0].statusLabel, "stopped")
        XCTAssertEqual(settled[0].finishedAtMs, 90)

        var older = ended
        older.updatedAtMs = 10
        let kept = AgentLines.merge(entries: [.launched(repo, stale, [], cloud: true)], rows: [older])
        XCTAssertTrue(kept[0].isActive)
        XCTAssertEqual(kept[0].status, .running)
    }

    func testCloudRunIsCountedOnceWhenListedTwiceOrClaimedByTheDesktopInReach() {
        let run = AgentLaunchSession(
            sessionId: "s1", owner: "forkmesh", repo: "forkmesh", status: "running", targetNode: "studio",
            updatedAtMs: 50, localAgentId: 7)
        let mirror = Repository(owner: "forkmesh-org", name: "forkmesh")
        let twice = AgentLines.merge(
            entries: [.launched(repo, run, [], cloud: true), .launched(mirror, run, [], cloud: true)], rows: [])
        XCTAssertEqual(twice.filter(\.isActive).count, 1)

        let desktopRun = UnifiedAgentRow(
            id: "desktop:7", badge: .desktopLan, title: "t", repoOwner: "forkmesh", repoName: "ForkMesh", node: "Studio",
            status: "running", statusLabel: "Running", isActive: true, localId: 7)
        let claimed = AgentLines.merge(
            entries: [.desktop(repo, AgentSession(id: 7, status: "running")), .launched(repo, run, [], cloud: true)],
            rows: [desktopRun])
        XCTAssertEqual(claimed.map(\.id), ["desktop:forkmesh/forkmesh:7"])

        var elsewhere = run
        elsewhere.targetNode = "m1"
        let other = AgentLines.merge(entries: [.launched(repo, elsewhere, [], cloud: true)], rows: [desktopRun])
        XCTAssertEqual(other.filter(\.isActive).count, 2)

        var unclaimed = run
        unclaimed.localAgentId = 0
        let waiting = AgentLines.merge(entries: [.launched(repo, unclaimed, [], cloud: true)], rows: [desktopRun])
        XCTAssertEqual(waiting.filter(\.isActive).count, 2)
    }

    func testEveryCloudRunOpensTheAgentRunScreen() {
        let queued = AgentLaunchSession(sessionId: "s1", status: "queued", updatedAtMs: 50)
        let fresh = AgentLines.runRow(repo, queued, rows: [])
        XCTAssertEqual(fresh?.id, "node:s1")
        XCTAssertEqual(fresh?.repoOwner, "forkmesh")
        XCTAssertEqual(fresh?.repoName, "forkmesh")
        XCTAssertEqual(fresh?.cloudSessionId, "s1")
        let live = UnifiedAgentRow(
            id: "node:s1", badge: .node, title: "q", repoOwner: "forkmesh", repoName: "forkmesh", node: "m1",
            status: "running", statusLabel: "Running", isActive: true, cloudSessionId: "s1")
        XCTAssertEqual(AgentLines.runRow(repo, queued, rows: [live])?.statusLabel, "Running")
        var onRunner = queued
        onRunner.runner = "fmnext-r1"
        XCTAssertEqual(AgentLines.runRow(repo, onRunner, rows: [])?.runner, "fmnext-r1")
        XCTAssertNil(AgentLines.runRow(repo, AgentLaunchSession(sessionId: ""), rows: []))
    }

    func testADesktopQueuedRunOpensTheAgentRunScreen() {
        var queued = AgentLaunchSession(sessionId: "s2", status: "queued", targetNode: "studio", updatedAtMs: 50)
        let waiting = AgentLines.runRow(repo, queued, rows: [])
        XCTAssertEqual(waiting?.id, "node:s2")
        XCTAssertEqual(waiting?.node, "studio")
        queued.localAgentId = 9
        let desktop = UnifiedAgentRow(
            id: "studio:9", badge: .desktopLan, title: "q", repoOwner: "forkmesh", repoName: "forkmesh", node: "studio",
            status: "running", statusLabel: "Working", isActive: true, localId: 9)
        XCTAssertEqual(AgentLines.runRow(repo, queued, rows: [desktop])?.id, "studio:9")
    }

    func testWatchKeysMatchTheDotsWatchTargets() {
        let pushed = AgentSession(id: 7, status: "running")
        let queued = AgentLaunchSession(sessionId: "s1", owner: "forkmesh", repo: "forkmesh", status: "running", updatedAtMs: 50)
        let live = UnifiedAgentRow(
            id: "desktop:7", badge: .desktopLan, title: "t", repoOwner: "forkmesh", repoName: "ForkMesh", node: "studio",
            status: "running", statusLabel: "Running", isActive: true, localId: 7)
        let node = UnifiedAgentRow(
            id: "node:s1", badge: .node, title: "q", repoOwner: "forkmesh", repoName: "forkmesh", node: "m1",
            status: "running", statusLabel: "Running", isActive: true, cloudSessionId: "s1")
        let rows = [live, node]
        XCTAssertEqual(
            AgentLines.watchKeys(AgentLines.line(.desktop(repo, pushed)), rows: rows),
            ["control:desktop:7", "relay:forkmesh/forkmesh#7"])
        XCTAssertEqual(AgentLines.watchKeys(AgentLines.line(.desktop(repo, pushed)), rows: []), ["relay:forkmesh/forkmesh#7"])
        XCTAssertEqual(AgentLines.watchKeys(AgentLines.line(.launched(repo, queued, [], cloud: true)), rows: rows), ["control:node:s1"])
        XCTAssertEqual(AgentLines.watchKeys(AgentLines.line(node), rows: rows), ["control:node:s1"])
    }

    func testSwipeDeleteGoesToTheTransportThatOwnsTheRunWhateverItsState() {
        let idle = UnifiedAgentRow(
            id: "desktop:7", badge: .desktopLan, title: "t", repoOwner: "forkmesh", repoName: "ForkMesh", node: "studio",
            status: "done", statusLabel: "Done", isActive: false, localId: 7)
        var busy = idle
        busy.isActive = true
        var outside = idle
        outside.outside = true
        outside.localId = -1004
        var unclaimed = idle
        unclaimed.localId = 0
        let node = UnifiedAgentRow(
            id: "node:s1", badge: .node, title: "q", repoOwner: "forkmesh", repoName: "forkmesh", node: "m1",
            status: "completed", statusLabel: "Completed", isActive: false, cloudSessionId: "s1", orgScope: true,
            branchName: "agent/s1", baseBranch: "main", merged: true)
        var runningNode = node
        runningNode.isActive = true
        runningNode.merged = false
        var unqueued = node
        unqueued.cloudSessionId = ""
        let nodeRef = RunnerAgentRef(
            owner: "forkmesh", repo: "forkmesh", sessionId: "s1", orgScope: true, branch: "agent/s1", base: "main")

        XCTAssertEqual(AgentLines.deletion(AgentLines.line(idle)), .desktop(sessionId: 7))
        XCTAssertEqual(AgentLines.deletion(AgentLines.line(busy)), .desktop(sessionId: 7))
        XCTAssertEqual(AgentLines.deletion(AgentLines.line(outside)), .outside(id: -1004))
        XCTAssertNil(AgentLines.deletion(AgentLines.line(unclaimed)))
        XCTAssertEqual(AgentLines.deletion(AgentLines.line(node)), .runner(nodeRef, merged: true, active: false))
        XCTAssertEqual(AgentLines.deletion(AgentLines.line(runningNode)), .runner(nodeRef, merged: false, active: true))
        XCTAssertNil(AgentLines.deletion(AgentLines.line(unqueued)))

        let pushed = AgentSession(id: 7, status: "done")
        XCTAssertEqual(AgentLines.deletion(AgentLines.line(.desktop(repo, pushed))), .desktop(sessionId: 7))
        XCTAssertEqual(AgentLines.deletion(AgentLines.line(.desktop(repo, pushed), live: busy)), .desktop(sessionId: 7))
        XCTAssertEqual(
            AgentLines.deletion(AgentLines.line(.desktop(repo, AgentSession(id: 7, status: "running")))), .desktop(sessionId: 7))

        let queued = AgentLaunchSession(
            sessionId: "s2", owner: "forkmesh", repo: "forkmesh", status: "failed", branchName: "agent/s2", baseBranch: "main")
        let queuedRef = RunnerAgentRef(owner: "forkmesh", repo: "forkmesh", sessionId: "s2", branch: "agent/s2", base: "main")
        XCTAssertEqual(
            AgentLines.deletion(AgentLines.line(.launched(repo, queued, [], cloud: true))),
            .runner(queuedRef, merged: false, active: false))
        var running = queued
        running.status = "running"
        XCTAssertEqual(
            AgentLines.deletion(AgentLines.line(.launched(repo, running, [], cloud: true))),
            .runner(queuedRef, merged: false, active: true))
    }

    func testPushedDesktopSessionTakesItsStateFromTheDesktopInReach() {
        let idle = UnifiedAgentRow(
            id: "desktop:7", badge: .desktopLan, title: "t", repoOwner: "forkmesh", repoName: "ForkMesh", node: "studio",
            status: "stopped", statusLabel: "Stopped", isActive: false, localId: 7)
        var busy = idle
        busy.status = "running"
        busy.statusLabel = "Running"
        busy.isActive = true
        let stale = AgentEntry.desktop(repo, AgentSession(id: 7, status: "running"))

        let settled = AgentLines.merge(entries: [stale], rows: [idle])
        XCTAssertEqual(settled.count, 1)
        XCTAssertFalse(settled[0].isActive)
        XCTAssertEqual(settled[0].status, .stopped)
        XCTAssertEqual(settled[0].statusLabel, "Stopped")
        XCTAssertEqual(AgentLines.deletion(settled[0]), .desktop(sessionId: 7))

        let started = AgentLines.merge(entries: [.desktop(repo, AgentSession(id: 7, status: "done"))], rows: [busy])
        XCTAssertTrue(started[0].isActive)
        XCTAssertEqual(AgentLines.deletion(started[0]), .desktop(sessionId: 7))

        let unreached = AgentLines.merge(entries: [stale], rows: [])
        XCTAssertTrue(unreached[0].isActive)
        XCTAssertEqual(AgentLines.deletion(unreached[0]), .desktop(sessionId: 7))
    }

    func testAPushedSessionTheWholeDesktopListLacksIsDropped() {
        let live = UnifiedAgentRow(
            id: "desktop:7", badge: .desktopLan, title: "t", repoOwner: "forkmesh", repoName: "ForkMesh", node: "studio",
            status: "stopped", statusLabel: "Stopped", isActive: false, localId: 7)
        let kept = AgentEntry.desktop(repo, AgentSession(id: 7, status: "done", createdAtMs: 500))
        let deleted = AgentEntry.desktop(repo, AgentSession(id: 8, status: "done", createdAtMs: 900))
        let old = AgentEntry.desktop(repo, AgentSession(id: 2, status: "done", createdAtMs: 100))

        let whole = AgentLines.merge(entries: [kept, deleted, old], rows: [live], cover: DesktopListCover())
        XCTAssertEqual(whole.map(\.id), [kept.id])

        let capped = AgentLines.merge(
            entries: [kept, deleted, old], rows: [live], cover: DesktopListCover(oldestCreatedAtMs: 400))
        XCTAssertEqual(Set(capped.map(\.id)), [kept.id, old.id], "older than the capped list is not contradicted")

        let unreached = AgentLines.merge(entries: [kept, deleted, old], rows: [live])
        XCTAssertEqual(unreached.count, 3)
    }

    func testDesktopListCoverIsWholeBelowTheCap() {
        let few = LocalAgentsSnapshot(sessions: [LocalAgentRow(owner: "a", name: "b", session: AgentSession(id: 1, createdAtMs: 50))])
        XCTAssertEqual(DesktopListCover(few), DesktopListCover())
        let many = LocalAgentsSnapshot(sessions: (1...DesktopListCover.listCap).map {
            LocalAgentRow(owner: "a", name: "b", session: AgentSession(id: $0, createdAtMs: $0 * 10))
        })
        XCTAssertEqual(DesktopListCover(many).oldestCreatedAtMs, 10)
        XCTAssertTrue(DesktopListCover(many).covers(createdAtMs: 10))
        XCTAssertFalse(DesktopListCover(many).covers(createdAtMs: 9))
    }

    func testLinesGroupIntoTheDesktopsFolders() {
        func desktop(_ id: Int, active: Bool = false) -> UnifiedAgentRow {
            UnifiedAgentRow(
                id: "desktop:\(id)", badge: .desktopLan, title: "t\(id)", repoOwner: "forkmesh", repoName: "ForkMesh",
                node: "studio", status: active ? "running" : "done", statusLabel: "", isActive: active, localId: id)
        }
        let cloud = UnifiedAgentRow(
            id: "node:abc", badge: .node, title: "c", repoOwner: "forkmesh", repoName: "forkmesh", node: "m1",
            status: "done", statusLabel: "", isActive: false, cloudSessionId: "abc")
        let pushed = AgentEntry.desktop(repo, AgentSession(id: 5, status: "done"))
        let lines = [AgentLines.line(desktop(1)), AgentLines.line(desktop(2, active: true)), AgentLines.line(cloud),
                     AgentLines.line(pushed), AgentLines.line(desktop(3))]
        let folders = [
            LocalAgentFolder(id: "empty", owner: "forkmesh", repoName: "forkmesh", name: "Empty"),
            LocalAgentFolder(id: "b", owner: "forkmesh", repoName: "ForkMesh", name: "Bugs", collapsed: true,
                             members: ["2", "cloud:abc", "5"]),
            LocalAgentFolder(id: "o", owner: "forkmesh", repoName: "other", name: "Other repo", members: ["1"]),
        ]

        let sections = AgentLines.folderSections(lines, folders: folders)
        XCTAssertEqual(sections.map(\.id), ["b", ""])
        XCTAssertEqual(sections[0].lines.map(\.title), ["t2", "c", lines[3].title])
        XCTAssertTrue(sections[0].collapsed)
        XCTAssertTrue(sections[0].isFolder)
        XCTAssertEqual(sections[1].lines.map(\.title), ["t1", "t3"])
        XCTAssertFalse(sections[1].isFolder)

        XCTAssertEqual(AgentLines.folderSections(lines, folders: []).map(\.lines.count), [5])
        XCTAssertEqual(AgentLines.folderSections([], folders: folders), [])
    }

    func testFolderFoldsOutliveTheScreen() {
        let open = AgentFolderSection(id: "a", name: "A", lines: [])
        let shut = AgentFolderSection(id: "b", name: "B", collapsed: true, lines: [])
        let loose = AgentFolderSection(lines: [])
        let store = InMemoryKeyValueStore()

        var folds = AgentFolderFolds.load(store)
        XCTAssertFalse(folds.isCollapsed(open))
        XCTAssertTrue(folds.isCollapsed(shut))
        XCTAssertFalse(folds.isCollapsed(loose))

        folds.set("a", collapsed: true)
        folds.set("b", collapsed: false)
        folds.set("", collapsed: true)
        folds.save(store)

        let reloaded = AgentFolderFolds.load(store)
        XCTAssertEqual(reloaded, folds)
        XCTAssertTrue(reloaded.isCollapsed(open))
        XCTAssertFalse(reloaded.isCollapsed(shut))
        XCTAssertFalse(reloaded.isCollapsed(loose))
        XCTAssertEqual(store.stringList(forKey: AgentFolderFolds.collapsedKey), ["a"])
        XCTAssertEqual(store.stringList(forKey: AgentFolderFolds.expandedKey), ["b"])

        var again = reloaded
        again.set("a", collapsed: false)
        again.save(store)
        XCTAssertFalse(AgentFolderFolds.load(store).isCollapsed(open))
        XCTAssertEqual(store.stringList(forKey: AgentFolderFolds.collapsedKey), [])
    }
}
