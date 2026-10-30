package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.agentwatch.ControlWatchFeeds
import com.forkmesh.nativeapp.control.AgentTranscriptFormat
import com.forkmesh.nativeapp.control.TranscriptBlock
import com.forkmesh.nativeapp.control.UnifiedAgentRow

object AgentEagle {
    const val TRANSCRIPT_TAIL = 6000
    const val LINE_LIMIT = 120

    fun dialog(transcript: String, limit: Int = LINE_LIMIT): String {
        var text = transcript
        if (text.toByteArray(Charsets.UTF_8).size > TRANSCRIPT_TAIL) {
            text = text.takeLast(TRANSCRIPT_TAIL)
            val cut = text.indexOf("\n\n")
            if (cut >= 0) text = text.substring(cut + 2)
        }
        text = text.replace("\r\n", "\n")
        if (!text.endsWith("\n")) {
            val end = text.lastIndexOf('\n')
            text = if (end >= 0) text.substring(0, end + 1) else ""
        }
        for (block in AgentTranscriptFormat.blocks(text).asReversed()) {
            val said = oneLine(spoken(block))
            if (said.isEmpty()) continue
            return if (said.length > limit) said.take(limit - 1) + "…" else said
        }
        return ""
    }

    fun dialog(session: AgentLaunchSession): String {
        if (session.isActive) {
            val live = dialog(session.liveText + "\n")
            if (live.isNotEmpty()) return live
        }
        return dialog((session.history.lastOrNull { !it.isUser }?.text ?: "") + "\n")
    }

    private fun spoken(block: TranscriptBlock): String = when (block) {
        is TranscriptBlock.Text -> block.text
        is TranscriptBlock.Heading -> block.text
        is TranscriptBlock.Bullet -> block.text
        else -> ""
    }

    private fun oneLine(text: String): String =
        AgentTranscriptFormat.inline(text).joinToString("") { it.text }.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")
}

class AgentEagleFeed(
    private val said: (String) -> String = { AgentEagle.dialog(it) },
    private val runner: (Runnable) -> Unit = { Thread(it, "AgentEagle").apply { isDaemon = true }.start() },
) {
    companion object {
        const val FETCH_LIMIT = 3

        fun desktopKey(repo: String, localId: Int): String = "${repo.lowercase()}#$localId"
    }

    private val lock = Any()
    private var dialogs: Map<String, String> = emptyMap()
    private val desktopRows = HashMap<String, String>()
    private var feeds: ControlWatchFeeds? = null
    private val followed = HashSet<String>()
    private val launched = HashMap<String, String>()
    private val rows = HashMap<String, UnifiedAgentRow>()
    private var fetch: ((UnifiedAgentRow) -> String)? = null
    private val asked = HashSet<String>()
    private val waiting = ArrayDeque<String>()
    private var fetching = 0
    private val read = HashMap<String, Int>()
    private var generation = 0

    @Volatile
    var onChange: (() -> Unit)? = null

    val lines: Map<String, String> get() = synchronized(lock) { dialogs }

    fun follow(rows: List<UnifiedAgentRow>, feeds: () -> ControlWatchFeeds, fetch: ((UnifiedAgentRow) -> String)? = null) {
        val live = rows.filter { it.isActive && it.isDesktop && it.localId != 0 }
        val watch = synchronized(lock) {
            if (fetch != null) this.fetch = fetch
            for (row in rows) {
                this.rows[row.id] = row
                if (row.isDesktop && row.localId != 0 && !row.outside && !row.elsewhere) desktopRows[desktopKey(row.repoName, row.localId)] = row.id
            }
            followed.addAll(live.map { it.id })
            this.feeds ?: feeds().also { made ->
                this.feeds = made
                made.onChange = { refresh() }
            }
        }
        watch.follow(live)
        refresh()
    }

    fun stop() {
        val held = synchronized(lock) {
            generation++
            val held = feeds
            feeds = null
            followed.clear()
            launched.clear()
            rows.clear()
            fetch = null
            asked.clear()
            waiting.clear()
            fetching = 0
            read.clear()
            dialogs = emptyMap()
            desktopRows.clear()
            held
        }
        held?.stop()
    }

    fun desktopRowId(repoName: String, sessionId: Int): String? = synchronized(lock) { desktopRows[desktopKey(repoName, sessionId)] }

    fun dialog(rowId: String): String = synchronized(lock) { dialogs[rowId] ?: "" }

    fun launchedDialog(id: String, session: AgentLaunchSession): String {
        val text = AgentEagle.dialog(session)
        return synchronized(lock) {
            if (text.isEmpty()) {
                launched[id] ?: ""
            } else {
                launched[id] = text
                text
            }
        }
    }

    fun recall(rowId: String, active: Boolean, row: UnifiedAgentRow? = null) {
        if (active) return
        synchronized(lock) {
            if (fetch == null || (dialogs[rowId] ?: "").isNotEmpty() || rowId in asked) return
            if (rows[rowId] == null && row != null) rows[rowId] = row
            if (rows[rowId] == null) return
            asked += rowId
            waiting.addLast(rowId)
        }
        drain()
    }

    private fun drain() {
        while (true) {
            val job = synchronized(lock) {
                if (fetching >= FETCH_LIMIT || waiting.isEmpty()) return
                val id = waiting.removeFirst()
                val fetch = fetch
                val row = rows[id]
                if (fetch == null || row == null) {
                    null
                } else {
                    fetching++
                    Triple(id, row, fetch)
                }
            } ?: continue
            val (id, row, fetch) = job
            val mine = synchronized(lock) { generation }
            runner(
                Runnable {
                    val text = try {
                        fetch(row)
                    } catch (_: Exception) {
                        ""
                    }
                    val line = said(text + "\n")
                    val changed = synchronized(lock) {
                        if (generation != mine) return@Runnable
                        fetching--
                        recalled(id, line)
                    }
                    if (changed) onChange?.invoke()
                    drain()
                },
            )
        }
    }

    private fun recalled(id: String, line: String): Boolean {
        if (id !in asked || line.isEmpty() || (dialogs[id] ?: "").isNotEmpty()) return false
        dialogs = dialogs + (id to line)
        return true
    }

    private fun refresh() {
        val changed = synchronized(lock) {
            val feeds = feeds ?: return
            var next = dialogs
            for (id in followed) {
                val revision = feeds.revision(id)
                if (read[id] == revision) continue
                read[id] = revision
                val line = said(feeds.text(id)?.text ?: "")
                if (line.isNotEmpty()) next = next + (id to line)
            }
            if (next == dialogs) {
                false
            } else {
                dialogs = next
                true
            }
        }
        if (changed) onChange?.invoke()
    }
}
