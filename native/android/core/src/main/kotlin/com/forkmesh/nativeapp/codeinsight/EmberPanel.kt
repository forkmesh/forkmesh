package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.codeinsight.Ember.decimal
import com.forkmesh.nativeapp.codeinsight.Ember.fileNameOf
import com.forkmesh.nativeapp.codeinsight.Ember.folderOf
import com.forkmesh.nativeapp.codeinsight.Ember.number
import com.forkmesh.nativeapp.codeinsight.Ember.percentText
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.abs
import kotlin.math.floor
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

data class EmberRow(
    val id: Int,
    val title: String,
    val detail: String,
    val value: String,
    val fraction: Double,
    val color: EmberColor,
    val rank: Int = 0,
    val notes: List<String> = emptyList(),
)

data class EmberMetric(val label: String, val value: String, val line: Int = 0)

data class EmberCard(
    val crumb: String,
    val name: String,
    val health: Double,
    val band: HealthBand,
    val note: String,
    val metrics: List<EmberMetric>,
    val issues: List<String>,
)

data class EmberAuthorOption(val label: String, val author: String)

object EmberPanel {
    const val TARGETS_INTRO = "Frequently changed files with low code health, ranked by hotspot × (10 − health). " +
        "Improving these pays back on every future change."
    const val COUPLING_INTRO = "Files committed together with this one. Degree is the share of their commits made together."
    const val COUPLING_EMPTY = "This file does not change together with other files often enough to count as coupled."
    const val COUPLING_NO_HISTORY = "Change coupling needs git history; this folder has none."
    const val TREND_NO_HISTORY = "Complexity trends need git history; this folder has none."
    const val TREND_EMPTY = "No earlier versions of this file in the history window."
    const val INFO_TITLE = "How Ember scores files"
    const val INFO_TEXT = "Each circle is a file, sized by lines of code; grey discs are folders.\n\n" +
        "Code health runs from 10 (healthy) to 1 (unhealthy). It drops for long and deeply nested functions, " +
        "high cyclomatic complexity, many parameters, large files, duplicated blocks and import cycles. " +
        "Healthy is 8 or above, problematic 4 to 8, unhealthy below 4.\n\n" +
        "Hotspots are files that change often and are large: change frequency in the git history window times size. " +
        "Refactoring targets are hotspots with low health, ranked by hotspot × (10 − health)."

    val bandRanges: List<Pair<Double, Double>> = listOf(8.0 to 10.0, 4.0 to 7.5, 1.0 to 3.5)

    fun pathCrumb(folder: String): String = if (folder.isEmpty()) "/" else folder.split('/').joinToString(" / ")

    fun toggleBand(filters: EmberFilters, band: Int): EmberFilters {
        if (band !in 0..2) return filters
        val (low, high) = bandRanges[band]
        val same = filters.minHealth == low && filters.maxHealth == high
        return filters.copy(minHealth = if (same) 1.0 else low, maxHealth = if (same) 10.0 else high)
    }

    fun bandRows(snapshot: InsightSnapshot): List<EmberRow> {
        val s = Ember.summarize(snapshot)
        val total = max(1L, s.healthyLines + s.problematicLines + s.unhealthyLines)
        fun row(id: Int, band: HealthBand, files: Int, lines: Long) = EmberRow(
            id, band.label, "${number(files)} files · ${number(lines)} lines of code",
            percentText(lines.toDouble() / total) + " of code", lines.toDouble() / total, Ember.bandColor(band),
        )
        return listOf(
            row(0, HealthBand.Healthy, s.healthy, s.healthyLines),
            row(1, HealthBand.Problematic, s.problematic, s.problematicLines),
            row(2, HealthBand.Unhealthy, s.unhealthy, s.unhealthyLines),
        )
    }

    fun hotspotRows(snapshot: InsightSnapshot, limit: Int = 8): List<EmberRow> =
        Ember.topHotspotOrder(snapshot).take(limit).map { i ->
            val f = snapshot.files[i]
            EmberRow(i, fileNameOf(f.path), folderOf(f.path), "${number(f.commits)} commits", f.hotspot, Ember.healthColor(f.health))
        }

    fun hotspotsEmptyText(history: Boolean): String =
        if (history) "No file changed in the history window." else "Hotspots need git history; this folder has none."

    fun targetRows(snapshot: InsightSnapshot, targets: List<EmberTarget>): List<EmberRow> {
        val top = targets.firstOrNull()?.score ?: 1.0
        return targets.map { t ->
            val f = snapshot.files[t.file]
            EmberRow(
                t.file, fileNameOf(f.path), folderOf(f.path), "health ${decimal(f.health)}", t.score / max(1e-9, top),
                Ember.healthColor(f.health), t.rank, t.reasons.take(1) + t.reasons.drop(2).take(2),
            )
        }
    }

