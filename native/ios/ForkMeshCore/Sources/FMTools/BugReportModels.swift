// The state and decisions of the bug report sheet (the composer) and the
// geometry of the bug report button (the draggable control's resting place,
// drag clamp and release snap), over `POST /api/bug-reports` (ApiService.submitBugReport).

import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

/// Reporter-facing severities, in the order they are offered: four plain words rather than a
/// numeric scale.
public struct BugSeverity: Equatable, Sendable {
    public let value: String
    public let label: String

    public static let all = [
        BugSeverity(value: "low", label: "Minor"),
        BugSeverity(value: "normal", label: "Normal"),
        BugSeverity(value: "high", label: "Serious"),
        BugSeverity(value: "crash", label: "Crash"),
    ]
}

/// What `submitBugReport` is handed (ApiService's signature, as a closure for tests).
public typealias BugReportSubmit = @MainActor (_ message: String, _ contact: String, _ screen: String, _ severity: String,
                                               _ platform: String, _ appVersion: String, _ osVersion: String,
                                               _ package: String, _ attachments: [BugReportAttachment]) async -> BugReportResult

@MainActor
@Observable
public final class ReportBugModel {
    public static let title = "Report a bug"
    public static let subtitle = "Sent privately to ForkMesh administrators."
    public static let messageHint = "What happened, and what did you expect instead?"
    public static let severityLabel = "How bad is it?"
    public static let feedbackHint = "What works, what is missing, or what went wrong?"
    public static let thanks = "Thanks — your report reached the ForkMesh team."

    @ObservationIgnored private let submitter: BugReportSubmit?
    public let screen: String
    public let build: AppBuild
    public let signedIn: Bool
    public let package: String
    public let packageName: String

    public var message = "" { didSet { if !problem.isEmpty && !message.dartTrimmed.isEmpty { problem = "" } } }
    public var contact = ""
    public var severity = "normal"
    public var attachments: [BugReportAttachment] = []
    public private(set) var problem = ""
    public private(set) var busy = false

    /// `submit` nil is a harness without an ApiService: reporting is unavailable.
    public init(
        screen: String = "", package: String = "", packageName: String = "", build: AppBuild, signedIn: Bool,
        submit: BugReportSubmit?
    ) {
        self.screen = screen
        self.package = package.dartTrimmed
        self.packageName = packageName.dartTrimmed.isEmpty ? package.dartTrimmed : packageName.dartTrimmed
        self.build = build
        self.signedIn = signedIn
        submitter = submit
    }

    public convenience init(
        screen: String = "", package: String = "", packageName: String = "", build: AppBuild, signedIn: Bool, api: ApiService?
    ) {
        let submit: BugReportSubmit? = api.map { api in
            { message, contact, screen, severity, platform, appVersion, osVersion, package, attachments in
                await api.submitBugReport(message: message, contact: contact, screen: screen, severity: severity,
                                          platform: platform, appVersion: appVersion, osVersion: osVersion, package: package,
                                          attachments: attachments)
            }
        }
        self.init(screen: screen, package: package, packageName: packageName, build: build, signedIn: signedIn, submit: submit)
    }

    public var isFeedback: Bool { !package.isEmpty }
    public var heading: String { isFeedback ? "Feedback on \(packageName)" : ReportBugModel.title }
    public var intro: String { isFeedback ? "Goes to the ForkMesh team, tagged with \(packageName)." : ReportBugModel.subtitle }
    public var hint: String { isFeedback ? ReportBugModel.feedbackHint : ReportBugModel.messageHint }
    public var thanksText: String { isFeedback ? ReportBugModel.feedbackThanks(packageName) : ReportBugModel.thanks }

    public static func feedbackThanks(_ name: String) -> String {
        "Thanks — your feedback on \(name) reached the ForkMesh team."
    }

    public var contactLabel: String { signedIn ? "Reply to (optional)" : "Email or handle (optional)" }
    public var contactHint: String { signedIn ? "Only if you want the reply somewhere else" : "Only if you want a reply" }
    public var submitLabel: String { busy ? "Sending…" : isFeedback ? "Send feedback" : "Send report" }

    /// "Attached: ForkMesh 1.2.2+13 · ios · code · your account."
    public var attached: String {
        var parts = ["ForkMesh \(build.version)", build.platform]
        if !screen.isEmpty { parts.append(screen) }
        if isFeedback && packageName != screen { parts.append(packageName) }
        parts.append(signedIn ? "your account" : "no account")
        return "Attached: \(parts.joined(separator: " · "))."
    }

    /// True once the report was filed (close the sheet and thank them); false keeps the sheet
    /// open with `problem` set and the typed text intact.
    public func submit() async -> Bool {
        if busy { return false }
        let text = message.dartTrimmed
        if text.isEmpty {
            problem = isFeedback ? "Describe the problem or idea." : "Describe what went wrong."
            return false
        }
        busy = true
        problem = ""
        guard let submitter else {
            busy = false
            problem = "Reporting is unavailable in this build."
            return false
        }
        let result = await submitter(text, contact, screen, severity, build.platform, build.version, build.osVersion, package, attachments)
        if !result.ok {
            busy = false
            problem = result.message
            return false
        }
        busy = false
        return true
    }
}

public typealias BugReportInboxLoad = @MainActor (_ status: String, _ offset: Int) async -> BugReportInbox

