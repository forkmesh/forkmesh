package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import com.forkmesh.nativeapp.model.Issue

data class IssueEdit(val type: String, val fields: Map<String, Any?> = emptyMap()) {
    companion object {
        const val MAX_TITLE = 240
        const val MAX_LABELS = 30
        const val MAX_ASSIGNEES = 20
        const val MAX_LABEL_LENGTH = 50

        fun title(title: String) = IssueEdit("title", linkedMapOf("title" to title.trim().take(MAX_TITLE)))

        fun status(open: Boolean) = IssueEdit("status", linkedMapOf("status" to if (open) "open" else "closed"))

        fun labels(labels: List<String>) =
            IssueEdit("labels", linkedMapOf("labels" to IssueFields.cleanList(labels, MAX_LABELS, MAX_LABEL_LENGTH)))

        fun assignees(names: List<String>) =
            IssueEdit("assignees", linkedMapOf("assignees" to IssueFields.cleanList(names.map { it.lowercase() }, MAX_ASSIGNEES, 64)))

        fun priority(value: Int) = IssueEdit("priority", linkedMapOf("priority" to IssueFields.clampPriority(value).toLong()))

        fun progress(value: Int) = IssueEdit("progress", linkedMapOf("progress" to value.coerceIn(0, 100).toLong()))

        fun milestone(name: String) = IssueEdit("milestone", linkedMapOf("milestone" to name.trim()))

        fun dates(startMs: Long, endMs: Long) =
            IssueEdit("dates", linkedMapOf("startDate" to maxOf(0L, startMs), "endDate" to maxOf(0L, endMs)))

        fun comment(body: String) = IssueEdit("comment", linkedMapOf("body" to body, "attachments" to emptyList<String>()))

        fun editBody(target: String, body: String) =
            IssueEdit("edit", linkedMapOf("target" to target, "body" to body, "attachments" to emptyList<String>()))

        fun react(target: String, reaction: String) = IssueEdit("reaction", linkedMapOf("target" to target, "reaction" to reaction))

        fun retract(eventId: String) = IssueEdit("delete", linkedMapOf("target" to eventId))

        fun deleteOwnIssue() = IssueEdit("delete", linkedMapOf("target" to "self"))
    }
}

object IssueFields {
    val PRIORITIES = listOf(0 to "None", 1 to "Low", 2 to "Medium", 3 to "High", 4 to "Urgent")

    fun clampPriority(value: Int): Int = value.coerceIn(0, 4)

    fun priorityLabel(value: Int): String = PRIORITIES.firstOrNull { it.first == clampPriority(value) }?.second ?: "None"

    fun cleanList(values: List<String>, max: Int, length: Int): List<String> {
        val seen = HashSet<String>()
        val out = ArrayList<String>()
        for (value in values) {
            val clean = value.trim().take(length)
            if (clean.isEmpty() || !seen.add(clean.lowercase())) continue
            out.add(clean)
            if (out.size >= max) break
        }
        return out
    }

    fun parseList(text: String): List<String> = text.split(',', '\n').map { it.trim() }.filter { it.isNotEmpty() }

    fun diff(
        issue: Issue,
        title: String? = null,
        labels: List<String>? = null,
        assignees: List<String>? = null,
        priority: Int? = null,
        progress: Int? = null,
        milestone: String? = null,
        open: Boolean? = null,
    ): List<IssueEdit> {
        val edits = ArrayList<IssueEdit>()
        if (title != null && title.trim() != issue.title.trim() && title.isNotBlank()) edits.add(IssueEdit.title(title))
        if (labels != null) {
            val clean = cleanList(labels, IssueEdit.MAX_LABELS, IssueEdit.MAX_LABEL_LENGTH)
            if (clean != issue.labels) edits.add(IssueEdit.labels(clean))
        }
        if (assignees != null) {
            val clean = cleanList(assignees.map { it.lowercase() }, IssueEdit.MAX_ASSIGNEES, 64)
            if (clean != issue.assignees) edits.add(IssueEdit.assignees(clean))
        }
        if (priority != null && clampPriority(priority) != issue.priority) edits.add(IssueEdit.priority(priority))
        if (progress != null && progress.coerceIn(0, 100) != issue.progress) edits.add(IssueEdit.progress(progress))
        if (milestone != null && milestone.trim() != issue.milestone) edits.add(IssueEdit.milestone(milestone))
        if (open != null && open != issue.isOpen) edits.add(IssueEdit.status(open))
        return edits
    }

