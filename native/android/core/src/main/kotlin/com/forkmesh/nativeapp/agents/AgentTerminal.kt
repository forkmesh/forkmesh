package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Bytes
import java.nio.ByteBuffer
import java.nio.CharBuffer
import java.nio.charset.CodingErrorAction
import java.util.Timer
import java.util.TimerTask
import java.util.concurrent.CopyOnWriteArrayList

const val TERMINAL_MAX_SCROLLBACK = 10_000
const val TERMINAL_INPUT_FLUSH_MS = 25L
const val TERMINAL_OPEN_TIMEOUT_MS = 9000L
private const val MAX_OSC_CHARS = 4096
private const val MAX_INPUT_BYTES_PER_FRAME = 8000

val AGENT_LOGIN_PROVIDERS: Map<String, String> = linkedMapOf("claude-code" to "Claude Code", "codex" to "Codex")

fun agentProviderLabel(provider: String): String = AGENT_LOGIN_PROVIDERS[provider] ?: provider

const val TERMINAL_DEFAULT_COLOR = -1

const val TERMINAL_TRUE_COLOR = 0x1000000

private val SAFE_LINK = Regex("^https?://[\\x21-\\x7e]+$", RegexOption.IGNORE_CASE)
private val PLAIN_URL = Regex("https?://[\\x21-\\x7e]+")
private val URL_TRAILER = Regex("[.,;:!?)\\]}>'\"]+$")

private fun safeLink(uri: String): String = if (SAFE_LINK.matches(uri)) uri else ""

data class TerminalCell(
    val ch: String,
    val fg: Int = TERMINAL_DEFAULT_COLOR,
    val bg: Int = TERMINAL_DEFAULT_COLOR,
    val bold: Boolean = false,
    val dim: Boolean = false,
    val inverse: Boolean = false,
    val underline: Boolean = false,
    val link: String = "",
) {
    fun sameStyle(other: TerminalCell): Boolean =
        fg == other.fg && bg == other.bg && bold == other.bold && dim == other.dim && inverse == other.inverse &&
            underline == other.underline && link.isEmpty() == other.link.isEmpty()

    companion object {
        val BLANK = TerminalCell(" ")
    }
}

class TerminalRow(cols: Int) {
    val cells: MutableList<TerminalCell> = MutableList(cols) { TerminalCell.BLANK }

    var wrapped = false

    val text: String
        get() {
            var width = cells.size
            while (width > 0 && cells[width - 1].ch == " ") width--
            return cells.take(width).joinToString("") { it.ch }
        }
}

private enum class ParseState { Ground, Esc, EscCharset, Csi, Osc, OscEsc }

class TerminalScreen(val rows: Int, val cols: Int) {
    var grid: MutableList<TerminalRow> = MutableList(rows) { TerminalRow(cols) }
    private var alt: MutableList<TerminalRow>? = null
    var scrollback: MutableList<TerminalRow> = ArrayList()
    var cx = 0
    var cy = 0
    private var savedCx = 0
    private var savedCy = 0
    var scrollTop = 0
    var scrollBottom = rows - 1
    private var fg = TERMINAL_DEFAULT_COLOR
    private var bg = TERMINAL_DEFAULT_COLOR
    private var bold = false
    private var dim = false
    private var inverse = false
    private var underline = false
    private var wrap = true
    private var wrapPending = false
    var cursorVisible = true
    var appCursorKeys = false
    var bracketedPaste = false
    private var state = ParseState.Ground
    private val params = StringBuilder()
    private val intermediate = StringBuilder()
    private val osc = StringBuilder()
    private var link = ""

    val onAltScreen: Boolean get() = alt != null

    val allRows: List<TerminalRow> get() = scrollback + grid

    private fun clampCursor() {
        cx = cx.coerceIn(0, cols - 1)
        cy = cy.coerceIn(0, rows - 1)
    }

    private fun scrollUp(top: Int, bottom: Int, count: Int) {
        repeat(count) {
            val removed = grid.removeAt(top)
            if (alt == null && top == 0) {
                scrollback.add(removed)
                if (scrollback.size > TERMINAL_MAX_SCROLLBACK) scrollback.removeAt(0)
            }
            grid.add(bottom, TerminalRow(cols))
        }
    }

    private fun scrollDown(top: Int, bottom: Int, count: Int) {
        repeat(count) {
            grid.removeAt(bottom)
            grid.add(top, TerminalRow(cols))
        }
    }

