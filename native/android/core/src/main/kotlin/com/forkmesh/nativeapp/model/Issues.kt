package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.str

private fun dartString(value: Any?): String = if (value == null) "null" else jsonText(value)

data class Issue(
    val number: Int,
    val title: String,
    val status: String = "open",
    val body: String = "",
    val author: String = "",
    val labels: List<String> = emptyList(),
    val milestone: String = "",
    val priority: Int = 0,
    val progress: Int = 0,
    val assignees: List<String> = emptyList(),
    val events: List<IssueEvent> = emptyList(),
    val votes: Int = 0,
    val bountyUsd: Double = 0.0,
    val bountyAddress: String = "",
    val bountyStatus: String = "",
    val bountyRequiredLamports: Long = 0,
    val bountyReceivedLamports: Long = 0,
    val bountyAmountSol: Double = 0.0,
    val bountyPayUri: String = "",
    val bountyPayee: String = "",
    val bountyPayoutSig: String = "",
) {
    val isOpen: Boolean get() = status != "closed"

    val bounty: IssueBounty
        get() = IssueBounty(
            address = bountyAddress,
            status = if (bountyStatus.isEmpty()) "open" else bountyStatus,
            amountUsd = bountyUsd,
            requiredLamports = bountyRequiredLamports,
            receivedLamports = bountyReceivedLamports,
            amountSol = bountyAmountSol,
            payUri = bountyPayUri,
            payee = bountyPayee,
            payoutSig = bountyPayoutSig,
        )

    val bountyStatusLabel: String get() = bounty.statusLabel

    val bountyProgressLabel: String get() = bounty.progressLabel

    val hasBountyFunding: Boolean
        get() = bountyUsd > 0 ||
            bountyAddress.isNotEmpty() ||
            bountyRequiredLamports > 0 ||
            bountyReceivedLamports > 0 ||
            bountyPayoutSig.isNotEmpty()

    fun copyWithBounty(bounty: IssueBounty): Issue = copy(
        bountyUsd = if (bounty.amountUsd > 0) bounty.amountUsd else bountyUsd,
        bountyAddress = bounty.address,
        bountyStatus = bounty.statusLabel,
        bountyRequiredLamports = bounty.requiredLamports,
        bountyReceivedLamports = bounty.receivedLamports,
        bountyAmountSol = bounty.amountSol,
        bountyPayUri = bounty.payUri,
        bountyPayee = bounty.payee,
        bountyPayoutSig = bounty.payoutSig,
    )

    companion object {
        fun fromJson(j: Map<String, Any?>): Issue {
            val bounty = IssueBounty.fromJson(
                if (j["bounty"] is Map<*, *>) {
                    j["bounty"]
                } else {
                    linkedMapOf(
                        "address" to (j["bountyAddress"] ?: j["address"]),
                        "status" to j["bountyStatus"],
                        "amountUsd" to (j["bountyUsd"] ?: j["amountUsd"]),
                        "requiredLamports" to (j["bountyRequiredLamports"] ?: j["requiredLamports"]),
                        "receivedLamports" to (j["bountyReceivedLamports"] ?: j["receivedLamports"]),
                        "amountSol" to (j["bountyAmountSol"] ?: j["amountSol"]),
                        "uri" to (j["bountyPayUri"] ?: j["payUri"] ?: j["uri"]),
                        "payee" to (j["bountyPayee"] ?: j["payee"]),
                        "payoutSig" to (j["bountyPayoutSig"] ?: j["payoutSig"]),
                    )
                },
            )
            val labels = j["labels"] as List<*>?
            val assignees = j["assignees"] as List<*>?
            val events = j["events"] as List<*>?
            val bountyUsd = j["bountyUsd"]
            return Issue(
                number = modelInt(j["number"]).toInt(),
                title = j.str("title"),
                status = j.str("status", "open"),
                body = j.str("body"),
                author = j.str("author"),
                labels = labels?.map(::dartString) ?: emptyList(),
                milestone = j.str("milestone"),
                priority = modelInt(j["priority"]).toInt(),
                progress = modelInt(j["progress"]).coerceIn(0L, 100L).toInt(),
                assignees = assignees?.map(::dartString) ?: emptyList(),
                events = events?.let { list -> jsonMaps(list).map { IssueEvent.fromJson(LinkedHashMap(it)) } } ?: emptyList(),
                votes = modelInt(j["votes"]).toInt(),
                bountyUsd = if (bountyUsd is Number) bountyUsd.toDouble() else bounty.amountUsd,
                bountyAddress = bounty.address,
                bountyStatus = bounty.status,
                bountyRequiredLamports = bounty.requiredLamports,
                bountyReceivedLamports = bounty.receivedLamports,
                bountyAmountSol = bounty.amountSol,
                bountyPayUri = bounty.payUri,
                bountyPayee = bounty.payee,
                bountyPayoutSig = bounty.payoutSig,
            )
        }
    }
}

