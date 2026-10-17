// The bug report composer (ReportBugModel) and
// the floating control's geometry (BugButtonGeometry). The API half (`submitBugReport`) is
// ApiService's and is tested in FMApiTests.

import FMApi
import FMBase
import XCTest
@testable import FMTools

@MainActor
final class RecordingReports {
    struct Sent {
        let message, contact, screen, severity, platform, appVersion, osVersion, package: String
        let attachments: [BugReportAttachment]
    }

    var answer = BugReportResult(ok: true, id: String(repeating: "a", count: 32), message: "")
    private(set) var sent: [Sent] = []

    var submit: BugReportSubmit {
        { [self] message, contact, screen, severity, platform, appVersion, osVersion, package, attachments in
            self.sent.append(Sent(message: message, contact: contact, screen: screen, severity: severity,
                                  platform: platform, appVersion: appVersion, osVersion: osVersion, package: package,
                                  attachments: attachments))
            return self.answer
        }
    }
}

final class BugReportTests: XCTestCase {
    let build = AppBuild(version: "1.2.2+13", platform: "ios", osVersion: "Version 26.1")

    @MainActor
    func testTheComposerSendsWhatWasTypedAndConfirmsIt() async {
        let reports = RecordingReports()
        let model = ReportBugModel(screen: "agents", build: build, signedIn: true, submit: reports.submit)
        model.message = "The agent transcript stops mid-run."
        model.severity = "crash"
        let filed = await model.submit()
        XCTAssertTrue(filed)
        let sent = reports.sent.first!
        XCTAssertEqual(sent.message, "The agent transcript stops mid-run.")
        XCTAssertEqual(sent.severity, "crash")
        XCTAssertEqual(sent.screen, "agents")
        XCTAssertEqual(sent.platform, "ios")
        XCTAssertEqual(sent.appVersion, "1.2.2+13")
        XCTAssertEqual(sent.osVersion, "Version 26.1")
        XCTAssertTrue(ReportBugModel.thanks.contains("Thanks"))
    }

    @MainActor
    func testSeverityDefaultsToNormalSoOneFieldIsEnough() async {
        let reports = RecordingReports()
        let model = ReportBugModel(build: build, signedIn: false, submit: reports.submit)
        model.message = "Just this one line."
        _ = await model.submit()
        XCTAssertEqual(reports.sent.first?.severity, "normal")
        XCTAssertEqual(BugSeverity.all.map(\.label), ["Minor", "Normal", "Serious", "Crash"])
    }

    @MainActor
    func testTheComposerRefusesToSendAnEmptyDescription() async {
        let reports = RecordingReports()
        let model = ReportBugModel(build: build, signedIn: false, submit: reports.submit)
        model.message = "   "
        let filed = await model.submit()
        XCTAssertFalse(filed)
        XCTAssertEqual(model.problem, "Describe what went wrong.")
        XCTAssertTrue(reports.sent.isEmpty)
        // Typing clears the problem.
        model.message = "x"
        XCTAssertEqual(model.problem, "")
    }

    @MainActor
    func testARefusalKeepsTheSheetOpenWithTheTextIntact() async {
        let reports = RecordingReports()
        reports.answer = BugReportResult(ok: false, id: "", message: "You have sent several reports in a short time.")
        let model = ReportBugModel(build: build, signedIn: false, submit: reports.submit)
        let typed = "Everything breaks when I rotate the phone."
        model.message = typed
        let filed = await model.submit()
        XCTAssertFalse(filed)
        XCTAssertEqual(model.problem, "You have sent several reports in a short time.")
        XCTAssertEqual(model.message, typed)
        XCTAssertFalse(model.busy)
    }

