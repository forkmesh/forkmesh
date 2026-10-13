// The issue logic: the issue row, detail chips, body, parents and bounty
// refresh, the bounty funding card, status text, parent links, sub-issues,
// timeline ordering and tiles, issue actions and USD formatting. After
// native/android core/.../collab/IssueThread.kt.

import FMApi
import FMBase
import Foundation

/// `_formatUsd`: whole dollars without cents, otherwise `toStringAsFixed(2)`.
public func formatUsd(_ value: Double) -> String {
    if value.isFinite && value == value.rounded() && abs(value) < 9.2e18 {
        return "$\(Int(value))"
    }
    return "$" + DartCore.toStringAsFixed(value, 2)
}

/// `_issueStatusText`.
public func issueStatusText(_ issue: Issue?) -> String {
    guard let issue else { return "Not synced" }
    return issue.isOpen ? "Open" : "Closed"
}

/// The issue row of the Issues tab.
public enum IssueCard {
    public static func icon(_ issue: Issue) -> String { issue.isOpen ? "error_outline" : "check_circle_outline" }

    public static func tone(_ issue: Issue) -> CollabTone { issue.isOpen ? .success : .accentEdge }

    public static func subtitle(_ issue: Issue) -> String {
        "#\(issue.number)" + (issue.author.isEmpty ? "" : " · \(issue.author)")
    }
}

public enum IssueDetail {
    public static let noDescription = "No description provided."
    public static let fundingGateCopy =
        "Custodial funding is disabled. Do not fund a legacy deposit address. New bounty transfers require an owner-controlled external wallet, program, or multisig."
    public static let nonCustodialCopy =
        "Non-custodial boundary: live Worker creation, funding, signing, and payout of bounty wallets are disabled. Historical addresses and signatures are migration-only public status. Wallet keys must stay in an owner-controlled external wallet."
    public static let bountyRefreshed = "Historical migration status refreshed."
    public static let bountyNotFound = "No historical bounty migration record was found."
    public static let noPledge = "No pledge amount published yet."

    public static func title(_ number: Int) -> String { "#\(number)" }

    /// Parents named on relationship lines, never the issue itself.
    public static func parents(_ issue: Issue) -> [Int] {
        issueRelationNumbers(issue.body).filter { $0 != issue.number }
    }

    /// The description without its relationship lines, or the placeholder.
    public static func body(_ issue: Issue) -> String {
        let body = issueBodyWithoutRelationLines(issue.body)
        return body.isEmpty ? noDescription : body
    }

    /// The chips under the title, in display order.
    public static func chips(_ issue: Issue, repoFullName: String) -> [CollabChip] {
        var chips = [CollabChip("label_outline", issue.status)]
        if !issue.author.isEmpty { chips.append(CollabChip("person_outline", issue.author)) }
        for label in issue.labels { chips.append(CollabChip("sell_outlined", label)) }
        if issue.priority > 0 { chips.append(CollabChip("priority_high_outlined", "priority \(issue.priority)")) }
        if !issue.milestone.isEmpty { chips.append(CollabChip("flag_outlined", issue.milestone)) }
        for assignee in issue.assignees { chips.append(CollabChip("assignment_ind_outlined", assignee)) }
        if issue.votes > 0 {
            chips.append(CollabChip("how_to_vote_outlined", "\(issue.votes) \(issue.votes == 1 ? "vote" : "votes")"))
        }
        if issue.bountyUsd > 0 { chips.append(CollabChip("attach_money", "\(formatUsd(issue.bountyUsd)) bounty")) }
        chips.append(CollabChip("folder_outlined", repoFullName))
        return chips
    }

    /// `_refreshBounty`'s success branch: the issue to show and the message
    /// under the card.
    public static func bountyRefreshed(_ issue: Issue, _ bounty: IssueBounty) -> (Issue, String) {
        bounty.hasFunding ? (issue.copyWithBounty(bounty), bountyRefreshed) : (issue, bountyNotFound)
    }

    public static func bountyRefreshFailed(_ error: Error) -> String { "Refresh failed: \(dartErrorText(error))" }

    /// The bounty message turns danger for failures.
    public static func bountyMessageIsError(_ message: String) -> Bool {
        message.hasPrefix("Refresh failed") || message.hasPrefix("Exception")
    }

    /// `'${_formatUsd(bountyUsd)} pledged'`, or nil for "No pledge amount
    /// published yet.".
    public static func pledgeText(_ issue: Issue) -> String? {
        issue.bountyUsd > 0 ? "\(formatUsd(issue.bountyUsd)) pledged" : nil
    }

    public static func statusLine(_ issue: Issue) -> String { "Status: \(issue.bountyStatusLabel)" }
}

/// One copyable row of `_BountyFundingCard` (`_CopyableValue`).
public struct BountyCopyRow: Equatable, Sendable {
    public let label: String
    public let value: String
    /// What the "copied" notice names.
    public let copyLabel: String
    /// The widget key, used as the accessibility identifier.
    public let key: String
}