data class IssueEvent(
    val type: String,
    val id: String = "",
    val target: String = "",
    val body: String = "",
    val author: String = "",
    val authorName: String = "",
    val status: String = "",
    val ts: Long = 0,
) {
    val displayAuthor: String get() = if (authorName.isNotEmpty()) authorName else author

    companion object {
        fun fromJson(j: Map<String, Any?>): IssueEvent = IssueEvent(
            type = j.str("type"),
            id = j.str("id"),
            target = j.str("target"),
            body = j.str("body"),
            author = j.str("author"),
            authorName = j.str("authorName"),
            status = j.str("status"),
            ts = modelInt(j["ts"]),
        )
    }
}

private val ISSUE_RELATION_LINE = Regex(
    """^[ \t]*(?:[-*+][ \t]+)?(?:\*\*)?(?:parent\s+issues?|depends?\s+on)(?:\*\*)?[ \t]*:(?:\*\*)?[ \t]*(.+)$""",
    setOf(RegexOption.IGNORE_CASE, RegexOption.MULTILINE),
)

private val RELATION_REF = Regex("""#([1-9]\d*)\b""")
private val RELATION_HAS_REF = Regex("""#[1-9]""")
private val ISSUE_REFERENCE = Regex("""(?<![\w&#/])#([1-9]\d{0,8})\b""")
private val INLINE_CODE = Regex("""`+[^`]*`+""")
private val SUB_ISSUE_ROW = Regex("""\(sub-issue\s+#([1-9]\d*)\)""", RegexOption.IGNORE_CASE)

private fun issueNumberOf(digits: String): Int? = digits.toLongOrNull()?.takeIf { it <= Int.MAX_VALUE }?.toInt()

fun issueRelationNumbers(body: String): List<Int> {
    val numbers = ArrayList<Int>()
    for (line in ISSUE_RELATION_LINE.findAll(body)) {
        for (ref in RELATION_REF.findAll(line.groupValues[1])) {
            val number = issueNumberOf(ref.groupValues[1]) ?: continue
            if (!numbers.contains(number)) numbers.add(number)
            if (numbers.size == 25) return numbers
        }
    }
    return numbers
}

fun issueBodyWithoutRelationLines(body: String): String = body
    .replace(ISSUE_RELATION_LINE) { match ->
        if (RELATION_HAS_REF.containsMatchIn(match.groupValues[1])) "" else match.value
    }
    .replace(Regex("\n{3,}"), "\n\n")
    .trim()

fun issueReferenceNumbers(body: String): List<Int> {
    val numbers = ArrayList<Int>()
    var fence = ""
    for (rawLine in body.split('\n')) {
        val trimmed = rawLine.trim()
        val fenceLine = trimmed.startsWith("```") || trimmed.startsWith("~~~")
        if (fence.isNotEmpty()) {
            if (fenceLine && trimmed.startsWith(fence)) fence = ""
            continue
        }
        if (fenceLine) {
            fence = trimmed.substring(0, 3)
            continue
        }
        val line = rawLine.replace(INLINE_CODE, " ")
        for (match in ISSUE_REFERENCE.findAll(line)) {
            val number = match.groupValues[1].toInt()
            if (!numbers.contains(number)) numbers.add(number)
        }
    }
    return numbers
}

fun subIssueNumbersOf(issues: List<Issue>, parent: Issue): List<Int> {
    val numbers = LinkedHashSet<Int>()
    for (issue in issues) {
        if (issue.number == parent.number) continue
        if (issueRelationNumbers(issue.body).contains(parent.number)) numbers.add(issue.number)
    }
    for (match in SUB_ISSUE_ROW.findAll(parent.body)) {
        val number = issueNumberOf(match.groupValues[1]) ?: continue
        if (number != parent.number) numbers.add(number)
    }
    return numbers.sorted()
}

data class IssueMention(
    val sourceNumber: Int,
    val sourceTitle: String,
    val sourceStatus: String,
    val authorName: String,
    val ts: Long,
)

fun issueMentionsOf(issues: List<Issue>, number: Int): List<IssueMention> {
    val mentions = ArrayList<IssueMention>()
    for (source in issues) {
        if (source.number == number) continue
        val edits = HashMap<String, String>()
        val deleted = HashSet<String>()
        for (event in source.events) {
            if (event.type == "edit" && event.target.isNotEmpty()) {
                edits[event.target] = event.body
            } else if (event.type == "delete" && event.target.isNotEmpty()) {
                deleted.add(event.target)
            }
        }
        val credited = HashSet<String>()
        for (event in source.events) {
            val opening = event.type == "open"
            if (!opening && event.type != "comment") continue
            if (event.id.isNotEmpty() && deleted.contains(event.id)) continue
            val body = edits[event.id] ?: event.body
            if (!issueReferenceNumbers(body).contains(number)) continue
            val who = if (opening && source.author.isNotEmpty()) source.author else event.displayAuthor
            if (!credited.add(who)) continue
            mentions.add(
                IssueMention(
                    sourceNumber = source.number,
                    sourceTitle = source.title,
                    sourceStatus = source.status,
                    authorName = who,
                    ts = event.ts,
                ),
            )
        }
    }
    return mentions.sortedBy { it.ts }
}
