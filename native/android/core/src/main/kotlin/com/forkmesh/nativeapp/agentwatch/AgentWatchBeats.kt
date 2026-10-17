package com.forkmesh.nativeapp.agentwatch

import kotlin.math.min

class WatchBeat(
    val motion: AgentMotion,
    val text: String,
    bytes: Int? = null,
    val said: String = "",
) {
    val bytes: Int = min(bytes ?: text.length, kPayloadCap)

    val seed: Long
        get() {
            val hash = fnv1a("${motion.name}|$text")
            return if (hash == 0L) 1L else hash
        }

    override fun equals(other: Any?): Boolean = other is WatchBeat && other.motion == motion && other.text == text

    override fun hashCode(): Int = motion.hashCode() * 31 + text.hashCode()

    override fun toString(): String = "WatchBeat(${motion.name}, $text)"
}

fun motionForTool(rawName: String, command: String = ""): AgentMotion {
    val name = rawName.lowercase()
    fun has(needle: String) = name.contains(needle)
    if (name.startsWith("mcp__")) return AgentMotion.mcp
    if (has("askuser") || name.startsWith("ask")) return AgentMotion.ask
    if (has("skill")) return AgentMotion.skill
    if (has("todo") || name == "taskcreate" || name == "taskupdate" || name == "tasklist" || name == "taskget") {
        return AgentMotion.todo
    }
    if (has("plan")) return AgentMotion.plan
    if (has("monitor") || has("bashoutput") || has("taskoutput") || has("sleep") || has("wait") ||
        has("killshell") || has("taskstop")
    ) {
        return AgentMotion.wait
    }
    if (has("web") || has("browser") || has("url")) return AgentMotion.web
    if (has("task") || has("agent") || has("workflow")) return AgentMotion.delegate
    if (has("image") || has("screenshot")) return AgentMotion.image
    if (has("git_")) return AgentMotion.git
    if (has("glob") || name == "ls" || has("list_dir") || has("listdir") || has("list_files") || has("tree")) {
        return AgentMotion.scan
    }
    if (has("grep") || has("search") || has("find")) return AgentMotion.search
    if (has("edit") || has("patch") || has("apply") || has("notebook") || has("replace")) return AgentMotion.write
    if (has("write") || has("create")) return AgentMotion.create
    if (has("read") || has("fetch") || has("view") || has("list")) return AgentMotion.read
    return motionForCommand(command)
}

private val testCommands = listOf(
    "pytest", "ctest", "cargo test", "go test", "npm test", "npm run test", "yarn test", "pnpm test",
    "dart test", "jest", "vitest", "playwright", "gradlew test", "-tests", "unittest",
)
private val installCommands = listOf(
    "npm install", "npm i ", "npm ci", "pip install", "pip3 install", "uv add", "uv pip", "uv sync",
    "apt install", "apt-get install", "brew install", "cargo add", "cargo install", "yarn add",
    "yarn install", "pnpm add", "pnpm install", "pub get", "go get", "gem install", "poetry add",
    "poetry install",
)
private val buildCommands = listOf(
    "ninja", "cmake", "make ", "cargo build", "go build", "npm run build", "gradlew",
    "xcodebuild", "webpack", "vite build", "g++ ", "gcc ", "clang",
)
private val gitCommand = Regex("""(^|[;&|(]\s*)(git|gh)\s""")
private val sleepCommand = Regex("""(^|[;&|]\s*)sleep\s""")
private val tscCommand = Regex("""\btsc\b""")

fun motionForCommand(command: String, background: Boolean = false): AgentMotion {
    if (background) return AgentMotion.spawn
    val c = command.lowercase()
    if (testCommands.any { c.contains(it) }) return AgentMotion.test
    if (installCommands.any { c.contains(it) }) return AgentMotion.install
    if (buildCommands.any { c.contains(it) } || c.trim() == "make" || tscCommand.containsMatchIn(c)) {
        return AgentMotion.build
    }
    val trimmed = c.trim()
    if (gitCommand.containsMatchIn(trimmed)) return AgentMotion.git
    if (sleepCommand.containsMatchIn(trimmed)) return AgentMotion.wait
    return AgentMotion.run
}

private class BeatDraft(val motion: AgentMotion, val text: String, var bytes: Int) {
    val said = StringBuilder()

    fun build(): WatchBeat =
        WatchBeat(motion, text, bytes, if (motion == AgentMotion.speak) plainProse(said.toString()) else "")
}

