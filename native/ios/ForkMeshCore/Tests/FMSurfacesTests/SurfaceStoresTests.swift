import FMApi
import FMBase
import FMPlanning
import XCTest

@testable import FMSurfaces

final class SurfaceStoresTests: XCTestCase {
    private var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent("fmsurfaces-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        if let root, root.path.hasPrefix(FileManager.default.temporaryDirectory.path) {
            try? FileManager.default.removeItem(at: root)
        }
    }

    func testSnapshotStoreWritesOnlyChanges() {
        let store = SurfaceSnapshotStore(directory: root)
        XCTAssertNil(store.read())
        var snapshot = SurfaceSnapshot(generatedAtMs: 1, signedIn: true, agents: AgentsSurface(running: 2))
        XCTAssertEqual(store.write(snapshot), Set(SurfaceSection.allCases))
        XCTAssertEqual(store.read(), snapshot)
        snapshot.generatedAtMs = 2
        XCTAssertTrue(store.write(snapshot).isEmpty)
        XCTAssertEqual(store.read()?.generatedAtMs, 1)
        snapshot.timer = TimerActivitySnapshot(
            timerId: "t", kind: .stopwatch, label: "Focus", endsAtMs: 0, countsFromMs: 5, paused: false, pausedText: "")
        XCTAssertEqual(store.write(snapshot), [.timer])
        store.clear()
        XCTAssertNil(store.read())
    }

    func testSnapshotStoreRejectsOversizeAndForeignVersions() throws {
        let store = SurfaceSnapshotStore(directory: root)
        let url = root.appendingPathComponent(SurfaceSnapshotStore.fileName)
        try Data(repeating: 0x20, count: SurfaceSnapshotStore.maxBytes + 1).write(to: url)
        XCTAssertNil(store.read())
        var snapshot = SurfaceSnapshot(generatedAtMs: 1, signedIn: true)
        snapshot.version = 99
        let data = try JSONEncoder().encode(snapshot)
        try data.write(to: url)
        XCTAssertNil(store.read())
        let huge = AgentsSurface(runs: (0..<2_000).map {
            AgentRunCard(id: "desktop:\($0)", title: String(repeating: "t", count: 80), repo: "o/r", state: .running)
        })
        XCTAssertTrue(store.write(SurfaceSnapshot(generatedAtMs: 1, signedIn: true, agents: huge)).isEmpty)
    }

    func testTheWidgetReadsWhatTheAppWroteThroughOneSharedItemStore() {
        let items = InMemorySharedItemStore()
        let app = SurfaceSnapshotStore(items: items)
        let widget = SurfaceSnapshotStore(items: items)
        let snapshot = SurfaceSnapshot(generatedAtMs: 1, signedIn: true, agents: AgentsSurface(running: 3))
        XCTAssertFalse(app.write(snapshot).isEmpty)
        XCTAssertEqual(widget.read(), snapshot)
        let commands = PendingSurfaceCommandStore(items: items)
        XCTAssertTrue(commands.enqueue(.open(.timers)))
        XCTAssertEqual(PendingSurfaceCommandStore(items: items).drain(), [.open(.timers)])
        XCTAssertEqual(widget.read(), snapshot)
        XCTAssertEqual(SurfaceContainer.keychainGroup, "742ZANCB57.com.forkmesh.forkmesh")
    }

    func testPendingCommandsKeepOnlyFreshOpens() {
        let clock = FixedClock(epochMillis: 1_000_000)
        let store = PendingSurfaceCommandStore(directory: root, clock: clock)
        XCTAssertTrue(store.enqueue(.open(.inbox(.all))))
        XCTAssertTrue(store.enqueue(.open(.timers)))
        XCTAssertTrue(store.enqueue(.open(.inbox(.all))))
        XCTAssertFalse(store.enqueue(.approve(approvalId: "a")))
        XCTAssertFalse(store.enqueue(.stopAgent(runId: "desktop:1")))
        XCTAssertEqual(store.drain(), [.open(.timers), .open(.inbox(.all))])
        XCTAssertEqual(store.drain(), [])
        XCTAssertTrue(store.enqueue(.open(.timers)))
        clock.advance(by: 11 * 60)
        XCTAssertEqual(store.drain(), [])
        for index in 0..<20 {
            store.enqueue(.open(.note(id: "n\(index)")))
        }
        XCTAssertEqual(store.drain().count, PendingSurfaceCommandStore.maxCommands)
    }

    @MainActor func testCommandCenterUsesHandlerOrHandsOff() async throws {
        let center = SurfaceCommandCenter()
        let store = PendingSurfaceCommandStore(directory: root)
        center.pendingStore = { store }
        center.waitStepNanoseconds = 1_000_000
        try await center.perform(.open(.timers))
        XCTAssertEqual(center.drainPending(), [.open(.timers)])
        do {
            try await center.perform(.stopAgent(runId: "desktop:1"), waitSeconds: 0.005)
            XCTFail("expected appNotReady")
        } catch let error as SurfaceCommandError {
            XCTAssertEqual(error, .appNotReady)
        }
        var handled: [SurfaceCommand] = []
        center.handler = { handled.append($0) }
        try await center.perform(.approve(approvalId: "x"))
        XCTAssertEqual(handled, [.approve(approvalId: "x")])
        XCTAssertEqual(center.drainPending(), [])
    }

    func testShareHandoffRoundTrip() throws {
        let clock = FixedClock(epochMillis: 5_000)
        var next = 0
        let store = ShareHandoffStore(directory: root, clock: clock, makeId: {
            next += 1
            return "h\(next)"
        })
        let saved = try store.save(
            text: "  Look at this crash  ",
            urls: ["https://example.com/a", "ftp://nope", "https://example.com/a", "not a url", "http://x.org"],
            images: [Data([1, 2, 3]), Data()],
            repo: "forkmesh/app")
        XCTAssertEqual(saved.text, "Look at this crash")
        XCTAssertEqual(saved.urls, ["https://example.com/a", "http://x.org"])
        XCTAssertEqual(saved.images.count, 1)
        XCTAssertEqual(saved.prompt, "Look at this crash\n\nhttps://example.com/a\nhttp://x.org")
        XCTAssertEqual(saved.route, .compose(ComposeTarget(
            repo: RepoRef(owner: "forkmesh", name: "app"), text: saved.prompt)))
        let pending = store.pending()
        XCTAssertEqual(pending, [saved])
        XCTAssertEqual(store.imageData(saved), [Data([1, 2, 3])])
        store.remove(saved.id)
        XCTAssertTrue(store.pending().isEmpty)
    }

    func testShareHandoffLimitsAndExpiry() throws {
        let clock = FixedClock(epochMillis: 5_000)
        var next = 0
        let store = ShareHandoffStore(directory: root, clock: clock, makeId: {
            next += 1
            return "h\(next)"
        })
        XCTAssertThrowsError(try store.save(text: "   ", urls: [], images: []))
        XCTAssertThrowsError(try store.save(text: "", urls: [], images: Array(repeating: Data([1]), count: 11)))
        let text = try store.save(text: String(repeating: "z", count: 30_000), urls: [], images: [], repo: "bad slug")
        XCTAssertEqual(text.text.count, ShareHandoffStore.maxTextLength)
        XCTAssertEqual(text.repo, "")
        XCTAssertNil(text.composeTarget.repo)
        for index in 0..<7 {
            clock.advance(by: 1)
            _ = try store.save(text: "item \(index)", urls: [], images: [])
        }
        XCTAssertEqual(store.pending().count, ShareHandoffStore.maxPending)
        XCTAssertEqual(store.pending().last?.text, "item 6")
        clock.advance(by: 25 * 60 * 60)
        XCTAssertTrue(store.pending().isEmpty)
        let evil = ShareHandoff(id: "../x", createdAtMs: 0, text: "", urls: [], images: ["../../etc"])
        XCTAssertTrue(store.imageData(evil).isEmpty)
        let bad = ShareHandoffStore(directory: root, makeId: { "../../escape" })
        XCTAssertThrowsError(try bad.save(text: "x", urls: [], images: []))
    }

    func testShareDropRoundTrip() throws {
        let (saved, items) = try ShareDrop.items(
            text: "  Fix this  ", urls: ["https://example.com/a", "ftp://nope"],
            images: [Data([1, 2]), Data(), Data([3])], id: "drop-1", createdAtMs: 10_000)
        XCTAssertEqual(Set(items.keys), ["manifest", "image-0", "image-1"])
        XCTAssertEqual(saved.images, ["image-0", "image-1"])
        let read = try XCTUnwrap(ShareDrop.read({ items[$0] }, nowMs: 20_000))
        XCTAssertEqual(read.handoff, saved)
        XCTAssertEqual(read.handoff.text, "Fix this")
        XCTAssertEqual(read.handoff.urls, ["https://example.com/a"])
        XCTAssertEqual(read.images, [Data([1, 2]), Data([3])])
        XCTAssertEqual(read.handoff.route, .compose(ComposeTarget(repo: nil, text: "Fix this\n\nhttps://example.com/a")))
    }

    func testShareDropImageOnlyOpensAnEmptyPrompt() throws {
        let (_, items) = try ShareDrop.items(text: "", urls: [], images: [Data([9])], id: "drop-2", createdAtMs: 0)
        let read = try XCTUnwrap(ShareDrop.read({ items[$0] }, nowMs: 1))
        XCTAssertEqual(read.images, [Data([9])])
        XCTAssertEqual(read.handoff.route, .compose(ComposeTarget(repo: nil, text: nil)))
    }

    func testShareDropRefusesStaleForgedAndOversized() throws {
        XCTAssertThrowsError(try ShareDrop.items(text: "", urls: [], images: [], id: "a", createdAtMs: 0))
        XCTAssertThrowsError(try ShareDrop.items(text: "x", urls: [], images: [], id: "../a", createdAtMs: 0))
        XCTAssertThrowsError(try ShareDrop.items(
            text: "", urls: [], images: [Data(count: ShareDrop.maxImageBytes + 1)], id: "a", createdAtMs: 0))
        let (_, items) = try ShareDrop.items(text: "x", urls: [], images: [Data([1])], id: "a", createdAtMs: 0)
        XCTAssertNil(ShareDrop.read({ items[$0] }, nowMs: ShareHandoffStore.maxAgeMs))
        XCTAssertNil(ShareDrop.read({ items[$0] }, nowMs: -ShareDrop.clockSkewMs))
        XCTAssertNil(ShareDrop.read({ _ in nil }, nowMs: 0))
        let forged = ShareHandoff(id: "b", createdAtMs: 0, text: "x", urls: [], images: ["../secret", "image-1"])
        var tampered = items
        tampered["manifest"] = try JSONEncoder().encode(forged)
        tampered["image-1"] = Data([7])
        let read = try XCTUnwrap(ShareDrop.read({ tampered[$0] }, nowMs: 1))
        XCTAssertEqual(read.images, [Data([7])])
    }

    func testRecentsStoreIsBoundedAndMostRecentFirst() {
        let clock = FixedClock(epochMillis: 1)
        let store = SurfaceRecentsStore(directory: root, clock: clock)
        let repo = RepoRef(owner: "forkmesh", name: "app")
        XCTAssertNil(store.record(.inbox(.all)))
        store.record(.repo(repo, section: nil))
        clock.advance(by: 1)
        store.record(.note(id: "n1"), title: "Groceries", detail: "Milk")
        clock.advance(by: 1)
        let again = store.record(.repo(repo, section: .issues), title: "ForkMesh app")
        XCTAssertEqual(again?.title, "ForkMesh app")
        XCTAssertEqual(store.all().map(\.ref.kind), [.repo, .note])
        XCTAssertEqual(store.all(of: .note).first?.detail, "Milk")
        store.record(.note(id: "n1"))
        XCTAssertEqual(store.find(SurfaceEntityRef(kind: .note, id: "n1"))?.title, "Groceries")
        XCTAssertEqual(SurfaceRecentsStore.matching(store.all(), query: "groc").count, 1)
        XCTAssertEqual(SurfaceRecentsStore.matching(store.all(), query: " ").count, 2)
        for index in 0..<250 {
            store.record(.note(id: "x\(index)"))
        }
        XCTAssertEqual(store.all().count, SurfaceRecentsStore.maxItems)
        XCTAssertEqual(store.all().first?.ref.id, "x249")
        store.removeAll()
        XCTAssertTrue(store.all().isEmpty)
    }
}
