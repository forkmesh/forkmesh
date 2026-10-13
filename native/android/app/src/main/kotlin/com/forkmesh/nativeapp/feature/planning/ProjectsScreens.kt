package com.forkmesh.nativeapp.feature.planning

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.feature.tools.ToolsAppBar
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.navigation.RepoRef
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.planning.PlanningProgress
import com.forkmesh.nativeapp.planning.PlanningRecords
import com.forkmesh.nativeapp.planning.PlanningRepo
import com.forkmesh.nativeapp.planning.PlanningSpan
import com.forkmesh.nativeapp.planning.RepoPlanningModel
import com.forkmesh.nativeapp.planning.RepoProject
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmKeyValueRow
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmStatusBadge
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.time.ZoneId
import java.time.format.DateTimeFormatter

private val shortDate = DateTimeFormatter.ofPattern("d MMM yyyy")

private fun date(ms: Long) = formatAt(ms, ZoneId.systemDefault(), shortDate)

private val planningModels = HashMap<String, RepoPlanningModel>()

internal fun forgetPlanningModels() = planningModels.clear()

private fun planningModel(repo: PlanningRepo): RepoPlanningModel? {
    val api = Services.get<ApiService>() ?: return null
    return planningModels.getOrPut(repo.slug) { RepoPlanningModel(api, repo) }
}

private fun openIssue(navigator: Navigator, repo: PlanningRepo, issue: Issue) {
    navigator.push(NavigationFeature.screen(Route.Issue(RepoRef(repo.owner, repo.name), issue.number)))
}