fun beatsFromTranscript(transcript: String): List<WatchBeat> {
    val beats = ArrayList<BeatDraft>()
    var text = transcript.replace("\r\n", "\n")
    if (text.isNotEmpty() && !text.endsWith("\n")) {
        val cut = text.lastIndexOf('\n')
        text = if (cut >= 0) text.substring(0, cut + 1) else ""
    }
    var inTool = false
    var inPrompt = false
    var inProse = false
    var sawStart = false
    for (line in text.split("\n")) {
        val length = line.length
        if (inTool) {
            beats.last().bytes += length + 1
            if (line.trimEnd().endsWith("]")) inTool = false
            continue
        }
        if (line.trim().isEmpty()) {
            inProse = false
            inPrompt = false
            continue
        }
        if (line.startsWith("==>")) {
            inProse = false
            inPrompt = false
            val body = line.substring(3).trim()
            val lower = body.lowercase()
            val motion = when {
                lower.contains("fail") || lower.contains("error") || lower.contains("refused") ||
                    lower.contains("denied") -> AgentMotion.notice
                lower.contains("stop requested") || lower.startsWith("stopping") -> AgentMotion.notice
                !sawStart && (lower.startsWith("started") || lower.startsWith("running")) -> {
                    sawStart = true
                    AgentMotion.start
                }
                lower.contains("compact") -> AgentMotion.compact
                lower.contains("retry") || lower.contains("retrying") -> AgentMotion.retry
                lower.startsWith("hook") -> AgentMotion.hook
                else -> AgentMotion.system
            }
            beats += BeatDraft(motion, clip(body), body.length)
            continue
        }
        if (line.startsWith("> ")) {
            inProse = false
            if (!inPrompt) beats += BeatDraft(AgentMotion.prompt, clip(line.substring(2)), length)
            else beats.last().bytes += length + 1
            inPrompt = true
            continue
        }
        val name = toolName(line)
        if (name != null) {
            inProse = false
            inPrompt = false
            var args = line.substring(1 + name.length).trim()
            val closed = line.trimEnd().endsWith("]")
            if (closed && args.endsWith("]")) args = args.dropLast(1)
            if (!closed) inTool = true
            val background = args.contains("\"run_in_background\":true")
            val motion = if (name.lowercase() == "bash" && background) AgentMotion.spawn else motionForTool(name, args)
            beats += BeatDraft(motion, clip("$name $args"), length)
            continue
        }
        if (inPrompt) {
            beats.last().bytes += length + 1
            continue
        }
        if (!inProse) {
            val lower = line.lowercase()
            val motion = if (lower.startsWith("thinking") || lower.startsWith("(thinking")) AgentMotion.think else AgentMotion.speak
            beats += BeatDraft(motion, clip(line), 0)
            inProse = true
        }
        val last = beats.last()
        last.bytes += length + 1
        if (last.said.isNotEmpty()) last.said.append('\n')
        last.said.append(line)
    }
    return beats.map { it.build() }
}

private fun isAsciiLetter(c: Char) = c in 'A'..'Z' || c in 'a'..'z'

private fun toolName(line: String): String? {
    if (line.length < 2 || line[0] != '[' || !isAsciiLetter(line[1])) return null
    var end = 2
    while (end < line.length && (isAsciiLetter(line[end]) || line[end] in '0'..'9' || line[end] == '_' || line[end] == '-')) {
        end++
    }
    if (end < line.length) {
        val next = line[end]
        if (next != ']' && !next.isWhitespace()) return null
    }
    return line.substring(1, end)
}

internal fun clip(text: String, limit: Int = 90): String {
    val line = collapseWhitespace(text)
    if (line.length <= limit) return line
    return line.substring(0, limit - 1) + "…"
}

internal fun collapseWhitespace(text: String): String {
    val out = StringBuilder()
    var pendingSpace = false
    for (c in text) {
        if (c.isWhitespace()) {
            pendingSpace = out.isNotEmpty()
        } else {
            if (pendingSpace) out.append(' ')
            pendingSpace = false
            out.append(c)
        }
    }
    return out.toString()
}

fun plainProse(text: String, limit: Int = 240): String {
    val kept = ArrayList<String>()
    var fenced = false
    for (raw in text.split("\n")) {
        var line = raw.trim()
        if (line.startsWith("```")) {
            fenced = !fenced
            continue
        }
        if (fenced || line.isEmpty()) continue
        while (line.startsWith("#") || line.startsWith(">")) line = line.substring(1).trim()
        if (line.startsWith("- ") || line.startsWith("* ")) line = line.substring(2)
        kept += line
    }
    var plain = kept.joinToString(" ")
    for (mark in listOf("**", "__", "`")) plain = plain.replace(mark, "")
    plain = collapseWhitespace(plain)
    if (plain.length > limit) plain = plain.substring(0, limit - 1).trim() + "…"
    return plain
}

fun newBeats(previous: List<WatchBeat>, next: List<WatchBeat>, fallback: Int = 4): List<WatchBeat> {
    val last = previous.lastOrNull() ?: return next
    if (next.size >= previous.size && next[previous.size - 1] == last) return next.subList(previous.size, next.size)
    for (i in next.indices.reversed()) {
        if (next[i] == last) return next.subList(i + 1, next.size)
    }
    return if (next.size <= fallback) next else next.subList(next.size - fallback, next.size)
}
