package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.list
import com.forkmesh.nativeapp.json.long
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.optBool
import com.forkmesh.nativeapp.json.str
import java.math.BigDecimal
import java.math.RoundingMode
import java.time.Instant
import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.random.Random

enum class SurveyQuestionType(val wire: String, val title: String, val icon: String) {
    SINGLE("single", "Single choice", "radio_button_checked"),
    MULTIPLE("multiple", "Multiple choice", "check_box"),
    TEXT("text", "Short text", "text_fields"),
    PARAGRAPH("paragraph", "Paragraph", "subject"),
    RATING("rating", "Rating", "star");

    val isChoice: Boolean get() = this == SINGLE || this == MULTIPLE

    val isText: Boolean get() = this == TEXT || this == PARAGRAPH

    companion object {
        fun of(wire: String?): SurveyQuestionType? = entries.firstOrNull { it.wire == wire?.trim() }
    }
}

object SurveyLimits {
    const val QUESTIONS = 50
    const val TITLE = 300
    const val HELP = 1000
    const val OPTION = 200
    const val MIN_OPTIONS = 2
    const val MAX_OPTIONS = 30
    const val MIN_SCALE = 3
    const val MAX_SCALE = 10
    const val DEFAULT_SCALE = 5
    const val MAX_RESPONSES = 10_000
    const val RECENT_ANSWERS = 20

    fun isQuestionId(value: String): Boolean = value.length == 8 && value.all { it in '0'..'9' || it in 'a'..'f' }

    fun isResponseId(value: String): Boolean = value.isNotEmpty() && value.length <= 64 && value.all { it in '0'..'9' || it in 'a'..'z' }

    fun isPublicToken(value: String): Boolean {
        val parts = value.split('.')
        return parts.size == 2 && PackagesText.isHexId(parts[0]) && parts[1].length in 8..64 && parts[1].all { it in '0'..'9' || it in 'a'..'f' }
    }
}

object SurveyDates {
    private val INPUT: DateTimeFormatter = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm", Locale.ENGLISH)
    private val DAY: DateTimeFormatter = DateTimeFormatter.ISO_LOCAL_DATE
    private val LABEL: DateTimeFormatter = DateTimeFormatter.ofPattern("d MMM yyyy, HH:mm", Locale.ENGLISH)

