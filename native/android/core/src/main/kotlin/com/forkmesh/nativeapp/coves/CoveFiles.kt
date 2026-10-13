package com.forkmesh.nativeapp.coves

import java.net.URI
import java.net.URLDecoder
import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

enum class CoveEditorKind { Text, Spreadsheet, Pipeline }

data class CoveFileTemplate(
    val id: String,
    val title: String,
    val description: String,
    val defaultPath: String,
    val mime: String,
    val body: String,
    val editor: CoveEditorKind,
    val icon: String,
)

data class CoveLink(val owner: String, val repository: String, val coveId: String)

object CoveFiles {
    const val PIPELINE_MIME = "application/vnd.forkmesh.pipeline+json"

    private val ISO = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'").withZone(ZoneOffset.UTC)

    fun isoNow(nowMs: Long = System.currentTimeMillis()): String = ISO.format(Instant.ofEpochMilli(nowMs))

    fun defaultPipelineObject(nowMs: Long = System.currentTimeMillis()): Map<String, Any?> {
        val stages = listOf(
            Triple("new", "New", "#58a6ff"),
            Triple("qualified", "Qualified", "#a371f7"),
            Triple("proposal", "Proposal", "#d29922"),
            Triple("won", "Won", "#3fb950"),
            Triple("lost", "Lost", "#f85149"),
        )
        return linkedMapOf(
            "createdAt" to isoNow(nowMs),
            "history" to emptyList<Any?>(),
            "leads" to emptyList<Any?>(),
            "name" to "Lead pipeline",
            "stages" to stages.map { linkedMapOf("color" to it.third, "id" to it.first, "name" to it.second) },
            "version" to 1L,
        )
    }

    fun defaultPipelineJson(nowMs: Long = System.currentTimeMillis()): String = CoveJson.pretty(defaultPipelineObject(nowMs)) + "\n"

    fun templates(nowMs: Long = System.currentTimeMillis()): List<CoveFileTemplate> = listOf(
        CoveFileTemplate("blank-note", "Blank note", "A clean Markdown document for ideas and drafts.", "notes.md",
            "text/markdown", "# Notes\n\nStart writing here.\n", CoveEditorKind.Text, "description_outlined"),
        CoveFileTemplate("spreadsheet", "Spreadsheet", "An editable CSV grid for lists, tracking, and analysis.",
            "spreadsheet.csv", "text/csv", "Name,Status,Owner,Due date\r\nFirst item,Not started,,\r\n",
            CoveEditorKind.Spreadsheet, "table_chart_outlined"),
        CoveFileTemplate("lead-pipeline", "Lead pipeline", "Stages, leads, movement history, and value reporting.",
            "leads.pipeline.json", PIPELINE_MIME, defaultPipelineJson(nowMs), CoveEditorKind.Pipeline, "view_kanban_outlined"),
        CoveFileTemplate("project-brief", "Project brief", "Goals, audience, scope, milestones, and success measures.",
            "project-brief.md", "text/markdown",
            "# Project brief\n\n## Goal\n\n## Audience\n\n## Scope\n\n### In scope\n\n### Out of scope\n\n## Milestones\n\n## Success measures\n",
            CoveEditorKind.Text, "article_outlined"),
        CoveFileTemplate("meeting-notes", "Meeting notes", "Agenda, attendees, decisions, and follow-up actions.",
            "meeting-notes.md", "text/markdown",
            "# Meeting notes\n\n**Date:** \n**Attendees:** \n\n## Agenda\n\n## Notes\n\n## Decisions\n\n## Action items\n\n- [ ] Owner — next step\n",
            CoveEditorKind.Text, "groups_outlined"),
        CoveFileTemplate("contacts", "Contact list", "A spreadsheet ready for people and account details.", "contacts.csv",
            "text/csv", "Name,Company,Role,Email,Phone,Notes\r\n", CoveEditorKind.Spreadsheet, "contacts_outlined"),
        CoveFileTemplate("budget", "Budget tracker", "Plan and compare categories, estimates, and actual spend.",
            "budget.csv", "text/csv", "Category,Item,Estimated,Actual,Owner,Notes\r\nOperations,Example,0,0,,\r\n",
            CoveEditorKind.Spreadsheet, "attach_money"),
        CoveFileTemplate("checklist", "Checklist", "A lightweight Markdown checklist grouped by phase.", "checklist.md",
            "text/markdown", "# Checklist\n\n## Prepare\n\n- [ ] First task\n\n## Review\n\n- [ ] Final review\n",
            CoveEditorKind.Text, "checklist"),
        CoveFileTemplate("decision-log", "Decision log", "Record context, options, owners, and outcomes over time.",
            "decision-log.md", "text/markdown",
            "# Decision log\n\n## Decision title\n\n**Date:** \n**Owner:** \n**Status:** Proposed\n\n### Context\n\n### Options considered\n\n### Decision\n\n### Consequences\n",
            CoveEditorKind.Text, "alt_route"),
        CoveFileTemplate("structured-data", "Structured JSON", "A valid JSON starting point for structured private data.",
            "data.json", "application/json", "{\n  \"items\": []\n}\n", CoveEditorKind.Text, "data_object"),
    )

