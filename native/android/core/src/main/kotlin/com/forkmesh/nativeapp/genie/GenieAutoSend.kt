package com.forkmesh.nativeapp.genie

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.market.PackagesException
import java.time.Instant
import java.time.ZoneId

data class GenieAutoSendReceipt(
    val id: String,
    val organization: String,
    val contactName: String,
    val to: String,
    val fromEmail: String,
    val subject: String,
    val text: String,
    val via: String,
    val model: String,
    val sentToday: Int,
    val dailyLimit: Int,
) {
    val viaLabel: String get() = when (via) {
        "zoho" -> "Zoho Mail"
        "mailtrap" -> "ForkMesh mail"
        else -> via
    }

    companion object {
        fun of(json: JsonMap, pick: GenieOutreachPick): GenieAutoSendReceipt {
            val email = json.obj("email")
            return GenieAutoSendReceipt(
                id = email.str("id"),
                organization = pick.organization,
                contactName = pick.contact.displayName,
                to = email.str("to", pick.contact.email),
                fromEmail = json.str("fromEmail", email.str("fromEmail")),
                subject = json.str("subject", email.str("subject")),
                text = json.str("text", email.str("text")),
                via = json.str("via"),
                model = json.str("model"),
                sentToday = json.int("sentToday"),
                dailyLimit = json.int("dailyLimit"),
            )
        }
    }
}

enum class GenieAutoSendStage { Finding, Writing, Sending, Sent }

sealed class GenieAutoSendOutcome {
    data class Sent(val receipt: GenieAutoSendReceipt) : GenieAutoSendOutcome()
    data class Skip(val code: String) : GenieAutoSendOutcome()
    data class Pause(val reason: String) : GenieAutoSendOutcome()
}

data class GenieCountdown(val start: Double, val end: Double) {
    val duration: Double get() = end - start

    fun left(now: Double): Double = if (duration <= 0) 0.0 else ((end - now) / duration).coerceIn(0.0, 1.0)
}

class GenieAutoSendPace(private val store: PrefStore?, private val zone: () -> ZoneId = { ZoneId.systemDefault() }) {
    companion object {
        const val ON_KEY = "shell/auto_send"
        const val DAY_KEY = "shell/auto_send_day"
        const val COUNT_KEY = "shell/auto_send_count"
        const val LAST_KEY = "shell/auto_send_last"
        const val PER_DAY = 100
        const val EVERY = 24 * 60 * 60.0 / PER_DAY
    }

    var on: Boolean = store?.getBool(ON_KEY) ?: false
        private set
    var last: Double = store?.getDouble(LAST_KEY) ?: 0.0
        private set
    private var day: String = store?.getString(DAY_KEY).orEmpty()
    private var count: Int = store?.getDouble(COUNT_KEY)?.toInt() ?: 0

    fun setOn(value: Boolean) {
        on = value
        store?.putBool(ON_KEY, value)
    }

    fun sentToday(now: Double): Int = if (day == key(now)) count else 0

    fun due(now: Double): Boolean = on && sentToday(now) < PER_DAY && now - last >= EVERY

    fun wait(now: Double): Double {
        if (sentToday(now) >= PER_DAY) {
            val today = Instant.ofEpochMilli((now * 1000).toLong()).atZone(zone()).toLocalDate()
            val tomorrow = today.plusDays(1).atStartOfDay(zone()).toInstant().toEpochMilli() / 1000.0
            return tomorrow - now
        }
        return maxOf(0.0, EVERY - (now - last))
    }

    fun countdown(now: Double): GenieCountdown? {
        val wait = wait(now)
        if (!on || wait <= 0) return null
        return GenieCountdown(minOf(last, now), now + wait)
    }

    fun sent(now: Double) {
        val today = key(now)
        count = (if (day == today) count else 0) + 1
        day = today
        held(now)
        store?.putString(DAY_KEY, today)
        store?.putDouble(COUNT_KEY, count.toDouble())
    }

    fun held(now: Double) {
        last = now
        store?.putDouble(LAST_KEY, now)
    }

    private fun key(now: Double): String {
        val date = Instant.ofEpochMilli((now * 1000).toLong()).atZone(zone()).toLocalDate()
        return "${date.year}-${date.monthValue}-${date.dayOfMonth}"
    }
}

object GenieAutoSend {
    val skipped: Set<String> = setOf("contact_has_no_email", "contact_closed", "recently_emailed", "contact_not_found")
    const val UNWRITTEN =
        "Nothing could write the email: the server has no AI model, and no agent API key, on-device model or desktop running Claude Code or Codex gave a usable draft."
    const val NO_WRITER =
        "Nothing can write the email: the server has no AI model and this phone has no writer. Open ForkMesh on a desktop signed in to this account with Claude Code or Codex as its default agent, or save an agent API key in Settings."

    fun outcome(reply: ApiJsonResult, pick: GenieOutreachPick): GenieAutoSendOutcome {
        if (reply.ok) return GenieAutoSendOutcome.Sent(GenieAutoSendReceipt.of(reply.body, pick))
        val code = reply.body.str("error")
        val detail = reply.body.str("detail")
        if (code in skipped) return GenieAutoSendOutcome.Skip(code)
        val reason = when (code) {
            "daily_limit_reached" -> "The CRM's limit of ${reply.body.int("dailyLimit", GenieAutoSendPace.PER_DAY)} emails a day is reached."
            "outreach_forbidden" -> "Only owners, admins and the outreach team of ${pick.organization} can send outreach."
            "ai_unavailable", "draft_unusable" -> UNWRITTEN
            "rate_limited" -> "The AI model is busy. Trying again in a while."
            else -> detail.ifEmpty { PackagesException.sentence(code, reply.statusCode) }
        }
        return GenieAutoSendOutcome.Pause(reason)
    }

    fun parseReply(text: String): Pair<String, String>? {
        val marks = setOf('*', '#', '_', '"')
        fun clean(value: String) = value.trim { it.isWhitespace() || it in marks }
        val lines = text.trim().split("\n")
        val index = lines.indexOfFirst { clean(it).lowercase().startsWith("subject:") }
        if (index < 0) return null
        val subject = clean(clean(lines[index]).drop(8))
        val body = lines.drop(index + 1).joinToString("\n").trim()
        if (subject.isEmpty() || body.isEmpty()) return null
        return subject to body
    }

    fun write(prompt: String, writers: List<GenieOutreachWriter>): Triple<String, String, String>? {
        val messages = listOf(GenieMessage.user(prompt))
        for (writer in writers) {
            val text = GenieOutreach.attempt(writer, messages) ?: continue
            val draft = parseReply(text) ?: continue
            return Triple(draft.first, draft.second, writer.name)
        }
        return null
    }
}
