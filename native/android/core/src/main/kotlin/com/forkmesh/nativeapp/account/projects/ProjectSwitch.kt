package com.forkmesh.nativeapp.account.projects

import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.net.HttpClient
import java.util.concurrent.Callable
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

enum class SwitchPhase { idle, leaving, clearing, adopting, joining, done, failed }

data class SwitchResult(
    val ok: Boolean,
    val project: Project?,
    val needsSignIn: Boolean = false,
    val error: String = "",
)

interface ProjectSwitchDriver {
    val directory: ProjectDirectory

    val hasAppAccess: Boolean

    fun isConnected(): Boolean?

    fun leave()

    fun clear()

    fun adopt(projectId: String)

    fun join()

    fun forget(projectId: String)
}

class ProjectSwitchController(
    private val driver: ProjectSwitchDriver,
    private val joinTimeoutMs: Long = 12_000,
    private val sleep: (Long) -> Unit = { Thread.sleep(it) },
    private val clock: () -> Long = System::currentTimeMillis,
) {
    private val directory: ProjectDirectory get() = driver.directory
    private val generation = AtomicInteger()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    var phase: SwitchPhase = SwitchPhase.idle
        private set

    @Volatile
    var target: Project? = null
        private set

    @Volatile
    var error: String = ""
        private set

    @Volatile
    private var fromLabel: String = ""

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    val busy: Boolean
        get() = phase == SwitchPhase.leaving || phase == SwitchPhase.clearing || phase == SwitchPhase.adopting || phase == SwitchPhase.joining

    val statusLine: String
        get() = when (phase) {
            SwitchPhase.leaving -> "Leaving $fromLabel…"
            SwitchPhase.clearing -> "Clearing cached data…"
            SwitchPhase.adopting -> "Loading your account…"
            SwitchPhase.joining -> "Opening ${target?.label ?: "relay"}…"
            SwitchPhase.done -> "Switched to ${target?.label ?: "relay"}"
            SwitchPhase.failed -> error
            SwitchPhase.idle -> ""
        }

    fun clearError() {
        if (error.isEmpty() && phase != SwitchPhase.failed) return
        error = ""
        setPhase(SwitchPhase.idle)
    }

    fun switchTo(projectId: String): SwitchResult {
        val project = directory.byId(projectId)
            ?: return SwitchResult(false, null, error = "That project is no longer in your list.")
        synchronized(this) {
            if (busy) return SwitchResult(false, project, error = "Already switching projects. Wait for that to finish.")
            if (projectId == directory.activeId) return SwitchResult(true, project, needsSignIn = !driver.hasAppAccess)
            target = project
            fromLabel = directory.active.label
            error = ""
            setPhase(SwitchPhase.leaving)
        }
        val gen = generation.incrementAndGet()
        try {
            driver.leave()
            if (gen != generation.get()) return abandoned(project)
            setPhase(SwitchPhase.clearing)
            driver.clear()
            if (gen != generation.get()) return abandoned(project)
            setPhase(SwitchPhase.adopting)
            driver.adopt(projectId)
            if (gen != generation.get()) return abandoned(project)
            setPhase(SwitchPhase.joining)
            driver.join()
            if (gen != generation.get()) return abandoned(project)
            val needsSignIn = !driver.hasAppAccess
            setPhase(SwitchPhase.done)
            return SwitchResult(ok = true, project = project, needsSignIn = needsSignIn)
        } catch (failure: Exception) {
            if (gen != generation.get()) return abandoned(project)
            error = "Could not finish switching to ${project.label}. ${humanize(failure)}"
            setPhase(SwitchPhase.failed)
            return SwitchResult(false, project, error = error)
        }
    }

    fun removeProject(projectId: String): SwitchResult {
        val project = directory.byId(projectId)
            ?: return SwitchResult(false, null, error = "That project is no longer in your list.")
        if (project.builtIn) {
            return SwitchResult(false, project, error = "${project.label} is built in to this app and cannot be removed. You can switch away from it instead.")
        }
        var result = SwitchResult(true, project)
        if (projectId == directory.activeId) {
            result = switchTo(directory.fallbackFor(projectId).id)
            if (!result.ok) return result
        }
        driver.forget(projectId)
        directory.remove(projectId)
        return result
    }

    fun awaitConnection(projectId: String): Boolean? {
        val gen = generation.get()
        val deadline = clock() + joinTimeoutMs
        while (true) {
            if (gen != generation.get() || busy || directory.activeId != projectId) return null
            val connected = driver.isConnected()
            if (connected != null) return connected
            if (clock() >= deadline) return false
            sleep(80)
        }
    }

    fun offlineNote(project: Project): String =
        "Switched to ${project.label}, but its relay is not answering yet. It will keep retrying in the background."

    private fun abandoned(project: Project) = SwitchResult(false, project)

    private fun setPhase(next: SwitchPhase) {
        if (phase == next) return
        phase = next
        for (listener in listeners) listener()
    }

    private fun humanize(error: Exception): String {
        val text = (error.message ?: error.toString()).replaceFirst("Exception: ", "").trim()
        if (text.isEmpty()) return "Try again."
        return if (text.endsWith(".")) text else "$text."
    }
}