    @MainActor
    func testTheComposerNamesTheContextItAttaches() async {
        let signedIn = ReportBugModel(screen: "chat", build: build, signedIn: true, submit: nil)
        XCTAssertEqual(signedIn.attached, "Attached: ForkMesh 1.2.2+13 · ios · chat · your account.")
        XCTAssertEqual(signedIn.contactLabel, "Reply to (optional)")
        let guest = ReportBugModel(build: build, signedIn: false, submit: nil)
        XCTAssertEqual(guest.attached, "Attached: ForkMesh 1.2.2+13 · ios · no account.")
        XCTAssertEqual(guest.contactHint, "Only if you want a reply")
    }

    @MainActor
    func testPluginFeedbackIsTaggedWithItsPackageAndTitledForIt() async {
        let reports = RecordingReports()
        let model = ReportBugModel(
            screen: "Notes", package: " notes ", packageName: "Notes", build: build, signedIn: true, submit: reports.submit)
        XCTAssertTrue(model.isFeedback)
        XCTAssertEqual(model.heading, "Feedback on Notes")
        XCTAssertTrue(model.intro.contains("Notes"))
        XCTAssertEqual(model.hint, ReportBugModel.feedbackHint)
        XCTAssertEqual(model.submitLabel, "Send feedback")
        XCTAssertEqual(model.attached, "Attached: ForkMesh 1.2.2+13 · ios · Notes · your account.")
        XCTAssertEqual(model.thanksText, ReportBugModel.feedbackThanks("Notes"))
        let filed = await model.submit()
        XCTAssertFalse(filed)
        XCTAssertEqual(model.problem, "Describe the problem or idea.")
        model.message = "Rating stars do not stick."
        let sent = await model.submit()
        XCTAssertTrue(sent)
        XCTAssertEqual(reports.sent.first?.package, "notes")
        XCTAssertEqual(reports.sent.first?.screen, "Notes")
    }

    @MainActor
    func testAPlainReportCarriesNoPackageAndKeepsItsCopy() async {
        let reports = RecordingReports()
        let model = ReportBugModel(screen: "agents", build: build, signedIn: false, submit: reports.submit)
        XCTAssertFalse(model.isFeedback)
        XCTAssertEqual(model.heading, ReportBugModel.title)
        XCTAssertEqual(model.intro, ReportBugModel.subtitle)
        XCTAssertEqual(model.hint, ReportBugModel.messageHint)
        XCTAssertEqual(model.submitLabel, "Send report")
        XCTAssertEqual(model.thanksText, ReportBugModel.thanks)
        model.message = "Broken."
        _ = await model.submit()
        XCTAssertEqual(reports.sent.first?.package, "")
    }

    @MainActor
    func testFeedbackWithoutANameFallsBackToThePackageId() {
        let model = ReportBugModel(screen: "crm", package: "crm", build: build, signedIn: true, submit: nil)
        XCTAssertEqual(model.heading, "Feedback on crm")
        XCTAssertEqual(model.attached, "Attached: ForkMesh 1.2.2+13 · ios · crm · your account.")
    }

    @MainActor
    func testWithoutAnApiReportingIsUnavailable() async {
        let model = ReportBugModel(build: build, signedIn: false, submit: nil)
        model.message = "Broken"
        let filed = await model.submit()
        XCTAssertFalse(filed)
        XCTAssertEqual(model.problem, "Reporting is unavailable in this build.")
    }

    // MARK: The floating control

    let layer = BugButtonGeometry(width: 800, height: 600)

    func testTheDefaultPlacementSitsOnTheRightEdge() {
        let at = layer.resting(anchor: 0.72, onLeft: false)
        XCTAssertEqual(at.x, 800 - 42 - 12)
        XCTAssertEqual(at.y, 12 + 0.72 * (600 - 42 - 24), accuracy: 1e-9)
        XCTAssertLessThanOrEqual(BugButtonGeometry.size, 48)
    }

    func testTheControlMovesOutOfTheWayAndSnapsToTheNearerEdge() {
        let start = layer.resting(anchor: 0.72, onLeft: false)
        // Far enough to cross the midline, so the release snaps to the far edge.
        let dragged = layer.clamp(x: start.x - 500, y: start.y - 250)
        let released = layer.release(x: dragged.x, y: dragged.y)
        XCTAssertTrue(released.onLeft)
        XCTAssertLessThan(released.anchor, 0.72)
        let settled = layer.resting(anchor: released.anchor, onLeft: released.onLeft)
        XCTAssertLessThan(settled.x, start.x)
        XCTAssertLessThan(settled.y, start.y)
    }

