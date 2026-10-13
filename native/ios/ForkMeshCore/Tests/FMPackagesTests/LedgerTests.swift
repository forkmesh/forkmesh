import FMBase
import XCTest

@testable import FMPackages

final class LedgerTests: XCTestCase {
    func testLamportsFormatting() {
        XCTAssertEqual(Lamports.sol(0), "0 SOL")
        XCTAssertEqual(Lamports.sol(1_000_000_000), "1 SOL")
        XCTAssertEqual(Lamports.sol(50_000_000), "0.05 SOL")
        XCTAssertEqual(Lamports.sol(1_000_000_001), "1.000000001 SOL")
    }

    func testLedgerPage() {
        let page = LedgerPage(json: [
            "entries": [
                ["id": "tip:1", "category": "tip", "title": "Tip", "currency": "SOL", "amountLamports": 5_000_000, "transactionId": "sig"],
                ["id": "ad:1", "category": "advertising", "currency": "USD", "amountUsd": "5.00", "amountKnown": true],
                ["id": ""],
            ],
            "page": ["hasMore": true, "nextCursor": ["beforeAt": 10, "beforeId": "tip:1"]],
            "solUsd": ["available": true, "price": "150.000000"],
        ])
        XCTAssertEqual(page.entries.count, 2)
        XCTAssertEqual(page.entries[0].amountText, "0.005 SOL")
        XCTAssertTrue(page.entries[0].verified)
        XCTAssertEqual(page.entries[1].amountText, "$5.00")
        XCTAssertEqual(page.next?.query.first?.1, "10")
        XCTAssertEqual(page.solUsd, "150.000000")
    }

    @MainActor func testLedgerModelPagesAndKeepsPersonalRecordsInSecureStore() async {
        let transport = FakePackagesTransport()
        transport.on("GET", "/api/ledger", ["entries": [["id": "a"]], "page": ["hasMore": true, "nextCursor": ["beforeAt": 1, "beforeId": "a"]]])
        transport.on("GET", "/api/rewards/estimates", ["rewards": [["rewardId": "r1", "status": "ready"], ["rewardId": "r2", "status": "paid"]]])
        let secure = InMemorySecureStore()
        let model = LedgerModel(client: PackagesClient(transport: transport), store: PersonalLedgerStore(store: secure))
        await model.refresh()
        XCTAssertEqual(model.pendingRewards.map(\.rewardId), ["r1"])
        XCTAssertEqual(model.paidRewards.map(\.rewardId), ["r2"])
        transport.routes["GET /api/ledger"] = [PackagesReply(status: 200, json: ["entries": [["id": "a"], ["id": "b"]], "page": ["hasMore": false]])]
        await model.loadMore()
        XCTAssertEqual(model.entries.map(\.id), ["a", "b"])
        XCTAssertNil(model.next)
        XCTAssertEqual(transport.last("GET", "/api/ledger")?.query.map(\.0), ["limit", "beforeAt", "beforeId"])

        model.add(PersonalRecord(kind: .expense, title: "Hosting", amountMinor: 1200, occurredAt: 2))
        model.add(PersonalRecord(kind: .income, title: "Sponsor", amountMinor: 5000, occurredAt: 3))
        XCTAssertEqual(model.records.map(\.title), ["Sponsor", "Hosting"])
        XCTAssertEqual(model.totals.first?.income, 5000)
        XCTAssertEqual(model.totals.first?.expense, 1200)
        model.toggleSettled(model.records[1].id)
        XCTAssertTrue(model.records[1].settled)
        let reloaded = LedgerModel(client: PackagesClient(transport: transport), store: PersonalLedgerStore(store: secure))
        XCTAssertEqual(reloaded.records.count, 2)
        model.remove(model.records[0].id)
        XCTAssertEqual(PersonalLedgerStore(store: secure).load().map(\.title), ["Hosting"])
    }

    @MainActor func testPersonalStoreIsBounded() {
        let store = PersonalLedgerStore(store: InMemorySecureStore())
        let many = (0..<600).map { PersonalRecord(kind: .expense, title: "r\($0)", amountMinor: 1, occurredAt: Int64($0)) }
        XCTAssertEqual(store.save(many).count, PersonalLedgerStore.maxRecords)
        XCTAssertEqual(store.load().first?.title, "r599")
    }
}
