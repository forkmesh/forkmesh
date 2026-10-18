package com.forkmesh.nativeapp.codeinsight

import java.util.Locale
import kotlin.math.floor
import kotlin.math.ln1p
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

enum class EmberTab(val title: String) {
    Hotspots("Hotspots"),
    RefactoringTargets("Refactoring targets"),
    CodeHealth("Code health"),
    ChangeCoupling("Change coupling"),
    Knowledge("Knowledge"),
    Defects("Defects"),
    Languages("Languages"),
    ;

    val needsHistory: Boolean get() = this != CodeHealth && this != Languages
}

data class EmberFilters(
    val minHealth: Double = 1.0,
    val maxHealth: Double = 10.0,
    val minCommits: Int = 0,
    val author: String = "",
    val search: String = "",
) {
    val active: Boolean
        get() = minHealth > 1 || maxHealth < 10 || minCommits > 0 || author.isNotEmpty() || search.trim().isNotEmpty()
}

data class EmberCouplingRef(val file: Int, val shared: Int, val degree: Double)

data class EmberTarget(val file: Int, val rank: Int, val score: Double, val reasons: List<String>)

data class EmberColor(val r: Double, val g: Double, val b: Double, val a: Double = 1.0) {
    val argb: Int
        get() {
            fun c(v: Double) = (v.coerceIn(0.0, 1.0) * 255 + 0.5).toInt()
            return (c(a) shl 24) or (c(r) shl 16) or (c(g) shl 8) or c(b)
        }

    val hex: String get() = String.format(Locale.US, "#%06x", argb and 0xffffff)

    companion object {
        fun hex(value: String): EmberColor {
            val v = value.removePrefix("#").toLong(16)
            return EmberColor(((v shr 16) and 0xff) / 255.0, ((v shr 8) and 0xff) / 255.0, (v and 0xff) / 255.0)
        }
    }
}

data class EmberLegendEntry(val label: String, val color: EmberColor?, val ring: Boolean = false)

data class EmberFileStyle(
    val color: EmberColor,
    val dimmed: Boolean = false,
    val glow: Double = 0.0,
    val rank: Int = 0,
    val marked: Boolean = false,
)

data class EmberColoring(
    val tab: EmberTab,
    val files: List<EmberFileStyle>,
    val legend: List<EmberLegendEntry> = emptyList(),
    val scale: Boolean = false,
    val scaleLow: EmberColor? = null,
    val scaleHigh: EmberColor? = null,
    val scaleLowLabel: String = "",
    val scaleHighLabel: String = "",
    val note: String = "",
    val targets: List<EmberTarget> = emptyList(),
    val glowing: Int = 0,
    val matching: Int = 0,
)

data class EmberSummary(
    val unhealthy: Int,
    val problematic: Int,
    val healthy: Int,
    val unhealthyLines: Long,
    val problematicLines: Long,
    val healthyLines: Long,
    val topHotspot: Int,
    val text: String,
)

object Ember {
    val healthyColor = EmberColor.hex("#3fb98f")
    val problematicColor = EmberColor.hex("#e3b341")
    val unhealthyColor = EmberColor.hex("#e5534b")
    val neutral = EmberColor.hex("#6e7681")
    val couplingLink = EmberColor.hex("#f2cc60")
    val hotspotGrey = EmberColor.hex("#6e7681")
    val hotspotOrange = EmberColor.hex("#f0883e")
    val hotspotRed = EmberColor.hex("#da3633")
    val glowColor = EmberColor.hex("#ff7b52")
    val couplingWeak = EmberColor.hex("#7d6a3a")
    val quiet = EmberColor.hex("#3d444d")
    val defectsLow = EmberColor.hex("#e3a8a2")
    val defectsHigh = EmberColor.hex("#d1242f")
    val palette: List<EmberColor> = listOf(
        "#3987e5", "#d95926", "#199e70", "#c98500", "#d55181", "#008300", "#9085e9", "#e66767",
    ).map(EmberColor::hex)

    val tabs: List<EmberTab> = EmberTab.entries

    fun bandColor(band: HealthBand): EmberColor = when (band) {
        HealthBand.Healthy -> healthyColor
        HealthBand.Problematic -> problematicColor
        HealthBand.Unhealthy -> unhealthyColor
    }

    fun healthColor(health: Double): EmberColor = bandColor(Insight.healthBand(health))

    fun mix(a: EmberColor, b: EmberColor, value: Double): EmberColor {
        val t = value.coerceIn(0.0, 1.0)
        fun channel(x: Double, y: Double): Double {
            val f = (x + (y - x) * t).toFloat()
            return Math.round(f * 65535f) / 65535.0
        }
        return EmberColor(channel(a.r, b.r), channel(a.g, b.g), channel(a.b, b.b), channel(a.a, b.a))
    }