    fun input(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String =
        if (ms <= 0) "" else Instant.ofEpochMilli(ms).atZone(zone).toLocalDateTime().format(INPUT)

    fun parse(text: String, zone: ZoneId = ZoneId.systemDefault()): Long? {
        val trimmed = text.trim().replace('T', ' ')
        if (trimmed.isEmpty()) return null
        runCatching { return LocalDateTime.parse(trimmed, INPUT).atZone(zone).toInstant().toEpochMilli() }
        runCatching { return LocalDate.parse(trimmed, DAY).atStartOfDay(zone).toInstant().toEpochMilli() }
        return null
    }

    fun label(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String =
        if (ms <= 0) "—" else Instant.ofEpochMilli(ms).atZone(zone).toLocalDateTime().format(LABEL)
}

data class SurveySettings(
    val oneResponsePerPerson: Boolean = false,
    val opensAt: Long = 0,
    val closesAt: Long = 0,
    val maxResponses: Int = 0,
    val notifyOnResponse: Boolean = true,
    val notifyOnVisit: Boolean = true,
    val shuffleOptions: Boolean = false,
) {
    val body: Map<String, Any?>
        get() = linkedMapOf(
            "oneResponsePerPerson" to oneResponsePerPerson,
            "opensAt" to maxOf(0L, opensAt),
            "closesAt" to maxOf(0L, closesAt),
            "maxResponses" to maxResponses.coerceIn(0, SurveyLimits.MAX_RESPONSES),
            "notifyOnResponse" to notifyOnResponse,
            "notifyOnVisit" to notifyOnVisit,
            "shuffleOptions" to shuffleOptions,
        )

    companion object {
        fun fromJson(json: JsonMap) = SurveySettings(
            oneResponsePerPerson = json.bool("oneResponsePerPerson"),
            opensAt = maxOf(0L, json.long("opensAt")),
            closesAt = maxOf(0L, json.long("closesAt")),
            maxResponses = json.int("maxResponses").coerceIn(0, SurveyLimits.MAX_RESPONSES),
            notifyOnResponse = json.optBool("notifyOnResponse") ?: true,
            notifyOnVisit = json.optBool("notifyOnVisit") ?: true,
            shuffleOptions = json.bool("shuffleOptions"),
        )
    }
}

data class SurveyQuestion(
    val id: String,
    val type: SurveyQuestionType = SurveyQuestionType.TEXT,
    val title: String = "",
    val help: String = "",
    val required: Boolean = false,
    val options: List<String> = emptyList(),
    val scale: Int = 0,
) {
    companion object {
        fun fromJson(json: JsonMap): SurveyQuestion {
            val type = SurveyQuestionType.of(json.str("type")) ?: SurveyQuestionType.TEXT
            val scale = json.int("scale")
            return SurveyQuestion(
                id = json.str("id"),
                type = type,
                title = json.str("title"),
                help = json.str("help"),
                required = json.bool("required"),
                options = if (type.isChoice) json.list("options").map(::jsonText).filter { it.isNotBlank() } else emptyList(),
                scale = if (type == SurveyQuestionType.RATING) {
                    if (scale in SurveyLimits.MIN_SCALE..SurveyLimits.MAX_SCALE) scale else SurveyLimits.DEFAULT_SCALE
                } else {
                    0
                },
            )
        }
    }
}

data class SurveyOptionCount(val label: String, val count: Int)

data class SurveyResult(
    val id: String,
    val type: SurveyQuestionType = SurveyQuestionType.TEXT,
    val title: String = "",
    val answered: Int = 0,
    val skipped: Int = 0,
    val options: List<SurveyOptionCount> = emptyList(),
    val average: Double = 0.0,
    val distribution: List<Int> = emptyList(),
    val answers: List<String> = emptyList(),
) {
    val scale: Int get() = distribution.size

    val answeredText: String
        get() = listOf(SurveyFormat.plural(answered, "answer"), if (skipped > 0) "$skipped skipped" else "").filter { it.isNotEmpty() }.joinToString(" · ")

    val averageText: String get() = if (answered > 0 && scale > 0) "${SurveyFormat.oneDecimal(average)} of $scale" else "—"

    fun share(count: Int): Float = if (answered <= 0) 0f else (count.toFloat() / answered).coerceIn(0f, 1f)

    fun percent(count: Int): String = SurveyFormat.percent(count, answered)

    fun optionLine(option: SurveyOptionCount): String = "${option.count} · ${percent(option.count)}"

    val distributionPeak: Int get() = distribution.maxOrNull() ?: 0

    fun distributionShare(count: Int): Float {
        val peak = distributionPeak
        return if (peak <= 0) 0f else (count.toFloat() / peak).coerceIn(0f, 1f)
    }

    val ratingRows: List<Pair<Int, Int>> get() = distribution.mapIndexed { index, count -> (index + 1) to count }

    companion object {
        fun fromJson(json: JsonMap) = SurveyResult(
            id = json.str("id"),
            type = SurveyQuestionType.of(json.str("type")) ?: SurveyQuestionType.TEXT,
            title = json.str("title"),
            answered = maxOf(0, json.int("answered")),
            skipped = maxOf(0, json.int("skipped")),
            options = json.maps("options").map { SurveyOptionCount(it.str("label"), maxOf(0, it.int("count"))) },
            average = json.double("average").takeIf { it.isFinite() } ?: 0.0,
            distribution = json.list("distribution").map { maxOf(0L, jsonLong(it)).toInt() },
            answers = json.list("answers").map(::jsonText).filter { it.isNotBlank() }.take(SurveyLimits.RECENT_ANSWERS),
        )
    }
}

enum class SurveyAction(val title: String, val icon: String) {
    EDIT("Edit", "edit"),
    PUBLISH("Open survey", "play_arrow"),
    CLOSE("Close survey", "stop"),
    REOPEN("Reopen", "replay"),
    DUPLICATE("Duplicate", "content_copy"),
    SHARE("Share link", "share"),
    EXPORT("Export CSV", "table_view"),
    DELETE("Delete", "delete");

    val wire: String?
        get() = when (this) {
            PUBLISH, REOPEN -> "open"
            CLOSE -> "close"
            DUPLICATE -> "duplicate"
            else -> null
        }
}

enum class SurveyFilter(val wire: String, val title: String) {
    ALL("", "All"), OPEN("open", "Open"), SCHEDULED("scheduled", "Scheduled"), DRAFT("draft", "Drafts"), CLOSED("closed", "Closed");

    companion object {
        fun of(wire: String?): SurveyFilter = entries.firstOrNull { it.wire == wire } ?: ALL
    }
}

data class Survey(
    val id: String,
    val organization: String = "",
    val title: String = "",
    val description: String = "",
    val thankYou: String = "",
    val status: String = "draft",
    val displayStatus: String = "",
    val settings: SurveySettings = SurveySettings(),
    val questionCount: Int = 0,
    val responseCount: Int = 0,
    val lastResponseAt: Long = 0,
    val publishedAt: Long = 0,
    val closedAt: Long = 0,
    val createdBy: String = "",
    val updatedBy: String = "",
    val createdAt: Long = 0,
    val updatedAt: Long = 0,
    val canDelete: Boolean = false,
    val publicUrl: String = "",
    val questions: List<SurveyQuestion>? = null,
    val results: List<SurveyResult>? = null,
    val activity: PageActivity = PageActivity(),
) {
    val state: String get() = displayStatus.ifEmpty { status }

    val statusLabel: String get() = STATUS_LABELS[state] ?: CrmStageLabel.title(state)

    val displayTitle: String get() = title.ifBlank { "Untitled survey" }

    val isDraft: Boolean get() = status == "draft"

    val acceptsAnswers: Boolean get() = state == "open"

    val canPublish: Boolean get() = status == "draft"

    val canClose: Boolean get() = status == "open"

    val canReopen: Boolean get() = status == "closed"

    val canShare: Boolean get() = publicUrl.isNotEmpty()

    val canExport: Boolean get() = !isDraft || responseCount > 0

    val publishProblem: String? get() = if (canPublish && questionCount <= 0 && questions.isNullOrEmpty()) SurveyErrors.sentence("survey_has_no_questions") else null

    val actions: List<SurveyAction>
        get() = buildList {
            add(SurveyAction.EDIT)
            if (canPublish) add(SurveyAction.PUBLISH)
            if (canClose) add(SurveyAction.CLOSE)
            if (canReopen) add(SurveyAction.REOPEN)
            add(SurveyAction.DUPLICATE)
            if (canShare) add(SurveyAction.SHARE)
            if (canExport) add(SurveyAction.EXPORT)
            if (canDelete) add(SurveyAction.DELETE)
        }

    val countsLine: String
        get() = listOf(SurveyFormat.plural(questionCount, "question"), SurveyFormat.plural(responseCount, "response")).joinToString(" · ")

    val scheduleLine: String
        get() = listOf(
            if (settings.opensAt > 0) "Opens ${SurveyDates.label(settings.opensAt)}" else "",
            if (settings.closesAt > 0) "Closes ${SurveyDates.label(settings.closesAt)}" else "",
            if (settings.maxResponses > 0) "Up to ${SurveyFormat.plural(settings.maxResponses, "response")}" else "",
        ).filter { it.isNotEmpty() }.joinToString(" · ")

    val settingsLine: String
        get() = listOf(
            if (settings.oneResponsePerPerson) "One response per person" else "",
            if (settings.shuffleOptions) "Options shuffled" else "",
            if (settings.notifyOnResponse) "Pings on new responses" else "No Pings",
        ).filter { it.isNotEmpty() }.joinToString(" · ")

    fun question(id: String): SurveyQuestion? = questions?.firstOrNull { it.id == id }

    fun matches(query: String, filter: SurveyFilter): Boolean {
        val passes = filter == SurveyFilter.ALL || state == filter.wire
        return passes && PackagesText.matches(query, title, description, createdBy)
    }

    companion object {
        val STATUS_LABELS = mapOf("draft" to "Draft", "open" to "Open", "scheduled" to "Scheduled", "closed" to "Closed")

        fun fromJson(json: JsonMap): Survey = Survey(
            id = json.str("id"),
            organization = json.str("organization"),
            title = json.str("title"),
            description = json.str("description"),
            thankYou = json.str("thankYou"),
            status = json.str("status", "draft").ifEmpty { "draft" },
            displayStatus = json.str("displayStatus"),
            settings = SurveySettings.fromJson(json.obj("settings")),
            questionCount = maxOf(0, json.int("questionCount", json.list("questions").size)),
            responseCount = maxOf(0, json.int("responseCount")),
            lastResponseAt = json.long("lastResponseAt"),
            publishedAt = json.long("publishedAt"),
            closedAt = json.long("closedAt"),
            createdBy = json.str("createdBy"),
            updatedBy = json.str("updatedBy"),
            createdAt = json.long("createdAt"),
            updatedAt = json.long("updatedAt"),
            canDelete = json.bool("canDelete"),
            publicUrl = json.str("publicUrl").trim().takeIf { it.startsWith("https://") && it.none(Char::isWhitespace) }.orEmpty(),
            questions = if (json.containsKey("questions")) json.maps("questions").map(SurveyQuestion::fromJson) else null,
            results = if (json.containsKey("results")) json.maps("results").map(SurveyResult::fromJson) else null,
            activity = PageActivity.fromJson(json.obj("activity")),
        )
    }
}

data class SurveySummary(
    val surveys: Int = 0,
    val open: Int = 0,
    val drafts: Int = 0,
    val closed: Int = 0,
    val responses: Int = 0,
    val responsesLast7Days: Int = 0,
) {
    val surveysDetail: String get() = "$open open · $drafts ${if (drafts == 1) "draft" else "drafts"} · $closed closed"

    val weekDetail: String get() = if (responsesLast7Days > 0) "$responsesLast7Days in the last 7 days" else "None in the last 7 days"

    companion object {
        fun fromJson(json: JsonMap) = SurveySummary(
            surveys = maxOf(0, json.int("surveys")),
            open = maxOf(0, json.int("open")),
            drafts = maxOf(0, json.int("drafts")),
            closed = maxOf(0, json.int("closed")),
            responses = maxOf(0, json.int("responses")),
            responsesLast7Days = maxOf(0, json.int("responsesLast7Days")),
        )
    }
}

data class SurveysOverview(
    val organization: String,
    val role: String = "member",
    val canManagePlugin: Boolean = false,
    val surveys: List<Survey> = emptyList(),
    val summary: SurveySummary = SurveySummary(),
) {
    val canManage: Boolean get() = canManagePlugin || role == "owner" || role == "admin"

    fun survey(id: String): Survey? = surveys.firstOrNull { it.id == id }

    fun filtered(query: String, filter: SurveyFilter): List<Survey> = surveys.filter { it.matches(query, filter) }

    fun count(filter: SurveyFilter): Int = surveys.count { it.matches("", filter) }

    companion object {
        fun fromJson(json: JsonMap) = SurveysOverview(
            organization = json.str("organization"),
            role = json.str("role", "member"),
            canManagePlugin = json.bool("canManagePlugin"),
            surveys = json.maps("surveys").map(Survey::fromJson).filter { PackagesText.isHexId(it.id) },
            summary = SurveySummary.fromJson(json.obj("summary")),
        )
    }
}

data class SurveyResponse(val id: String, val submittedAt: Long = 0, val answers: Map<String, Any?> = emptyMap()) {
    fun answer(question: SurveyQuestion): String = SurveyFormat.answer(answers[question.id], question)

    fun lines(questions: List<SurveyQuestion>): List<Pair<String, String>> =
        questions.map { it.title.ifBlank { "Untitled question" } to answer(it) }

    val answeredCount: Int get() = answers.values.count { !SurveyFormat.isEmptyAnswer(it) }

    companion object {
        fun fromJson(json: JsonMap) = SurveyResponse(json.str("id"), json.long("submittedAt"), json.obj("answers"))

        fun list(json: JsonMap): List<SurveyResponse> = json.maps("responses").map(::fromJson).filter { SurveyLimits.isResponseId(it.id) }
    }
}

data class SurveyExport(val filename: String, val csv: String) {
    val safeFilename: String
        get() {
            val cleaned = filename.trim().map { if (it.isLetterOrDigit() && it.code < 128 || it in "._-") it else '-' }.joinToString("")
                .trim('-', '.').take(120)
            val base = cleaned.ifEmpty { "survey" }
            return if (base.lowercase().endsWith(".csv")) base else "$base.csv"
        }

    val rows: Int get() = maxOf(0, csv.lineSequence().count { it.isNotEmpty() } - 1)

    companion object {
        fun fromJson(json: JsonMap) = SurveyExport(json.str("filename", "survey.csv"), json.str("csv"))
    }
}

data class PublicSurvey(
    val title: String,
    val description: String = "",
    val questions: List<SurveyQuestion> = emptyList(),
    val status: String = "",
    val thankYou: String = "",
    val oneResponsePerPerson: Boolean = false,
    val shuffleOptions: Boolean = false,
    val organization: String = "",
) {
    val acceptsAnswers: Boolean get() = status == "open"

    companion object {
        fun fromJson(json: JsonMap) = PublicSurvey(
            json.str("title"), json.str("description"), json.maps("questions").map(SurveyQuestion::fromJson), json.str("status"),
            json.str("thankYou"), json.bool("oneResponsePerPerson"), json.bool("shuffleOptions"), json.str("organization"),
        )
    }
}

object SurveyFormat {
    fun plural(count: Int, one: String, many: String = one + "s"): String = "$count ${if (count == 1) one else many}"

    fun oneDecimal(value: Double): String =
        if (!value.isFinite()) "0" else BigDecimal.valueOf(value).setScale(1, RoundingMode.HALF_UP).toPlainString()

    fun percent(count: Int, of: Int): String = if (of <= 0) "0%" else "${(count * 100.0 / of).let { Math.round(it) }}%"

    fun isEmptyAnswer(value: Any?): Boolean = when (value) {
        null -> true
        is String -> value.isBlank()
        is List<*> -> value.all { it == null || jsonText(it).isBlank() }
        else -> false
    }

    fun answer(value: Any?, question: SurveyQuestion? = null): String {
        if (isEmptyAnswer(value)) return ""
        val text = when (value) {
            is List<*> -> value.filterNotNull().map(::jsonText).filter { it.isNotBlank() }.joinToString("; ")
            is Number -> number(value)
            else -> jsonText(value)
        }
        if (question?.type == SurveyQuestionType.RATING && question.scale > 0 && value is Number) return "$text of ${question.scale}"
        return text
    }

    private fun number(value: Number): String {
        val double = value.toDouble()
        return if (double == Math.floor(double) && double.isFinite()) double.toLong().toString() else BigDecimal.valueOf(double).stripTrailingZeros().toPlainString()
    }
}

data class SurveyQuestionDraft(
    val id: String,
    val type: SurveyQuestionType = SurveyQuestionType.SINGLE,
    val title: String = "",
    val help: String = "",
    val required: Boolean = false,
    val options: List<String> = listOf("", ""),
    val scale: Int = SurveyLimits.DEFAULT_SCALE,
) {
    val cleanOptions: List<String>
        get() = options.map { it.trim().take(SurveyLimits.OPTION) }.filter { it.isNotEmpty() }.distinct()

    val isBlank: Boolean get() = title.isBlank() && help.isBlank() && (!type.isChoice || cleanOptions.isEmpty())

    fun problem(number: Int): String? = when {
        isBlank -> null
        title.isBlank() -> "Give question $number a title."
        title.trim().length > SurveyLimits.TITLE -> "Question $number's title is longer than ${SurveyLimits.TITLE} characters."
        help.trim().length > SurveyLimits.HELP -> "Question $number's help text is longer than ${SurveyLimits.HELP} characters."
        type.isChoice && cleanOptions.size < SurveyLimits.MIN_OPTIONS -> "Question $number needs at least ${SurveyLimits.MIN_OPTIONS} different options."
        type.isChoice && cleanOptions.size > SurveyLimits.MAX_OPTIONS -> "Question $number can have at most ${SurveyLimits.MAX_OPTIONS} options."
        type == SurveyQuestionType.RATING && scale !in SurveyLimits.MIN_SCALE..SurveyLimits.MAX_SCALE ->
            "Question $number's rating scale runs from ${SurveyLimits.MIN_SCALE} to ${SurveyLimits.MAX_SCALE}."
        else -> null
    }

    fun withType(next: SurveyQuestionType): SurveyQuestionDraft = copy(
        type = next,
        options = if (next.isChoice && options.size < SurveyLimits.MIN_OPTIONS) options + List(SurveyLimits.MIN_OPTIONS - options.size) { "" } else options,
        scale = if (scale in SurveyLimits.MIN_SCALE..SurveyLimits.MAX_SCALE) scale else SurveyLimits.DEFAULT_SCALE,
    )

    fun option(index: Int, text: String): SurveyQuestionDraft = copy(options = options.mapIndexed { at, old -> if (at == index) text else old })

    fun addOption(): SurveyQuestionDraft = if (options.size >= SurveyLimits.MAX_OPTIONS) this else copy(options = options + "")

    fun removeOption(index: Int): SurveyQuestionDraft = copy(options = options.filterIndexed { at, _ -> at != index })

    fun stepScale(delta: Int): SurveyQuestionDraft = copy(scale = (scale + delta).coerceIn(SurveyLimits.MIN_SCALE, SurveyLimits.MAX_SCALE))

    val body: Map<String, Any?>
        get() {
            val body = linkedMapOf<String, Any?>()
            if (SurveyLimits.isQuestionId(id)) body["id"] = id
            body["type"] = type.wire
            body["title"] = title.trim()
            body["help"] = help.trim()
            body["required"] = required
            body["options"] = if (type.isChoice) cleanOptions else emptyList<String>()
            body["scale"] = if (type == SurveyQuestionType.RATING) scale.coerceIn(SurveyLimits.MIN_SCALE, SurveyLimits.MAX_SCALE) else 0
            return body
        }

    companion object {
        fun newId(random: Random = Random.Default): String = buildString { repeat(8) { append("0123456789abcdef"[random.nextInt(16)]) } }

        fun blank(type: SurveyQuestionType = SurveyQuestionType.SINGLE, random: Random = Random.Default): SurveyQuestionDraft =
            SurveyQuestionDraft(id = newId(random)).withType(type).let { if (type.isChoice) it else it.copy(options = emptyList()) }

        fun of(question: SurveyQuestion, random: Random = Random.Default) = SurveyQuestionDraft(
            id = question.id.takeIf(SurveyLimits::isQuestionId) ?: newId(random),
            type = question.type,
            title = question.title,
            help = question.help,
            required = question.required,
            options = if (question.type.isChoice) question.options else emptyList(),
            scale = if (question.type == SurveyQuestionType.RATING) question.scale else SurveyLimits.DEFAULT_SCALE,
        )
    }
}

data class SurveyDraft(
    val id: String = "",
    val title: String = "",
    val description: String = "",
    val thankYou: String = "",
    val oneResponsePerPerson: Boolean = false,
    val notifyOnResponse: Boolean = true,
    val notifyOnVisit: Boolean = true,
    val shuffleOptions: Boolean = false,
    val opens: String = "",
    val closes: String = "",
    val maxResponses: String = "",
    val questions: List<SurveyQuestionDraft> = emptyList(),
) {
    val isNew: Boolean get() = id.isEmpty()

    val keptQuestions: List<SurveyQuestionDraft> get() = questions.filter { !it.isBlank }

    val canAddQuestion: Boolean get() = questions.size < SurveyLimits.QUESTIONS

    fun settings(zone: ZoneId = ZoneId.systemDefault()) = SurveySettings(
        oneResponsePerPerson = oneResponsePerPerson,
        opensAt = SurveyDates.parse(opens, zone) ?: 0L,
        closesAt = SurveyDates.parse(closes, zone) ?: 0L,
        maxResponses = maxResponses.trim().toIntOrNull()?.coerceIn(0, SurveyLimits.MAX_RESPONSES) ?: 0,
        notifyOnResponse = notifyOnResponse,
        notifyOnVisit = notifyOnVisit,
        shuffleOptions = shuffleOptions,
    )

    fun problem(zone: ZoneId = ZoneId.systemDefault()): String? {
        if (title.isBlank()) return "Give the survey a title."
        if (keptQuestions.size > SurveyLimits.QUESTIONS) return "A survey holds up to ${SurveyLimits.QUESTIONS} questions."
        questions.forEachIndexed { index, question -> question.problem(index + 1)?.let { return it } }
        val opensAt = SurveyDates.parse(opens, zone)
        val closesAt = SurveyDates.parse(closes, zone)
        if (opens.isNotBlank() && opensAt == null) return "Write the opening time as YYYY-MM-DD HH:MM."
        if (closes.isNotBlank() && closesAt == null) return "Write the closing time as YYYY-MM-DD HH:MM."
        if (opensAt != null && closesAt != null && closesAt <= opensAt) return "The survey must close after it opens."
        val limit = maxResponses.trim()
        if (limit.isNotEmpty() && (limit.toIntOrNull() ?: -1) !in 0..SurveyLimits.MAX_RESPONSES) {
            return "The response limit is a number from 0 to 10,000; 0 means no limit."
        }
        return null
    }

    fun question(index: Int, change: (SurveyQuestionDraft) -> SurveyQuestionDraft): SurveyDraft =
        copy(questions = questions.mapIndexed { at, question -> if (at == index) change(question) else question })

    fun addQuestion(type: SurveyQuestionType = SurveyQuestionType.SINGLE, random: Random = Random.Default): SurveyDraft =
        if (!canAddQuestion) this else copy(questions = questions + SurveyQuestionDraft.blank(type, random))

    fun removeQuestion(index: Int): SurveyDraft = copy(questions = questions.filterIndexed { at, _ -> at != index })

    fun moveQuestion(index: Int, delta: Int): SurveyDraft {
        val target = index + delta
        if (index !in questions.indices || target !in questions.indices) return this
        val list = questions.toMutableList()
        val moved = list.removeAt(index)
        list.add(target, moved)
        return copy(questions = list)
    }

    fun body(zone: ZoneId = ZoneId.systemDefault()): Map<String, Any?> = linkedMapOf(
        "title" to title.trim(),
        "description" to description.trim(),
        "thankYou" to thankYou.trim(),
        "questions" to keptQuestions.map { it.body },
        "settings" to settings(zone).body,
    )

    companion object {
        fun blank(random: Random = Random.Default) = SurveyDraft(questions = listOf(SurveyQuestionDraft.blank(SurveyQuestionType.SINGLE, random)))

        fun of(survey: Survey, zone: ZoneId = ZoneId.systemDefault(), random: Random = Random.Default) = SurveyDraft(
            id = survey.id,
            title = survey.title,
            description = survey.description,
            thankYou = survey.thankYou,
            oneResponsePerPerson = survey.settings.oneResponsePerPerson,
            notifyOnResponse = survey.settings.notifyOnResponse,
            notifyOnVisit = survey.settings.notifyOnVisit,
            shuffleOptions = survey.settings.shuffleOptions,
            opens = SurveyDates.input(survey.settings.opensAt, zone),
            closes = SurveyDates.input(survey.settings.closesAt, zone),
            maxResponses = if (survey.settings.maxResponses > 0) survey.settings.maxResponses.toString() else "",
            questions = survey.questions.orEmpty().map { SurveyQuestionDraft.of(it, random) },
        )
    }
}

object SurveyErrors {
    const val PACKAGE_ID = "surveys"

    fun sentence(code: String): String? = when (code) {
        "plugin_not_installed" -> "Surveys is not installed for that organization yet."
        "survey_not_found" -> "That survey was removed or you no longer have access."
        "survey_delete_forbidden" -> "Only the survey's creator or an owner or admin of the organization can delete it."
        "survey_has_no_questions" -> "Add at least one question before opening the survey."
        "invalid_question_options" -> "Choice questions need at least 2 different options."
        "survey_closed" -> "This survey is not taking answers right now."
        "already_responded" -> "You have already answered this survey."
        "too_many_responses" -> "Too many responses from this connection. Try again in an hour."
        "invalid_answer" -> "One of the answers does not fit its question."
        "answer_required" -> "Answer every required question."
        "survey_close_date_passed" -> "Its closing time has passed. Change it before reopening."
        "survey_response_limit_reached" -> "It reached its response limit. Raise the limit before reopening."
        "survey_not_open" -> "Only an open survey can be closed."
        "survey_catalog_full" -> "This organization already has 500 surveys. Delete some first."
        "invalid_question_type" -> "A question has a type this version does not know."
        "too_many_questions" -> "A survey can have at most 50 questions."
        "invalid_schedule" -> "The closing time must be after the opening time."
        else -> null
    }

    fun describe(error: Throwable): String {
        if (error is PackagesException) sentence(error.code)?.let { return it }
        return PackagesException.describe(error)
    }

    fun code(error: Throwable): String = (error as? PackagesException)?.code.orEmpty()

    fun status(error: Throwable): Int = (error as? PackagesException)?.status ?: 0
}

private fun PackagesClient.surveyPath(organization: String, suffix: String = ""): String = "/api/surveys/" + segment(organization) + suffix

private fun PackagesClient.surveyItem(organization: String, id: String, suffix: String = ""): String = surveyPath(organization, "/items/" + item(id) + suffix)

private fun publicSurveyPath(token: String): String {
    if (!SurveyLimits.isPublicToken(token)) throw PackagesException("survey_not_found", 404)
    return "/api/surveys/-/public/$token"
}

fun PackagesClient.surveyOrganizations(): List<PackageOrganization> = PackageOrganization.list(call("GET", "/api/surveys"))

fun PackagesClient.surveys(organization: String): SurveysOverview = SurveysOverview.fromJson(call("GET", surveyPath(organization)))

fun PackagesClient.survey(organization: String, id: String): Survey = Survey.fromJson(call("GET", surveyItem(organization, id)).obj("survey"))

fun PackagesClient.saveSurvey(organization: String, draft: SurveyDraft, zone: ZoneId = ZoneId.systemDefault()): Survey {
    val body = draft.body(zone)
    if (draft.isNew) return Survey.fromJson(call("POST", surveyPath(organization, "/items"), body = body).obj("survey"))
    return Survey.fromJson(call("PATCH", surveyItem(organization, draft.id), body = body).obj("survey"))
}

fun PackagesClient.deleteSurvey(organization: String, id: String) {
    call("DELETE", surveyItem(organization, id), body = emptyMap())
}

fun PackagesClient.surveyAction(organization: String, id: String, action: SurveyAction): Survey {
    val wire = action.wire ?: throw PackagesException("invalid_action")
    return Survey.fromJson(call("POST", surveyItem(organization, id, "/$wire"), body = emptyMap()).obj("survey"))
}

fun PackagesClient.surveyResponses(organization: String, id: String): List<SurveyResponse> =
    SurveyResponse.list(call("GET", surveyItem(organization, id, "/responses")))

fun PackagesClient.deleteSurveyResponse(organization: String, id: String, responseId: String): Survey? {
    if (!SurveyLimits.isResponseId(responseId)) throw PackagesException("not_found", 404)
    val reply = call("DELETE", surveyItem(organization, id, "/responses/$responseId"), body = emptyMap())
    return reply.obj("survey").takeIf { it.isNotEmpty() }?.let(Survey::fromJson)
}

fun PackagesClient.exportSurvey(organization: String, id: String): SurveyExport = SurveyExport.fromJson(call("GET", surveyItem(organization, id, "/export")))

fun PackagesClient.publicSurvey(token: String): PublicSurvey = PublicSurvey.fromJson(call("GET", publicSurveyPath(token)).obj("survey"))

fun PackagesClient.submitSurvey(token: String, answers: Map<String, Any?>): String =
    call("POST", publicSurveyPath(token), body = linkedMapOf("answers" to answers)).str("thankYou")

class SurveysModel(val client: PackagesClient, var organization: String = "") {
    var organizations: List<PackageOrganization> = emptyList()
        private set
    var overview: SurveysOverview? = null
        private set
    var errorText: String = ""

    val selected: PackageOrganization? get() = organizations.firstOrNull { it.name == organization }

    val needsInstall: Boolean get() = selected?.installed == false

    val canInstall: Boolean get() = selected?.canManage == true

    val canManage: Boolean get() = overview?.canManage ?: (selected?.canManage == true)

    fun refresh(preferred: String? = null) {
        try {
            organizations = client.surveyOrganizations()
            errorText = ""
        } catch (thrown: Exception) {
            errorText = SurveyErrors.describe(thrown)
            return
        }
        val wanted = preferred?.takeIf { name -> organizations.any { it.name == name } }
        organization = wanted
            ?: organization.takeIf { name -> organizations.any { it.name == name } }
            ?: organizations.firstOrNull { it.installed }?.name
            ?: organizations.firstOrNull()?.name
            ?: ""
        load()
    }

    fun choose(name: String) {
        if (name == organization) return
        organization = name
        overview = null
        load()
    }

    fun load() {
        if (!PackagesText.isOrgName(organization) || needsInstall) {
            overview = null
            return
        }
        try {
            overview = client.surveys(organization)
            errorText = ""
        } catch (thrown: Exception) {
            if (SurveyErrors.code(thrown) == "plugin_not_installed") {
                markInstalled(false)
                overview = null
                errorText = ""
            } else {
                errorText = SurveyErrors.describe(thrown)
            }
        }
    }

    fun install(): Boolean = try {
        client.install(SurveyErrors.PACKAGE_ID, organization)
        markInstalled(true)
        errorText = ""
        load()
        true
    } catch (thrown: Exception) {
        errorText = SurveyErrors.describe(thrown)
        false
    }

    fun ensureOverview() {
        if (overview == null) {
            if (organizations.isEmpty() && organization.isEmpty()) refresh() else load()
        }
    }

    fun adopt(survey: Survey) {
        val current = overview ?: return
        val listed = survey.copy(questions = null, results = null, activity = PageActivity())
        val surveys = if (current.surveys.any { it.id == survey.id }) {
            current.surveys.map { if (it.id == survey.id) listed else it }
        } else {
            listOf(listed) + current.surveys
        }
        overview = current.copy(surveys = surveys)
    }

    fun forget(id: String) {
        val current = overview ?: return
        overview = current.copy(surveys = current.surveys.filter { it.id != id })
    }

    fun locate(surveyId: String): Survey? {
        if (!PackagesText.isHexId(surveyId)) return null
        overview?.survey(surveyId)?.let { return it }
        for (org in organizations.filter { it.installed && it.name != organization }) {
            val found = runCatching { client.survey(org.name, surveyId) }.getOrNull() ?: continue
            organization = org.name
            load()
            return found
        }
        return null
    }

    private fun markInstalled(installed: Boolean) {
        organizations = organizations.map { if (it.name == organization) it.copy(installed = installed) else it }
    }
}
