package com.forkmesh.nativeapp.hosts

import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import java.util.concurrent.CopyOnWriteArrayList

abstract class RemoteStoreBase(channel: ControlCalling, familyName: DesktopControlFamilyName) {
    val family = ControlFamilyChannel(familyName, channel)
    private val listeners = CopyOnWriteArrayList<() -> Unit>()
    protected val lock = Object()
    private var tap: Any? = null

    @Volatile
    var revision: Int = 0
        private set

    protected fun startTap() {
        tap = family.addEventTap(::onEvent)
    }

    protected open fun onEvent(suffix: String, data: Map<String, Any?>) {}

    fun addListener(listener: () -> Unit) = listeners.add(listener)

    fun removeListener(listener: () -> Unit) = listeners.remove(listener)

    protected fun changed() {
        revision++
        for (listener in listeners) runCatching { listener() }
    }

    open fun stop() {
        tap?.let(family::removeEventTap)
        tap = null
        listeners.clear()
    }
}

class FleetStore(
    channel: ControlCalling,
    private val executor: HostsExecutor = HostsExecutor.Threads,
    private val now: () -> Long = System::currentTimeMillis,
) : RemoteStoreBase(channel, DesktopControlFamilyName.Fleet) {
    private var refreshing = false
    private var again = false

    @Volatile
    var summary: FleetSummary? = null
        private set

    @Volatile
    var summaryAtMs: Long = 0
        private set

    @Volatile
    var repos: List<FleetRepo> = emptyList()
        private set

    @Volatile
    var loading: Boolean = false
        private set

    @Volatile
    var error: String = ""
        private set

    @Volatile
    var unsupported: Boolean = false
        private set

    @Volatile
    var outcomes: List<RemoteActionOutcome> = emptyList()
        private set

    @Volatile
    var running: Set<String> = emptySet()
        private set

    init {
        startTap()
    }

    override fun onEvent(suffix: String, data: Map<String, Any?>) {
        when (suffix) {
            "changed" -> executor.run { refresh() }
            "action" -> {
                val outcome = RemoteActionOutcome.fromJson(data, now())
                synchronized(lock) {
                    running = running - "${outcome.action}:${outcome.subject}"
                    outcomes = (listOf(outcome) + outcomes).take(20)
                }
                changed()
            }
        }
    }

    fun refresh() {
        synchronized(lock) {
            if (refreshing) {
                again = true
                return
            }
            refreshing = true
            loading = true
        }
        changed()
        try {
            while (true) {
                synchronized(lock) { again = false }
                try {
                    summary = FleetSummary.fromJson(family.call("summary"))
                    summaryAtMs = now()
                    error = ""
                    unsupported = false
                } catch (failure: Exception) {
                    unsupported = isUnsupportedControl(failure)
                    error = RemoteErrorText.describe(failure)
                }
                val repeat = synchronized(lock) { again }
                if (!repeat) break
            }
        } finally {
            synchronized(lock) {
                refreshing = false
                loading = false
            }
            changed()
        }
    }

    fun refreshAsync() = executor.run { refresh() }

    fun provision(): ProvisionStatus = ProvisionStatus.fromJson(family.call("provision"))

    fun loadRepos() {
        try {
            repos = family.call("repos").maps("repos").map(FleetRepo::fromJson)
        } catch (failure: Exception) {
            error = RemoteErrorText.describe(failure)
        }
        changed()
    }

    fun diskUsage(host: String, path: String): DiskUsage =
        DiskUsage.fromJson(family.call("disk", mapOf("host" to host, "path" to path), 75_000))

    fun prepare(action: FleetAction): RemoteConfirmation = family.prepare(action.params)

    fun run(confirmation: RemoteConfirmation, typed: String = ""): String {
        val started = family.run(confirmation, typed)
        val subject = started.subject.ifEmpty { confirmation.subject }
        synchronized(lock) { running = running + "${confirmation.action}:$subject" }
        changed()
        refresh()
        return started.message
    }

    fun isRunning(action: String, subject: String): Boolean {
        val key = "$action:$subject"
        return key in running || summary?.running?.contains(key) == true
    }

    fun outcomesFor(subject: String): List<RemoteActionOutcome> = outcomes.filter { it.subject == subject }

    fun targetCountdown(): Long = summary?.fleet?.countdown(now() - summaryAtMs) ?: -1
}