    fun canDeleteOwn(issue: Issue, author: String): Boolean {
        val creator = issue.events.firstOrNull { it.type == "open" }?.author ?: issue.author
        return author.isNotEmpty() && creator == author
    }
}

data class IssueChecklistItem(val lineIndex: Int, val text: String, val done: Boolean)

object IssueChecklist {
    private data class Parsed(val prefix: String, val done: Boolean, val text: String)

    private fun parse(line: String): Parsed? {
        val indent = line.takeWhile { it == ' ' || it == '\t' }
        var rest = line.substring(indent.length)
        val bullet = rest.firstOrNull() ?: return null
        if (bullet != '-' && bullet != '*' && bullet != '+') return null
        rest = rest.substring(1)
        if (!rest.startsWith(" [")) return null
        rest = rest.substring(2)
        val mark = rest.firstOrNull() ?: return null
        if (mark != ' ' && mark != 'x' && mark != 'X') return null
        rest = rest.substring(1)
        if (!rest.startsWith("]")) return null
        rest = rest.substring(1)
        if (rest.isNotEmpty() && !rest.startsWith(" ")) return null
        return Parsed("$indent$bullet [", mark != ' ', if (rest.isEmpty()) "" else rest.substring(1))
    }

    fun items(body: String): List<IssueChecklistItem> {
        val out = ArrayList<IssueChecklistItem>()
        var inFence = false
        body.split("\n").forEachIndexed { index, line ->
            if (line.trim().startsWith("```")) {
                inFence = !inFence
                return@forEachIndexed
            }
            if (inFence) return@forEachIndexed
            parse(line)?.let { out.add(IssueChecklistItem(index, it.text, it.done)) }
        }
        return out
    }

    fun progress(body: String): Pair<Int, Int> {
        val all = items(body)
        return all.count { it.done } to all.size
    }

    fun toggled(body: String, lineIndex: Int): String? {
        val lines = body.split("\n").toMutableList()
        if (lineIndex !in lines.indices || items(body).none { it.lineIndex == lineIndex }) return null
        val item = parse(lines[lineIndex]) ?: return null
        lines[lineIndex] = "${item.prefix}${if (item.done) " " else "x"}]" + if (item.text.isEmpty()) "" else " ${item.text}"
        return lines.joinToString("\n")
    }
}

object IssueLinks {
    private val CLOSING_VERBS = setOf("close", "closes", "closed", "fix", "fixes", "fixed", "resolve", "resolves", "resolved")

    fun linkedIssueNumbers(text: String): List<Int> {
        val out = ArrayList<Int>()
        val words = text.replace("\n", " ").split(" ").filter { it.isNotEmpty() }
        for (i in 0 until words.size - 1) {
            val verb = words[i].lowercase().trim(':')
            if (verb !in CLOSING_VERBS) continue
            val target = words[i + 1].trim('.', ',', ';', ':', ')')
            if (!target.startsWith("#")) continue
            val number = target.substring(1).toIntOrNull() ?: continue
            if (number > 0 && number !in out) out.add(number)
        }
        return out
    }

    fun linkComment(pull: Int): String = "Linked to pull request #$pull."

    fun closingLine(issue: Int): String = "Closes #$issue"

    fun bodyLinking(body: String, issue: Int): String {
        if (issue in linkedIssueNumbers(body)) return body
        val trimmed = body.trim()
        return if (trimmed.isEmpty()) closingLine(issue) else "$trimmed\n\n${closingLine(issue)}"
    }
}

object IssueThreadText {
    fun copyText(repo: String, issue: Issue, includeThread: Boolean): String {
        val parts = mutableListOf("$repo#${issue.number}: ${issue.title}", "")
        if (issue.body.isNotBlank()) parts.add(issue.body.trim())
        if (includeThread) {
            for (event in issue.events) {
                if (event.type != "comment" || event.body.isBlank()) continue
                parts.add("")
                parts.add("— ${event.displayAuthor}:")
                parts.add(event.body.trim())
            }
        }
        return parts.joinToString("\n")
    }

    fun promptText(repo: String, issue: Issue): String = "Work on $repo#${issue.number}: ${issue.title}\n\n${issue.body.trim()}"
}