    private fun newLine() {
        if (cy == scrollBottom) scrollUp(scrollTop, scrollBottom, 1) else if (cy < rows - 1) cy++
    }

    private fun putChar(ch: String) {
        if (wrapPending && wrap) {
            grid[cy].wrapped = true
            cx = 0
            newLine()
        }
        wrapPending = false
        clampCursor()
        grid[cy].cells[cx] = TerminalCell(ch, fg, bg, bold, dim, inverse, underline, link)
        if (cx + 1 >= cols) wrapPending = true else cx++
    }

    private fun erase(row: TerminalRow, from: Int, to: Int) {
        if (from <= 0 && to >= row.cells.size - 1) row.wrapped = false
        val cell = TerminalCell(" ", bg = bg)
        var i = maxOf(0, from)
        while (i <= to && i < row.cells.size) {
            row.cells[i] = cell
            i++
        }
    }

    private fun sgrCodes(raw: String): List<Int> = raw.split(';').flatMap { group ->
        val parts = group.split(':').map { it.toIntOrNull() ?: 0 }
        when {
            parts.size == 1 -> parts
            parts[0] == 4 -> listOf(if (parts[1] == 0) 24 else 4)
            (parts[0] == 38 || parts[0] == 48) && parts[1] == 5 && parts.size >= 3 -> parts.take(3)
            (parts[0] == 38 || parts[0] == 48) && parts[1] == 2 && parts.size >= 5 -> listOf(parts[0], 2) + parts.takeLast(3)
            else -> emptyList()
        }
    }

    private fun applySgr(input: List<Int>) {
        val codes = input.ifEmpty { listOf(0) }
        fun at(i: Int): Int = if (i < codes.size) codes[i] else 0
        var i = 0
        while (i < codes.size) {
            val code = codes[i]
            when {
                code == 0 -> {
                    fg = TERMINAL_DEFAULT_COLOR
                    bg = TERMINAL_DEFAULT_COLOR
                    bold = false
                    dim = false
                    inverse = false
                    underline = false
                }
                code == 1 -> bold = true
                code == 2 -> dim = true
                code == 4 -> underline = true
                code == 7 -> inverse = true
                code == 22 -> {
                    bold = false
                    dim = false
                }
                code == 24 -> underline = false
                code == 27 -> inverse = false
                code in 30..37 -> fg = code - 30
                code == 39 -> fg = TERMINAL_DEFAULT_COLOR
                code in 40..47 -> bg = code - 40
                code == 49 -> bg = TERMINAL_DEFAULT_COLOR
                code in 90..97 -> fg = code - 90 + 8
                code in 100..107 -> bg = code - 100 + 8
                code == 38 || code == 48 -> {
                    var value: Int? = null
                    if (at(i + 1) == 5) {
                        value = at(i + 2).coerceIn(0, 255)
                        i += 2
                    } else if (at(i + 1) == 2) {
                        val r = at(i + 2).coerceIn(0, 255)
                        val g = at(i + 3).coerceIn(0, 255)
                        val b = at(i + 4).coerceIn(0, 255)
                        value = TERMINAL_TRUE_COLOR + ((r shl 16) or (g shl 8) or b)
                        i += 4
                    }
                    if (value != null) {
                        if (code == 38) fg = value else bg = value
                    }
                }
            }
            i++
        }
    }

    private fun useAltScreen(on: Boolean) {
        if (on && alt == null) {
            alt = grid
            grid = MutableList(rows) { TerminalRow(cols) }
            cx = 0
            cy = 0
        } else if (!on && alt != null) {
            grid = alt!!
            alt = null
        }
    }

    private fun handleMode(params: List<Int>, enable: Boolean, isPrivate: Boolean) {
        if (!isPrivate) return
        for (code in params) {
            when (code) {
                1 -> appCursorKeys = enable
                7 -> wrap = enable
                25 -> cursorVisible = enable
                2004 -> bracketedPaste = enable
                47, 1047, 1049 -> {
                    if (code == 1049 && enable) {
                        savedCx = cx
                        savedCy = cy
                    }
                    useAltScreen(enable)
                    if (code == 1049 && !enable) {
                        cx = savedCx
                        cy = savedCy
                    }
                }
            }
        }
    }