class FilesBrowserStore(
    channel: ControlCalling,
    private val now: () -> Long = System::currentTimeMillis,
) : RemoteStoreBase(channel, DesktopControlFamilyName.FilesBrowse) {
    private val listingCache = HashMap<String, FileListing>()
    private val statusCache = HashMap<String, GitWorkingStatus>()

    @Volatile
    var roots: List<FileRoot> = emptyList()
        private set

    @Volatile
    var loadingRoots: Boolean = false
        private set

    @Volatile
    var error: String = ""
        private set

    @Volatile
    var unsupported: Boolean = false
        private set

    @Volatile
    var outcomes: List<RemoteActionOutcome> = emptyList()
        private set

    @Volatile
    var changedRoots: Set<String> = emptySet()
        private set

    init {
        startTap()
    }

    override fun onEvent(suffix: String, data: Map<String, Any?>) {
        when (suffix) {
            "changed" -> {
                val root = data.str("root")
                if (root.isNotEmpty()) synchronized(lock) { changedRoots = changedRoots + root }
                changed()
            }
            "action" -> {
                synchronized(lock) { outcomes = (listOf(RemoteActionOutcome.fromJson(data, now())) + outcomes).take(20) }
                changed()
            }
        }
    }

    val groups: List<FileRootGroup> get() = FileRootGroup.group(roots)

    fun loadRoots() {
        loadingRoots = true
        changed()
        try {
            roots = family.call("roots").maps("roots").map(FileRoot::fromJson)
            error = ""
            unsupported = false
        } catch (failure: Exception) {
            unsupported = isUnsupportedControl(failure)
            error = RemoteErrorText.describe(failure)
        } finally {
            loadingRoots = false
            changed()
        }
    }

    fun list(root: String, path: String): FileListing {
        val listing = FileListing.fromJson(family.call("list", mapOf("root" to root, "path" to path)))
        synchronized(lock) { listingCache[key(root, listing.path)] = listing }
        changed()
        return listing
    }

    fun cached(root: String, path: String): FileListing? = synchronized(lock) { listingCache[key(root, path)] }

    fun read(root: String, path: String, offset: Double = 0.0, length: Int = 256 * 1024): FileSlice = FileSlice.fromJson(
        family.call("read", mapOf("root" to root, "path" to path, "offset" to offset, "length" to length.toLong()), 30_000),
    )

    fun status(root: String): GitWorkingStatus {
        val status = GitWorkingStatus.fromJson(family.call("status", mapOf("root" to root), 25_000))
        synchronized(lock) {
            statusCache[root] = status
            changedRoots = changedRoots - root
        }
        changed()
        return status
    }

    fun cachedStatus(root: String): GitWorkingStatus? = synchronized(lock) { statusCache[root] }

    fun prepare(action: GitRemoteAction, root: String): RemoteConfirmation =
        family.prepare(mapOf("action" to action.wire, "root" to root))

    fun run(confirmation: RemoteConfirmation): String = family.run(confirmation).message

    companion object {
        fun key(root: String, path: String): String = "$root|$path"
    }
}

