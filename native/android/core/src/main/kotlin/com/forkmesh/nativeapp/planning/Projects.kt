package com.forkmesh.nativeapp.planning

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.Issue
import kotlin.math.roundToInt

data class PlanningRepo(val owner: String, val name: String) {
    val slug: String get() = "$owner/$name"

    companion object {
        fun of(slug: String): PlanningRepo? {
            val parts = slug.trim().split('/')
            if (parts.size != 2 || parts[0].isEmpty() || parts[1].isEmpty()) return null
            return PlanningRepo(parts[0], parts[1])
        }
    }
}

data class RepoMilestone(val title: String, val dueMs: Long = 0, val status: String = "open", val description: String = "") {
    val isOpen: Boolean get() = status != "closed"
}

data class RepoProject(
    val number: Int,
    val title: String,
    val body: String = "",
    val status: String = "open",
    val startMs: Long = 0,
    val endMs: Long = 0,
    val milestone: String = "",
    val issues: List<Int> = emptyList(),
    val authorName: String = "",
    val updatedAtMs: Long = 0,
) {
    val hasDates: Boolean get() = startMs > 0 && endMs > startMs

    companion object {
        fun fromJson(json: JsonMap): RepoProject? {
            val number = json.int("number")
            if (number <= 0 || json.str("schema", "forkmesh-project-v1") != "forkmesh-project-v1") return null
            val deleted = json.maps("events").any { it.str("type") == "delete" && it.str("target", "self") == "self" }
            if (deleted) return null
            return RepoProject(
                number = number,
                title = json.str("title"),
                body = json.str("body"),
                status = json.str("status", "open"),
                startMs = json.long("startDate"),
                endMs = json.long("endDate"),
                milestone = json.str("milestone"),
                issues = json.list("issues").filterIsInstance<Number>().map { it.toInt() }.filter { it > 0 },
                authorName = json.str("authorName"),
                updatedAtMs = json.long("updatedAt"),
            )
        }
    }
}

data class PlanningProgress(val closed: Int, val total: Int) {
    val fraction: Double get() = if (total == 0) 0.0 else closed.toDouble() / total
    val percent: Int get() = (fraction * 100).roundToInt()
    val label: String get() = if (total == 0) "No issues" else "$closed of $total closed · $percent%"
}

data class PlanningSpan(val start: Long, val end: Long)

data class PlanningBar(val offset: Double, val width: Double)

object PlanningRecords {
    const val PROJECTS_PATH = ".forkmesh/projects"
    const val MILESTONES_PATH = ".forkmesh/issues/milestones.json"
    const val MAX_PROJECTS = 200

    fun milestones(text: String): List<RepoMilestone> {
        val list = runCatching { Json.parse(text) as? List<*> }.getOrNull() ?: return emptyList()
        return list.filterIsInstance<Map<*, *>>().take(500).mapNotNull { raw ->
            val map = raw.asJsonMap()
            val title = map.str("title").trim()
            if (title.isEmpty()) null else RepoMilestone(title, map.long("due"), map.str("status", "open"), map.str("description"))
        }
    }

    fun linkedIssues(project: RepoProject, issues: List<Issue>): List<Issue> = when {
        project.issues.isNotEmpty() -> project.issues.toSet().let { wanted -> issues.filter { it.number in wanted } }
        project.milestone.isNotEmpty() -> issues.filter { it.milestone == project.milestone }
        else -> emptyList()
    }

    fun progress(project: RepoProject, issues: List<Issue>): PlanningProgress =
        linkedIssues(project, issues).let { linked -> PlanningProgress(linked.count { !it.isOpen }, linked.size) }

    fun progress(milestone: RepoMilestone, issues: List<Issue>): PlanningProgress =
        issues.filter { it.milestone == milestone.title }.let { linked -> PlanningProgress(linked.count { !it.isOpen }, linked.size) }

    fun timeline(projects: List<RepoProject>): PlanningSpan? {
        val dated = projects.filter { it.hasDates }
        val start = dated.minOfOrNull { it.startMs } ?: return null
        val end = dated.maxOfOrNull { it.endMs } ?: return null
        return if (end > start) PlanningSpan(start, end) else null
    }

    fun bar(project: RepoProject, span: PlanningSpan): PlanningBar? {
        if (!project.hasDates) return null
        val total = (span.end - span.start).toDouble()
        val offset = (project.startMs - span.start) / total
        val width = (project.endMs - project.startMs) / total
        return PlanningBar(offset.coerceIn(0.0, 1.0), maxOf(0.01, minOf(1 - maxOf(0.0, offset), width)))
    }
}

class RepoPlanningModel(private val api: ApiService, val repo: PlanningRepo) {
    var projects: List<RepoProject> = emptyList()
        private set
    var milestones: List<RepoMilestone> = emptyList()
        private set
    var issues: List<Issue> = emptyList()
        private set
    var error: String? = null
        private set
    var showClosed: Boolean = false

    val shownProjects: List<RepoProject>
        get() = (if (showClosed) projects else projects.filter { it.status != "closed" }).sortedWith(
            compareBy<RepoProject> { !it.hasDates }.thenBy { it.startMs }.thenBy { it.number },
        )

    val shownMilestones: List<RepoMilestone>
        get() = (if (showClosed) milestones else milestones.filter { it.isOpen }).sortedBy { if (it.dueMs == 0L) Long.MAX_VALUE else it.dueMs }

    val timeline: PlanningSpan? get() = PlanningRecords.timeline(shownProjects)

    fun project(number: Int): RepoProject? = projects.firstOrNull { it.number == number }

    fun progress(project: RepoProject): PlanningProgress = PlanningRecords.progress(project, issues)

    fun progress(milestone: RepoMilestone): PlanningProgress = PlanningRecords.progress(milestone, issues)

    fun linkedIssues(project: RepoProject): List<Issue> = PlanningRecords.linkedIssues(project, issues)

    fun issuesIn(milestone: RepoMilestone): List<Issue> = issues.filter { it.milestone == milestone.title }

    fun load(refresh: Boolean = false) {
        if (refresh) api.clearRepoCache(repo.owner, repo.name)
        val found = ArrayList<RepoProject>()
        val listing = runCatching { api.tree(repo.owner, repo.name, PlanningRecords.PROJECTS_PATH, "") }.getOrNull()
        if (listing != null) {
            val dirs = listing.entries.filter { it.isDirectory && it.name.toIntOrNull() != null }.take(PlanningRecords.MAX_PROJECTS)
            for (dir in dirs) {
                val path = "${PlanningRecords.PROJECTS_PATH}/${dir.name}/project-${dir.name}.json"
                val blob = runCatching { api.blob(repo.owner, repo.name, path, "") }.getOrNull() ?: continue
                val map = runCatching { Json.parse(blob.content) }.getOrNull() as? Map<*, *> ?: continue
                RepoProject.fromJson(map.asJsonMap())?.let(found::add)
            }
        }
        projects = found
        milestones = runCatching { api.blob(repo.owner, repo.name, PlanningRecords.MILESTONES_PATH, "") }.getOrNull()
            ?.let { PlanningRecords.milestones(it.content) } ?: emptyList()
        issues = runCatching { api.publishedIssues(repo.owner, repo.name) }.getOrNull() ?: emptyList()
        error = null
    }
}