    fun couplingRows(snapshot: InsightSnapshot, refs: List<EmberCouplingRef>): List<EmberRow> = refs.map { ref ->
        val f = snapshot.files[ref.file]
        EmberRow(
            ref.file, fileNameOf(f.path), "${folderOf(f.path).ifEmpty { "/" }} · ${number(ref.shared)} shared commits",
            percentText(ref.degree), ref.degree, Ember.couplingLink,
        )
    }

    fun metricLabel(snapshot: InsightSnapshot, tab: EmberTab, coupling: List<List<EmberCouplingRef>>, file: Int): String {
        if (file < 0 || file >= snapshot.files.size) return ""
        val f = snapshot.files[file]
        return when (tab) {
            EmberTab.Hotspots -> if (f.commits > 0) "${number(f.commits)} commits" else ""
            EmberTab.RefactoringTargets, EmberTab.CodeHealth -> "health ${decimal(f.health)}"
            EmberTab.ChangeCoupling -> if (file < coupling.size && coupling[file].isNotEmpty()) "${coupling[file].size} coupled" else ""
            EmberTab.Knowledge -> if (f.mainAuthor.isEmpty()) "" else "${f.mainAuthor} ${percentText(f.mainAuthorShare)}"
            EmberTab.Defects -> if (f.fixCommits > 0) "${f.fixCommits} fixes" else ""
            EmberTab.Languages -> f.language
        }
    }

    fun describeFile(snapshot: InsightSnapshot, file: Int, rank: Int): String {
        val f = snapshot.files[file]
        val parts = mutableListOf(
            fileNameOf(f.path),
            f.path,
            "Code health ${decimal(f.health)} · ${Insight.healthBand(f.health).label}",
            "${number(f.codeLines)} lines of code · complexity ${number(f.complexity)}",
        )
        if (Ember.hasHistory(snapshot)) {
            parts.add("${number(f.commits)} commits · ${number(f.authors)} authors · ${number(f.fixCommits)} fixes")
            if (f.mainAuthor.isNotEmpty()) parts.add("Main author ${f.mainAuthor} (${percentText(f.mainAuthorShare)})")
        }
        if (rank > 0) parts.add("Refactoring target #$rank")
        return parts.joinToString(". ")
    }

    fun describeFolder(node: EmberNode): String =
        "${node.name}. ${number(node.files)} files · ${number(node.codeLines)} lines of code. Code health ${decimal(node.health)} (weighted by lines)"

    fun rankedFiles(
        snapshot: InsightSnapshot,
        coloring: EmberColoring,
        coupling: List<List<EmberCouplingRef>>,
    ): List<EmberRow> {
        val files = snapshot.files
        val visible = files.indices.filter { it < coloring.files.size && !coloring.files[it].dimmed }
        fun color(i: Int) = coloring.files.getOrNull(i)?.color ?: Ember.neutral
        fun row(i: Int, value: String, fraction: Double, rank: Int = 0, notes: List<String> = emptyList()) =
            EmberRow(i, fileNameOf(files[i].path), folderOf(files[i].path), value, fraction.coerceIn(0.0, 1.0), color(i), rank, notes)
        val byPath = Comparator<Int> { a, b -> files[a].path.compareTo(files[b].path) }
        return when (coloring.tab) {
            EmberTab.Hotspots -> {
                val maxCommits = max(1, files.maxOfOrNull { it.commits } ?: 0)
                visible.sortedWith(compareByDescending<Int> { files[it].hotspot }.thenByDescending { files[it].commits }.then(byPath))
                    .map { row(it, if (files[it].commits > 0) "${number(files[it].commits)} commits" else "no commits", files[it].commits.toDouble() / maxCommits) }
            }
            EmberTab.RefactoringTargets -> targetRows(snapshot, coloring.targets).filter { it.id in visible }
            EmberTab.CodeHealth -> visible.sortedWith(compareBy<Int> { files[it].health }.thenByDescending { files[it].codeLines }.then(byPath))
                .map { row(it, "health ${decimal(files[it].health)}", (files[it].health - 1) / 9.0) }
            EmberTab.ChangeCoupling -> {
                fun strongest(i: Int) = coupling.getOrNull(i)?.firstOrNull()?.degree ?: 0.0
                visible.sortedWith(compareByDescending<Int> { strongest(it) }.thenByDescending { coupling.getOrNull(it)?.size ?: 0 }.then(byPath))
                    .map {
                        val count = coupling.getOrNull(it)?.size ?: 0
                        row(it, if (count > 0) "$count coupled · ${percentText(strongest(it))}" else "not coupled", strongest(it))
                    }
            }
            EmberTab.Knowledge -> visible.sortedWith(
                compareByDescending<Int> { files[it].formerAuthorShare }.thenByDescending { files[it].codeLines }.then(byPath),
            ).map {
                val f = files[it]
                val value = if (f.mainAuthor.isEmpty()) "no commits in the window" else "${f.mainAuthor} ${percentText(f.mainAuthorShare)}"
                row(it, value, f.formerAuthorShare, notes = if (f.formerAuthorShare > 0) listOf("${percentText(f.formerAuthorShare)} code by former contributors") else emptyList())
            }
            EmberTab.Defects -> {
                val maxFixes = max(1, files.maxOfOrNull { it.fixCommits } ?: 0)
                visible.sortedWith(compareByDescending<Int> { files[it].fixCommits }.thenByDescending { files[it].commits }.then(byPath))
                    .map { row(it, if (files[it].fixCommits > 0) "${number(files[it].fixCommits)} fixes" else "no fixes", files[it].fixCommits.toDouble() / maxFixes) }
            }
            EmberTab.Languages -> {
                val order = Ember.topLanguages(snapshot, 0).withIndex().associate { it.value to it.index }
                val maxLines = max(1, files.maxOfOrNull { it.codeLines } ?: 0)
                visible.sortedWith(compareBy<Int> { order[files[it].language] ?: Int.MAX_VALUE }.thenByDescending { files[it].codeLines }.then(byPath))
                    .map { row(it, files[it].language, files[it].codeLines.toDouble() / maxLines) }
            }
        }
    }

