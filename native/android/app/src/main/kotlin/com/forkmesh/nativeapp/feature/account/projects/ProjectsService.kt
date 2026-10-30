package com.forkmesh.nativeapp.feature.account.projects

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.account.projects.CrossProjectCatalog
import com.forkmesh.nativeapp.account.projects.ProbeResult
import com.forkmesh.nativeapp.account.projects.Project
import com.forkmesh.nativeapp.account.projects.ProjectCatalogEntry
import com.forkmesh.nativeapp.account.projects.ProjectDirectory
import com.forkmesh.nativeapp.account.projects.ProjectProbe
import com.forkmesh.nativeapp.account.projects.ProjectSwitchController
import com.forkmesh.nativeapp.account.projects.ProjectSwitchDriver
import com.forkmesh.nativeapp.account.projects.SwitchPhase
import com.forkmesh.nativeapp.account.projects.SwitchResult
import com.forkmesh.nativeapp.api.AuthController
import com.forkmesh.nativeapp.chat.ChatHistoryStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.Features
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.feature.account.PrefsStore
import com.forkmesh.nativeapp.market.PackageGate
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.platform.SecureStore
import com.forkmesh.nativeapp.shell.HomeShell
import com.forkmesh.nativeapp.ui.navigation.AppNavigation
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import java.io.File

fun interface ProjectConnectionProbe {
    fun connected(): Boolean?
}

