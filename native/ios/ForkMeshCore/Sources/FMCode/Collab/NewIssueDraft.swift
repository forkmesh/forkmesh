// The new issue draft logic: the parent-issue opening line, list splitting and
// submit validation, plus the composer's guest action. After native/android
// core/.../collab/NewIssueDraft.kt.

import FMApi
import FMBase
import Foundation

/// Values collected by the new issue screen, submitted to the signed inbox.
public struct NewIssueDraft: Equatable, Sendable {
    public var title: String
    public var body: String
    public var labels: [String]
    public var milestone: String
    public var priority: Int
    public var assignees: [String]

    public init(
        title: String, body: String = "", labels: [String] = [], milestone: String = "", priority: Int = 0,
        assignees: [String] = []
    ) {
        self.title = title
        self.body = body
        self.labels = labels
        self.milestone = milestone
        self.priority = priority
        self.assignees = assignees
    }
}

public enum NewIssueComposer {
    public static let titleRequired = "Give the issue a title."
    public static let guestAction = "open an issue"
    public static let titleHint = "Issue title"
    public static let bodyHint = "Add a description"
    public static let labelsHelper = "Comma-separated, e.g. mobile, bug"
    public static let assigneesHelper = "Comma-separated node/user names"

    /// The page title: a sub-issue names its parent.
    public static func title(_ parentNumber: Int) -> String {
        parentNumber > 0 ? "New sub-issue of #\(parentNumber)" : "New issue"
    }

    /// A sub-issue's description starts with the portable "Parent issue: #N"
    /// line every client reads.
    public static func initialBody(_ parentNumber: Int) -> String {
        parentNumber > 0 ? "Parent issue: #\(parentNumber)\n\n" : ""
    }

    /// `_split`: comma-separated, trimmed, empty parts dropped.
    public static func split(_ value: String) -> [String] {
        value.split(separator: ",", omittingEmptySubsequences: false)
            .map { String($0).dartTrimmed }
            .filter { !$0.isEmpty }
    }

    /// `_submit`: the draft, or nil when the title is blank (show
    /// `titleRequired`).
    public static func draft(
        title: String, body: String, labels: String, priority: String, milestone: String, assignees: String
    ) -> NewIssueDraft? {
        let cleanTitle = title.dartTrimmed
        if cleanTitle.isEmpty { return nil }
        return NewIssueDraft(
            title: cleanTitle,
            body: body.dartTrimmed,
            labels: split(labels),
            milestone: milestone.dartTrimmed,
            priority: DartCore.intTryParse(priority.dartTrimmed) ?? 0,
            assignees: split(assignees))
    }
}