data class IssueBoardColumn(val name: String, val issues: List<Issue>, val isDone: Boolean)

data class IssueBoardMove(val number: Int, val from: String, val to: String, val edits: List<IssueEdit>) {
    val summary: String get() = "Moved #$number to $to."
}

object IssueBoard {
    val DEFAULT_COLUMNS = listOf("Backlog", "Todo", "In Progress", "Done")
    const val MAX_COLUMNS = 12
    const val TOO_FEW_COLUMNS = "A board needs at least two columns."

    fun statusLabel(column: String): String = "status:" + column.trim().lowercase()

    fun isStatusLabel(label: String): Boolean = label.lowercase().startsWith("status:")

    fun cleanColumns(columns: List<String>): List<String> {
        val out = ArrayList<String>()
        for (column in columns) {
            val name = column.trim().take(40)
            if (name.isEmpty() || out.any { it.lowercase() == name.lowercase() }) continue
            out.add(name)
            if (out.size >= MAX_COLUMNS) break
        }
        return out
    }

    fun columns(saved: List<String>): List<String> = cleanColumns(saved).ifEmpty { DEFAULT_COLUMNS }

    fun validate(columns: List<String>): String? = if (cleanColumns(columns).size < 2) TOO_FEW_COLUMNS else null

    fun columnOf(issue: Issue, columns: List<String>): String {
        val first = columns.firstOrNull() ?: return ""
        val last = columns.last()
        if (!issue.isOpen) return last
        for (column in columns) {
            val want = statusLabel(column)
            if (issue.labels.any { it.lowercase() == want }) return column
        }
        return first
    }

    fun layout(issues: List<Issue>, columns: List<String>, filter: (Issue) -> Boolean = { true }): List<IssueBoardColumn> {
        val buckets = HashMap<String, MutableList<Issue>>()
        for (issue in issues) {
            if (!filter(issue)) continue
            buckets.getOrPut(columnOf(issue, columns)) { ArrayList() }.add(issue)
        }
        return columns.mapIndexed { index, name ->
            IssueBoardColumn(
                name,
                buckets[name].orEmpty().sortedWith(compareByDescending<Issue> { it.priority }.thenByDescending { it.number }),
                index == columns.lastIndex,
            )
        }
    }

    fun move(issue: Issue, target: String, columns: List<String>): IssueBoardMove? {
        val last = columns.lastOrNull() ?: return null
        val column = columns.firstOrNull { it.lowercase() == target.lowercase() } ?: return null
        val current = columnOf(issue, columns)
        if (current.lowercase() == column.lowercase()) return null
        val toDone = column.lowercase() == last.lowercase()
        val labels = issue.labels.filterNot(::isStatusLabel).toMutableList()
        if (!toDone) labels.add(statusLabel(column))
        val edits = ArrayList<IssueEdit>()
        if (labels != issue.labels) edits.add(IssueEdit.labels(labels))
        if (issue.isOpen == toDone) edits.add(IssueEdit.status(!toDone))
        return IssueBoardMove(issue.number, current, column, edits)
    }

    fun neighbours(column: String, columns: List<String>): Pair<String?, String?> {
        val index = columns.indexOfFirst { it.lowercase() == column.lowercase() }
        if (index < 0) return null to null
        return (if (index > 0) columns[index - 1] else null) to (if (index + 1 < columns.size) columns[index + 1] else null)
    }

    fun storageKey(owner: String, name: String): String = "issueBoard/columns/$owner/$name"

    fun load(store: PrefStore, owner: String, name: String): List<String> {
        val raw = store.getString(storageKey(owner, name)) ?: return DEFAULT_COLUMNS
        val parsed = try {
            (Json.parse(raw) as? List<*>).orEmpty().map { it?.toString().orEmpty() }
        } catch (_: Exception) {
            emptyList()
        }
        return columns(parsed)
    }

    fun save(store: PrefStore, owner: String, name: String, columns: List<String>) {
        store.putString(storageKey(owner, name), Json.stringify(cleanColumns(columns)))
    }
}

