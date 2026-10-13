// The issue filter model: status and metadata filters, the none/me values,
// value matching, filter values and options, and normalization. After
// native/android core/.../collab/IssueFilters.kt.

import FMApi
import FMBase
import Foundation

/// The open/closed/all switch; the raw value names the widget keys
/// (`issue-status-<name>`).
public enum IssueStatusFilter: String, Sendable, CaseIterable {
    case open, closed, all

    public var label: String {
        switch self {
        case .open: return "Open"
        case .closed: return "Closed"
        case .all: return "All"
        }
    }

    /// "Open" means "not closed", matching the desktop advert, the served
    /// counts and the web dashboard. A strict `status == "open"` test drops
    /// issues with any other non-closed status (for example "reopened").
    public func matches(_ issue: Issue) -> Bool {
        switch self {
        case .open: return issue.status != "closed"
        case .closed: return issue.status == "closed"
        case .all: return true
        }
    }
}

/// One metadata filter; the raw value names the widget keys
/// (`issue-filter-<name>`).
public enum IssueFilterField: String, Sendable, CaseIterable {
    case author, label, milestone, assignee

    /// Pill caption while nothing is selected for this field.
    public var title: String {
        switch self {
        case .author: return "Author"
        case .label: return "Label"
        case .milestone: return "Milestone"
        case .assignee: return "Assignee"
        }
    }

    /// First menu entry, the one that turns the field off again.
    public var anyLabel: String {
        switch self {
        case .author: return "Any author"
        case .label: return "Any label"
        case .milestone: return "Any milestone"
        case .assignee: return "Anyone"
        }
    }
}

// Sentinels shared with the web dashboard's issue toolbar (issueMatchesFilters
// in dashboard/js/06-repo-content.js) so both surfaces offer the same
// "No milestone" / "Unassigned" / "Assigned to me" choices.
public let issueFilterNone = "__forkmesh_none__"
public let issueFilterMe = "__forkmesh_me__"

/// The four metadata filters plus the open/closed/all switch. A value: every
/// change returns (or mutates) a copy the view model holds.
public struct IssueFilters: Equatable, Sendable {
    public var status: IssueStatusFilter
    public private(set) var values: [IssueFilterField: String]

    public init(status: IssueStatusFilter = .open, values: [IssueFilterField: String] = [:]) {
        self.status = status
        self.values = values.filter { !$0.value.isEmpty }
    }

    public func valueOf(_ field: IssueFilterField) -> String { values[field] ?? "" }

    public func selecting(_ field: IssueFilterField, _ value: String) -> IssueFilters {
        var copy = self
        copy.select(field, value)
        return copy
    }

    public mutating func select(_ field: IssueFilterField, _ value: String) {
        if value.isEmpty {
            values.removeValue(forKey: field)
        } else {
            values[field] = value
        }
    }

    public func withStatus(_ status: IssueStatusFilter) -> IssueFilters {
        var copy = self
        copy.status = status
        return copy
    }

    public var hasMetadataFilter: Bool { !values.isEmpty }

    public func clearingMetadata() -> IssueFilters {
        var copy = self
        copy.values = [:]
        return copy
    }
}

public struct IssueFilterOption: Equatable, Hashable, Sendable {
    public let value: String
    public let label: String

    public init(_ value: String, _ label: String) {
        self.value = value
        self.label = label
    }
}

/// The trimmed value, lower-cased.
private func issueKey(_ value: String) -> String { value.dartTrimmed.lowercased() }

private func sameIssueValue(_ left: String, _ right: String) -> Bool { issueKey(left) == issueKey(right) }

private func hasIssueValue(_ values: [String], _ wanted: String) -> Bool {
    values.contains { sameIssueValue($0, wanted) }
}

public func issueMatchesFilters(_ issue: Issue, _ filters: IssueFilters, account: String) -> Bool {
    if !filters.status.matches(issue) { return false }

    let author = filters.valueOf(.author)
    if !author.isEmpty && !sameIssueValue(issue.author, author) { return false }

    let label = filters.valueOf(.label)
    if !label.isEmpty && !hasIssueValue(issue.labels, label) { return false }

    let milestone = filters.valueOf(.milestone)
    if milestone == issueFilterNone {
        if !issue.milestone.dartTrimmed.isEmpty { return false }
    } else if !milestone.isEmpty && !sameIssueValue(issue.milestone, milestone) {
        return false
    }

    let assignee = filters.valueOf(.assignee)
    if assignee == issueFilterNone {
        if issue.assignees.contains(where: { !$0.dartTrimmed.isEmpty }) { return false }
    } else if assignee == issueFilterMe {
        if account.isEmpty || !hasIssueValue(issue.assignees, account) { return false }
    } else if !assignee.isEmpty && !hasIssueValue(issue.assignees, assignee) {
        return false
    }
    return true
}

/// Distinct values a field actually carries across the loaded issues, first
/// spelling wins, sorted case-insensitively (the lower-cased keys in UTF-16
/// code unit order). Offering only values that exist keeps every choice
/// capable of matching something.
public func issueFilterValues(_ field: IssueFilterField, _ issues: [Issue]) -> [String] {
    var order: [String] = []
    var spelling: [String: String] = [:]
    func add(_ raw: String) {
        let value = raw.dartTrimmed
        if value.isEmpty { return }
        let key = value.lowercased()
        if spelling[key] == nil {
            spelling[key] = value
            order.append(key)
        }
    }
    for issue in issues {
        switch field {
        case .author: add(issue.author)
        case .label: issue.labels.forEach(add)
        case .milestone: add(issue.milestone)
        case .assignee: issue.assignees.forEach(add)
        }
    }
    let sorted = order.sorted { Array($0.utf16).lexicographicallyPrecedes(Array($1.utf16)) }
    return sorted.map { spelling[$0] ?? $0 }
}

public func issueFilterOptions(_ field: IssueFilterField, _ issues: [Issue], account: String) -> [IssueFilterOption] {
    let values = issueFilterValues(field, issues).map { IssueFilterOption($0, $0) }
    switch field {
    case .author, .label:
        return [IssueFilterOption("", field.anyLabel)] + values
    case .milestone:
        return [IssueFilterOption("", field.anyLabel), IssueFilterOption(issueFilterNone, "No milestone")] + values
    case .assignee:
        // "Assigned to me" can only ever match while signed in, so it is
        // offered only then instead of sitting in the menu as a guaranteed
        // empty result.
        var options = [IssueFilterOption("", field.anyLabel)]
        if !account.isEmpty { options.append(IssueFilterOption(issueFilterMe, "Assigned to me")) }
        options.append(IssueFilterOption(issueFilterNone, "Unassigned"))
        return options + values
    }
}

/// A saved selection can disappear after the list is refreshed, or "Assigned
/// to me" can become unavailable after sign-out: such stale values are
/// cleared before both matching and rendering, so a pill never looks inactive
/// while a hidden filter still applies.
public func normalizeIssueFilters(_ filters: IssueFilters, _ issues: [Issue], account: String) -> IssueFilters {
    var result = filters
    for field in IssueFilterField.allCases {
        let selected = result.valueOf(field)
        if selected.isEmpty { continue }
        let options = issueFilterOptions(field, issues, account: account)
        if !options.contains(where: { $0.value == selected }) { result.select(field, "") }
    }
    return result
}

/// The pill's defensive fallback: the selected option, or the "any" entry
/// for a stale value.
public func issueFilterSelection(_ options: [IssueFilterOption], _ value: String) -> IssueFilterOption {
    let selected = options.contains { $0.value == value } ? value : ""
    return options.first { $0.value == selected } ?? IssueFilterOption("", "")
}
