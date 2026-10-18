package com.forkmesh.nativeapp.teammates

import com.forkmesh.nativeapp.agents.ops.OpsState
import com.forkmesh.nativeapp.control.ControlCalling
import com.forkmesh.nativeapp.control.ControlFamilyChannel
import com.forkmesh.nativeapp.control.DesktopControlFamilyName
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings

data class ShortcutStep(val name: String, val shortcut: String = "") {
    val linked: Boolean get() = shortcut.isNotEmpty()

    fun spoken(index: Int, count: Int, opens: Boolean): String =
        "Step ${index + 1} of $count, $name" + if (opens) ", opens shortcut" else ""

    companion object {
        const val MAX = 32

        fun list(value: Any?): List<ShortcutStep> = jsonMaps(value).mapNotNull(::from).take(MAX)

        fun from(json: Map<String, Any?>): ShortcutStep? {
            val shortcut = (json["shortcut"] as? String)?.trim().orEmpty()
            val name = (json["name"] as? String)?.trim().orEmpty().ifEmpty { shortcut }
            return if (name.isEmpty()) null else ShortcutStep(name, shortcut)
        }
    }
}

data class ShortcutEntry(
    val file: String,
    val scope: String,
    val name: String,
    val description: String,
    val kind: String,
    val agent: String,
    val model: String,
    val uses: Int,
    val runnable: Boolean,
    val running: Boolean = false,
    val lastOutput: String = "",
    val lastOk: Boolean = false,
    val finished: Int = 0,
    val lastMs: Long = -1,
    val expectedMs: Long = -1,
    val startedAtMs: Long = 0,
    val queued: Int = 0,
    val steps: List<ShortcutStep> = emptyList(),
) {
    val id: String get() = "$scope|$file"

    val stepsText: String get() = when (steps.size) {
        0 -> ""
        1 -> "1 step"
        else -> "${steps.size} steps"
    }

    val isSkill: Boolean get() = scope != "shortcut"

    val isScript: Boolean get() = kind == "script"

    val queueable: Boolean get() = isScript && runnable

    companion object {
        fun from(json: Map<String, Any?>, nowMs: Long = System.currentTimeMillis()): ShortcutEntry = ShortcutEntry(
            file = json.str("file"),
            scope = json.str("scope"),
            name = json.str("name"),
            description = json.str("description"),
            kind = json.str("kind"),
            agent = json.str("agent"),
            model = json.str("model"),
            uses = json.int("uses"),
            runnable = json.bool("runnable"),
            running = json.bool("running"),
            lastOutput = json.str("lastOutput"),
            lastOk = json.bool("lastOk"),
            finished = json.int("finished"),
            lastMs = json.long("lastMs", -1),
            expectedMs = json.long("expectedMs", json.long("lastMs", -1)),
            startedAtMs = startedAt(json, nowMs),
            queued = json.int("queued"),
            steps = ShortcutStep.list(json["steps"]),
        )
    }
}

internal fun startedAt(json: Map<String, Any?>, nowMs: Long): Long {
    val startedAt = json.long("startedAtMs")
    val elapsed = json.long("elapsedMs", -1)
    return when {
        json.bool("running") && startedAt > 0 -> startedAt
        json.bool("running") && elapsed >= 0 -> nowMs - elapsed
        else -> 0
    }
}

data class ShortcutProgress(val fraction: Float?, val text: String) {
    companion object {
        fun of(elapsedMs: Long, expectedMs: Long): ShortcutProgress {
            val elapsed = maxOf(0L, elapsedMs)
            return when {
                expectedMs <= 0 -> ShortcutProgress(null, "Running for ${duration(elapsed)} · no earlier run to estimate from")
                elapsed < expectedMs -> ShortcutProgress(
                    minOf(0.99f, elapsed.toFloat() / expectedMs),
                    "${duration(elapsed)} of about ${duration(expectedMs)} · ${duration(expectedMs - elapsed)} left",
                )
                else -> ShortcutProgress(0.99f, "${duration(elapsed)} · ${duration(elapsed - expectedMs)} longer than the last run")
            }
        }

        fun duration(ms: Long): String {
            val total = maxOf(0L, ms / 1000)
            val hours = total / 3600
            val minutes = total % 3600 / 60
            val seconds = total % 60
            return when {
                hours > 0 -> "${hours}h ${minutes.toString().padStart(2, '0')}m"
                minutes > 0 -> "${minutes}m ${seconds.toString().padStart(2, '0')}s"
                else -> "${seconds}s"
            }
        }
    }
}

data class ShortcutCatalog(val repo: String = "", val repos: List<String> = emptyList(), val entries: List<ShortcutEntry> = emptyList()) {
    val shortcuts: List<ShortcutEntry> get() = entries.filter { !it.isSkill }.sortedByDescending { it.uses }

    val skills: List<ShortcutEntry> get() = entries.filter { it.isSkill }.sortedByDescending { it.uses }

    fun target(from: ShortcutEntry, step: ShortcutStep): ShortcutEntry? {
        if (!step.linked) return null
        val same = entries.filter { it.scope == from.scope && it.id != from.id }
        return same.firstOrNull { it.file == step.shortcut } ?: same.firstOrNull { it.file.substringBeforeLast('.') == step.shortcut }
    }

