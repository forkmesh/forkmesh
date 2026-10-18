package com.forkmesh.nativeapp.codeinsight

import java.text.DateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.min

object StrataReportHtml {
    val ratingColors = listOf("#3fb98f", "#8cc265", "#e3b341", "#f0883e", "#e5534b")
    val severityColors = listOf("#e5534b", "#f0883e", "#e3b341", "#8b949e")

    fun gateColor(status: StrataGateStatus): String = when (status) {
        StrataGateStatus.Fail -> "#e5534b"
        StrataGateStatus.Warn -> "#e3b341"
        StrataGateStatus.Pass -> "#3fb98f"
    }

    fun escape(text: String): String {
        val out = StringBuilder(text.length + 16)
        for (c in text) {
            when (c) {
                '<' -> out.append("&lt;")
                '>' -> out.append("&gt;")
                '&' -> out.append("&amp;")
                '"' -> out.append("&quot;")
                else -> out.append(c)
            }
        }
        return out.toString()
    }

    fun shortDate(seconds: Long): String =
        DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT, Locale.getDefault()).format(Date(seconds * 1000))

    private const val STYLE = "<style>" +
        "body{font-family:-apple-system,Segoe UI,Helvetica,Arial,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:32px 40px;}" +
        "h1{font-size:26px;margin:0 0 4px}h2{font-size:18px;margin:36px 0 12px;border-bottom:1px solid #30363d;padding-bottom:6px}" +
        ".muted{color:#8b949e}.row{display:flex;gap:16px;flex-wrap:wrap}" +
        ".tile{background:#161b22;border-radius:10px;padding:14px 18px;min-width:140px}" +
        ".tile b{display:block;font-size:22px;margin-top:4px}" +
        ".rating{width:84px;height:84px;border-radius:16px;font-size:48px;font-weight:700;color:#0d1117;display:flex;align-items:center;justify-content:center}" +
        "table{border-collapse:collapse;width:100%;font-size:13px}td,th{text-align:left;padding:6px 10px;border-bottom:1px solid #21262d;vertical-align:top}" +
        "th{color:#8b949e;font-weight:600}.num{text-align:right;font-variant-numeric:tabular-nums}" +
        ".dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:6px}" +
        "details{background:#161b22;border-radius:8px;margin:6px 0;padding:8px 12px}summary{cursor:pointer}" +
        "a{color:#58a6ff}" +
        "</style></head><body>"

    fun render(
        snapshot: InsightSnapshot,
        report: StrataReport,
        title: String,
        dateText: (Long) -> String = ::shortDate,
    ): String {
        val html = StringBuilder()
        html.append("<!DOCTYPE html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">")
        html.append("<title>Strata report · ${escape(title)}</title>")
        html.append(STYLE)
        html.append("<h1>Strata · ${escape(title)}</h1>")
        val facts = ArrayList<String>()
        if (snapshot.branch.isNotEmpty()) facts.add("branch ${escape(snapshot.branch)}")
        if (snapshot.head.isNotEmpty()) facts.add("commit ${escape(snapshot.head.take(10))}")
        if (snapshot.analyzedAt > 0) facts.add("analyzed ${dateText(snapshot.analyzedAt)}")
        html.append("<div class=\"muted\">${facts.joinToString(" · ")}</div>")

        html.append("<h2 id=\"dashboard\">Dashboard</h2><div class=\"row\">")
        html.append(
            "<div class=\"tile\" style=\"display:flex;gap:16px;align-items:center\"><div class=\"rating\" style=\"background:" +
                "${ratingColors[report.total.rating]}\">${Strata.ratingLetter(report.total.rating)}</div>" +
                "<div>Debt ratio<b>${Strata.formatPercent(report.total.ratio)}</b><span class=\"muted\">" +
                "${Strata.formatDebt(report.total.debt)} debt · ${Strata.formatDebt(report.total.interest)} interest per year</span></div></div>",
        )
        val m = report.metrics
        val tiles = listOf(
            "Lines of code" to Strata.number(m.codeLines),
            "Files" to Strata.number(m.files),
            "Modules" to Strata.number(m.modules),
            "Types" to Strata.number(m.types),
            "Functions" to Strata.number(m.functions),
            "Average complexity" to Strata.decimal(m.averageComplexity, 1),
            "Max complexity" to Strata.number(m.maxComplexity),
            "Comments" to "${Strata.decimal(m.commentPercent, 1)} %",
            "Duplicated" to "${Strata.decimal(m.duplicatedPercent, 1)} %",
        )
        for ((label, value) in tiles) html.append("<div class=\"tile\">${escape(label)}<b>${escape(value)}</b></div>")
        html.append("</div>")

        html.append("<h2 id=\"gates\">Quality gates</h2><table><tr><th>Gate</th><th>Status</th><th>Measured</th><th>Rule</th></tr>")
        for (gate in report.gates) {
            html.append(
                "<tr><td>${escape(gate.name)}</td><td><span class=\"dot\" style=\"background:${gateColor(gate.status)}\"></span>" +
                    "${gate.status.label}</td><td>${escape(gate.measured)}</td><td class=\"muted\">${escape(gate.detail)}</td></tr>",
            )
        }
        html.append("</table>")

        html.append("<h2 id=\"rules\">Rules</h2>")
        val ruleOrder = report.rules.indices.sortedWith { a, b ->
            val da = report.rules[a].debt
            val db = report.rules[b].debt
            if (da > db) -1 else if (db > da) 1 else 0
        }
        for (r in ruleOrder) {
            val rule = report.rules[r]
            val worst = if (rule.violations.isEmpty()) rule.severity.ordinal else rule.worst
            val count = rule.violations.size
            html.append(
                "<details><summary><span class=\"dot\" style=\"background:" +
                    "${if (rule.violations.isEmpty()) "#3fb98f" else severityColors[worst]}\"></span><b>${escape(rule.id)}</b> " +
                    "${escape(rule.title)} · ${rule.category.label} · $count ${if (count == 1) "violation" else "violations"} · " +
                    "${Strata.formatDebt(rule.debt)}</summary>",
            )
            html.append("<p>${escape(rule.description)}</p><p class=\"muted\">How to fix: ${escape(rule.howToFix)}</p>")
            if (rule.violations.isNotEmpty()) {
                html.append("<table><tr><th>Severity</th><th>Where</th><th>Issue</th><th class=\"num\">Debt</th></tr>")
                val shown = min(rule.violations.size, 200)
                for (i in 0 until shown) {
                    val v = rule.violations[i]
                    val where = when {
                        v.file >= 0 && v.file < snapshot.files.size && !rule.moduleScope ->
                            snapshot.files[v.file].path + (if (v.line > 0) ":${v.line}" else "")
                        v.module >= 0 && v.module < report.graph.nodes.size -> report.graph.nodes[v.module].path
                        else -> ""
                    }
                    html.append(
                        "<tr><td>${v.severity.label}</td><td>${escape(where)}</td><td>${escape(v.message)}</td>" +
                            "<td class=\"num\">${Strata.formatDebt(v.debt)}</td></tr>",
                    )
                }
                if (rule.violations.size > shown) {
                    html.append("<tr><td colspan=\"4\" class=\"muted\">and ${rule.violations.size - shown} more</td></tr>")
                }
                html.append("</table>")
            }
            html.append("</details>")
        }

        html.append(
            "<h2 id=\"modules\">Modules</h2><table><tr><th>Module</th><th>Rating</th><th class=\"num\">Lines of code</th>" +
                "<th class=\"num\">Files</th><th class=\"num\">Ca</th><th class=\"num\">Ce</th><th class=\"num\">I</th>" +
                "<th class=\"num\">A</th><th class=\"num\">D</th><th class=\"num\">Debt</th></tr>",
        )
        val modules = report.graph.nodes.indices.sortedWith { a, b ->
            val da = report.modules[a].debt
            val db = report.modules[b].debt
            if (da > db) -1 else if (db > da) 1 else 0
        }
        for (i in modules) {
            val node = report.graph.nodes[i]
            val score = report.modules[i]
            html.append(
                "<tr><td>${escape(node.path.ifEmpty { node.name })}" +
                    "${if (node.cycle >= 0) " <span style=\"color:#e5534b\">(cycle)</span>" else ""}</td>" +
                    "<td><span class=\"dot\" style=\"background:${ratingColors[score.rating]}\"></span>${Strata.ratingLetter(score.rating)}</td>" +
                    "<td class=\"num\">${Strata.number(node.codeLines)}</td>" +
                    "<td class=\"num\">${node.files.size}</td><td class=\"num\">${node.afferent}</td><td class=\"num\">${node.efferent}</td>" +
                    "<td class=\"num\">${Strata.fixed(node.instability, 2)}</td>",
            )
            html.append(
                "<td class=\"num\">${Strata.fixed(node.abstractness, 2)}</td><td class=\"num\">${Strata.fixed(node.distance, 2)}</td>" +
                    "<td class=\"num\">${Strata.formatDebt(score.debt)}</td></tr>",
            )
        }
        html.append("</table>")
        html.append(
            "<p class=\"muted\">Debt ratio = estimated remediation time ÷ development effort (18 man-days per 1,000 lines of code). " +
                "Ratings: A ≤ 5 %, B ≤ 10 %, C ≤ 20 %, D ≤ 50 %, E above. Generated by Strata in ForkMesh.</p>",
        )
        html.append("</body></html>\n")
        return html.toString()
    }
}