    fun bestMatch(snapshot: InsightSnapshot, text: String): Int {
        val query = text.trim()
        if (query.isEmpty()) return -1
        var best = -1
        var bestRank = 1 shl 30
        snapshot.files.forEachIndexed { i, file ->
            val name = fileNameOf(file.path)
            var rank = when {
                name.equals(query, ignoreCase = true) -> 0
                name.startsWith(query, ignoreCase = true) -> 1
                name.contains(query, ignoreCase = true) -> 2
                file.path.contains(query, ignoreCase = true) -> 3
                else -> -1
            }
            if (rank < 0) return@forEachIndexed
            rank = rank * 100000 + file.path.length
            if (rank < bestRank) {
                bestRank = rank
                best = i
            }
        }
        return best
    }

    fun authorOptions(snapshot: InsightSnapshot): List<EmberAuthorOption> {
        val ranked = Ember.topAuthors(snapshot, 0)
        val all = snapshot.authors.toMutableList()
        for (a in ranked) if (a !in all) all.add(a)
        val former = snapshot.formerAuthors.toSet()
        fun label(a: String) = if (a in former) "$a (former)" else a
        val out = mutableListOf(EmberAuthorOption("All authors", ""))
        for (a in ranked) out.add(EmberAuthorOption(label(a), a))
        for (a in all) if (a !in ranked) out.add(EmberAuthorOption(label(a), a))
        return out
    }

    fun commitMax(snapshot: InsightSnapshot): Int = max(1, min(snapshot.files.maxOfOrNull { it.commits } ?: 0, 200))

    fun commitThresholdText(value: Int): String = if (value == 0) "any" else "at least $value commits"

    fun defaultTab(snapshot: InsightSnapshot, chosen: EmberTab?): EmberTab {
        val history = Ember.hasHistory(snapshot)
        return when {
            chosen == null -> if (history) EmberTab.Hotspots else EmberTab.CodeHealth
            !history && chosen.needsHistory -> EmberTab.CodeHealth
            else -> chosen
        }
    }

    fun bandNote(file: InsightFile, rank: Int): String = when {
        rank > 0 -> "Refactoring target #$rank"
        file.hotspot >= 0.5 -> "Hotspot: changes often and is large"
        file.test -> "Test file"
        else -> ""
    }