    fun hasHistory(snapshot: InsightSnapshot): Boolean {
        if (!snapshot.git) return false
        if (snapshot.commitsScanned > 0) return true
        return snapshot.files.any { it.commits > 0 }
    }

    fun matchesFilters(file: InsightFile, filters: EmberFilters): Boolean {
        if (file.health < filters.minHealth - 1e-9 || file.health > filters.maxHealth + 1e-9) return false
        if (file.commits < filters.minCommits) return false
        if (filters.author.isNotEmpty() && file.mainAuthor != filters.author && file.topAuthors.none { it.name == filters.author }) return false
        val search = filters.search.trim()
        if (search.isNotEmpty() && !file.path.contains(search, ignoreCase = true)) return false
        return true
    }

    fun buildCouplingIndex(snapshot: InsightSnapshot): List<List<EmberCouplingRef>> {
        val index = List(snapshot.files.size) { ArrayList<EmberCouplingRef>() }
        for (c in snapshot.couplings) {
            if (c.a < 0 || c.b < 0 || c.a >= index.size || c.b >= index.size || c.a == c.b) continue
            index[c.a].add(EmberCouplingRef(c.b, c.shared, c.degree))
            index[c.b].add(EmberCouplingRef(c.a, c.shared, c.degree))
        }
        val order = compareByDescending<EmberCouplingRef> { it.degree }.thenByDescending { it.shared }.thenBy { it.file }
        for (list in index) list.sortWith(order)
        return index
    }

    fun targetScore(file: InsightFile): Double = max(0.0, file.hotspot) * max(0.0, 10.0 - file.health)

    fun refactoringTargets(snapshot: InsightSnapshot, coupling: List<List<EmberCouplingRef>>, limit: Int = 15): List<EmberTarget> {
        val scored = ArrayList<Pair<Int, Double>>()
        snapshot.files.forEachIndexed { i, file ->
            val s = targetScore(file)
            if (s > 0.05 && file.commits > 0) scored.add(i to s)
        }
        scored.sortWith { a, b ->
            if (a.second != b.second) b.second.compareTo(a.second) else snapshot.files[a.first].path.compareTo(snapshot.files[b.first].path)
        }
        val kept = if (limit > 0 && scored.size > limit) scored.subList(0, limit) else scored
        return kept.mapIndexed { r, (index, score) ->
            val file = snapshot.files[index]
            val reasons = ArrayList<String>()
            reasons.add("Changed in ${number(file.commits)} commits (${number(file.recentCommits)} in the last 90 days)")
            reasons.add("Code health ${decimal(file.health)}, ${Insight.healthBand(file.health).label.lowercase()}")
            for (k in 0 until min(2, file.healthIssues.size)) reasons.add(file.healthIssues[k])
            if (file.fixCommits > 0) reasons.add("${number(file.fixCommits)} bug-fix commits")
            if (index < coupling.size && coupling[index].isNotEmpty()) reasons.add("Changes together with ${number(coupling[index].size)} other files")
            if (file.formerAuthorShare >= 0.5) reasons.add("${percentText(file.formerAuthorShare)} written by former contributors")
            EmberTarget(index, r + 1, score, reasons)
        }
    }

    private fun rankedNames(snapshot: InsightSnapshot, limit: Int, key: (InsightFile) -> String): List<String> {
        val lines = HashMap<String, Long>()
        for (file in snapshot.files) {
            val name = key(file)
            if (name.isNotEmpty()) lines[name] = (lines[name] ?: 0L) + max(1, file.codeLines)
        }
        val names = lines.keys.sortedWith { a, b ->
            val la = lines.getValue(a)
            val lb = lines.getValue(b)
            if (la != lb) lb.compareTo(la) else a.compareTo(b)
        }
        return if (limit > 0) names.take(limit) else names
    }

    fun topAuthors(snapshot: InsightSnapshot, limit: Int): List<String> = rankedNames(snapshot, limit) { it.mainAuthor }

    fun topLanguages(snapshot: InsightSnapshot, limit: Int): List<String> = rankedNames(snapshot, limit) { it.language }

    private fun logShare(value: Double, max: Double): Double {
        if (value <= 0 || max <= 0) return 0.0
        return (ln1p(value) / ln1p(max)).coerceIn(0.0, 1.0)
    }