    private fun handleCsi(finalByte: Char) {
        val isPrivate = intermediate.toString() == "?"
        if (intermediate.isNotEmpty() && !(isPrivate && finalByte in "hlJK")) return
        val raw = params.toString()
        val parsed = raw.split(';').map { if (it.isEmpty()) 0 else it.toIntOrNull() ?: 0 }
        val first = parsed.first()
        val count = maxOf(1, first)
        fun param(i: Int, fallback: Int): Int = if (i < parsed.size && parsed[i] != 0) parsed[i] else fallback
        when (finalByte) {
            'A' -> cy = maxOf(scrollTop, cy - count)
            'B' -> cy = minOf(scrollBottom, cy + count)
            'C' -> {
                cx = minOf(cols - 1, cx + count)
                wrapPending = false
            }
            'D' -> {
                cx = maxOf(0, cx - count)
                wrapPending = false
            }
            'E' -> {
                cx = 0
                cy = minOf(rows - 1, cy + count)
            }
            'F' -> {
                cx = 0
                cy = maxOf(0, cy - count)
            }
            'G', '`' -> {
                cx = count - 1
                wrapPending = false
            }
            'd' -> cy = count - 1
            'H', 'f' -> {
                cy = param(0, 1) - 1
                cx = param(1, 1) - 1
                wrapPending = false
            }
            'J' -> {
                clampCursor()
                when (first) {
                    0 -> {
                        erase(grid[cy], cx, cols - 1)
                        for (y in cy + 1 until rows) erase(grid[y], 0, cols - 1)
                    }
                    1 -> {
                        for (y in 0 until cy) erase(grid[y], 0, cols - 1)
                        erase(grid[cy], 0, cx)
                    }
                    else -> for (y in 0 until rows) erase(grid[y], 0, cols - 1)
                }
            }
            'K' -> {
                clampCursor()
                when (first) {
                    0 -> erase(grid[cy], cx, cols - 1)
                    1 -> erase(grid[cy], 0, cx)
                    else -> erase(grid[cy], 0, cols - 1)
                }
            }
            'L' -> scrollDown(cy, scrollBottom, count)
            'M' -> scrollUp(cy, scrollBottom, count)
            'P' -> {
                clampCursor()
                val cells = grid[cy].cells
                val n = minOf(count, cols - cx)
                repeat(n) { cells.removeAt(cx) }
                repeat(n) { cells.add(TerminalCell.BLANK) }
            }
            '@' -> {
                clampCursor()
                val cells = grid[cy].cells
                val n = minOf(count, cols - cx)
                repeat(n) { cells.add(cx, TerminalCell.BLANK) }
                while (cells.size > cols) cells.removeAt(cells.size - 1)
            }
            'X' -> {
                clampCursor()
                erase(grid[cy], cx, cx + count - 1)
            }
            'S' -> scrollUp(scrollTop, scrollBottom, count)
            'T' -> scrollDown(scrollTop, scrollBottom, count)
            'r' -> {
                scrollTop = maxOf(0, param(0, 1) - 1)
                scrollBottom = minOf(rows - 1, param(1, rows) - 1)
                if (scrollBottom <= scrollTop) {
                    scrollTop = 0
                    scrollBottom = rows - 1
                }
                cx = 0
                cy = scrollTop
            }
            'h' -> handleMode(parsed, true, isPrivate)
            'l' -> handleMode(parsed, false, isPrivate)
            'm' -> applySgr(if (raw.isEmpty()) emptyList() else sgrCodes(raw))
            's' -> {
                savedCx = cx
                savedCy = cy
            }
            'u' -> {
                cx = savedCx
                cy = savedCy
            }
        }
        clampCursor()
    }

    private fun endOsc() {
        val text = osc.toString()
        osc.setLength(0)
        if (text.length > MAX_OSC_CHARS || !text.startsWith("8;")) return
        val split = text.indexOf(';', 2)
        if (split < 0) return
        link = safeLink(text.substring(split + 1))
    }

    private fun reset() {
        grid = MutableList(rows) { TerminalRow(cols) }
        alt = null
        cx = 0
        cy = 0
        savedCx = 0
        savedCy = 0
        scrollTop = 0
        scrollBottom = rows - 1
        fg = TERMINAL_DEFAULT_COLOR
        bg = TERMINAL_DEFAULT_COLOR
        bold = false
        dim = false
        inverse = false
        underline = false
        wrap = true
        wrapPending = false
        cursorVisible = true
        appCursorKeys = false
        bracketedPaste = false
        link = ""
    }

