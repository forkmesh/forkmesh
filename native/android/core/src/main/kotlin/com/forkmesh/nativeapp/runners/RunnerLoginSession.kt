package com.forkmesh.nativeapp.runners

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import java.io.ByteArrayOutputStream
import java.util.Base64

object RunnerLoginText {
    const val MAX_INPUT_BYTES = 4096
    const val MAX_OUTPUT_BYTES = 200_000
    const val POLL_MS = 1000L
    const val TRIMMED_NOTICE = "\r\n[older sign-in output was trimmed]\r\n"

    private val OSC = Regex("\u001b\\][^\u0007\u001b]*(?:\u0007|\u001b\\\\)")
    private val CSI = Regex("\u001b\\[[0-?]*[ -/]*[@-~]")
    private val ESC = Regex("\u001b[@-Z\\\\-_]")
    private val CONTROL = Regex("[\u0000-\u0008\u000b-\u001f\u007f]")
    private val URL = Regex("https://[^\\s\"'<>]+")
    private val TRAILING = Regex("[.,;)]+$")

    fun stateOpen(state: String): Boolean = state == "pending" || state == "running"

    fun stateText(state: String): String = when (state) {
        "pending" -> "Waiting for the runner to pick this up. It checks in about every 20 seconds."
        "running" -> "The runner is running the sign-in. Open the link below, approve it, and paste the code."
        "succeeded" -> "Signed in. The credential stays on the runner, which adds it to its accounts and makes it the active one."
        "failed" -> "The sign-in did not finish. Read the terminal above, then try again."
        "expired" -> "The sign-in ran out of time (10 minutes). Start it again when you are ready."
        "cancelled" -> "The sign-in was cancelled."
        else -> state
    }

    fun terminalText(raw: String): String {
        val stripped = raw.replace(OSC, "").replace(CSI, "").replace(ESC, "").replace("\r\n", "\n")
        return stripped.split("\n").joinToString("\n") { line ->
            val visible = line.split("\r").asReversed().firstOrNull { it.isNotEmpty() } ?: ""
            visible.replace(CONTROL, "")
        }
    }

    fun urls(text: String): List<String> {
        val seen = LinkedHashSet<String>()
        for (match in URL.findAll(text)) {
            val candidate = match.value.replace(TRAILING, "")
            val uri = runCatching { java.net.URI(candidate) }.getOrNull() ?: continue
            if (uri.scheme == "https" && !uri.host.isNullOrEmpty() && uri.userInfo == null) seen += candidate
        }
        return seen.toList()
    }

    fun inputValid(data: String): Boolean {
        if (data.isEmpty() || data.toByteArray(Charsets.UTF_8).size > MAX_INPUT_BYTES) return false
        for (ch in data) {
            if (ch == '\r' || ch == '\n' || ch == '\t') continue
            if (Character.isSurrogate(ch)) continue
            when (Character.getType(ch).toByte()) {
                Character.CONTROL, Character.FORMAT, Character.PRIVATE_USE, Character.UNASSIGNED -> return false
            }
        }
        return true
    }

    fun codeSubmission(code: String): String {
        val clean = code.replace("\r", "").replace("\n", "").trim()
        return if (clean.isEmpty()) "" else "$clean\r"
    }
}

class RunnerLoginSession(runner: String, provider: String) {
    enum class Start { Started, Attached, Failed }

    val runner: String = runner.trim().lowercase()
    val provider: String = provider.trim().lowercase()

    var id: String = ""
        private set
    var state: String = ""
        private set
    var message: String = ""
        private set
    var error: String = ""
        private set
    var afterSeq: Long = 0
        private set
    var more: Boolean = false
        private set
    private var failed = false
    private val output = ByteArrayOutputStream()

    val open: Boolean get() = !failed && id.isNotEmpty() && RunnerLoginText.stateOpen(state)

    val needsCancelOnClose: Boolean get() = id.isNotEmpty() && RunnerLoginText.stateOpen(state)