    fun colorize(snapshot: InsightSnapshot, tab: EmberTab, filters: EmberFilters, coupling: List<List<EmberCouplingRef>>): EmberColoring {
        val n = snapshot.files.size
        val colors = MutableList(n) { neutral }
        val dimmed = BooleanArray(n) { !matchesFilters(snapshot.files[it], filters) }
        val glow = DoubleArray(n)
        val rank = IntArray(n)
        val marked = BooleanArray(n)
        if (tab.needsHistory && !hasHistory(snapshot)) {
            return EmberColoring(
                tab = tab,
                files = List(n) { EmberFileStyle(neutral, dimmed = true) },
                note = "${tab.title} need git history. This folder has none, so only Code health and Languages can be shown.",
            )
        }
        val legend = ArrayList<EmberLegendEntry>()
        var scale = false
        var scaleLow: EmberColor? = null
        var scaleHigh: EmberColor? = null
        var scaleLowLabel = ""
        var scaleHighLabel = ""
        var note = ""
        var targets: List<EmberTarget> = emptyList()
        var glowing = 0
        when (tab) {
            EmberTab.Hotspots -> {
                val maxCommits = snapshot.files.maxOfOrNull { it.commits } ?: 0
                for (i in 0 until n) {
                    val commits = snapshot.files[i].commits
                    val t = logShare(commits.toDouble(), max(0, maxCommits).toDouble()).pow(1.6)
                    colors[i] = when {
                        commits <= 0 -> neutral
                        t < 0.5 -> mix(hotspotGrey, hotspotOrange, t / 0.5)
                        else -> mix(hotspotOrange, hotspotRed, (t - 0.5) / 0.5)
                    }
                }
                val order = topHotspotOrder(snapshot)
                for (r in 0 until min(12, order.size)) {
                    val file = snapshot.files[order[r]]
                    if (file.hotspot < 0.35) break
                    glow[order[r]] = file.hotspot
                    glowing++
                }
                scale = true
                scaleLow = hotspotGrey
                scaleHigh = hotspotRed
                scaleLowLabel = "Rarely changed"
                scaleHighLabel = "Changed often"
                legend.add(EmberLegendEntry("Top hotspot", glowColor, true))
            }
            EmberTab.RefactoringTargets -> {
                targets = refactoringTargets(snapshot, coupling, 15)
                for (t in targets) rank[t.file] = t.rank
                for (i in 0 until n) {
                    colors[i] = healthColor(snapshot.files[i].health)
                    if (rank[i] == 0) dimmed[i] = true
                }
                legend.add(EmberLegendEntry("Unhealthy target", unhealthyColor))
                legend.add(EmberLegendEntry("Problematic target", problematicColor))
                if (targets.isEmpty()) note = "No refactoring targets: every frequently changed file is healthy."
            }
            EmberTab.CodeHealth -> {
                for (i in 0 until n) colors[i] = healthColor(snapshot.files[i].health)
                legend.add(EmberLegendEntry("Healthy", healthyColor))
                legend.add(EmberLegendEntry("Problematic", problematicColor))
                legend.add(EmberLegendEntry("Unhealthy", unhealthyColor))
            }
            EmberTab.ChangeCoupling -> {
                var coupled = 0
                for (i in 0 until n) {
                    if (i >= coupling.size || coupling[i].isEmpty()) continue
                    coupled++
                    colors[i] = mix(couplingWeak, couplingLink, ((coupling[i][0].degree - 0.2) / 0.8).coerceIn(0.0, 1.0))
                }
                scale = true
                scaleLow = couplingWeak
                scaleHigh = couplingLink
                scaleLowLabel = "Weak"
                scaleHighLabel = "Strong coupling"
                if (coupled == 0) note = "No files change together often enough to count as coupled."
            }
            EmberTab.Knowledge -> {
                val authors = topAuthors(snapshot, palette.size)
                val slot = authors.withIndex().associate { it.value to it.index }
                val former = snapshot.formerAuthors.toSet()
                val anyQuiet = snapshot.files.any { it.mainAuthor.isEmpty() }
                for (i in 0 until n) {
                    val file = snapshot.files[i]
                    val s = slot[file.mainAuthor] ?: -1
                    colors[i] = if (s >= 0) palette[s] else if (file.mainAuthor.isEmpty()) quiet else neutral
                    marked[i] = file.formerAuthorShare >= 0.5
                }
                authors.forEachIndexed { a, name ->
                    legend.add(EmberLegendEntry(if (name in former) "$name (former)" else name, palette[a]))
                }
                if (topAuthors(snapshot, 0).size > authors.size) legend.add(EmberLegendEntry("Other authors", neutral))
                if (anyQuiet) legend.add(EmberLegendEntry("No commits in the window", quiet))
                legend.add(EmberLegendEntry("Mostly former contributors", null, true))
            }
            EmberTab.Defects -> {
                val maxFixes = snapshot.files.maxOfOrNull { it.fixCommits } ?: 0
                for (i in 0 until n) {
                    val fixes = snapshot.files[i].fixCommits
                    colors[i] = if (fixes > 0) mix(defectsLow, defectsHigh, logShare(fixes.toDouble(), max(0, maxFixes).toDouble())) else neutral
                }
                scale = true
                scaleLow = defectsLow
                scaleHigh = defectsHigh
                scaleLowLabel = "Few fixes"
                scaleHighLabel = "Many fixes"
                if (maxFixes <= 0) note = "No bug-fix commits found in the history window."
            }
            EmberTab.Languages -> {
                val languages = topLanguages(snapshot, palette.size)
                val slot = languages.withIndex().associate { it.value to it.index }
                for (i in 0 until n) {
                    val s = slot[snapshot.files[i].language] ?: -1
                    colors[i] = if (s >= 0) palette[s] else neutral
                }
                languages.forEachIndexed { l, name -> legend.add(EmberLegendEntry(name, palette[l])) }
                if (topLanguages(snapshot, 0).size > languages.size) legend.add(EmberLegendEntry("Other", neutral))
            }
        }
        val files = List(n) { EmberFileStyle(colors[it], dimmed[it], glow[it], rank[it], marked[it]) }
        return EmberColoring(
            tab, files, legend, scale, scaleLow, scaleHigh, scaleLowLabel, scaleHighLabel, note, targets, glowing,
            files.count { !it.dimmed },
        )
    }

