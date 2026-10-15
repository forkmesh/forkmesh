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
    }

    var answer = BugReportResult(ok: true, id: String(repeating: "a", count: 32), message: "")
    private(set) var sent: [Sent] = []

    var submit: BugReportSubmit {
        { [self] message, contact, screen, severity, platform, appVersion, osVersion, package in
            self.sent.append(Sent(message: message, contact: contact, screen: screen, severity: severity,
                                  platform: platform, appVersion: appVersion, osVersion: osVersion, package: package))
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
}