    fun resized(newRows: Int, newCols: Int): TerminalScreen {
        val next = TerminalScreen(newRows, newCols)
        fun fit(row: TerminalRow): TerminalRow {
            if (row.cells.size == newCols) return row
            val out = TerminalRow(newCols)
            var i = 0
            while (i < newCols && i < row.cells.size) {
                out.cells[i] = row.cells[i]
                i++
            }
            return out
        }
        fun refit(from: List<TerminalRow>, cursorRow: Int, keep: Boolean): MutableList<TerminalRow> {
            val kept = ArrayList(from)
            while (kept.size > newRows && kept.size - 1 > cursorRow) kept.removeAt(kept.size - 1)
            while (kept.size > newRows) {
                val removed = kept.removeAt(0)
                if (keep) next.scrollback.add(removed)
            }
            while (kept.size < newRows) kept.add(TerminalRow(newCols))
            return kept.map(::fit).toMutableList()
        }
        next.scrollback = ArrayList(scrollback)
        val shift = maxOf(0, cy + 1 - newRows)
        val altNow = alt
        val savedShift = if (altNow == null) shift else maxOf(0, savedCy + 1 - newRows)
        next.grid = refit(grid, cy, altNow == null)
        if (altNow != null) next.alt = refit(altNow, savedCy, true)
        if (next.scrollback.size > TERMINAL_MAX_SCROLLBACK) {
            next.scrollback.subList(0, next.scrollback.size - TERMINAL_MAX_SCROLLBACK).clear()
        }
        next.cx = minOf(cx, newCols - 1)
        next.cy = cy - shift
        next.savedCx = minOf(savedCx, newCols - 1)
        next.savedCy = minOf(maxOf(0, savedCy - savedShift), newRows - 1)
        next.fg = fg
        next.bg = bg
        next.bold = bold
        next.dim = dim
        next.inverse = inverse
        next.underline = underline
        next.wrap = wrap
        next.wrapPending = wrapPending && newCols == cols
        next.cursorVisible = cursorVisible
        next.appCursorKeys = appCursorKeys
        next.bracketedPaste = bracketedPaste
        next.state = state
        next.link = link
        next.params.append(params)
        next.intermediate.append(intermediate)
        next.osc.append(osc)
        return next
    }

    fun feed(text: String) {
        var index = 0
        while (index < text.length) {
            val rune = text.codePointAt(index)
            index += Character.charCount(rune)
            val ch = String(Character.toChars(rune))
            when (state) {
                ParseState.Osc -> {
                    if (rune == 0x07) {
                        endOsc()
                        state = ParseState.Ground
                    } else if (rune == 0x1b) {
                        state = ParseState.OscEsc
                    } else if (osc.length <= MAX_OSC_CHARS) {
                        osc.append(ch)
                    }
                    continue
                }
                ParseState.OscEsc -> {
                    if (ch == "\\") {
                        endOsc()
                        state = ParseState.Ground
                    } else {
                        state = ParseState.Osc
                    }
                    continue
                }
                ParseState.Esc -> {
                    state = ParseState.Ground
                    when (ch) {
                        "[" -> {
                            state = ParseState.Csi
                            params.setLength(0)
                            intermediate.setLength(0)
                        }
                        "]" -> {
                            state = ParseState.Osc
                            osc.setLength(0)
                        }
                        "(", ")", "#", "%" -> state = ParseState.EscCharset
                        "7" -> {
                            savedCx = cx
                            savedCy = cy
                        }
                        "8" -> {
                            cx = savedCx
                            cy = savedCy
                        }
                        "D" -> newLine()
                        "E" -> {
                            cx = 0
                            newLine()
                        }
                        "M" -> if (cy == scrollTop) scrollDown(scrollTop, scrollBottom, 1) else cy = maxOf(0, cy - 1)
                        "c" -> reset()
                    }
                    continue
                }
                ParseState.EscCharset -> {
                    state = ParseState.Ground
                    continue
                }
                ParseState.Csi -> {
                    if (rune in 0x30..0x3f) {
                        if (ch == "?" || ch == "<" || ch == "=" || ch == ">") intermediate.append(ch) else params.append(ch)
                        continue
                    }
                    if (rune in 0x20..0x2f) {
                        intermediate.append(ch)
                        continue
                    }
                    handleCsi(ch[0])
                    state = ParseState.Ground
                    continue
                }
                ParseState.Ground -> Unit
            }
            when {
                rune == 0x1b -> state = ParseState.Esc
                rune == 0x0d -> {
                    cx = 0
                    wrapPending = false
                }
                rune == 0x0a || rune == 0x0b || rune == 0x0c -> {
                    newLine()
                    wrapPending = false
                }
                rune == 0x08 -> {
                    cx = maxOf(0, cx - 1)
                    wrapPending = false
                }
                rune == 0x09 -> {
                    cx = minOf(cols - 1, (cx / 8 + 1) * 8)
                    wrapPending = false
                }
                rune >= 0x20 && rune != 0x7f -> putChar(ch)
            }
        }
    }