    fun topHotspotOrder(snapshot: InsightSnapshot): List<Int> = snapshot.files.indices
        .filter { snapshot.files[it].hotspot > 0 }
        .sortedWith { a, b ->
            val ha = snapshot.files[a].hotspot
            val hb = snapshot.files[b].hotspot
            if (ha != hb) hb.compareTo(ha) else snapshot.files[a].path.compareTo(snapshot.files[b].path)
        }

    fun summarize(snapshot: InsightSnapshot): EmberSummary {
        var unhealthy = 0
        var problematic = 0
        var healthy = 0
        var unhealthyLines = 0L
        var problematicLines = 0L
        var healthyLines = 0L
        var best = 0.0
        var top = -1
        snapshot.files.forEachIndexed { i, file ->
            when (Insight.healthBand(file.health)) {
                HealthBand.Healthy -> {
                    healthy++
                    healthyLines += file.codeLines
                }
                HealthBand.Problematic -> {
                    problematic++
                    problematicLines += file.codeLines
                }
                HealthBand.Unhealthy -> {
                    unhealthy++
                    unhealthyLines += file.codeLines
                }
            }
            if (file.hotspot > best) {
                best = file.hotspot
                top = i
            }
        }
        val parts = mutableListOf("${number(unhealthy)} unhealthy", "${number(problematic)} problematic")
        if (top >= 0) parts.add("top hotspot ${snapshot.files[top].path}")
        return EmberSummary(unhealthy, problematic, healthy, unhealthyLines, problematicLines, healthyLines, top, parts.joinToString(" · "))
    }

    fun number(value: Int): String = String.format(Locale.US, "%,d", value)

    fun number(value: Long): String = String.format(Locale.US, "%,d", value)

    fun decimal(value: Double, digits: Int = 1): String = String.format(Locale.US, "%,.${digits}f", value)

    fun fileNameOf(path: String): String {
        val slash = path.lastIndexOf('/')
        return if (slash < 0) path else path.substring(slash + 1)
    }

    fun folderOf(path: String): String {
        val slash = path.lastIndexOf('/')
        return if (slash < 0) "" else path.substring(0, slash)
    }

    fun spanText(days: Int): String {
        if (days >= 330) {
            val years = max(1, (days / 365.0).roundHalfAway().toInt())
            return if (years == 1) "1 year" else "$years years"
        }
        if (days >= 45) {
            val months = max(1, (days / 30.4).roundHalfAway().toInt())
            return "$months months"
        }
        return if (days == 1) "1 day" else "${max(0, days)} days"
    }

    fun agoText(time: Long, now: Long): String {
        if (time <= 0) return "never"
        val days = max(0L, (now - time) / 86400)
        return when {
            days == 0L -> "today"
            days == 1L -> "yesterday"
            days < 14 -> "$days days ago"
            days < 60 -> "${days / 7} weeks ago"
            days < 365 -> "${max(2L, (days / 30.4).roundHalfAway())} months ago"
            else -> {
                val years = (days / 365.0).roundHalfAway()
                if (years <= 1) "1 year ago" else "$years years ago"
            }
        }
    }

    fun percentText(share: Double): String = "${(share.coerceIn(0.0, 1.0) * 100).roundHalfAway()} %"

    fun Double.roundHalfAway(): Long = if (this >= 0) floor(this + 0.5).toLong() else -floor(-this + 0.5).toLong()
}
