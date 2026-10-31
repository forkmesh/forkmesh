package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.control.ControlErrorText
import com.forkmesh.nativeapp.control.ControlEvent
import com.forkmesh.nativeapp.control.ControlLink
import com.forkmesh.nativeapp.control.DesktopControlHub
import com.forkmesh.nativeapp.control.DesktopTerminalInfo
import com.forkmesh.nativeapp.control.LocalControlException
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.json.jsonText
import java.util.Timer
import java.util.TimerTask
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.ExecutionException
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

const val DESKTOP_TERMINAL_ENTER_DELAY_MS = 120L
private const val MAX_DESKTOP_INPUT_BYTES = 16 * 1024
private val FINAL_ATTACH_REFUSALS = setOf("unsupported", "unknown_method", "not_owner", "not_paired", "pair_on_lan")

class DesktopAgentTerminal(
    private val hub: DesktopControlHub,
    private val owner: String = "",
    private val name: String = "",
    val provider: String = "",
    terminal: String = "",
    val session: Int = 0,
    val mode: String = "",
    val account: String = "",
    private val onStarted: (DesktopTerminalInfo) -> Unit = {},
    private val inputFlushMs: Long = TERMINAL_INPUT_FLUSH_MS,
    private val notifyDelayMs: Long = 16,
    private val enterDelayMs: Long = DESKTOP_TERMINAL_ENTER_DELAY_MS,
    val desktopKey: String = "",
) : AgentTerminalSession {
    private val lock = Any()
    private val worker = Executors.newSingleThreadScheduledExecutor { Thread(it, "DesktopAgentTerminal").apply { isDaemon = true } }
    private val timer = Timer("DesktopAgentTerminal", true)
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    var terminalId: String = terminal
        private set

    @Volatile
    var info: DesktopTerminalInfo? = null
        private set

    @Volatile
    override var screen = TerminalScreen(24, 80)
        private set
    private var cols = 80
    private var rows = 24

    @Volatile
    override var statusText: String = when {
        session > 0 -> "Opening the agent in a terminal on your desktop…"
        terminal.isEmpty() && mode == "login" -> "Starting ${agentProviderLabel(provider)} sign-in on your desktop…"
        terminal.isEmpty() && mode == "logout" -> "Signing ${agentProviderLabel(provider)} out on your desktop…"
        terminal.isEmpty() -> "Starting ${agentProviderLabel(provider)} on your desktop…"
        else -> "Connecting to the terminal…"
    }
        private set

    @Volatile
    override var statusLevel = TerminalStatusLevel.Pending
        private set

    @Volatile
    var live = false
        private set

    @Volatile
    var exitCode: Int? = null
        private set

    @Volatile
    var failed = false
        private set

    @Volatile
    var gone = false
        private set

    @Volatile
    var detached = false
        private set

    private var started = false
    private var tap: Any? = null
    private var linkToken: Any? = null
    private var attachedLink: ControlLink? = null
    private var awaiting = false
    private var generation = 0
    private val early = ArrayList<ControlEvent>()
    private var utf8 = StreamingUtf8()
    private val pendingInput = StringBuilder()
    private var inputTask: TimerTask? = null
    private var notifyTask: TimerTask? = null
    private var retry: ScheduledFuture<*>? = null
    private var retryDelayMs = 1_000L

    val providerLabel: String
        get() = info?.label?.takeIf { it.isNotEmpty() } ?: agentProviderLabel(info?.provider?.takeIf { it.isNotEmpty() } ?: provider)

    val title: String
        get() {
            val repo = info?.repositoryName?.takeIf { it.isNotEmpty() } ?: name
            return if (repo.isEmpty()) providerLabel else "$providerLabel · $repo"
        }

    val finished: Boolean get() = gone || failed

    override val plainText: String get() = synchronized(lock) { screen.allRows.joinToString("\n") { it.text }.trim() }

    override fun addListener(listener: () -> Unit) = listeners.add(listener)

    override fun removeListener(listener: () -> Unit) = listeners.remove(listener)

    private fun notifyListeners() {
        for (listener in listeners) runCatching { listener() }
    }

    override fun start(cols: Int, rows: Int) {
        synchronized(lock) {
            if (started || detached) return
            started = true
            this.cols = cols.coerceIn(20, 500)
            this.rows = rows.coerceIn(5, 200)
            screen = TerminalScreen(this.rows, this.cols)
            tap = hub.addEventTap(desktopKey) { event -> onEvent(event) }
            linkToken = hub.addLinkListener { linkChanged() }
        }
        changed()
        worker.execute { connect() }
    }

    private fun connect() {
        val attempt = synchronized(lock) {
            if (detached || gone || failed) return
            generation++
            awaiting = true
            early.clear()
            generation
        }
        val starting = terminalId.isEmpty()
        try {
            val link = hub.link(desktopKey)
            val (sentCols, sentRows) = synchronized(lock) {
                attachedLink = link
                cols to rows
            }
            val answer = if (starting && session > 0) {
                link.request(
                    "terminal.resume",
                    linkedMapOf("id" to session, "cols" to sentCols, "rows" to sentRows),
                    30_000,
                )
            } else if (starting) {
                link.request("terminal.start", startParams(sentCols, sentRows), 30_000)
            } else {
                link.request("terminal.attach", linkedMapOf("terminal" to terminalId, "cols" to sentCols, "rows" to sentRows), 20_000)
            }
            val parsed = DesktopTerminalInfo.fromJson(answer)
            val held: List<ControlEvent>
            val resizeAfter: Boolean
            synchronized(lock) {
                if (detached || attempt != generation) return
                if (parsed.terminal.isNotEmpty()) {
                    terminalId = parsed.terminal
                    info = parsed
                }
                retryDelayMs = 1_000
                live = true
                rebuild(jsonText(answer["replay"]))
                held = ArrayList(early)
                early.clear()
                awaiting = false
                resizeAfter = cols != sentCols || rows != sentRows
            }
            setStatus(liveText(link), TerminalStatusLevel.Ok)
            changed()
            if (starting) info?.let { runCatching { onStarted(it) } }
            for (event in held) handle(event)
            if (resizeAfter) sendResize()
        } catch (refusal: LocalControlException) {
            if (!current(attempt)) return
            live = false
            when {
                starting && session > 0 && refusal.code == "unknown_terminal" -> {
                    failed = true
                    setStatus(ControlErrorText.describe("unsupported"), TerminalStatusLevel.Error)
                }
                refusal.code == "unknown_terminal" -> markGone("This terminal has ended on the desktop.", TerminalStatusLevel.Error)
                starting || refusal.code in FINAL_ATTACH_REFUSALS -> {
                    failed = true
                    setStatus(ControlErrorText.describe(refusal), TerminalStatusLevel.Error)
                }
                else -> {
                    setStatus("${ControlErrorText.describe(refusal)} Trying again…", TerminalStatusLevel.Error)
                    scheduleRetry()
                }
            }
        } catch (error: Throwable) {
            if (!current(attempt)) return
            live = false
            if (starting) {
                failed = true
                setStatus(ControlErrorText.describe(error), TerminalStatusLevel.Error)
            } else {
                setStatus("${ControlErrorText.describe(error)} Trying again…", TerminalStatusLevel.Error)
                scheduleRetry()
            }
        } finally {
            synchronized(lock) {
                if (attempt == generation) {
                    awaiting = false
                    early.clear()
                }
            }
        }
    }

    fun startParams(cols: Int, rows: Int): Map<String, Any?> {
        val params = linkedMapOf<String, Any?>("owner" to owner, "name" to name, "provider" to provider, "cols" to cols, "rows" to rows)
        if (mode.isNotEmpty()) params["mode"] = mode
        if (account.isNotEmpty()) params["account"] = account
        return params
    }

    private fun current(attempt: Int): Boolean = synchronized(lock) { attempt == generation && !detached }

    private fun liveText(link: ControlLink): String {
        val where = link.desktopName.ifEmpty { "your desktop" }
        val repo = info?.repoLabel?.takeIf { it.isNotEmpty() }?.let { " in $it" } ?: ""
        return "Live on $where: $providerLabel$repo. Leaving this screen keeps it running."
    }

    private fun rebuild(replay: String) {
        screen = TerminalScreen(rows, cols)
        utf8 = StreamingUtf8()
        if (replay.isEmpty()) return
        val bytes = B64.decodeOrNull(replay) ?: return
        screen.feed(utf8.decode(bytes))
    }

    private fun onEvent(event: ControlEvent) {
        if (event.event != "terminal.output" && event.event != "terminal.exit") return
        synchronized(lock) {
            if (detached) return
            if (awaiting) {
                early.add(event)
                return
            }
        }
        handle(event)
    }

    private fun handle(event: ControlEvent) {
        val id = jsonText(event.data["terminal"])
        if (id.isEmpty() || id != terminalId) return
        when (event.event) {
            "terminal.output" -> {
                val encoded = jsonText(event.data["data"])
                val bytes = if (encoded.isEmpty()) ByteArray(0) else B64.decodeOrNull(encoded) ?: return
                synchronized(lock) {
                    if (detached) return
                    if (event.data["reset"] == true) {
                        screen = TerminalScreen(rows, cols)
                        utf8 = StreamingUtf8()
                    }
                    if (bytes.isNotEmpty()) screen.feed(utf8.decode(bytes))
                }
                changed()
            }
            "terminal.exit" -> {
                val code = (event.data["code"] as? Number)?.toInt() ?: 0
                exitCode = code
                val text = when (code) {
                    0 -> "$providerLabel ended."
                    -1 -> "$providerLabel was stopped."
                    else -> "$providerLabel exited with status $code."
                }
                markGone(text, if (code == 0) TerminalStatusLevel.Ok else TerminalStatusLevel.Error)
            }
        }
    }

    private fun markGone(text: String, level: TerminalStatusLevel) {
        synchronized(lock) {
            gone = true
            live = false
            retry?.cancel(false)
            retry = null
        }
        setStatus(text, level)
    }

    private fun linkChanged() {
        val reattach = synchronized(lock) {
            if (!started || detached || gone || failed || awaiting || terminalId.isEmpty()) return
            val current = hub.openLink(desktopKey)
            if (current != null && current === attachedLink) return
            live = false
            current != null
        }
        setStatus("Reconnecting to your desktop…", TerminalStatusLevel.Pending)
        if (reattach) runCatching { worker.execute { connect() } } else scheduleRetry()
    }

    private fun scheduleRetry() {
        synchronized(lock) {
            if (detached || gone || failed || retry != null) return
            val delay = retryDelayMs
            retryDelayMs = minOf(retryDelayMs * 2, 15_000)
            retry = runCatching {
                worker.schedule({
                    synchronized(lock) { retry = null }
                    connect()
                }, delay, TimeUnit.MILLISECONDS)
            }.getOrNull()
        }
    }

    override fun input(text: String) {
        if (text.isEmpty()) return
        synchronized(lock) {
            if (detached || gone) return
            pendingInput.append(text)
            if (pendingInput.length > 4096) {
                inputTask?.cancel()
                inputTask = null
                flushInputLocked()
                return
            }
            if (inputTask == null) {
                inputTask = schedule(inputFlushMs) {
                    synchronized(lock) {
                        inputTask = null
                        flushInputLocked()
                    }
                }
            }
        }
    }

    override fun paste(text: String) {
        if (text.isEmpty()) return
        input(if (screen.bracketedPaste && text.length > 1) "\u001b[200~$text\u001b[201~" else text)
    }

    override fun key(key: TerminalKey) = input(terminalKeyBytes(key, screen.appCursorKeys))

    override fun submit(text: String) {
        paste(text)
        synchronized(lock) {
            inputTask?.cancel()
            inputTask = null
            flushInputLocked()
            if (detached || gone) return
        }
        schedule(if (text.isEmpty()) 0 else enterDelayMs) { key(TerminalKey.Enter) }
    }

    private fun flushInputLocked() {
        if (pendingInput.isEmpty()) return
        val bytes = pendingInput.toString().toByteArray(Charsets.UTF_8)
        pendingInput.setLength(0)
        var at = 0
        while (at < bytes.size) {
            val end = minOf(bytes.size, at + MAX_DESKTOP_INPUT_BYTES)
            val data = B64.encode(bytes.copyOfRange(at, end))
            runCatching { worker.execute { sendInput(data) } }
            at = end
        }
    }

    private fun sendInput(data: String) {
        val id = terminalId
        if (id.isEmpty() || gone) return
        try {
            hub.request("terminal.input", mapOf("terminal" to id, "data" to data), 10_000, desktopKey)
        } catch (refusal: LocalControlException) {
            if (refusal.code == "unknown_terminal") {
                markGone("This terminal has ended on the desktop.", TerminalStatusLevel.Error)
            } else {
                setStatus("Typing did not reach the desktop: ${ControlErrorText.describe(refusal)}", TerminalStatusLevel.Error)
            }
        } catch (error: Throwable) {
            setStatus("Typing did not reach the desktop: ${ControlErrorText.describe(error)}", TerminalStatusLevel.Error)
        }
    }

    override fun resize(cols: Int, rows: Int) {
        val c = cols.coerceIn(20, 500)
        val r = rows.coerceIn(5, 200)
        synchronized(lock) {
            if (c == this.cols && r == this.rows) return
            this.cols = c
            this.rows = r
            if (!started || detached) return
            screen = screen.resized(r, c)
        }
        changed()
        if (live) sendResize()
    }

    private fun sendResize() {
        runCatching {
            worker.execute {
                val id = terminalId
                if (id.isEmpty() || gone || detached) return@execute
                val (c, r) = synchronized(lock) { cols to rows }
                runCatching { hub.request("terminal.resize", mapOf("terminal" to id, "cols" to c, "rows" to r), 10_000, desktopKey) }
            }
        }
    }

    fun detach() {
        val link: ControlLink?
        val id: String
        synchronized(lock) {
            if (detached) return
            inputTask?.cancel()
            inputTask = null
            flushInputLocked()
            detached = true
            live = false
            generation++
            retry?.cancel(false)
            retry = null
            notifyTask?.cancel()
            notifyTask = null
            tap?.let { hub.removeEventTap(it) }
            linkToken?.let { hub.removeLinkListener(it) }
            tap = null
            linkToken = null
            link = hub.openLink(desktopKey)
            id = terminalId
        }
        timer.cancel()
        if (!gone && id.isNotEmpty() && link != null) {
            runCatching { worker.execute { runCatching { link.request("terminal.detach", mapOf("terminal" to id), 5_000) } } }
        }
        worker.shutdown()
    }

    fun end() {
        val id = terminalId
        if (id.isEmpty() || gone) {
            detach()
            return
        }
        synchronized(lock) {
            inputTask?.cancel()
            inputTask = null
            flushInputLocked()
        }
        val close = { hub.request("terminal.close", mapOf("terminal" to id), 20_000, desktopKey) }
        try {
            if (detached) close() else worker.submit<Map<String, Any?>> { close() }.get()
        } catch (failure: ExecutionException) {
            val cause = failure.cause ?: failure
            if (!(cause is LocalControlException && cause.code == "unknown_terminal")) throw cause
        } catch (refusal: LocalControlException) {
            if (refusal.code != "unknown_terminal") throw refusal
        }
        markGone("$providerLabel was ended from this phone.", TerminalStatusLevel.Ok)
        detach()
    }

    private fun setStatus(text: String, level: TerminalStatusLevel) {
        if (statusText == text && statusLevel == level) return
        statusText = text
        statusLevel = level
        if (!detached) notifyListeners()
    }

    private fun changed() {
        synchronized(lock) {
            if (detached || notifyTask != null) return
            notifyTask = schedule(notifyDelayMs) {
                synchronized(lock) { notifyTask = null }
                if (!detached) notifyListeners()
            }
        }
    }

    private fun schedule(delayMs: Long, block: () -> Unit): TimerTask? {
        if (delayMs <= 0) {
            block()
            return null
        }
        val task = object : TimerTask() {
            override fun run() = block()
        }
        return runCatching { timer.schedule(task, delayMs) }.map { task }.getOrNull()
    }
}