@Composable
private fun ProgressLine(progress: PlanningProgress) {
    Column(Modifier.fillMaxWidth()) {
        FmLinearProgressIndicator(Modifier.height(4.dp), progress = { progress.fraction.toFloat() }, color = Fm.colors.success)
        Text(progress.label, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
    }
}

@Composable
private fun GanttBar(project: RepoProject, span: PlanningSpan) {
    val bar = PlanningRecords.bar(project, span) ?: return
    BoxWithConstraints(Modifier.fillMaxWidth().height(10.dp).background(Fm.colors.bgOverlay, RoundedCornerShape(5.dp))) {
        Box(
            Modifier
                .offset(x = maxWidth * bar.offset.toFloat())
                .width(maxWidth * bar.width.toFloat())
                .height(10.dp)
                .background(if (project.status == "closed") Fm.colors.textTertiary else Fm.colors.accent, RoundedCornerShape(5.dp)),
        )
    }
}

private abstract class PlanningLoadScreen(protected val repo: PlanningRepo) : Screen() {
    protected val model = planningModel(repo)
    protected var loading by mutableStateOf(model?.projects?.isEmpty() ?: true)
    protected var version by mutableIntStateOf(0)
    protected var refresh by mutableIntStateOf(0)

    @Composable
    protected fun Loader() {
        val model = model ?: return
        LaunchedEffect(refresh) {
            loading = true
            withContext(Dispatchers.IO) { model.load(refresh = refresh > 0) }
            loading = false
            version++
        }
    }
}

private class ProjectsScreenImpl(repo: PlanningRepo) : PlanningLoadScreen(repo) {
    override val title: String get() = "Projects"

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        Loader()
        @Suppress("UNUSED_EXPRESSION") version
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Projects · ${repo.slug}", navigator) {
                FmIconButton("flag", "Milestones", onPressed = { navigator.push(MilestonesScreen(repo)) }, color = Fm.colors.textPrimary)
                FmIconButton("refresh", "Refresh", onPressed = { refresh++ }, color = Fm.colors.textPrimary)
            }
            FmListRow("Show closed", trailing = { FmSwitch(checked = model.showClosed, onCheckedChange = { model.showClosed = it; version++ }) }, showChevron = false, minHeight = 48.dp)
            val shown = model.shownProjects
            when {
                loading && shown.isEmpty() -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                shown.isEmpty() -> FmEmptyState("view_timeline", "No projects", message = "Projects are kept in .forkmesh/projects in the repository.")
                else -> {
                    val span = model.timeline
                    LazyColumn(Modifier.fillMaxSize()) {
                        if (span != null) item {
                            Text("${date(span.start)} – ${date(span.end)}", Modifier.padding(horizontal = 16.dp, vertical = 4.dp), style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
                        }
                        items(shown, key = { it.number }) { project ->
                            FmCard(Modifier.padding(horizontal = 16.dp, vertical = 6.dp), onTap = { navigator.push(ProjectDetailScreen(repo, project.number)) }) {
                                Column(Modifier.padding(12.dp)) {
                                    Text("#${project.number} ${project.title}", style = Fm.text.bodyMedium)
                                    if (project.hasDates) Text("${date(project.startMs)} – ${date(project.endMs)}", style = Fm.text.bodySmall, color = Fm.colors.textTertiary)
                                    if (span != null && project.hasDates) {
                                        Spacer(Modifier.height(6.dp))
                                        GanttBar(project, span)
                                    }
                                    Spacer(Modifier.height(6.dp))
                                    ProgressLine(model.progress(project))
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Suppress("FunctionName")
fun ProjectsScreen(repo: PlanningRepo): Screen = ProjectsScreenImpl(repo)

class ProjectDetailScreen(private val repo: PlanningRepo, private val number: Int) : Screen() {
    override val title: String get() = "Project"

    private val model = planningModel(repo)
    private var loading by mutableStateOf(model?.project(number) == null)
    private var version by mutableIntStateOf(0)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        LaunchedEffect(Unit) {
            if (model.project(number) == null) {
                withContext(Dispatchers.IO) { model.load() }
                loading = false
                version++
            }
        }
        @Suppress("UNUSED_EXPRESSION") version
        val project = model.project(number)
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar(project?.let { "#${it.number} ${it.title}" } ?: "Project", navigator)
            when {
                loading && project == null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                project == null -> FmEmptyState("view_timeline", "Project not found")
                else -> LazyColumn(Modifier.fillMaxSize()) {
                    item {
                        Column(Modifier.padding(16.dp)) {
                            FmStatusBadge(project.status, if (project.status == "closed") Fm.colors.textTertiary else Fm.colors.success)
                            if (project.hasDates) FmKeyValueRow("Dates", "${date(project.startMs)} – ${date(project.endMs)}")
                            if (project.milestone.isNotEmpty()) FmKeyValueRow("Milestone", project.milestone)
                            if (project.authorName.isNotEmpty()) FmKeyValueRow("Author", "@${project.authorName}")
                            ProgressLine(model.progress(project))
                            if (project.body.isNotEmpty()) {
                                Spacer(Modifier.height(12.dp))
                                Text(project.body, style = Fm.text.bodyMedium)
                            }
                        }
                    }
                    val linked = model.linkedIssues(project)
                    item { FmSectionHeader("Issues", Modifier.padding(horizontal = 16.dp, vertical = 8.dp), count = linked.size) }
                    items(linked, key = { it.number }) { issue ->
                        FmListRow("#${issue.number} ${issue.title}", subtitle = if (issue.isOpen) "Open" else "Closed", onTap = { openIssue(navigator, repo, issue) })
                    }
                }
            }
        }
    }
}

class MilestonesScreen(private val repo: PlanningRepo) : Screen() {
    override val title: String get() = "Milestones"

    private val model = planningModel(repo)
    private var loading by mutableStateOf(true)
    private var version by mutableIntStateOf(0)
    private var refresh by mutableIntStateOf(0)
    private var expanded by mutableStateOf<String?>(null)

    @Composable
    override fun Content(navigator: Navigator) {
        val model = model ?: return
        LaunchedEffect(refresh) {
            loading = true
            withContext(Dispatchers.IO) { model.load(refresh = refresh > 0) }
            loading = false
            version++
        }
        @Suppress("UNUSED_EXPRESSION") version
        Column(Modifier.fillMaxSize().background(Fm.colors.bgBase)) {
            ToolsAppBar("Milestones · ${repo.slug}", navigator) {
                FmIconButton("refresh", "Refresh", onPressed = { refresh++ }, color = Fm.colors.textPrimary)
            }
            FmListRow("Show closed", trailing = { FmSwitch(checked = model.showClosed, onCheckedChange = { model.showClosed = it; version++ }) }, showChevron = false, minHeight = 48.dp)
            val shown = model.shownMilestones
            when {
                loading && shown.isEmpty() -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) { FmCircularProgressIndicator() }
                shown.isEmpty() -> FmEmptyState("flag", "No milestones")
                else -> LazyColumn(Modifier.fillMaxSize()) {
                    shown.forEach { milestone ->
                        item(key = milestone.title) {
                            FmCard(Modifier.padding(horizontal = 16.dp, vertical = 6.dp), onTap = { expanded = if (expanded == milestone.title) null else milestone.title }) {
                                Column(Modifier.padding(12.dp)) {
                                    Text(milestone.title, style = Fm.text.bodyMedium)
                                    Text(
                                        listOfNotNull(if (milestone.dueMs > 0) "Due ${date(milestone.dueMs)}" else null, if (!milestone.isOpen) "Closed" else null).joinToString(" · ").ifEmpty { "No due date" },
                                        style = Fm.text.bodySmall,
                                        color = Fm.colors.textTertiary,
                                    )
                                    if (milestone.description.isNotEmpty()) Text(milestone.description, style = Fm.text.bodySmall)
                                    Spacer(Modifier.height(6.dp))
                                    ProgressLine(model.progress(milestone))
                                }
                            }
                        }
                        if (expanded == milestone.title) items(model.issuesIn(milestone), key = { "${milestone.title}#${it.number}" }) { issue ->
                            FmListRow("#${issue.number} ${issue.title}", subtitle = if (issue.isOpen) "Open" else "Closed", onTap = { openIssue(navigator, repo, issue) })
                        }
                    }
                }
            }
        }
    }
}