    fun links(): List<String> {
        val rows = allRows
        val wrapped = wrappedUrlTargets(rows)
        val seen = HashSet<String>()
        val found = ArrayList<String>()
        for ((r, row) in rows.withIndex()) {
            for ((col, cell) in row.cells.withIndex()) {
                val target = cell.link.ifEmpty { wrapped[r]?.get(col) ?: "" }
                if (target.isNotEmpty() && seen.add(target)) found.add(target)
            }
        }
        return found
    }

    companion object {
        fun wrappedUrlTargets(rows: List<TerminalRow>): List<Map<Int, String>?> {
            val targets = MutableList<MutableMap<Int, String>?>(rows.size) { null }
            var start = 0
            while (start < rows.size) {
                var end = start
                while (end < rows.size - 1 && rows[end].wrapped) end++
                val text = StringBuilder()
                val where = ArrayList<Pair<Int, Int>>()
                for (r in start..end) {
                    val cells = rows[r].cells
                    for (col in cells.indices) {
                        text.append(if (cells[col].ch.length == 1) cells[col].ch else " ")
                        where.add(r to col)
                    }
                }
                val joined = text.toString()
                if (joined.contains("://")) {
                    for (match in PLAIN_URL.findAll(joined)) {
                        val url = safeLink(match.value.replace(URL_TRAILER, ""))
                        for (i in url.indices) {
                            val (r, col) = where[match.range.first + i]
                            (targets[r] ?: HashMap<Int, String>().also { targets[r] = it })[col] = url
                        }
                    }
                }
                start = end + 1
            }
            return targets
        }
    }
}

enum class TerminalKey { Enter, Backspace, Tab, Escape, Up, Down, Right, Left, Home, End, PageUp, PageDown, Delete, CtrlC }

fun terminalKeyBytes(key: TerminalKey, appCursorKeys: Boolean = false, control: Boolean = false): String {
    val esc = "\u001b"
    fun cursor(letter: String) = if (control) "$esc[1;5$letter" else esc + (if (appCursorKeys) "O" else "[") + letter
    return when (key) {
        TerminalKey.Enter -> "\r"
        TerminalKey.Backspace -> "\u007f"
        TerminalKey.Tab -> "\t"
        TerminalKey.Escape -> esc
        TerminalKey.Up -> cursor("A")
        TerminalKey.Down -> cursor("B")
        TerminalKey.Right -> cursor("C")
        TerminalKey.Left -> cursor("D")
        TerminalKey.Home -> cursor("H")
        TerminalKey.End -> cursor("F")
        TerminalKey.PageUp -> "$esc[5~"
        TerminalKey.PageDown -> "$esc[6~"
        TerminalKey.Delete -> "$esc[3~"
        TerminalKey.CtrlC -> "\u0003"
    }
}

fun terminalChordTyped(old: String, new: String): Char? {
    for (index in new.indices) {
        if (index >= old.length || old[index] != new[index]) return new[index]
    }
    return null
}

fun terminalControlBytes(character: String): String {
    if (character.length != 1) return ""
    val upper = character.uppercase()[0].code
    if (upper in 0x41..0x5a) return (upper - 64).toChar().toString()
    return when (character) {
        " " -> "\u0000"
        "[" -> "\u001b"
        "\\" -> "\u001c"
        "]" -> "\u001d"
        else -> ""
    }
}

enum class TerminalStatusLevel { Pending, Ok, Error }

interface AgentTerminalSession {
    val screen: TerminalScreen
    val statusText: String
    val statusLevel: TerminalStatusLevel
    val plainText: String

    fun addListener(listener: () -> Unit): Boolean