public struct BugReportStatusFilter: Equatable, Sendable {
    public let value: String
    public let label: String

    public static let all = [
        BugReportStatusFilter(value: "new", label: "New"),
        BugReportStatusFilter(value: "triaged", label: "Triaged"),
        BugReportStatusFilter(value: "closed", label: "Closed"),
        BugReportStatusFilter(value: "", label: "All"),
    ]
}

@MainActor
@Observable
public final class BugReportInboxModel {
    public static let tabReport = "Report"
    public static let tabReported = "Reported"
    public static let empty = "No reported bugs here."

    @ObservationIgnored private let loader: BugReportInboxLoad?
    public var status = "new"
    public private(set) var reports: [BugReportInboxItem] = []
    public private(set) var counts: [String: Int] = [:]
    public private(set) var nextOffset: Int?
    public private(set) var problem = ""
    public private(set) var loading = false
    public private(set) var loaded = false

    public init(load: BugReportInboxLoad?) {
        loader = load
    }

    public convenience init(api: ApiService?) {
        let load: BugReportInboxLoad? = api.map { api in
            { status, offset in await api.bugReportInbox(status: status, offset: offset) }
        }
        self.init(load: load)
    }

    public func label(for filter: BugReportStatusFilter) -> String {
        if filter.value.isEmpty {
            return counts.isEmpty ? filter.label : "\(filter.label) \(counts.values.reduce(0, +))"
        }
        guard let count = counts[filter.value] else { return filter.label }
        return "\(filter.label) \(count)"
    }

    public func select(_ value: String) async {
        if value == status && loaded { return }
        status = value
        await reload()
    }

    public func reload() async {
        await fetch(offset: 0)
    }

    public func more() async {
        guard let offset = nextOffset else { return }
        await fetch(offset: offset)
    }

    private func fetch(offset: Int) async {
        if loading { return }
        guard let loader else {
            problem = "Reported bugs are unavailable in this build."
            loaded = true
            return
        }
        loading = true
        let requested = status
        let inbox = await loader(requested, offset)
        loading = false
        guard requested == status else {
            await fetch(offset: 0)
            return
        }
        loaded = true
        if !inbox.ok {
            problem = inbox.message
            if offset == 0 { reports = [] }
            nextOffset = nil
            return
        }
        problem = ""
        counts = inbox.counts
        reports = offset == 0 ? inbox.reports : reports + inbox.reports
        nextOffset = inbox.nextOffset
    }

    public static func severityLabel(_ value: String) -> String {
        BugSeverity.all.first { $0.value == value }?.label ?? value.capitalized
    }

    public static func details(_ report: BugReportInboxItem) -> String {
        var parts = [report.account.isEmpty ? "anonymous" : "@\(report.account)"]
        let platform = [report.platform, report.osVersion].filter { !$0.isEmpty }.joined(separator: " ")
        if !platform.isEmpty { parts.append(platform) }
        if !report.appVersion.isEmpty { parts.append(report.appVersion) }
        if !report.screen.isEmpty { parts.append(report.screen) }
        if !report.package.isEmpty { parts.append(report.package) }
        if !report.device.isEmpty { parts.append(report.device) }
        return parts.joined(separator: " · ")
    }
}

/// BugReportOverlay's geometry. The position is kept as a fraction of the vertical travel plus a
/// side, never as points, so rotation or a split window never strands the control off screen.
public struct BugButtonGeometry: Equatable, Sendable {
    /// Visual diameter of the control.
    public static let size = 42.0
    /// Gap kept between the control and the edges of its layer.
    public static let margin = 12.0

    public let width: Double
    public let height: Double
    public let safeTop: Double
    public let safeBottom: Double

    public init(width: Double, height: Double, safeTop: Double = 0, safeBottom: Double = 0) {
        self.width = width
        self.height = height
        self.safeTop = safeTop
        self.safeBottom = safeBottom
    }

    /// The vertical room the control can move in; at least 1 so the anchor arithmetic stays finite.
    public var travel: Double {
        let usable = height - safeTop - safeBottom - BugButtonGeometry.size - BugButtonGeometry.margin * 2
        return usable > 1 ? usable : 1
    }

    /// The top-left corner at rest for a stored placement.
    public func resting(anchor: Double, onLeft: Bool) -> (x: Double, y: Double) {
        let top = safeTop + BugButtonGeometry.margin + anchor * travel
        let left = onLeft ? BugButtonGeometry.margin : width - BugButtonGeometry.size - BugButtonGeometry.margin
        return (left, top)
    }

    /// A drag position kept inside the layer.
    public func clamp(x: Double, y: Double) -> (x: Double, y: Double) {
        let maxX = max(width - BugButtonGeometry.size - BugButtonGeometry.margin, BugButtonGeometry.margin)
        let minY = safeTop + BugButtonGeometry.margin
        return (min(max(x, BugButtonGeometry.margin), maxX), min(max(y, minY), minY + travel))
    }

    /// Where a release at (x, y) settles: the anchor fraction and the nearer edge.
    public func release(x: Double, y: Double) -> (anchor: Double, onLeft: Bool) {
        let anchor = (y - safeTop - BugButtonGeometry.margin) / travel
        let centre = x + BugButtonGeometry.size / 2
        return (min(max(anchor, 0), 1), centre < width / 2)
    }
}