    fun card(snapshot: InsightSnapshot, index: Int, rank: Int, now: Long, zone: ZoneId = ZoneId.systemDefault()): EmberCard {
        val f = snapshot.files[index]
        val history = Ember.hasHistory(snapshot)
        val span = Ember.spanText(if (snapshot.historyDays > 0) snapshot.historyDays else 365)
        val noHistory = "No git history"
        val metrics = ArrayList<EmberMetric>()
        metrics.add(EmberMetric("Commits", if (history) "${number(f.commits)} commits / $span · ${number(f.recentCommits)} in the last 90 days" else noHistory))
        metrics.add(EmberMetric("Size", "${number(f.codeLines)} lines of code · ${number(f.lines)} lines in total"))
        metrics.add(
            EmberMetric(
                "Main author",
                if (history && f.mainAuthor.isNotEmpty()) "${f.mainAuthor} (${percentText(f.mainAuthorShare)}) · ${number(f.authors)} authors" else noHistory,
            ),
        )
        metrics.add(EmberMetric("Knowledge", if (history) "${percentText(f.formerAuthorShare)} code by former contributors" else noHistory))
        metrics.add(EmberMetric("Defects", if (history) "${number(f.fixCommits)} fix commits / $span" else noHistory))
        metrics.add(
            EmberMetric(
                "Modified",
                if (history && f.lastChanged > 0) "${Ember.agoText(f.lastChanged, now)} · ${dateText(f.lastChanged, zone)}" else noHistory,
            ),
        )
        var worst: InsightFunction? = null
        for (fn in f.functions) if (worst == null || fn.complexity > worst.complexity) worst = fn
        val complexity = StringBuilder("${number(f.complexity)} in total")
        if (worst != null) complexity.append(" · worst ${worst.name}() ${number(worst.complexity)} at line ${worst.line}")
        metrics.add(EmberMetric("Complexity", complexity.toString(), worst?.line ?: 0))
        val longest = f.functions.maxOfOrNull { it.length } ?: 0
        metrics.add(EmberMetric("Functions", "${number(f.functions.size)} functions · longest ${number(longest)} lines · deepest nesting ${number(f.maxNesting)}"))
        metrics.add(EmberMetric("Duplication", if (f.duplicatedLines > 0) "${number(f.duplicatedLines)} lines in repeated blocks" else "No repeated blocks"))
        val imports = StringBuilder("imports ${number(f.dependsOn.size)} files · used by ${number(f.dependedOnBy.size)} files")
        if (f.externalImports.isNotEmpty()) imports.append(" · ${number(f.externalImports.size)} packages")
        if (f.cycle >= 0 && f.cycle < snapshot.cycles.size) imports.append(" · in an import cycle of ${snapshot.cycles[f.cycle].size} files")
        metrics.add(EmberMetric("Imports", imports.toString()))
        metrics.add(EmberMetric("Language", if (f.test) "${f.language} · test file" else f.language))
        return EmberCard(
            pathCrumb(folderOf(f.path)), fileNameOf(f.path), f.health, Insight.healthBand(f.health), bandNote(f, rank), metrics, f.healthIssues,
        )
    }

    fun couplingTabTitle(count: Int): String = if (count == 0) "Coupling" else "Coupling ($count)"

    fun healthText(health: Double): String = decimal(health, if (health >= 9.95) 0 else 1)

    fun healthShare(health: Double): Double = ((health - 1) / 9.0).coerceIn(0.02, 1.0)

    fun dateText(time: Long, zone: ZoneId = ZoneId.systemDefault()): String =
        DateTimeFormatter.ofPattern("d MMM yyyy", Locale.US).format(Instant.ofEpochSecond(time).atZone(zone))

    fun monthText(time: Long, zone: ZoneId = ZoneId.systemDefault()): String =
        DateTimeFormatter.ofPattern("MMM yyyy", Locale.US).format(Instant.ofEpochSecond(time).atZone(zone))

    fun niceMax(value: Double): Double {
        val maxV = max(1.0, value)
        val magnitude = 10.0.pow(floor(log10(maxV)))
        for (m in doubleArrayOf(1.0, 2.0, 2.5, 5.0, 10.0)) if (m * magnitude >= maxV) return m * magnitude
        return 10 * magnitude
    }

    fun compact(value: Double): String = when {
        abs(value) >= 10000 -> decimal(value / 1000.0, 0) + "k"
        abs(value) >= 1000 -> decimal(value / 1000.0, 1) + "k"
        else -> number(with(Ember) { value.roundHalfAway() })
    }

    fun trendX(points: List<InsightTrendPoint>, index: Int): Double {
        if (points.size < 2) return 0.5
        val t0 = points.first().time
        val t1 = points.last().time
        return if (t1 > t0) (points[index].time - t0).toDouble() / (t1 - t0).toDouble() else index.toDouble() / (points.size - 1)
    }

    fun nearestTrendPoint(points: List<InsightTrendPoint>, fraction: Double): Int {
        var best = -1
        var bestD = Double.MAX_VALUE
        for (i in points.indices) {
            val d = abs(trendX(points, i) - fraction)
            if (d < bestD) {
                bestD = d
                best = i
            }
        }
        return best
    }

    fun trendInfo(point: InsightTrendPoint, zone: ZoneId = ZoneId.systemDefault()): String =
        "${point.commit.take(8)} · ${point.author} · ${dateText(point.time, zone)}\nHealth ${decimal(point.health)} · worst function ${number(point.maxComplexity)}"
}
