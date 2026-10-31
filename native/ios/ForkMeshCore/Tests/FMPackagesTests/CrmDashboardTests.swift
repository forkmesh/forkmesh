import FMBase
import XCTest

@testable import FMPackages

final class CrmDashboardTests: XCTestCase {
    let day: Int64 = 86_400_000
    let now: Int64 = 100 * 86_400_000

    func contact(_ stage: String, value: Int64 = 0, created: Int64 = 0, contacted: Int64 = 0, flow: String = "customers")
        -> CrmContact
    {
        CrmContact(json: [
            "id": .string(String(repeating: "a", count: 32)), "name": "x", "flow": .string(flow), "stage": .string(stage),
            "value": .int(value), "createdAt": .int(created), "lastContactedAt": .int(contacted),
        ])
    }

    func testCountsOpenWonAndDueContacts() {
        let rows = [
            contact("lead", value: 500, created: now - day),
            contact("qualified", value: 300, created: now - 30 * day, contacted: now - 20 * day),
            contact("customer", value: 1000, contacted: now - day),
            contact("lost", value: 900),
            contact("lead", contacted: now - 2 * day),
        ]
        let board = CrmDashboard(contacts: rows, now: now)
        XCTAssertEqual(board.contacts, 5)
        XCTAssertEqual(board.newThisWeek, 1)
        XCTAssertEqual(board.openDeals, 2)
        XCTAssertEqual(board.openValue, 800)
        XCTAssertEqual(board.wonDeals, 1)
        XCTAssertEqual(board.wonValue, 1000)
        XCTAssertEqual(board.neverContacted, 1)
        XCTAssertEqual(board.quiet, 1)
    }

    func testGreeting() {
        XCTAssertEqual(CrmDashboard.greeting(hour: 8), "Good morning")
        XCTAssertEqual(CrmDashboard.greeting(hour: 13), "Good afternoon")
        XCTAssertEqual(CrmDashboard.greeting(hour: 21), "Good evening")
        XCTAssertEqual(CrmDashboard.greeting(hour: 2), "Good evening")
    }

    func testPipelineFlowPrefersTheActiveThenTheBusiest() {
        let data = CrmOrganizationData(json: [
            "contacts": [
                .object(["id": .string(String(repeating: "a", count: 32)), "flow": "hiring", "stage": "applied"]),
                .object(["id": .string(String(repeating: "b", count: 32)), "flow": "hiring", "stage": "applied"]),
            ],
            "settings": [
                "flows": [
                    ["id": "customers", "name": "Customers", "stages": ["lead"]],
                    ["id": "hiring", "name": "Hiring", "stages": ["applied"]],
                ]
            ],
        ])
        XCTAssertEqual(CrmDashboard.pipelineFlow(data, active: nil)?.id, "hiring")
        XCTAssertEqual(CrmDashboard.pipelineFlow(data, active: data.flow("customers"))?.id, "customers")
    }

    func testSectionsFollowTheTab() {
        XCTAssertEqual(CrmSection.of(.pipeline("customers")), .contacts)
        XCTAssertEqual(CrmSection.of(.mail), .activity)
        XCTAssertEqual(CrmDealState.of("committed"), .won)
        XCTAssertEqual(CrmDealState.of("declined"), .ended)
    }
}