class ProjectsService(
    private val app: App,
    val directory: ProjectDirectory,
    private val secrets: SecureStore,
    private val prefs: Prefs,
) {
    private val basePrefs = PrefsStore(prefs)
    private val projectsState = MutableStateFlow(directory.projects)
    private val activeState = MutableStateFlow(directory.active)
    private val phaseState = MutableStateFlow(SwitchPhase.idle)
    private val statusState = MutableStateFlow("")
    private val generationState = MutableStateFlow(0)
    private val probe = ProjectProbe()
    private val catalog = CrossProjectCatalog()

    val projects: StateFlow<List<Project>> = projectsState.asStateFlow()
    val active: StateFlow<Project> = activeState.asStateFlow()
    val phase: StateFlow<SwitchPhase> = phaseState.asStateFlow()
    val statusLine: StateFlow<String> = statusState.asStateFlow()
    val generation: StateFlow<Int> = generationState.asStateFlow()

    val switcher = ProjectSwitchController(Driver())

    init {
        directory.addListener { publish() }
        switcher.addListener {
            phaseState.value = switcher.phase
            statusState.value = switcher.statusLine
        }
    }

    private fun publish() {
        projectsState.value = directory.projects
        activeState.value = directory.active
    }

    fun sessionFor(projectId: String): AuthSession? = AuthController.readSession(secrets, projectId)

    fun isSignedInOn(projectId: String): Boolean = AuthController.isSignedInOn(secrets, basePrefs, projectId)

    suspend fun switchTo(projectId: String): SwitchResult = withContext(Dispatchers.IO) { switcher.switchTo(projectId) }

    suspend fun awaitConnection(projectId: String): Boolean? = withContext(Dispatchers.IO) { switcher.awaitConnection(projectId) }

    suspend fun remove(projectId: String): SwitchResult = withContext(Dispatchers.IO) { switcher.removeProject(projectId) }

    fun clearError() = switcher.clearError()

    suspend fun check(input: String, label: String = "", worldUrl: String = ""): ProbeResult =
        withContext(Dispatchers.IO) { probe.check(input, directory.projects, label, worldUrl) }

    suspend fun checkInstance(input: String): ProbeResult =
        withContext(Dispatchers.IO) { probe.check(input, directory.projects) }

    fun instanceOwner(instance: Project): Project? = directory.ownerOf(instance)

    suspend fun attachInstance(projectId: String, instance: Project): Project? = withContext(Dispatchers.IO) {
        val attached = directory.attach(projectId, instance) ?: return@withContext null
        if (projectId == directory.activeId) {
            val driver = Driver()
            driver.leave()
            driver.clear()
            driver.adopt(projectId)
            driver.join()
        }
        attached
    }

    suspend fun testConnection(project: Project): ProbeResult = withContext(Dispatchers.IO) {
        val result = probe.verify(project)
        if (result.ok) directory.markVerified(project.id, result.rev, result.features)
        result
    }

    fun add(project: Project): Project {
        val fresh = directory.byId(project.id) == null && directory.ownerOf(project) == null
        return directory.add(project).also { if (fresh) markNew(it) }
    }

    fun addNamed(name: String, logo: String = ""): Project? = directory.addNamed(name, logo)?.also(::markNew)

    private fun markNew(project: Project) {
        if (!project.scope.isLegacy) PackageGate.markNewProject(project.scope.prefs(basePrefs))
    }

    val logos: ProjectLogoStore = ProjectLogoStore(File(app.filesDir, "project-logos"))

    fun setLogo(projectId: String, logo: String) {
        val previous = directory.byId(projectId)?.logo.orEmpty()
        directory.setLogo(projectId, logo)
        if (previous.isNotEmpty() && previous != logo) logos.delete(previous)
    }

    suspend fun adoptInstanceLogo(projectId: String, logoUrl: String, branded: Boolean = false) {
        val project = directory.byId(projectId)?.takeIf { it.hasInstance && it.logo.isEmpty() } ?: return
        val saved = withContext(Dispatchers.IO) {
            ProjectImages.fetch(project.httpOrigin, logoUrl, favicon = !branded)?.let { bitmap ->
                logos.save(ProjectLogoStore.png(bitmap), project.label.ifEmpty { project.host })?.let { it to bitmap }
            }
        } ?: return
        if (directory.byId(projectId)?.logo?.isEmpty() != true) {
            logos.delete(saved.first)
            return
        }
        ProjectImages.remember(saved.first, saved.second)
        setLogo(projectId, saved.first)
    }

    fun update(projectId: String, label: String? = null, worldUrl: String? = null) = directory.update(projectId, label, worldUrl)

    suspend fun refreshCatalog(onUpdate: (List<ProjectCatalogEntry>) -> Unit): List<ProjectCatalogEntry> =
        withContext(Dispatchers.IO) { catalog.refresh(directory, { sessionFor(it)?.sessionToken }, onUpdate) }

    fun useUrl(url: String) {
        val target = if (url.isEmpty()) {
            directory.projects.firstOrNull { it.builtIn }?.id
        } else {
            Project.tryParse(url)?.let { parsed -> (directory.byId(parsed.id) ?: directory.add(parsed)).id }
        } ?: return
        AppLifecycle.scope.launch { switchTo(target) }
    }

    private fun eraseProject(project: Project) {
        if (project.logo.isNotEmpty()) logos.delete(project.logo)
        val scope = project.scope
        runCatching { ChatHistoryStore(File(AppFiles.projectSupportDir(app, scope), "chat-history")).eraseAll() }
        if (scope.isLegacy) return
        for (key in secrets.keys()) if (key.startsWith(scope.namespace)) secrets.delete(key)
        for (key in prefs.keys()) if (key.startsWith(scope.namespace)) prefs.remove(key)
        for (base in listOf(app.filesDir, app.noBackupFilesDir)) {
            val dir = scope.directory(base)
            val projectsRoot = File(base, "projects").canonicalPath + File.separator
            if (dir.canonicalPath.startsWith(projectsRoot) && dir.exists()) dir.deleteRecursively()
        }
    }

    private inner class Driver : ProjectSwitchDriver {
        override val directory: ProjectDirectory get() = this@ProjectsService.directory

        override val hasAppAccess: Boolean get() = Services.get<AuthService>()?.hasAppAccess?.value ?: true

        override fun isConnected(): Boolean? = Services.get<ProjectConnectionProbe>()?.connected() ?: true

        override fun leave() {
            runBlocking(Dispatchers.Main) { ProjectLifecycle.close() }
        }

        override fun clear() {
            Services.clear()
        }

        override fun adopt(projectId: String) {
            directory.setActive(projectId)
            runBlocking(Dispatchers.Main) {
                Features.install(app)
                generationState.value = generationState.value + 1
            }
        }

        override fun join() {
            runBlocking(Dispatchers.Main) { AppNavigation.navigator.setRoot(HomeShell()) }
        }

        override fun forget(projectId: String) {
            AuthController.forgetProject(secrets, basePrefs, projectId)
            directory.byId(projectId)?.let(::eraseProject)
        }
    }
}