class IssueBoardSession(
    val owner: String,
    val name: String,
    private val store: PrefStore,
    private val writer: CollabRecordWriter,
    private val loader: () -> List<Issue>,
) {
    @Volatile var issues: List<Issue> = emptyList()
        private set
    @Volatile var columns: List<String> = IssueBoard.load(store, owner, name)
        private set
    @Volatile var loading: Boolean = false
        private set
    @Volatile var loadError: String? = null
        private set
    @Volatile var movingNumber: Int? = null
        private set
    @Volatile var query: String = ""

    val layout: List<IssueBoardColumn>
        get() {
            val needle = query.trim().lowercase()
            return IssueBoard.layout(issues, columns) { issue ->
                needle.isEmpty() || issue.title.lowercase().contains(needle) || "#${issue.number}" == needle ||
                    issue.labels.any { it.lowercase().contains(needle) }
            }
        }

    fun refresh() {
        loading = true
        try {
            issues = loader()
            loadError = null
        } catch (failure: Exception) {
            loadError = failure.message ?: "$failure"
        } finally {
            loading = false
        }
    }

    fun saveColumns(names: List<String>): String? {
        IssueBoard.validate(names)?.let { return it }
        IssueBoard.save(store, owner, name, names)
        columns = IssueBoard.load(store, owner, name)
        return null
    }

    fun move(issue: Issue, column: String): CollabNotice? {
        val move = IssueBoard.move(issue, column, columns) ?: return null
        if (movingNumber != null) return CollabNotice("Still moving another card.", true)
        movingNumber = issue.number
        return try {
            writer.applyIssue(issue.number, move.edits)
            issues = issues.map { current ->
                if (current.number != issue.number) return@map current
                var updated = current
                for (edit in move.edits) {
                    if (edit.type == "labels") updated = updated.copy(labels = edit.fields.strings("labels"))
                    if (edit.type == "status") updated = updated.copy(status = edit.fields.str("status"))
                }
                updated
            }
            CollabNotice(move.summary)
        } catch (failure: Exception) {
            CollabNotice("Failed: ${failure.message ?: failure}", true)
        } finally {
            movingNumber = null
        }
    }
}

data class ModelMention(val alias: String, val model: String, val provider: String, val request: String, val intent: Intent) {
    enum class Intent { Answer, Work }
}

object IssueModelMentions {
    private val ALIAS_TABLE = listOf(
        "opus" to "claude-opus-5", "sonnet" to "claude-sonnet-5", "haiku" to "claude-haiku-4-5",
        "fable" to "claude-fable-5", "claude" to "claude-sonnet-5", "codex" to "gpt-5.5", "gpt" to "gpt-5.5",
        "sol" to "gpt-5.6-sol", "luna" to "gpt-5.6-luna", "terra" to "gpt-5.6-terra", "spark" to "gpt-5.3-codex-spark",
    )

    val aliases: List<String> get() = ALIAS_TABLE.map { it.first }

    private fun trimAlias(alias: String): String = alias.trimEnd('.', '-')

    fun modelForAlias(alias: String): String {
        val name = trimAlias(alias.trim().lowercase())
        if (name.isEmpty()) return ""
        ALIAS_TABLE.firstOrNull { it.first == name }?.let { return it.second }
        if (name.startsWith("claude-") && name.length > "claude-".length) return name
        if (name.startsWith("gpt-") && name.length > "gpt-".length) return name
        return ""
    }

    fun providerForModel(model: String): String {
        val id = model.trim().lowercase()
        return when {
            id.startsWith("claude") -> "claude-code"
            id.startsWith("gpt") -> "codex"
            else -> ""
        }
    }

