package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.PullRequest
import com.forkmesh.nativeapp.model.Repository
import java.time.OffsetDateTime

data class IssueBurnupPoint(val timestampMs: Long, val openCount: Int, val closedCount: Int) {
    val total: Int get() = openCount + closedCount
}

object IssueBurnup {
    fun createdAt(issue: Issue): Long {
        var created = 0L
        for (event in issue.events) {
            if (event.type != "open" || event.ts <= 0) continue
            if (created == 0L || event.ts < created) created = event.ts
        }
        return created
    }

    fun status(issue: Issue, at: Long): String {
        var status = "open"
        for (change in issue.events.filter { it.type == "status" && it.ts > 0 && it.ts <= at }.sortedBy { it.ts }) {
            if (change.status == "open" || change.status == "closed") status = change.status
        }
        return status
    }

    fun series(issues: List<Issue>, start: Long, end: Long, intervals: Int): List<IssueBurnupPoint> {
        val from = minOf(start, end)
        val to = maxOf(start, end)
        val steps = maxOf(1, intervals)
        val duration = to - from
        val created = issues.map { it to createdAt(it) }
        return (0..steps).map { i ->
            val timestamp = from + duration * i / steps
            var open = 0
            var closed = 0
            for ((issue, createdAt) in created) {
                if (createdAt <= 0 || createdAt > timestamp) continue
                if (status(issue, timestamp) == "closed") closed++ else open++
            }
            IssueBurnupPoint(timestamp, open, closed)
        }
    }

    fun firstTimestamp(issues: List<Issue>, fallback: Long): Long =
        issues.map(::createdAt).filter { it > 0 }.minOrNull() ?: fallback
}

data class ContributorCount(val name: String, val commits: Int)

data class WeekCount(val weekStartMs: Long, val commits: Int)

data class RepoInsightsSummary(
    val commitsSeen: Int,
    val weekly: List<WeekCount>,
    val contributors: List<ContributorCount>,
    val openIssues: Int,
    val closedIssues: Int,
    val openPulls: Int,
    val mergedPulls: Int,
    val closedPulls: Int,
    val burnup: List<IssueBurnupPoint>,
    val language: String,
    val stars: Int,
    val forks: Int,
    val mirrors: Int,
) {
    val commitSeries: StatsTileSeries get() = StatsTileSeries(weekly.map { it.weekStartMs.toString() }, weekly.map { it.commits.toDouble() })

    val commitsInWindow: Int get() = weekly.sumOf { it.commits }

    companion object {
        const val WEEKS = 12
        const val TOP_CONTRIBUTORS = 8
        private const val WEEK_MS = 7L * 24 * 60 * 60 * 1000

        fun build(
            commits: List<Map<String, Any?>>,
            issues: List<Issue>,
            pulls: List<PullRequest>,
            repository: Repository?,
            nowMs: Long,
            burnupIntervals: Int = 12,
        ): RepoInsightsSummary {
            val currentWeek = nowMs - nowMs % WEEK_MS
            val firstWeek = currentWeek - WEEK_MS * (WEEKS - 1)
            val buckets = IntArray(WEEKS)
            val authors = LinkedHashMap<String, Int>()
            for (commit in commits) {
                val author = author(commit)
                if (author.isNotEmpty()) authors[author] = (authors[author] ?: 0) + 1
                val ms = dateMs(commit) ?: continue
                if (ms < firstWeek || ms >= currentWeek + WEEK_MS) continue
                buckets[((ms - firstWeek) / WEEK_MS).toInt()]++
            }
            val open = issues.count { it.isOpen }
            val start = IssueBurnup.firstTimestamp(issues, nowMs - 30L * 24 * 60 * 60 * 1000)
            return RepoInsightsSummary(
                commitsSeen = commits.size,
                weekly = buckets.mapIndexed { index, count -> WeekCount(firstWeek + WEEK_MS * index, count) },
                contributors = authors.map { ContributorCount(it.key, it.value) }
                    .sortedWith(compareByDescending<ContributorCount> { it.commits }.thenBy { it.name.lowercase() })
                    .take(TOP_CONTRIBUTORS),
                openIssues = open,
                closedIssues = issues.size - open,
                openPulls = pulls.count { it.status == "open" },
                mergedPulls = pulls.count { it.status == "merged" },
                closedPulls = pulls.count { it.status == "closed" },
                burnup = if (issues.isEmpty()) emptyList() else IssueBurnup.series(issues, start, nowMs, burnupIntervals),
                language = repository?.language.orEmpty(),
                stars = repository?.stars ?: 0,
                forks = repository?.forks ?: 0,
                mirrors = repository?.mirrors ?: 0,
            )
        }

        fun author(commit: Map<String, Any?>): String {
            val nested = commit["author"]
            if (nested is Map<*, *>) {
                val json = nested.asJsonMap()
                return json.str("name").ifEmpty { json.str("email") }
            }
            for (key in listOf("author", "authorName", "committer")) {
                val value = commit.str(key).trim()
                if (value.isNotEmpty()) return value
            }
            return ""
        }

        fun dateMs(commit: Map<String, Any?>): Long? {
            for (key in listOf("date", "time", "timestamp", "createdAt", "committedDate", "authorDate", "ts")) {
                val value = commit[key] ?: continue
                if (value is Number) {
                    val n = value.toLong()
                    return if (n > 100_000_000_000L) n else n * 1000
                }
                if (value is String) {
                    val text = value.trim()
                    if (text.isEmpty()) continue
                    text.toLongOrNull()?.let { return if (it > 100_000_000_000L) it else it * 1000 }
                    parseIso(text)?.let { return it }
                }
            }
            return null
        }

        fun parseIso(text: String): Long? =
            runCatching { OffsetDateTime.parse(text).toInstant().toEpochMilli() }.getOrNull()
    }
}