data class ProjectCatalogEntry(
    val project: Project,
    val active: Boolean,
    val signedIn: Boolean,
    val repositories: List<Repository> = emptyList(),
    val loading: Boolean = false,
    val error: String = "",
)

class CrossProjectCatalog(
    private val transport: HttpTransport = HttpTransport.DEFAULT,
    private val timeoutMs: Int = 10_000,
) {
    fun initial(directory: ProjectDirectory, sessionToken: (String) -> String?): List<ProjectCatalogEntry> {
        val activeId = directory.activeId
        return directory.projects.filter { it.hasInstance }.map { project ->
            val signedIn = !sessionToken(project.id).isNullOrEmpty()
            ProjectCatalogEntry(project, project.id == activeId, signedIn, loading = signedIn)
        }
    }

    fun load(entry: ProjectCatalogEntry, token: String): ProjectCatalogEntry = try {
        entry.copy(repositories = fetch(entry.project, token), loading = false)
    } catch (error: Exception) {
        entry.copy(loading = false, error = humanize(error))
    }

    fun refresh(directory: ProjectDirectory, sessionToken: (String) -> String?, onUpdate: (List<ProjectCatalogEntry>) -> Unit): List<ProjectCatalogEntry> {
        val entries = initial(directory, sessionToken).toMutableList()
        onUpdate(entries.toList())
        val pool = Executors.newFixedThreadPool(entries.size.coerceIn(1, 4))
        try {
            val futures = entries.mapIndexedNotNull { index, entry ->
                if (!entry.signedIn) null else index to pool.submit(Callable { load(entry, sessionToken(entry.project.id) ?: "") })
            }
            for ((index, future) in futures) {
                val updated = future.get()
                synchronized(entries) {
                    entries[index] = updated
                    onUpdate(entries.toList())
                }
            }
        } finally {
            pool.shutdown()
        }
        return entries.toList()
    }

    private fun fetch(project: Project, token: String): List<Repository> {
        val response = transport.send(
            HttpClient.Request.get(project.httpOrigin + "/api/repositories")
                .header("Accept", "application/json")
                .bearer(token)
                .timeouts(timeoutMs, timeoutMs),
        )
        if (response.status == 401) throw IllegalStateException("Your session on ${project.label} has expired.")
        if (response.status !in 200..299) throw IllegalStateException("${project.label} answered ${response.status}.")
        val decoded = Json.parse(response.text())
        val list = when {
            decoded is List<*> -> decoded
            decoded is Map<*, *> && decoded["repositories"] is List<*> -> decoded["repositories"] as List<*>
            else -> emptyList<Any?>()
        }
        return com.forkmesh.nativeapp.model.collapseRepositoryMirrors(
            list.mapNotNull { item ->
                @Suppress("UNCHECKED_CAST")
                (item as? Map<String, Any?>)?.let(Repository::fromJson)
            }.filter { it.owner.isNotEmpty() && it.name.isNotEmpty() },
        )
    }

    private fun humanize(error: Exception): String {
        if (error is java.net.SocketTimeoutException) return "That relay did not answer in time."
        val text = (error.message ?: "").trim()
        if (text.isEmpty()) return "Could not read that relay."
        return if (text.endsWith(".")) text else "$text."
    }
}