    fun removeListener(listener: () -> Unit): Boolean

    fun start(cols: Int, rows: Int)

    fun resize(cols: Int, rows: Int)

    fun input(text: String)

    fun paste(text: String)

    fun key(key: TerminalKey)

    fun submit(text: String) {
        input(text)
        key(TerminalKey.Enter)
    }
}

interface RelayFrameChannel {
    var onFrame: ((Map<String, Any?>) -> Unit)?
    var onConnected: (() -> Unit)?
    var onSignedOut: (() -> Unit)?

    fun start()

    fun send(frame: Map<String, Any?>): Boolean

    fun dispose()
}

internal class StreamingUtf8 {
    private val decoder = Charsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPLACE)
        .onUnmappableCharacter(CodingErrorAction.REPLACE)
    private var carry = ByteArray(0)

    fun decode(bytes: ByteArray): String {
        val input = ByteBuffer.wrap(carry + bytes)
        val out = CharBuffer.allocate((carry.size + bytes.size) * 2 + 4)
        decoder.decode(input, out, false)
        carry = ByteArray(input.remaining()).also { input.get(it) }
        out.flip()
        return out.toString()
    }
}

class AgentLoginTerminal(
    val node: String,
    val provider: String,
    private val channel: RelayFrameChannel,
    session: String? = null,
    private val openTimeoutMs: Long = TERMINAL_OPEN_TIMEOUT_MS,
    private val inputFlushMs: Long = TERMINAL_INPUT_FLUSH_MS,
    private val notifyDelayMs: Long = 16,
) : AgentTerminalSession {
    val session: String = session ?: Bytes.hex(Bytes.random(16))
    private val lock = Any()
    private val timer = Timer("AgentLoginTerminal", true)
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    override var screen = TerminalScreen(24, 80)
        private set
    private var cols = 80
    private var rows = 24

    @Volatile
    override var statusText: String = "Connecting to $node…"
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
    var closed = false
        private set

    private var answered = false
    private var started = false
    private var openTask: TimerTask? = null
    private var inputTask: TimerTask? = null
    private var notifyTask: TimerTask? = null
    private val pendingInput = StringBuilder()
    private val utf8 = StreamingUtf8()
    private var linkCache: List<String>? = null
    private var signInLinkValue = ""

    init {
        channel.onConnected = { onConnected() }
        channel.onFrame = { onFrame(it) }
        channel.onSignedOut = { setStatus("Sign in to ForkMesh again to continue.", TerminalStatusLevel.Error) }
    }

    override fun addListener(listener: () -> Unit) = listeners.add(listener)

    override fun removeListener(listener: () -> Unit) = listeners.remove(listener)

    private fun notifyListeners() {
        for (listener in listeners) runCatching { listener() }
    }

    val providerLabel: String get() = agentProviderLabel(provider)

    val canRestart: Boolean get() = exitCode != null || statusLevel == TerminalStatusLevel.Error

    override val plainText: String get() = synchronized(lock) { screen.allRows.joinToString("\n") { it.text }.trim() }

    val links: List<String> get() = synchronized(lock) { linkCache ?: screen.links().also { linkCache = it } }

    val signInLink: String
        get() = synchronized(lock) {
            val onScreen = links
            if (onScreen.isNotEmpty()) signInLinkValue = onScreen.last()
            signInLinkValue
        }

    override fun start(cols: Int, rows: Int) {
        synchronized(lock) {
            if (started || closed) return
            started = true
            this.cols = cols.coerceIn(20, 500)
            this.rows = rows.coerceIn(5, 200)
            screen = TerminalScreen(this.rows, this.cols)
        }
        setStatus("Connecting to $node…", TerminalStatusLevel.Pending)
        channel.start()
    }

    private fun onConnected() {
        if (closed) return
        setStatus("Connecting to $node…", TerminalStatusLevel.Pending)
        sendOpen()
    }

    private fun send(frame: Map<String, Any?>): Boolean {
        val full = linkedMapOf<String, Any?>("type" to "terminal", "session" to session, "node" to node)
        full.putAll(frame)
        return channel.send(full)
    }

    private fun sendOpen() {
        synchronized(lock) {
            screen = TerminalScreen(rows, cols)
        }
        changed()
        if (!send(linkedMapOf("action" to "open", "provider" to provider, "cols" to cols, "rows" to rows))) return
        synchronized(lock) {
            openTask?.cancel()
            answered = false
            openTask = schedule(openTimeoutMs) {
                synchronized(lock) { openTask = null }
                if (answered || closed) return@schedule
                setStatus(
                    "$node did not answer. It can only be reached here while it is online and configured to accept " +
                        "agent work from the website.",
                    TerminalStatusLevel.Error,
                )
            }
        }
    }

    internal fun onFrame(frame: Map<String, Any?>) {
        if (closed || frame["type"] != "terminal") return
        if (frame["session"] != session || frame["from"] != "node") return
        synchronized(lock) {
            answered = true
            openTask?.cancel()
            openTask = null
        }
        when (frame["action"]) {
            "ready" -> {
                live = true
                exitCode = null
                setStatus("Live on $node. Complete $providerLabel's own sign-in below.", TerminalStatusLevel.Ok)
            }
            "output" -> {
                val bytes = B64.decodeOrNull(frame["data"]?.toString() ?: "") ?: return
                synchronized(lock) { screen.feed(utf8.decode(bytes)) }
                changed()
            }
            "error" -> {
                val message = frame["message"]?.toString()?.trim() ?: ""
                setStatus(message.ifEmpty { "The node refused the session." }, TerminalStatusLevel.Error)
            }
            "exit" -> {
                val code = (frame["code"] as? Number)?.toInt() ?: 0
                live = false
                exitCode = code
                setStatus(
                    if (code == 0) "The sign-in command finished." else "The sign-in command exited with status $code.",
                    if (code == 0) TerminalStatusLevel.Ok else TerminalStatusLevel.Error,
                )
            }
        }
    }

    override fun input(text: String) {
        if (text.isEmpty() || closed) return
        synchronized(lock) {
            pendingInput.append(text)
            if (pendingInput.length > 4096) {
                inputTask?.cancel()
                inputTask = null
                flushInputLocked()
                return
            }
            if (inputTask == null) inputTask = schedule(inputFlushMs) { synchronized(lock) { inputTask = null; flushInputLocked() } }
        }
    }

    override fun paste(text: String) {
        if (text.isEmpty()) return
        input(if (screen.bracketedPaste && text.length > 1) "\u001b[200~$text\u001b[201~" else text)
    }

    override fun key(key: TerminalKey) = input(terminalKeyBytes(key, screen.appCursorKeys))

    private fun flushInputLocked() {
        if (pendingInput.isEmpty()) return
        val bytes = pendingInput.toString().toByteArray(Charsets.UTF_8)
        pendingInput.setLength(0)
        var at = 0
        while (at < bytes.size) {
            val end = minOf(bytes.size, at + MAX_INPUT_BYTES_PER_FRAME)
            send(linkedMapOf("action" to "input", "data" to B64.encode(bytes.copyOfRange(at, end))))
            at = end
        }
    }

    override fun resize(cols: Int, rows: Int) {
        val c = cols.coerceIn(20, 500)
        val r = rows.coerceIn(5, 200)
        synchronized(lock) {
            if (c == this.cols && r == this.rows) return
            this.cols = c
            this.rows = r
            if (!started || closed) return
            screen = screen.resized(r, c)
        }
        send(linkedMapOf("action" to "resize", "cols" to c, "rows" to r))
        changed()
    }

    private fun setStatus(text: String, level: TerminalStatusLevel) {
        if (statusText == text && statusLevel == level) return
        statusText = text
        statusLevel = level
        if (!closed) notifyListeners()
    }

    private fun changed() {
        synchronized(lock) {
            linkCache = null
            if (closed || notifyTask != null) return
            notifyTask = schedule(notifyDelayMs) {
                synchronized(lock) { notifyTask = null }
                if (closed) return@schedule
                signInLink
                notifyListeners()
            }
        }
    }

    private fun schedule(delayMs: Long, block: () -> Unit): TimerTask {
        val task = object : TimerTask() {
            override fun run() = block()
        }
        if (delayMs <= 0) {
            block()
        } else {
            timer.schedule(task, delayMs)
        }
        return task
    }

    fun close() {
        synchronized(lock) {
            if (closed) return
            flushInputLocked()
        }
        send(mapOf("action" to "close"))
        synchronized(lock) {
            closed = true
            openTask?.cancel()
            inputTask?.cancel()
            notifyTask?.cancel()
        }
        timer.cancel()
        channel.dispose()
    }
}