public enum BountyFunding {
    /// The address, explorer and payout rows the card lists, in order.
    public static func rows(_ issue: Issue) -> [BountyCopyRow] {
        var rows: [BountyCopyRow] = []
        if !issue.bountyAddress.isEmpty {
            rows.append(BountyCopyRow(
                label: "Legacy address (do not fund)", value: issue.bountyAddress,
                copyLabel: "Bounty deposit address", key: "issue-bounty-copy-address"))
            let explorer = solanaExplorerAddressUrl(issue.bountyAddress)
            if !explorer.isEmpty {
                rows.append(BountyCopyRow(
                    label: "Address explorer URL", value: explorer,
                    copyLabel: "Bounty address explorer URL", key: "issue-bounty-copy-address-explorer"))
            }
        }
        if !issue.bountyPayoutSig.isEmpty {
            rows.append(BountyCopyRow(
                label: "Payout signature", value: issue.bountyPayoutSig,
                copyLabel: "Bounty payout signature", key: "issue-bounty-copy-payout-sig"))
            let explorer = solanaExplorerSignatureUrl(issue.bountyPayoutSig)
            if !explorer.isEmpty {
                rows.append(BountyCopyRow(
                    label: "Payout transaction explorer URL", value: explorer,
                    copyLabel: "Bounty payout explorer URL", key: "issue-bounty-copy-payout-explorer"))
            }
        }
        return rows
    }
}

/// `_IssueParentLinks`: one chip per parent.
public func issueParentLinkLabel(_ number: Int, _ parent: Issue?) -> String {
    let title = (parent == nil || parent?.title.isEmpty == true) ? "" : " \(parent?.title ?? "")"
    return "#\(number)\(title) · \(issueStatusText(parent))"
}

/// `_SubIssuesCard`.
public enum SubIssues {
    public static let empty = "No sub-issues yet."

    public static func title(_ numbers: [Int], find: (Int) -> Issue?) -> String {
        if numbers.isEmpty { return "Sub-issues" }
        let closed = numbers.filter { find($0)?.isOpen == false }.count
        return "Sub-issues · \(closed) of \(numbers.count) closed"
    }

    public static func rowTitle(_ number: Int, _ child: Issue?) -> String {
        "#\(number) \(child?.title ?? "")".dartTrimmed
    }

    public static func details(_ child: Issue?) -> String {
        var parts = [issueStatusText(child)]
        if let child {
            parts.append("\(child.progress)%")
            if child.priority > 0 { parts.append("P\(child.priority)") }
            parts.append(child.assignees.isEmpty ? "Unassigned" : child.assignees.joined(separator: ", "))
        }
        return parts.joined(separator: " · ")
    }

    /// A closed child reads as done whatever its progress says.
    public static func progress(_ child: Issue) -> Double {
        child.isOpen ? Double(child.progress) / 100 : 1
    }
}

/// One row of `_IssueTimeline`: a signed event or a derived mention.
public enum IssueTimelineEntry: Equatable, Sendable {
    case event(IssueEvent)
    case mention(IssueMention)

    public var ts: Int {
        switch self {
        case .event(let event): return event.ts
        case .mention(let mention): return mention.ts
        }
    }
}

public enum IssueTimeline {
    public static let empty = "No timeline events have been published yet."

    /// Other issues referencing this one join the history when they were
    /// written. Stable: equal timestamps keep signed events ahead of derived
    /// mentions.
    public static func entries(_ events: [IssueEvent], _ mentions: [IssueMention]) -> [IssueTimelineEntry] {
        let rows = events.map { IssueTimelineEntry.event($0) } + mentions.map { IssueTimelineEntry.mention($0) }
        return rows.enumerated()
            .sorted { $0.element.ts != $1.element.ts ? $0.element.ts < $1.element.ts : $0.offset < $1.offset }
            .map(\.element)
    }

    public static func isStatus(_ event: IssueEvent) -> Bool { event.type == "status" }

    public static func title(_ event: IssueEvent) -> String {
        if isStatus(event) { return "Status changed to \(event.status.isEmpty ? "updated" : event.status)" }
        return event.type == "vote" ? "Vote added" : "Comment"
    }

    public static func icon(_ event: IssueEvent) -> String { isStatus(event) ? "sync_alt" : "chat_bubble_outline" }

    public static func tone(_ event: IssueEvent) -> CollabTone { isStatus(event) ? .accent : .tertiary }

    public static func mentionTitle(_ mention: IssueMention) -> String {
        "Mentioned in #\(mention.sourceNumber) \(mention.sourceTitle)" + (mention.sourceStatus == "closed" ? " (closed)" : "")
    }
}

/// The rows of the issue actions sheet (`showIssueActions`).
public enum IssueActions {
    public static let voteSubmitted = "Vote submitted to the inbox."
    public static let quickActionsLabel = "Comment / vote / status"
    public static let guestAction = "comment on issues"

    public static func header(_ issue: Issue) -> String { "#\(issue.number) · \(issue.title)" }

    public static func commentTitle(_ issue: Issue) -> String { "Comment on #\(issue.number)" }

    public static func statusLabel(_ issue: Issue) -> String { issue.isOpen ? "Close issue" : "Reopen issue" }

    public static func statusIcon(_ issue: Issue) -> String { issue.isOpen ? "check_circle_outline" : "refresh" }

    public static func nextStatus(_ issue: Issue) -> String { issue.isOpen ? "closed" : "open" }
}