    fun pick(preferred: String): String =
        repos.firstOrNull { it.equals(preferred, ignoreCase = true) } ?: repos.firstOrNull() ?: preferred

    companion object {
        fun from(json: Map<String, Any?>): ShortcutCatalog =
            ShortcutCatalog(json.str("repo"), json.strings("repos"), json.maps("entries").map { ShortcutEntry.from(it) })
    }
}

data class ShortcutRun(
    val id: Int,
    val repo: String,
    val file: String,
    val name: String,
    val running: Boolean,
    val ok: Boolean,
    val message: String,
    val output: String,
    val expectedMs: Long = -1,
    val startedAtMs: Long = 0,
) {
    val title: String get() = name.ifEmpty { file }

    fun progress(nowMs: Long = System.currentTimeMillis()): ShortcutProgress? =
        if (running && startedAtMs > 0) ShortcutProgress.of(nowMs - startedAtMs, expectedMs) else null

    val lastLine: String get() = lastLine(output)

    fun matches(repo: String, file: String): Boolean = this.file == file && this.repo.equals(repo, ignoreCase = true)

    fun absorb(event: Map<String, Any?>, nowMs: Long = System.currentTimeMillis()): ShortcutRun = copy(
        startedAtMs = if (startedAtMs > 0) startedAtMs else startedAt(event, nowMs),
        expectedMs = if (startedAtMs > 0) expectedMs else event.long("expectedMs", expectedMs),
        running = event.bool("running"),
        ok = event.bool("ok"),
        message = event.str("message"),
        output = tail(output + event.str("chunk")),
    )

    companion object {
        const val MAX_OUTPUT = 96 * 1024

        fun from(json: Map<String, Any?>, nowMs: Long = System.currentTimeMillis()): ShortcutRun = ShortcutRun(
            id = json.int("id"),
            repo = json.str("repo"),
            file = json.str("file"),
            name = json.str("name"),
            running = json.bool("running"),
            ok = json.bool("ok"),
            message = json.str("message"),
            output = tail(json.str("output")),
            expectedMs = json.long("expectedMs", -1),
            startedAtMs = startedAt(json, nowMs),
        )

        private val ESCAPES = Regex("\u001B\\[[0-9;?]*[A-Za-z]")

        fun lastLine(text: String, maxChars: Int = 200): String {
            for (raw in text.split('\n').asReversed()) {
                val line = raw.substringAfterLast('\r').replace(ESCAPES, "").trim()
                if (line.isNotEmpty()) return line.take(maxChars)
            }
            return ""
        }

        fun tail(text: String): String {
            if (text.length <= MAX_OUTPUT) return text
            val cut = text.takeLast(MAX_OUTPUT)
            val newline = cut.indexOf('\n')
            return if (newline >= 0 && newline + 1 < cut.length) cut.substring(newline + 1) else cut
        }
    }
}

data class ShortcutRowStatus(
    val running: Boolean,
    val ok: Boolean,
    val line: String,
    val ran: Boolean = true,
    val lastMs: Long = -1,
    val startedAtMs: Long = 0,
    val expectedMs: Long = -1,
) {
    val tookText: String? get() = if (!running && lastMs >= 0) "took ${ShortcutProgress.duration(lastMs)}" else null

    fun progress(nowMs: Long = System.currentTimeMillis()): ShortcutProgress? =
        if (running && startedAtMs > 0) ShortcutProgress.of(nowMs - startedAtMs, expectedMs) else null
}

object ShortcutAction {
    fun run(repo: String, scope: String, file: String): Map<String, Any?> = named("runShortcut", repo, scope, file)

    fun queue(repo: String, scope: String, file: String): Map<String, Any?> = named("queueShortcut", repo, scope, file)

    fun unqueue(repo: String, scope: String, file: String): Map<String, Any?> = named("unqueueShortcut", repo, scope, file)

    private fun named(action: String, repo: String, scope: String, file: String): Map<String, Any?> =
        linkedMapOf("action" to action, "repo" to repo, "scope" to scope, "file" to file)
}

class ShortcutsStore(channel: ControlCalling, private val launch: (() -> Unit) -> Unit = { it() }) : OpsState() {
    val family = ControlFamilyChannel(DesktopControlFamilyName.Insights, channel)
    private val lock = Any()
    private var tap: Any? = family.addEventTap { suffix, data ->
        val repo = shownRepo
        if (suffix == "shortcutOutput") absorb(data)
        else if (suffix == "changed" && repo.isNotEmpty()) launch { refresh(repo) }
    }

    @Volatile
    private var shownRepo = ""

    @Volatile
    var catalog: ShortcutCatalog? = null
        private set

    @Volatile
    var runs: Map<Int, ShortcutRun> = emptyMap()
        private set

    val lastRun: ShortcutRun? get() = runs.values.maxByOrNull { it.id }

    val newestRuns: List<ShortcutRun> get() = runs.values.sortedByDescending { it.id }