    func testADragThatDoesNotCrossTheMidlineKeepsItsEdge() {
        let start = layer.resting(anchor: 0.72, onLeft: false)
        let dragged = layer.clamp(x: start.x - 120, y: start.y - 200)
        let released = layer.release(x: dragged.x, y: dragged.y)
        XCTAssertFalse(released.onLeft)
        let settled = layer.resting(anchor: released.anchor, onLeft: released.onLeft)
        XCTAssertEqual(settled.x, start.x)
        XCTAssertLessThan(settled.y, start.y)
    }

    func testARememberedPlacementIsRestored() {
        let at = layer.resting(anchor: 0.1, onLeft: true)
        XCTAssertLessThan(at.x, 400)
        XCTAssertLessThan(at.y, 600 * 0.25)
    }

    func testTheControlStaysInsideItsLayerWhenDraggedPastAnEdge() {
        let start = layer.resting(anchor: 0.72, onLeft: false)
        let dragged = layer.clamp(x: start.x + 500, y: start.y + 4000)
        XCTAssertGreaterThanOrEqual(dragged.x, 0)
        XCTAssertLessThanOrEqual(dragged.x + BugButtonGeometry.size, 800)
        XCTAssertGreaterThanOrEqual(dragged.y, 0)
        XCTAssertLessThanOrEqual(dragged.y + BugButtonGeometry.size, 600)
        let released = layer.release(x: dragged.x, y: dragged.y)
        XCTAssertEqual(released.anchor, 1)
    }

    func testALayerShorterThanTheButtonStaysFinite() {
        let tiny = BugButtonGeometry(width: 30, height: 20, safeTop: 10, safeBottom: 10)
        XCTAssertEqual(tiny.travel, 1)
        let released = tiny.release(x: 0, y: 1000)
        XCTAssertTrue(released.anchor.isFinite)
    }

    @MainActor
    func testTheInboxLoadsFiltersAndPages() async {
        var asked: [(String, Int)] = []
        func item(_ id: String) -> BugReportInboxItem {
            BugReportInboxItem(json: ["id": .string(id), "message": "Broken.", "account": "alice", "platform": "ios"])
        }
        let model = BugReportInboxModel(load: { status, offset in
            asked.append((status, offset))
            if offset == 0 {
                return BugReportInbox(ok: true, reports: [item("a")], counts: ["new": 2, "closed": 1], nextOffset: 1)
            }
            return BugReportInbox(ok: true, reports: [item("b")], counts: ["new": 2, "closed": 1])
        })
        await model.reload()
        XCTAssertEqual(model.reports.map(\.id), ["a"])
        XCTAssertEqual(model.label(for: BugReportStatusFilter.all[0]), "New 2")
        XCTAssertEqual(model.label(for: BugReportStatusFilter.all[3]), "All 3")
        await model.more()
        XCTAssertEqual(model.reports.map(\.id), ["a", "b"])
        XCTAssertNil(model.nextOffset)
        await model.select("closed")
        XCTAssertEqual(model.reports.map(\.id), ["a"])
        XCTAssertEqual(asked.map(\.0), ["new", "new", "closed"])
        XCTAssertEqual(asked.map(\.1), [0, 1, 0])
        XCTAssertEqual(BugReportInboxModel.details(model.reports[0]), "@alice · ios")
    }

    @MainActor
    func testTheInboxShowsARefusal() async {
        let model = BugReportInboxModel(load: { _, _ in .failure("Only ForkMesh administrators can read reported bugs.") })
        await model.reload()
        XCTAssertTrue(model.loaded)
        XCTAssertTrue(model.reports.isEmpty)
        XCTAssertEqual(model.problem, "Only ForkMesh administrators can read reported bugs.")
        let missing = BugReportInboxModel(load: nil)
        await missing.reload()
        XCTAssertFalse(missing.problem.isEmpty)
    }

