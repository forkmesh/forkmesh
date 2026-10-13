import FMApi
import FMBase
import Foundation

public enum PullMergeMethod: String, CaseIterable, Sendable {
    case merge

    public var title: String { "Merge pull request" }

    public static let offered: [PullMergeMethod] = [.merge]
}

public enum PullMergeState: Equatable, Sendable {
    case merged
    case closed
    case checking
    case notOwner
    case notPinned
    case conflicts([String])
    case changesRequested([String])
    case needsIndependentReview
    case ready

    public var canMerge: Bool {
        switch self {
        case .ready, .needsIndependentReview: return true
        default: return false
        }
    }

    public var needsWaiver: Bool { self == .needsIndependentReview }

    public var tone: CollabTone {
        switch self {
        case .merged: return .accentEdge
        case .ready: return .success
        case .closed, .conflicts, .changesRequested: return .danger
        case .checking, .notOwner, .notPinned, .needsIndependentReview: return .secondary
        }
    }
}

public struct PullMergeInputs: Equatable, Sendable {
    public var status: String
    public var author: String
    public var events: [ReviewEvent]
    public var isOwner: Bool?
    public var plan: PullMergePlan?
    public var planLoaded: Bool

    public init(status: String, author: String, events: [ReviewEvent], isOwner: Bool?, plan: PullMergePlan?, planLoaded: Bool) {
        self.status = status
        self.author = author
        self.events = events
        self.isOwner = isOwner
        self.plan = plan
        self.planLoaded = planLoaded
    }
}

public enum PullMergeRules {
    public static func state(_ input: PullMergeInputs) -> PullMergeState {
        let status = input.status.dartTrimmed.lowercased()
        if status == "merged" { return .merged }
        if status == "closed" { return .closed }
        guard let isOwner = input.isOwner else { return .checking }
        if !isOwner { return .notOwner }
        if !input.planLoaded { return .checking }
        guard let plan = input.plan, plan.isConsistent else { return .notPinned }
        if !plan.clean { return .conflicts(plan.conflicts) }
        let blockers = ReviewGate.changesRequestedBy(input.events, author: input.author)
        if !blockers.isEmpty { return .changesRequested(blockers) }
        if ReviewGate.independentApprovals(input.events, author: input.author) == 0 { return .needsIndependentReview }
        return .ready
    }

    public static func headline(_ state: PullMergeState, base: String) -> String {
        let target = base.isEmpty ? "the base branch" : base
        switch state {
        case .merged: return "Merged."
        case .closed: return "This pull request is closed."
        case .checking: return PullMerge.checking
        case .notOwner: return "Only an owner of this repository can merge it."
        case .notPinned: return PullMerge.notPinned
        case .conflicts(let files):
            if files.isEmpty { return "This branch conflicts with \(target)." }
            let shown = files.prefix(8).joined(separator: ", ")
            let more = files.count > 8 ? " and \(files.count - 8) more" : ""
            return "\(files.count) file\(files.count == 1 ? "" : "s") conflict with \(target): \(shown)\(more)."
        case .changesRequested(let who):
            return "Changes requested by \(who.joined(separator: ", ")). Resolve the review before merging."
        case .needsIndependentReview: return "No one else has approved this yet. You can merge anyway as the owner."
        case .ready: return "Ready to merge into \(target)."
        }
    }

    public static func blockers(_ state: PullMergeState, base: String) -> [String] {
        switch state {
        case .conflicts, .changesRequested, .notPinned, .closed: return [headline(state, base: base)]
        default: return []
        }
    }
}

public struct PullBranchState: Equatable, Sendable {
    public var ahead: Int
    public var behind: Int
    public var conflicts: [String]
    public var clean: Bool

    public init(ahead: Int = 0, behind: Int = 0, conflicts: [String] = [], clean: Bool = true) {
        self.ahead = ahead
        self.behind = behind
        self.conflicts = conflicts
        self.clean = clean
    }

    public var label: String {
        if !clean { return conflicts.isEmpty ? "Conflicts" : "\(conflicts.count) conflict\(conflicts.count == 1 ? "" : "s")" }
        var parts: [String] = []
        if ahead > 0 { parts.append("\(ahead) ahead") }
        if behind > 0 { parts.append("\(behind) behind") }
        return parts.isEmpty ? "Up to date" : parts.joined(separator: " · ")
    }

    public var tone: CollabTone { !clean ? .danger : behind > 0 ? .secondary : .success }
}