    fun template(id: String): CoveFileTemplate? = templates().firstOrNull { it.id == id }

    fun editorKind(path: String, mime: String): CoveEditorKind {
        val lowerPath = path.trim().lowercase()
        val lowerMime = mime.trim().lowercase()
        if (lowerMime == PIPELINE_MIME || lowerPath.endsWith(".pipeline.json")) return CoveEditorKind.Pipeline
        if (lowerMime == "text/csv" || lowerPath.endsWith(".csv")) return CoveEditorKind.Spreadsheet
        return CoveEditorKind.Text
    }

    fun uniqueName(wanted: String, existing: List<String>): String {
        val taken = existing.map { CoveStore.cleanPath(it).lowercase() }.toSet()
        val clean = CoveStore.cleanPath(wanted.trim())
        if (clean.lowercase() !in taken) return clean
        val slash = clean.lastIndexOf('/')
        val folder = if (slash >= 0) clean.substring(0, slash + 1) else ""
        val file = if (slash >= 0) clean.substring(slash + 1) else clean
        val dot = file.indexOf('.').let { if (it < 0) file.length else it }
        val stem = file.substring(0, dot)
        val ext = file.substring(dot)
        var counter = 2
        while (true) {
            val candidate = "$folder$stem-$counter$ext"
            if (candidate.lowercase() !in taken) return candidate
            counter++
        }
    }

    fun parseCsv(csv: String): List<List<String>> {
        val rows = ArrayList<List<String>>()
        var row = ArrayList<String>()
        val field = StringBuilder()
        var quoted = false
        val chars = csv.codePoints().toArray()
        var i = 0
        while (i < chars.size) {
            val ch = chars[i]
            if (quoted) {
                if (ch == '"'.code) {
                    if (i + 1 < chars.size && chars[i + 1] == '"'.code) {
                        field.append('"')
                        i++
                    } else {
                        quoted = false
                    }
                } else {
                    field.appendCodePoint(ch)
                }
                i++
                continue
            }
            when {
                ch == '"'.code && field.isEmpty() -> quoted = true
                ch == ','.code -> {
                    row.add(field.toString())
                    field.setLength(0)
                }
                ch == '\r'.code || ch == '\n'.code -> {
                    row.add(field.toString())
                    field.setLength(0)
                    rows.add(row)
                    row = ArrayList()
                    if (ch == '\r'.code && i + 1 < chars.size && chars[i + 1] == '\n'.code) i++
                }
                else -> field.appendCodePoint(ch)
            }
            i++
        }
        val last = chars.lastOrNull()
        val endedWithNewline = last == '\n'.code || last == '\r'.code
        if (!endedWithNewline || row.isNotEmpty() || field.isNotEmpty()) {
            row.add(field.toString())
            rows.add(row)
        }
        if (chars.isEmpty() || rows.isEmpty()) return listOf(listOf(""))
        return rows
    }

    fun serializeCsv(rows: List<List<String>>): String = rows.joinToString("\r\n") { row ->
        row.joinToString(",") { field ->
            val needsQuotes = field.contains(',') || field.contains('"') || field.contains('\r') || field.contains('\n')
            if (!needsQuotes) field else "\"" + field.replace("\"", "\"\"") + "\""
        }
    } + "\r\n"

    private fun encodeSegment(text: String): String {
        val out = StringBuilder()
        for (b in text.toByteArray(Charsets.UTF_8)) {
            val c = b.toInt() and 0xff
            val ch = c.toChar()
            val allowed = c < 128 && (ch.isLetterOrDigit() || ch in "-._~!$&'()*+,;=:@")
            if (allowed) out.append(ch) else out.append('%').append(String.format("%02X", c))
        }
        return out.toString()
    }

    fun shareUrl(owner: String, repository: String, coveId: String): String {
        val parts = listOf(owner, repository, coveId).map { it.trim() }
        if (parts.any { it.isEmpty() }) return ""
        return "forkmesh://cove/" + parts.joinToString("/") { encodeSegment(it) }
    }

    fun parseShareUrl(raw: String): CoveLink? {
        val uri = try {
            URI(raw.trim())
        } catch (_: Exception) {
            return null
        }
        if (uri.scheme != "forkmesh" || uri.host != "cove") return null
        val path = uri.rawPath ?: return null
        val parts = path.split('/').filter { it.isNotEmpty() }.map { URLDecoder.decode(it.replace("+", "%2B"), "UTF-8") }
        if (parts.size != 3) return null
        if (parts.any { it.trim().isEmpty() }) return null
        return CoveLink(parts[0], parts[1], parts[2])
    }

    fun routeId(owner: String, repository: String, coveId: String): String = "$owner/$repository/$coveId"

    fun parseRouteId(id: String): CoveLink? {
        parseShareUrl(id)?.let { return it }
        val parts = id.split('/')
        if (parts.size != 3 || parts.any { it.isEmpty() }) return null
        return CoveLink(parts[0], parts[1], parts[2])
    }
}