class DiagnosticsStore(
    channel: ControlCalling,
    private val executor: HostsExecutor = HostsExecutor.Threads,
) : RemoteStoreBase(channel, DesktopControlFamilyName.Diagnostics) {
    @Volatile
    var health: DesktopHealth? = null
        private set

    @Volatile
    var stalls: StallList? = null
        private set

    @Volatile
    var processes: ProcessList? = null
        private set

    @Volatile
    var backups: DesktopBackupStatus? = null
        private set

    @Volatile
    var loading: Boolean = false
        private set

    @Volatile
    var error: String = ""
        private set

    @Volatile
    var unsupported: Boolean = false
        private set

    init {
        startTap()
    }

    override fun onEvent(suffix: String, data: Map<String, Any?>) {
        if (suffix == "changed") executor.run { refreshProcesses() }
    }

    fun refreshHealth() {
        loading = true
        changed()
        try {
            health = DesktopHealth.fromJson(family.call("health"))
            error = ""
            unsupported = false
        } catch (failure: Exception) {
            unsupported = isUnsupportedControl(failure)
            error = RemoteErrorText.describe(failure)
        } finally {
            loading = false
            changed()
        }
    }

    fun refreshStalls() {
        try {
            stalls = StallList.fromJson(family.call("stalls"))
        } catch (failure: Exception) {
            error = RemoteErrorText.describe(failure)
        }
        changed()
    }

    fun stall(index: Int): StallRecord = StallRecord.fromJson(family.call("stall", mapOf("index" to index.toLong())))

    fun refreshBackups() {
        try {
            backups = DesktopBackupStatus.fromJson(family.call("backups"))
            unsupported = false
        } catch (failure: Exception) {
            if (isUnsupportedControl(failure)) unsupported = true
            error = RemoteErrorText.describe(failure)
        }
        changed()
    }

    fun refreshProcesses() {
        try {
            processes = ProcessList.fromJson(family.call("processes", emptyMap(), 15_000))
        } catch (failure: Exception) {
            error = RemoteErrorText.describe(failure)
        }
        changed()
    }

    fun log(before: Long? = null, lines: Int = 200, query: String = "", severity: LogSeverity = LogSeverity.All): DesktopLogPage {
        val params = linkedMapOf<String, Any?>("lines" to lines.toLong(), "query" to query, "severity" to severity.wire)
        if (before != null) params["before"] = before
        return DesktopLogPage.fromJson(family.call("log", params, 20_000))
    }

    fun prepare(action: DiagnosticsAction): RemoteConfirmation = family.prepare(action.params)

    fun run(confirmation: RemoteConfirmation): String {
        val started = family.run(confirmation)
        if (confirmation.action == "stopAgent") refreshProcesses()
        return started.message
    }
}

class DesktopLogTail(private val store: DiagnosticsStore, val pageSize: Int = 200, val maxLines: Int = 2000) {
    @Volatile
    var lines: List<DesktopLogLine> = emptyList()
        private set

    @Volatile
    var more: Boolean = false
        private set

    @Volatile
    var loading: Boolean = false
        private set

    @Volatile
    var error: String = ""
        private set

    @Volatile
    var query: String = ""

    @Volatile
    var severity: LogSeverity = LogSeverity.All

    fun loadLatest() {
        loading = true
        try {
            val page = store.log(lines = pageSize, query = query, severity = severity)
            lines = page.lines
            more = page.more
            error = ""
        } catch (failure: Exception) {
            error = RemoteErrorText.describe(failure)
        } finally {
            loading = false
        }
    }

    fun loadOlder() {
        val first = lines.firstOrNull() ?: return
        if (!more || loading) return
        loading = true
        try {
            val page = store.log(before = first.offset, lines = pageSize, query = query, severity = severity)
            val known = lines.map { it.offset }.toHashSet()
            var merged = page.lines.filter { it.offset !in known } + lines
            if (merged.size > maxLines) merged = merged.dropLast(merged.size - maxLines)
            lines = merged
            more = page.more
        } catch (failure: Exception) {
            error = RemoteErrorText.describe(failure)
        } finally {
            loading = false
        }
    }

    val errorCount: Int get() = lines.count { it.severity == LogSeverity.Error }
}