    val anyRunning: Boolean get() = runs.values.any { it.running }

    @Volatile
    var loading = false
        private set

    @Volatile
    var error = ""
        private set

    @Volatile
    var unsupported = false
        private set

    fun refresh(repo: String) {
        shownRepo = repo
        loading = true
        changed()
        try {
            val value = ShortcutCatalog.from(family.call("shortcuts", linkedMapOf("repo" to repo), 25_000))
            if (repo == shownRepo) {
                catalog = value
                error = ""
                unsupported = false
            }
        } catch (failure: Exception) {
            unsupported = TeamErrorText.isUnsupported(failure)
            error = TeamErrorText.describe(failure)
            return
        } finally {
            loading = false
            changed()
        }
        loadRun()
    }

    fun loadRun(id: Int? = null) {
        val answer = try {
            family.call("shortcutRun", if (id == null) linkedMapOf() else linkedMapOf("id" to id))
        } catch (failure: Exception) {
            return
        }
        synchronized(lock) {
            val next = runs.toMutableMap()
            if (id == null && answer.containsKey("runs")) {
                next.clear()
                for (row in answer.maps("runs")) {
                    val run = ShortcutRun.from(row)
                    if (run.id <= 0) continue
                    val known = runs[run.id]?.takeIf { it.matches(run.repo, run.file) }
                    next[run.id] = if (known != null) run.copy(output = known.output) else run
                }
            }
            if (answer.containsKey("run")) {
                val run = ShortcutRun.from(answer.obj("run"))
                if (run.id > 0 && (id == null || run.id == id)) next[run.id] = run
            }
            runs = next
        }
        changed()
    }

    fun absorb(event: Map<String, Any?>) {
        val id = event.int("id")
        if (id <= 0) return
        val known = synchronized(lock) {
            val current = runs[id]
            val next = runs.toMutableMap()
            next[id] = current?.absorb(event) ?: ShortcutRun.from(event).absorb(event)
            next.keys.sorted().dropLast(KEPT_RUNS).forEach { next.remove(it) }
            runs = next
            current != null
        }
        changed()
        if (!known) launch { loadRun(id) }
    }

    fun liveRun(repo: String, entry: ShortcutEntry): ShortcutRun? =
        if (entry.isSkill) null else newestRuns.firstOrNull { it.matches(repo, entry.file) }

    fun isRunning(repo: String, file: String): Boolean = runs.values.any { it.running && it.matches(repo, file) }

    fun status(repo: String, entry: ShortcutEntry): ShortcutRowStatus {
        val run = liveRun(repo, entry)
        if (run != null && (run.running || entry.running)) {
            val timed = run.startedAtMs > 0
            return ShortcutRowStatus(
                run.running, run.ok, run.lastLine, lastMs = entry.lastMs,
                startedAtMs = if (timed) run.startedAtMs else entry.startedAtMs,
                expectedMs = if (timed) run.expectedMs else entry.expectedMs,
            )
        }
        if (entry.running) {
            return ShortcutRowStatus(true, false, entry.lastOutput, lastMs = entry.lastMs, startedAtMs = entry.startedAtMs, expectedMs = entry.expectedMs)
        }
        if (entry.lastOutput.isEmpty() && entry.finished == 0) return ShortcutRowStatus(false, false, "", ran = false, lastMs = entry.lastMs)
        return ShortcutRowStatus(false, entry.lastOk, entry.lastOutput, lastMs = entry.lastMs)
    }

    fun text(repo: String, entry: ShortcutEntry): String =
        family.call("shortcut", linkedMapOf("repo" to repo, "scope" to entry.scope, "file" to entry.file)).str("text")

    fun prepare(repo: String, entry: ShortcutEntry): TeamConfirmation {
        val params = ShortcutAction.run(repo, entry.scope, entry.file)
        return TeamConfirmation.from(family.call("prepare", params), params) ?: throw LocalControlException("bad_answer")
    }

    fun run(confirmation: TeamConfirmation): String = family.call("run", confirmation.runParams, 45_000).str("message")

    fun repoBusy(repo: String): Boolean =
        catalog?.entries?.any { it.isScript && it.running } == true ||
            runs.values.any { it.running && it.repo.equals(repo, ignoreCase = true) }

    fun runNow(repo: String, entry: ShortcutEntry): String = perform(ShortcutAction.run(repo, entry.scope, entry.file), repo)

    fun queue(repo: String, entry: ShortcutEntry): String = perform(ShortcutAction.queue(repo, entry.scope, entry.file), repo)

    fun unqueue(repo: String, entry: ShortcutEntry): String = perform(ShortcutAction.unqueue(repo, entry.scope, entry.file), repo)

    private fun perform(params: Map<String, Any?>, repo: String): String {
        val confirmation = TeamConfirmation.from(family.call("prepare", params), params) ?: throw LocalControlException("bad_answer")
        val message = run(confirmation)
        refresh(repo)
        return message
    }

    fun stop() {
        val token = synchronized(lock) { tap.also { tap = null } } ?: return
        family.removeEventTap(token)
    }

    companion object {
        const val KEPT_RUNS = 20
    }
}
