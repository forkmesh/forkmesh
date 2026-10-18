import FMBase
import Foundation

public struct PluginFeedback: Equatable, Sendable {
    public static let hint = "What works, what is missing, or what went wrong?"
    public static let improvementHint = "What would make it five stars?"
    public static let sendLabel = "Send"
    public static let sendingLabel = "Sending…"
    public static let agentLabel = "Send to agent"
    public static let nothingToSend = "Tap a star or write something first."

    public var stars = 0
    public var text = ""
    public var improvement = ""

    public init(stars: Int = 0, text: String = "", improvement: String = "") {
        self.stars = stars
        self.text = text
        self.improvement = improvement
    }

    public var rates: Bool { (1...5).contains(stars) }
    public var reports: Bool { !text.dartTrimmed.isEmpty }
    public var canSend: Bool { rates || reports }
    public var canAskAgent: Bool { reports }

    public func asksImprovement(_ rules: ReviewRules) -> Bool {
        rates && stars < rules.reviewRequiredBelow
    }

    public func review(_ base: ReviewDraft, rules: ReviewRules) -> ReviewDraft? {
        guard rates else { return nil }
        var draft = base
        draft.stars = stars
        if asksImprovement(rules) {
            if reports { draft.problem = text.dartTrimmed }
            if !improvement.dartTrimmed.isEmpty { draft.improvement = improvement.dartTrimmed }
        }
        return draft
    }

    public static func thanks(_ name: String, rated: Bool, reported: Bool) -> String {
        switch (rated, reported) {
        case (true, true): return "Thanks — your rating and feedback on \(name) reached the ForkMesh team."
        case (true, false): return "Thanks for rating \(name)."
        default: return "Thanks — your feedback on \(name) reached the ForkMesh team."
        }
    }

    public func agentPrompt(name: String, packageId: String, screen: String, device: String) -> String {
        var lines = ["Feedback on the ForkMesh plugin \(name) (package `\(packageId)`), sent from the \(screen) screen of the \(device) app:"]
        lines.append("")
        lines.append(text.dartTrimmed)
        if rates { lines.append(""); lines.append("Rated \(stars) of 5 stars.") }
        lines.append("")
        lines.append("Find the cause in this repository and fix it, or say what you would change.")
        return lines.joined(separator: "\n")
    }
}