    private val MENTION = Regex("(?:^|[^A-Za-z0-9_@./-])@([A-Za-z][A-Za-z0-9.-]{0,63})")
    private val LEAD = Regex(
        "^(?:hey|hi|hello|ok|okay|so|also|and|please|pls|plz|can\\s+you|could\\s+you|would\\s+you|will\\s+you|do\\s+you\\s+mind|i\\s+need\\s+you\\s+to|we\\s+need\\s+you\\s+to|let'?s|go\\s+ahead\\s+and|if\\s+you\\s+can|when\\s+you\\s+can)\\b[\\s,:;.!-]*",
        RegexOption.IGNORE_CASE,
    )
    private val EXPLICIT_AGENT = Regex(
        "\\b(?:start|spawn|launch|kick\\s+off|fire\\s+up|run)\\s+(?:an?\\s+|the\\s+)?(?:coding\\s+|background\\s+|new\\s+)?agent\\b",
    )
    private val WORK = Regex(
        "\\b(?:fix|implement|patch|repair|resolve|solve|refactor|rewrite|migrate|unbreak|land|ship)\\b|\\b(?:open|send|raise|submit|make)\\s+(?:an?|the)\\s+(?:pr|pull\\s+request|patch|fix|change)\\b|\\bwork\\s+on\\s+(?:this|it|the\\s+issue)\\b|\\btake\\s+(?:this|it)\\s+on\\b|\\bwrite\\s+(?:the|a)\\s+(?:code|fix|patch)\\b|\\bdo\\s+(?:it|this|that)\\b",
    )
    private val WH_QUESTION = Regex(
        "^(?:how|what|what's|whats|why|when|where|which|who|whose|whom|thoughts|explain|describe|tell|summari[sz]e|review|compare|analy[sz]e|opinion|any\\s+idea)\\b",
    )
    private val AUX_QUESTION = Regex("^(?:is|are|was|were|do|does|did|can|could|would|will|should|shall|may|might|have|has|had)\\b")

    private fun simplified(text: String): String = text.split(Regex("\\s+")).filter { it.isNotEmpty() }.joinToString(" ")

    private fun stripPoliteLead(request: String): String {
        var text = request.trim()
        repeat(6) {
            val match = LEAD.find(text) ?: return text
            if (match.range.isEmpty() || match.range.first != 0) return text
            text = text.substring(match.range.last + 1).trim()
        }
        return text
    }

    fun requestsWork(request: String): Boolean {
        val text = stripPoliteLead(simplified(request)).lowercase()
        if (text.isEmpty()) return false
        if (EXPLICIT_AGENT.containsMatchIn(text)) return true
        if (!WORK.containsMatchIn(text)) return false
        if (WH_QUESTION.containsMatchIn(text)) return false
        if (text.endsWith("?") && AUX_QUESTION.containsMatchIn(text)) return false
        return true
    }

    private fun trimRequest(raw: String): String {
        val start = raw.indexOfFirst { !(it.isWhitespace() || it == ':' || it == ',' || it == ';' || it == '.' || it == '-') }
        return if (start < 0) "" else raw.substring(start).trim()
    }

    fun parse(text: String): List<ModelMention> {
        if (text.isBlank()) return emptyList()
        data class Hit(val start: Int, val end: Int, val alias: String, val model: String)
        val hits = ArrayList<Hit>()
        for (match in MENTION.findAll(text)) {
            val group = match.groups[1] ?: continue
            val model = modelForAlias(group.value)
            if (model.isEmpty()) continue
            hits.add(Hit(group.range.first - 1, group.range.last + 1, trimAlias(group.value.lowercase()), model))
        }
        if (hits.isEmpty()) return emptyList()
        val strippedBuilder = StringBuilder()
        var cursor = 0
        for (hit in hits) {
            strippedBuilder.append(text, cursor, hit.start)
            cursor = hit.end
        }
        strippedBuilder.append(text.substring(cursor))
        val stripped = simplified(strippedBuilder.toString())
        return hits.mapIndexed { i, hit ->
            val stop = if (i + 1 < hits.size) hits[i + 1].start else text.length
            var request = trimRequest(text.substring(hit.end, stop))
            if (request.length < 2) request = stripped
            ModelMention(
                hit.alias,
                hit.model,
                providerForModel(hit.model),
                request,
                if (requestsWork(request)) ModelMention.Intent.Work else ModelMention.Intent.Answer,
            )
        }
    }

    fun agentPrompt(issueNumber: Int, title: String, question: String): String {
        val ask = question.trim().ifEmpty { "Summarize this issue and propose next steps." }
        return "Issue #$issueNumber: $title\n\n$ask\n\nReply on the issue thread; do not change code unless asked."
    }

    fun reprioritizePrompt(openCount: Int, fromReadme: Boolean): String = if (fromReadme) {
        "Read the README and the $openCount open issues, then set each issue's priority (0-4) so the work the README describes as most important comes first. Only change priorities; explain the order in one comment on the highest-priority issue."
    } else {
        "Review the $openCount open issues and set each one's priority (0-4) by user impact and effort. Only change priorities; explain the new order in one comment on the highest-priority issue."
    }
}