    @MainActor
    func testACancelledLoadShowsNoErrorAndLoadsAgain() async {
        var cancel = true
        let model = BugReportInboxModel(load: { _, _ in
            if cancel { return .cancelledLoad }
            return BugReportInbox(ok: true, reports: [BugReportInboxItem(json: ["id": "a"])])
        })
        await model.reload()
        XCTAssertFalse(model.loaded)
        XCTAssertEqual(model.problem, "")
        XCTAssertFalse(model.loading)
        cancel = false
        await model.reload()
        XCTAssertTrue(model.loaded)
        XCTAssertEqual(model.reports.map(\.id), ["a"])
    }

    @MainActor
    func testTriageMovesAReportOutOfAFilteredQueue() async {
        var changes: [(String, String)] = []
        let model = BugReportInboxModel(
            load: { _, _ in
                BugReportInbox(ok: true, reports: [
                    BugReportInboxItem(json: ["id": "a", "status": "new"]),
                    BugReportInboxItem(json: ["id": "b", "status": "new"]),
                ], counts: ["new": 2])
            },
            setStatus: { id, status in
                changes.append((id, status))
                return BugReportStatusChange(ok: true, status: status, counts: ["new": 1, "triaged": 1])
            })
        await model.reload()
        await model.setStatus("a", to: "triaged")
        XCTAssertEqual(changes.map(\.0), ["a"])
        XCTAssertEqual(model.reports.map(\.id), ["b"])
        XCTAssertEqual(model.counts["triaged"], 1)
        await model.setStatus("b", to: "new")
        XCTAssertEqual(changes.count, 1)
        await model.select("")
        await model.setStatus("b", to: "closed")
        XCTAssertEqual(model.reports.map(\.status), ["new", "closed"])
    }

    @MainActor
    func testATriageRefusalIsShown() async {
        let model = BugReportInboxModel(
            load: { _, _ in BugReportInbox(ok: true, reports: [BugReportInboxItem(json: ["id": "a"])]) },
            setStatus: { _, _ in .failure("That report no longer exists.") })
        await model.reload()
        await model.setStatus("a", to: "closed")
        XCTAssertEqual(model.problem, "That report no longer exists.")
        XCTAssertEqual(model.reports.first?.status, "new")
    }

    @MainActor
    func testAnAttachmentIsSavedUnderItsNameForPreview() async throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
        var asked: [(String, String)] = []
        let model = BugReportInboxModel(
            load: { _, _ in BugReportInbox(ok: true) },
            attachment: { report, attachment in
                asked.append((report, attachment))
                return attachment == "bad" ? .failure("That attachment is no longer available.")
                    : BugReportFile(ok: true, data: Data("pixels".utf8), contentType: "image/png")
            },
            folder: folder)
        let attachment = BugReportInboxAttachment(json: ["id": "f1", "name": "../screen.png", "size": 6])
        let opened = await model.open(report: "r1", attachment: attachment)
        let url = try XCTUnwrap(opened)
        XCTAssertEqual(url.lastPathComponent, ".._screen.png")
        XCTAssertTrue(url.path.hasPrefix(folder.path))
        XCTAssertEqual(try Data(contentsOf: url), Data("pixels".utf8))
        XCTAssertEqual(asked.first?.0, "r1")
        XCTAssertTrue(model.downloading.isEmpty)
        let missing = await model.open(report: "r1", attachment: BugReportInboxAttachment(json: ["id": "bad"]))
        XCTAssertNil(missing)
        XCTAssertEqual(model.problem, "That attachment is no longer available.")
        XCTAssertEqual(BugReportInboxModel.sizeLabel(6), "6 B")
        XCTAssertEqual(BugReportInboxModel.sizeLabel(2_621_440), "2.5 MB")
    }
}
