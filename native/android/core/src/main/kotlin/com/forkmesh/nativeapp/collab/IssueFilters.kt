package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.model.Issue

enum class IssueStatusFilter(val label: String, val key: String) {
    Open("Open", "open"),
    Closed("Closed", "closed"),
    All("All", "all"),
    ;

    fun matches(issue: Issue): Boolean = when (this) {
        Open -> issue.status != "closed"
        Closed -> issue.status == "closed"
        All -> true
    }
}

enum class IssueFilterField(
    val title: String,
    val anyLabel: String,
    val key: String,
) {
    Author("Author", "Any author", "author"),
    Label("Label", "Any label", "label"),
    Milestone("Milestone", "Any milestone", "milestone"),
    Assignee("Assignee", "Anyone", "assignee"),
}

const val ISSUE_FILTER_NONE = "__forkmesh_none__"
const val ISSUE_FILTER_ME = "__forkmesh_me__"

data class IssueFilters(
    val status: IssueStatusFilter = IssueStatusFilter.Open,
    val values: Map<IssueFilterField, String> = emptyMap(),
) {
    fun valueOf(field: IssueFilterField): String = values[field] ?: ""

    fun select(field: IssueFilterField, value: String): IssueFilters =
        copy(values = if (value.isEmpty()) values - field else values + (field to value))

    fun withStatus(status: IssueStatusFilter): IssueFilters = copy(status = status)

    val hasMetadataFilter: Boolean get() = values.isNotEmpty()

    fun clearMetadata(): IssueFilters = copy(values = emptyMap())
}

data class IssueFilterOption(val value: String, val label: String)

private fun sameIssueValue(left: String, right: String): Boolean = left.trim().lowercase() == right.trim().lowercase()

private fun hasIssueValue(values: List<String>, wanted: String): Boolean = values.any { sameIssueValue(it, wanted) }

fun issueMatchesFilters(issue: Issue, filters: IssueFilters, account: String): Boolean {
    if (!filters.status.matches(issue)) return false

    val author = filters.valueOf(IssueFilterField.Author)
    if (author.isNotEmpty() && !sameIssueValue(issue.author, author)) return false

    val label = filters.valueOf(IssueFilterField.Label)
    if (label.isNotEmpty() && !hasIssueValue(issue.labels, label)) return false

    val milestone = filters.valueOf(IssueFilterField.Milestone)
    if (milestone == ISSUE_FILTER_NONE) {
        if (issue.milestone.trim().isNotEmpty()) return false
    } else if (milestone.isNotEmpty() && !sameIssueValue(issue.milestone, milestone)) {
        return false
    }

    val assignee = filters.valueOf(IssueFilterField.Assignee)
    if (assignee == ISSUE_FILTER_NONE) {
        if (issue.assignees.any { it.trim().isNotEmpty() }) return false
    } else if (assignee == ISSUE_FILTER_ME) {
        if (account.isEmpty() || !hasIssueValue(issue.assignees, account)) return false
    } else if (assignee.isNotEmpty() && !hasIssueValue(issue.assignees, assignee)) {
        return false
    }
    return true
}

fun issueFilterValues(field: IssueFilterField, issues: List<Issue>): List<String> {
    val values = LinkedHashMap<String, String>()
    fun add(raw: String) {
        val value = raw.trim()
        if (value.isEmpty()) return
        values.putIfAbsent(value.lowercase(), value)
    }
    for (issue in issues) {
        when (field) {
            IssueFilterField.Author -> add(issue.author)
            IssueFilterField.Label -> issue.labels.forEach(::add)
            IssueFilterField.Milestone -> add(issue.milestone)
            IssueFilterField.Assignee -> issue.assignees.forEach(::add)
        }
    }
    return values.keys.sorted().map { values.getValue(it) }
}

fun issueFilterOptions(field: IssueFilterField, issues: List<Issue>, account: String): List<IssueFilterOption> {
    val values = issueFilterValues(field, issues).map { IssueFilterOption(it, it) }
    return when (field) {
        IssueFilterField.Author, IssueFilterField.Label -> listOf(IssueFilterOption("", field.anyLabel)) + values
        IssueFilterField.Milestone -> listOf(
            IssueFilterOption("", field.anyLabel),
            IssueFilterOption(ISSUE_FILTER_NONE, "No milestone"),
        ) + values
        IssueFilterField.Assignee -> buildList {
            add(IssueFilterOption("", field.anyLabel))
            if (account.isNotEmpty()) add(IssueFilterOption(ISSUE_FILTER_ME, "Assigned to me"))
            add(IssueFilterOption(ISSUE_FILTER_NONE, "Unassigned"))
            addAll(values)
        }
    }
}

fun normalizeIssueFilters(filters: IssueFilters, issues: List<Issue>, account: String): IssueFilters {
    var result = filters
    for (field in IssueFilterField.entries) {
        val selected = result.valueOf(field)
        if (selected.isEmpty()) continue
        val options = issueFilterOptions(field, issues, account)
        if (options.none { it.value == selected }) result = result.select(field, "")
    }
    return result
}

fun issueFilterSelection(options: List<IssueFilterOption>, value: String): IssueFilterOption {
    val selected = if (options.any { it.value == value }) value else ""
    return options.first { it.value == selected }
}