    val ended: Boolean get() = failed || (id.isNotEmpty() && state.isNotEmpty() && !RunnerLoginText.stateOpen(state))

    val screenText: String get() = RunnerLoginText.terminalText(String(output.toByteArray(), Charsets.UTF_8))

    val urls: List<String> get() = RunnerLoginText.urls(screenText)

    val statusText: String get() = when {
        error.isNotEmpty() -> error
        state.isEmpty() -> "Starting the sign-in…"
        message.isNotEmpty() -> RunnerLoginText.stateText(state) + " " + message
        else -> RunnerLoginText.stateText(state)
    }

    private fun applyPublic(body: JsonMap) {
        val next = body.str("state")
        if (next.isNotEmpty()) state = next
        if (body.containsKey("state")) message = body.str("message")
    }

    fun applyStartReply(status: Int, body: JsonMap): Start {
        val replyId = body.str("id").trim()
        val session = body.obj("session")
        if ((status == 200 || status == 201) && RunnerAttention.validLoginId(replyId)) {
            begin(replyId, session)
            return Start.Started
        }
        if (status == 409 && body.str("error") == "login_active" && RunnerAttention.validLoginId(replyId)) {
            begin(replyId, session)
            return Start.Attached
        }
        val code = body.str("error").trim()
        val text = body.str("message").trim().ifEmpty {
            when {
                status == 401 -> "Sign in to your ForkMesh account on this phone first."
                status == 403 || status == 404 -> "That runner is not one of yours. Only the account that added a runner can see it or sign its agent CLIs in."
                code.isNotEmpty() -> code
                else -> "The sign-in could not be started."
            }
        }
        markFailed(text)
        return Start.Failed
    }

    private fun begin(replyId: String, session: JsonMap) {
        id = replyId
        state = "pending"
        applyPublic(session)
        error = ""
        failed = false
    }

    fun attach(loginId: String) {
        if (!RunnerAttention.validLoginId(loginId)) return
        id = loginId
        state = "pending"
        error = ""
        failed = false
    }

    fun applyOutputReply(body: JsonMap): ByteArray {
        applyPublic(body)
        val ordered = sortedMapOf<Long, ByteArray>()
        for (chunk in body.maps("chunks")) {
            val seq = jsonLong(chunk["seq"])
            if (seq <= afterSeq) continue
            val decoded = runCatching { Base64.getDecoder().decode(chunk.str("data")) }.getOrNull() ?: continue
            ordered[seq] = decoded
        }
        val out = ByteArrayOutputStream()
        if (body.bool("truncated") && ordered.isNotEmpty()) out.write(RunnerLoginText.TRIMMED_NOTICE.toByteArray(Charsets.UTF_8))
        for ((seq, bytes) in ordered) {
            out.write(bytes)
            afterSeq = seq
        }
        val next = jsonLong(body["nextSeq"])
        if (next > afterSeq) afterSeq = next
        more = body.bool("more")
        val added = out.toByteArray()
        append(added)
        return added
    }

    private fun append(bytes: ByteArray) {
        if (bytes.isEmpty()) return
        output.write(bytes)
        if (output.size() > RunnerLoginText.MAX_OUTPUT_BYTES) {
            val all = output.toByteArray()
            output.reset()
            output.write(all, all.size - RunnerLoginText.MAX_OUTPUT_BYTES, RunnerLoginText.MAX_OUTPUT_BYTES)
        }
    }

    fun applyOutputStatus(status: Int) {
        when (status) {
            401 -> markFailed("The account session was refused; sign in again.")
            403 -> markFailed("This account may not read that sign-in.")
            404 -> markFailed("The sign-in session is gone.")
        }
    }

    fun applyCancelReply(body: JsonMap) {
        applyPublic(body)
        if (RunnerLoginText.stateOpen(state)) state = "cancelled"
    }

    fun markFailed(text: String) {
        failed = true
        error = text
    }
}
