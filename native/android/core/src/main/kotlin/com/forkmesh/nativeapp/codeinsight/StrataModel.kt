package com.forkmesh.nativeapp.codeinsight

import java.util.Locale
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

enum class StrataSeverity(val label: String) {
    Critical("Critical"),
    High("High"),
    Medium("Medium"),
    Low("Low"),
}

enum class StrataCategory(val label: String) {
    Architecture("Architecture"),
    Design("Design"),
    CodeSmells("Code smells"),
    Maintainability("Maintainability"),
}

enum class StrataGateStatus(val label: String) {
    Pass("Pass"),
    Warn("Warn"),
    Fail("Fail"),
}

data class StrataViolation(
    val file: Int = -1,
    val module: Int = -1,
    val line: Int = 0,
    val severity: StrataSeverity = StrataSeverity.Medium,
    val message: String = "",
    val debt: Double = 0.0,
    val peerModule: Int = -1,
)

data class StrataRule(
    val id: String,
    val title: String,
    val category: StrataCategory,
    val severity: StrataSeverity,
    val description: String,
    val howToFix: String,
    val moduleScope: Boolean,
    val violations: List<StrataViolation> = emptyList(),
    val debt: Double = 0.0,
) {
    val worst: Int
        get() = violations.minOfOrNull { it.severity.ordinal } ?: -1
}

data class StrataGate(
    val name: String,
    val status: StrataGateStatus,
    val measured: String,
    val detail: String,
)

data class StrataScore(
    var debt: Double = 0.0,
    var effort: Double = 0.0,
    var ratio: Double = 0.0,
    var rating: Int = 0,
    var interest: Double = 0.0,
    var violations: Int = 0,
    var worst: Int = -1,
)

data class StrataMetrics(
    var codeLines: Int = 0,
    var commentLines: Int = 0,
    var files: Int = 0,
    var modules: Int = 0,
    var types: Int = 0,
    var functions: Int = 0,
    var averageComplexity: Double = 0.0,
    var maxComplexity: Int = 0,
    var maxComplexityFunction: String = "",
    var maxComplexityFile: Int = -1,
    var commentPercent: Double = 0.0,
    var duplicatedPercent: Double = 0.0,
    var testFiles: Int = 0,
)

data class StrataRef(val rule: Int, val index: Int)

class StrataReport(
    val graph: InsightModuleGraph,
    val rules: List<StrataRule>,
    val files: List<StrataScore>,
    val modules: List<StrataScore>,
    val fileViolations: List<List<StrataRef>>,
    val moduleViolations: List<List<StrataRef>>,
    val total: StrataScore,
    val gates: List<StrataGate>,
    val metrics: StrataMetrics,
    val bySeverity: List<Int>,
    val history: Boolean,
) {
    fun ruleIndex(id: String): Int = rules.indexOfFirst { it.id == id }

    fun gatesWith(status: StrataGateStatus): Int = gates.count { it.status == status }

    fun violation(ref: StrataRef): StrataViolation = rules[ref.rule].violations[ref.index]

    fun bySeverityThenDebt(refs: List<StrataRef>): List<StrataRef> = refs.sortedWith { a, b ->
        val va = violation(a)
        val vb = violation(b)
        when {
            va.severity != vb.severity -> va.severity.ordinal.compareTo(vb.severity.ordinal)
            va.debt > vb.debt -> -1
            va.debt < vb.debt -> 1
            else -> 0
        }
    }

    fun worstForModule(module: Int, limit: Int): List<StrataRef> {
        if (module < 0 || module >= graph.nodes.size) return emptyList()
        val refs = ArrayList<StrataRef>()
        if (module < moduleViolations.size) refs += moduleViolations[module]
        for (file in graph.nodes[module].files) {
            if (file >= 0 && file < fileViolations.size) refs += fileViolations[file]
        }
        val sorted = bySeverityThenDebt(refs)
        return if (limit > 0 && sorted.size > limit) sorted.subList(0, limit) else sorted
    }

    fun worstForFile(file: Int): List<StrataRef> =
        bySeverityThenDebt(if (file >= 0 && file < fileViolations.size) fileViolations[file] else emptyList())
}

class StrataModel(
    val snapshot: InsightSnapshot,
    val report: StrataReport,
    val layout: StrataGraphLayout,
    val order: List<Int>,
    val depth: Int,
)

object Strata {
    private const val ModuleCycles = 0
    private const val FileCycles = 1
    private const val StableDependencies = 2
    private const val ZoneOfPain = 3
    private const val ZoneOfUselessness = 4
    private const val HubFiles = 5
    private const val TooManyImports = 6
    private const val ComplexFunctions = 7
    private const val LongFunctions = 8
    private const val DeepNesting = 9
    private const val TooManyParameters = 10
    private const val BrainMethods = 11
    private const val LargeFiles = 12
    private const val DuplicatedCode = 13
    private const val UncommentedComplexity = 14
    private const val KnowledgeIslands = 15
    private const val FormerContributors = 16

    private fun capped(value: Double, cap: Double): Double = value.coerceIn(0.0, cap)

    fun fileName(path: String): String {
        val slash = path.lastIndexOf('/')
        return if (slash < 0) path else path.substring(slash + 1)
    }

    fun roundHalfAway(value: Double): Double =
        if (value < 0) -Math.round(-value).toDouble() else Math.round(value).toDouble()

    fun number(value: Int): String = String.format(Locale.US, "%,d", value)

    fun number(value: Long): String = String.format(Locale.US, "%,d", value)

    fun decimal(value: Double, digits: Int): String = String.format(Locale.US, "%,.${digits}f", value)

    fun fixed(value: Double, digits: Int): String = String.format(Locale.US, "%.${digits}f", value)

    fun effortMinutes(codeLines: Int): Double = max(0, codeLines) * 18.0 * 8.0 * 60.0 / 1000.0

    fun ratingFor(ratio: Double): Int = when {
        ratio <= 0.05 -> 0
        ratio <= 0.10 -> 1
        ratio <= 0.20 -> 2
        ratio <= 0.50 -> 3
        else -> 4
    }

    fun ratingLetter(rating: Int): Char = 'A' + rating.coerceIn(0, 4)

    fun formatDebt(minutes: Double): String {
        val total = max(0L, Math.round(minutes))
        if (total < 60) return "$total min"
        val hours = total / 60
        val rest = total % 60
        if (hours < 8) return if (rest != 0L) "$hours h $rest min" else "$hours h"
        val days = hours / 8
        val hourRest = hours % 8
        val dayText = number(days)
        if (days >= 100 || hourRest == 0L) return "$dayText d"
        return "$dayText d $hourRest h"
    }

    fun formatPercent(ratio: Double): String {
        val percent = ratio * 100.0
        if (percent > 0 && percent < 0.1) return "<0.1 %"
        return "${decimal(percent, if (percent >= 10) 0 else 1)} %"
    }

    fun ruleCatalog(): List<StrataRule> = listOf(
        StrataRule(
            "ARC01", "Avoid module dependency cycles", StrataCategory.Architecture, StrataSeverity.Critical,
            "Modules that depend on each other, directly or through other modules, can only be " +
                "understood, tested, built and released together. A cycle turns separate layers into " +
                "one big tangle and every change risks rippling around the loop.",
            "Find the weakest edge of the cycle (fewest imports, shown in red) and remove it: move " +
                "the shared code down into a lower module, or invert the dependency with an interface " +
                "or callback owned by the lower module.",
            true,
        ),
        StrataRule(
            "ARC02", "Avoid file import cycles", StrataCategory.Architecture, StrataSeverity.High,
            "Files that import each other in a loop cannot be read, reused or tested on their own " +
                "and make initialization order fragile.",
            "Extract the definitions both files need into a third file that imports neither, or " +
                "pass the dependency in instead of importing it.",
            false,
        ),
        StrataRule(
            "ARC03", "Depend in the direction of stability", StrataCategory.Architecture, StrataSeverity.Medium,
            "Stable dependencies principle: a module should only depend on modules that are more " +
                "stable than itself. When a stable module (many dependents, few dependencies) depends on a " +
                "clearly less stable one, changes in the volatile module ripple into everything that " +
                "relies on the stable one.",
            "Move the code the stable module needs into it or into a lower module, or let the stable " +
                "module define an abstraction that the volatile module implements.",
            true,
        ),
        StrataRule(
            "ARC04", "Modules in the zone of pain", StrataCategory.Architecture, StrataSeverity.Medium,
            "A module that is concrete (few abstract types) and stable (used by many, depends on few) " +
                "is hard to change because every dependent breaks, yet offers no abstractions to extend " +
                "it. It sits far below the main sequence in the abstractness × instability diagram.",
            "Introduce interfaces or abstract types that dependents use instead of the concrete " +
                "code, or split the module so that fewer modules depend on the concrete parts.",
            true,
        ),
        StrataRule(
            "ARC05", "Modules in the zone of uselessness", StrataCategory.Architecture, StrataSeverity.Low,
            "A module that is mostly abstract yet has no dependents is abstraction nobody uses: " +
                "interfaces without callers, speculative extension points.",
            "Remove the unused abstractions, or merge them into the module that implements them.",
            true,
        ),
        StrataRule(
            "DES01", "Avoid hub files", StrataCategory.Design, StrataSeverity.Medium,
            "A file that many files import and that itself imports many files is a hub: every change " +
                "to its dependencies can break its many dependents. Hubs are usually god objects or " +
                "grab-bag utility files.",
            "Split the file by responsibility so that each dependent imports only the part it needs.",
            false,
        ),
        StrataRule(
            "DES02", "Avoid too many imports in one file", StrataCategory.Design, StrataSeverity.Low,
            "A file with a very long import list knows too much about the rest of the system and " +
                "is likely doing several jobs.",
            "Split the file along its responsibilities, or group related imports behind a facade.",
            false,
        ),
        StrataRule(
            "CS01", "Avoid complex functions", StrataCategory.CodeSmells, StrataSeverity.High,
            "Cyclomatic complexity counts the independent paths through a function. Above 10 a " +
                "function becomes hard to test fully; above 20 it is hard to understand at all.",
            "Extract the branches into well-named helper functions, replace conditionals with " +
                "lookup tables or polymorphism, and return early instead of nesting.",
            false,
        ),
        StrataRule(
            "CS02", "Avoid long functions", StrataCategory.CodeSmells, StrataSeverity.Medium,
            "Functions longer than 60 lines rarely do one thing; above 100 lines they are hard to " +
                "name, read and reuse.",
            "Extract cohesive blocks into functions whose names say what the block does.",
            false,
        ),
        StrataRule(
            "CS03", "Avoid deep nesting", StrataCategory.CodeSmells, StrataSeverity.Medium,
            "Code nested more than five levels deep forces the reader to hold every enclosing " +
                "condition in mind.",
            "Return early, invert conditions, and extract inner loops into their own functions.",
            false,
        ),
        StrataRule(
            "CS04", "Avoid functions with too many parameters", StrataCategory.CodeSmells, StrataSeverity.Low,
            "More than six parameters make calls hard to read and easy to get wrong.",
            "Group related parameters into a struct or options object, or split the function.",
            false,
        ),
        StrataRule(
            "CS05", "Avoid brain methods", StrataCategory.CodeSmells, StrataSeverity.High,
            "A brain method is long, complex and deeply nested at the same time: it centralizes the " +
                "logic of its file and is where most bugs hide.",
            "Break it up step by step: extract each nested block into a named function, then " +
                "simplify the remaining control flow.",
            false,
        ),
        StrataRule(
            "MNT01", "Avoid large files", StrataCategory.Maintainability, StrataSeverity.Medium,
            "Files with more than 1,000 lines of code are hard to navigate and attract merge " +
                "conflicts; above 2,000 lines they usually hold several unrelated concerns.",
            "Split the file by responsibility, one concept per file.",
            false,
        ),
        StrataRule(
            "MNT02", "Avoid duplicated code", StrataCategory.Maintainability, StrataSeverity.Medium,
            "Copied blocks must be fixed in every copy; a bug fixed in one copy lives on in the " +
                "others.",
            "Extract the repeated block into a shared function and call it from each place.",
            false,
        ),
        StrataRule(
            "MNT03", "Comment complex files", StrataCategory.Maintainability, StrataSeverity.Low,
            "A complex file with almost no comments or documentation leaves its reasoning in its " +
                "author's head.",
            "Document why the code is shaped the way it is, near the code or in the project's docs.",
            false,
        ),
        StrataRule(
            "MNT04", "Avoid knowledge islands", StrataCategory.Maintainability, StrataSeverity.Medium,
            "One person wrote more than 90 % of a file that changes often. When they are away, " +
                "nobody else knows the code.",
            "Pair on the next changes, review them with a second person, and write down how the " +
                "file works.",
            false,
        ),
        StrataRule(
            "MNT05", "Hotspots written by former contributors", StrataCategory.Maintainability, StrataSeverity.Medium,
            "Most of this frequently changed code was written by people who have not committed in " +
                "the last six months. The knowledge behind it has left with them.",
            "Give the file a current owner, cover it with tests before changing it, and document " +
                "what you learn.",
            false,
        ),
    )

    fun components(adjacency: List<List<Int>>): List<List<Int>> {
        val n = adjacency.size
        val index = IntArray(n) { -1 }
        val low = IntArray(n)
        val onStack = BooleanArray(n)
        val stack = ArrayList<Int>()
        val callNode = ArrayList<Int>()
        val callNext = ArrayList<Int>()
        val out = ArrayList<List<Int>>()
        var counter = 0
        for (start in 0 until n) {
            if (index[start] >= 0) continue
            index[start] = counter
            low[start] = counter
            counter++
            stack.add(start)
            onStack[start] = true
            callNode.add(start)
            callNext.add(0)
            while (callNode.isNotEmpty()) {
                val top = callNode.size - 1
                val v = callNode[top]
                val next = callNext[top]
                if (next < adjacency[v].size) {
                    callNext[top] = next + 1
                    val w = adjacency[v][next]
                    if (w < 0 || w >= n) continue
                    if (index[w] < 0) {
                        index[w] = counter
                        low[w] = counter
                        counter++
                        stack.add(w)
                        onStack[w] = true
                        callNode.add(w)
                        callNext.add(0)
                    } else if (onStack[w]) {
                        low[v] = min(low[v], index[w])
                    }
                    continue
                }
                callNode.removeAt(top)
                callNext.removeAt(top)
                if (callNode.isNotEmpty()) {
                    val parent = callNode[callNode.size - 1]
                    low[parent] = min(low[parent], low[v])
                }
                if (low[v] == index[v]) {
                    val component = ArrayList<Int>()
                    var w: Int
                    do {
                        w = stack.removeAt(stack.size - 1)
                        onStack[w] = false
                        component.add(w)
                    } while (w != v)
                    component.sort()
                    out.add(component)
                }
            }
        }
        return out
    }

    fun normalizeGraph(source: InsightModuleGraph): InsightModuleGraph {
        val nodes = source.nodes.map { it.copy() }
        val edges = source.edges.map { it.copy() }
        val n = nodes.size
        val adjacency = List(n) { ArrayList<Int>() }
        for (edge in edges) {
            if (edge.from in 0 until n && edge.to in 0 until n && edge.from != edge.to) adjacency[edge.from].add(edge.to)
        }
        val cycles = ArrayList<List<Int>>()
        for (node in nodes) node.cycle = -1
        for (part in components(adjacency)) {
            if (part.size < 2) continue
            for (v in part) nodes[v].cycle = cycles.size
            cycles.add(part)
        }
        for (edge in edges) {
            edge.inCycle = edge.from in 0 until n && edge.to in 0 until n &&
                nodes[edge.from].cycle >= 0 && nodes[edge.from].cycle == nodes[edge.to].cycle
        }
        val coupled = nodes.any { it.afferent > 0 || it.efferent > 0 }
        if (!coupled && edges.isNotEmpty()) {
            val incoming = List(n) { HashSet<Int>() }
            val outgoing = List(n) { HashSet<Int>() }
            for (edge in edges) {
                if (edge.from !in 0 until n || edge.to !in 0 until n || edge.from == edge.to) continue
                outgoing[edge.from].add(edge.to)
                incoming[edge.to].add(edge.from)
            }
            for (i in 0 until n) {
                val node = nodes[i]
                node.afferent = incoming[i].size
                node.efferent = outgoing[i].size
                val sum = node.afferent + node.efferent
                node.instability = if (sum != 0) node.efferent.toDouble() / sum else 0.0
                node.abstractness = if (node.types != 0) node.abstractTypes.toDouble() / node.types else 0.0
                node.distance = abs(node.abstractness + node.instability - 1.0)
            }
        }
        return InsightModuleGraph(source.depth, nodes, edges, cycles, source.moduleOfFile)
    }

    fun evaluateRules(snapshot: InsightSnapshot, graph: InsightModuleGraph): List<StrataRule> {
        val catalog = ruleCatalog()
        val found = List(catalog.size) { ArrayList<StrataViolation>() }
        val files = snapshot.files
        val fileCount = files.size
        fun moduleOf(file: Int): Int = if (file >= 0 && file < graph.moduleOfFile.size) graph.moduleOfFile[file] else -1
        fun add(rule: Int, violation: StrataViolation) {
            found[rule].add(violation.copy(debt = roundHalfAway(violation.debt)))
        }

        for (edge in graph.edges) {
            if (!edge.inCycle || edge.from < 0 || edge.to < 0) continue
            val from = graph.nodes[edge.from]
            val to = graph.nodes[edge.to]
            add(
                ModuleCycles,
                StrataViolation(
                    file = edge.fileEdges.firstOrNull()?.first ?: -1,
                    module = edge.from,
                    peerModule = edge.to,
                    severity = StrataSeverity.Critical,
                    message = "${from.name} uses ${to.name} (${edge.weight} imports) and ${to.name} depends back on ${from.name}",
                    debt = capped(30 + 15.0 * max(1, edge.weight), 480.0),
                ),
            )
        }

        run {
            val parts = components(files.map { it.dependsOn })
            for (part in parts) {
                if (part.size < 2) continue
                val members = part.toHashSet()
                for (f in part) {
                    val names = ArrayList<String>()
                    var inside = 0
                    for (d in files[f].dependsOn) {
                        if (d !in members) continue
                        inside++
                        if (names.size < 3) names.add(fileName(files[d].path))
                    }
                    add(
                        FileCycles,
                        StrataViolation(
                            file = f,
                            module = moduleOf(f),
                            severity = StrataSeverity.High,
                            message = "Part of an import cycle of ${part.size} files; imports " +
                                names.joinToString(", ") + (if (inside > names.size) " …" else ""),
                            debt = capped(15.0 * inside + 5.0 * min(part.size, 20), 240.0),
                        ),
                    )
                }
            }
        }

        for (edge in graph.edges) {
            if (edge.inCycle || edge.from < 0 || edge.to < 0) continue
            val from = graph.nodes[edge.from]
            val to = graph.nodes[edge.to]
            val gap = to.instability - from.instability
            if (gap <= 0.25) continue
            add(
                StableDependencies,
                StrataViolation(
                    file = edge.fileEdges.firstOrNull()?.first ?: -1,
                    module = edge.from,
                    peerModule = edge.to,
                    severity = StrataSeverity.Medium,
                    message = "${from.name} (I = ${fixed(from.instability, 2)}) depends on the less stable ${to.name} (I = ${fixed(to.instability, 2)})",
                    debt = capped(20 + 120.0 * (gap - 0.25) + 4.0 * edge.weight, 240.0),
                ),
            )
        }

        for ((i, node) in graph.nodes.withIndex()) {
            if (node.types < 1) continue
            if (node.codeLines >= 150 && node.afferent >= 2 && node.abstractness <= 0.2 && node.instability <= 0.3) {
                val over = ((node.distance - 0.5) / 0.5).coerceIn(0.0, 1.0)
                add(
                    ZoneOfPain,
                    StrataViolation(
                        module = i,
                        severity = StrataSeverity.Medium,
                        message = "Concrete and stable: A = ${fixed(node.abstractness, 2)}, I = ${fixed(node.instability, 2)}, used by ${node.afferent} modules",
                        debt = capped(30 + 150.0 * over * min(3.0, sqrt(node.codeLines / 1000.0)) + 5.0 * node.afferent, 960.0),
                    ),
                )
            }
            if (node.codeLines >= 50 && node.abstractness >= 0.7 && node.instability >= 0.7) {
                add(
                    ZoneOfUselessness,
                    StrataViolation(
                        module = i,
                        severity = StrataSeverity.Low,
                        message = "Abstract and unused: A = ${fixed(node.abstractness, 2)}, I = ${fixed(node.instability, 2)}",
                        debt = capped(20 + 100.0 * ((node.distance - 0.4) / 0.6).coerceIn(0.0, 1.0), 240.0),
                    ),
                )
            }
        }

        val changed = files.filter { it.commits > 0 }.map { it.commits }.sorted()
        val frequent = max(8, if (changed.isEmpty()) 0 else changed[(changed.size * 0.75).toInt()])
        val team = snapshot.authors.size >= 2

        for (f in 0 until fileCount) {
            val file = files[f]
            val module = moduleOf(f)
            fun fileViolation(severity: StrataSeverity, line: Int, message: String, debt: Double) =
                StrataViolation(file = f, module = module, line = line, severity = severity, message = message, debt = debt)
            val fanIn = file.dependedOnBy.size
            val fanOut = file.dependsOn.size
            if (fanIn >= 10 && fanOut >= 10) {
                add(
                    HubFiles,
                    fileViolation(
                        if (fanIn >= 25 && fanOut >= 25) StrataSeverity.High else StrataSeverity.Medium, 0,
                        "Imported by $fanIn files and imports $fanOut",
                        capped(30 + 3.0 * ((fanIn - 10) + (fanOut - 10)), 480.0),
                    ),
                )
            }
            val imports = max(file.imports.size, fanOut + file.externalImports.size)
            if (imports > 25) {
                add(
                    TooManyImports,
                    fileViolation(
                        if (imports > 50) StrataSeverity.Medium else StrataSeverity.Low, 1,
                        "$imports imports",
                        capped(10 + 2.0 * (imports - 25), 240.0),
                    ),
                )
            }
            for (fn in file.functions) {
                val name = fn.name.ifEmpty { "(anonymous)" }
                if (fn.complexity > 10) {
                    add(
                        ComplexFunctions,
                        fileViolation(
                            if (fn.complexity > 20) StrataSeverity.High else StrataSeverity.Medium, fn.line,
                            "$name has cyclomatic complexity ${fn.complexity}",
                            capped(15 + 6.0 * (fn.complexity - 10), 480.0),
                        ),
                    )
                }
                if (fn.length > 60) {
                    add(
                        LongFunctions,
                        fileViolation(
                            if (fn.length > 100) StrataSeverity.Medium else StrataSeverity.Low, fn.line,
                            "$name is ${fn.length} lines long",
                            capped(10 + 0.6 * (fn.length - 60), 480.0),
                        ),
                    )
                }
                if (fn.nesting > 5) {
                    add(
                        DeepNesting,
                        fileViolation(
                            if (fn.nesting > 7) StrataSeverity.High else StrataSeverity.Medium, fn.line,
                            "$name nests ${fn.nesting} levels deep",
                            capped(20 + 15.0 * (fn.nesting - 5), 240.0),
                        ),
                    )
                }
                if (fn.parameters > 6) {
                    add(
                        TooManyParameters,
                        fileViolation(
                            if (fn.parameters > 9) StrataSeverity.Medium else StrataSeverity.Low, fn.line,
                            "$name takes ${fn.parameters} parameters",
                            capped(10 + 6.0 * (fn.parameters - 6), 120.0),
                        ),
                    )
                }
                if (fn.length > 80 && fn.complexity > 15 && fn.nesting >= 4) {
                    add(
                        BrainMethods,
                        fileViolation(
                            StrataSeverity.High, fn.line,
                            "$name: ${fn.length} lines, complexity ${fn.complexity}, nesting ${fn.nesting}",
                            capped(60 + 0.5 * (fn.length - 80) + 5.0 * (fn.complexity - 15) + 15.0 * (fn.nesting - 4), 960.0),
                        ),
                    )
                }
            }
            if (file.codeLines > 1000) {
                add(
                    LargeFiles,
                    fileViolation(
                        if (file.codeLines > 2000) StrataSeverity.High else StrataSeverity.Medium, 1,
                        "${number(file.codeLines)} lines of code",
                        capped(30 + 0.06 * (file.codeLines - 1000), 960.0),
                    ),
                )
            }
            if (file.duplicatedLines >= 12) {
                add(
                    DuplicatedCode,
                    fileViolation(
                        if (file.duplicatedLines >= 100) StrataSeverity.Medium else StrataSeverity.Low, 1,
                        "${file.duplicatedLines} duplicated lines",
                        capped(10 + 1.5 * file.duplicatedLines, 960.0),
                    ),
                )
            }
            val commented = file.codeLines + file.commentLines
            if (!file.test && file.complexity >= 40 && file.codeLines >= 150 && commented > 0 &&
                file.commentLines.toDouble() / commented < 0.05
            ) {
                add(
                    UncommentedComplexity,
                    fileViolation(
                        StrataSeverity.Low, 1,
                        "Complexity ${file.complexity} with ${file.commentLines} comment lines",
                        capped(15 + 0.25 * (file.complexity - 40), 240.0),
                    ),
                )
            }
            if (team && file.commits >= frequent && file.mainAuthorShare > 0.9 && file.mainAuthor.isNotEmpty()) {
                add(
                    KnowledgeIslands,
                    fileViolation(
                        StrataSeverity.Medium, 0,
                        "${file.mainAuthor} wrote ${formatPercent(file.mainAuthorShare)} of ${file.commits} commits' lines",
                        capped(30 + 4.0 * (file.commits - frequent) + 0.02 * file.codeLines, 480.0),
                    ),
                )
            }
            if (file.commits > 0 && file.formerAuthorShare > 0.5 && file.hotspot >= 0.3) {
                val severe = file.formerAuthorShare > 0.8 && file.hotspot > 0.6
                add(
                    FormerContributors,
                    fileViolation(
                        if (severe) StrataSeverity.High else StrataSeverity.Medium, 0,
                        "${formatPercent(file.formerAuthorShare)} written by former contributors, hotspot ${fixed(file.hotspot, 2)}",
                        capped(20 + 200.0 * file.hotspot * file.formerAuthorShare, 480.0),
                    ),
                )
            }
        }

        return catalog.mapIndexed { r, rule ->
            val list = found[r]
            var sum = 0.0
            for (v in list) sum += v.debt
            val sorted = list.sortedWith { a, b ->
                when {
                    a.severity != b.severity -> a.severity.ordinal.compareTo(b.severity.ordinal)
                    a.debt > b.debt -> -1
                    a.debt < b.debt -> 1
                    else -> 0
                }
            }
            rule.copy(violations = sorted, debt = sum)
        }
    }

    fun evaluateGates(snapshot: InsightSnapshot, report: StrataReport): List<StrataGate> {
        val gates = ArrayList<StrataGate>()
        gates.add(
            StrataGate(
                "Technical debt rating",
                when {
                    report.total.rating >= 4 -> StrataGateStatus.Fail
                    report.total.rating == 3 -> StrataGateStatus.Warn
                    else -> StrataGateStatus.Pass
                },
                "${ratingLetter(report.total.rating)} · ${formatPercent(report.total.ratio)}",
                "Warns at D (debt above 20 % of the effort), fails at E (above 50 %).",
            ),
        )
        run {
            var critical = 0
            var rules = 0
            for (rule in report.rules) {
                val here = rule.violations.count { it.severity == StrataSeverity.Critical }
                critical += here
                if (here != 0) rules++
            }
            gates.add(
                StrataGate(
                    "No critical rule violated",
                    if (critical != 0) StrataGateStatus.Fail else StrataGateStatus.Pass,
                    if (critical != 0) "$critical in $rules ${if (rules == 1) "rule" else "rules"}" else "None",
                    "Fails when any violation has critical severity.",
                ),
            )
        }
        run {
            val cycles = report.graph.cycles.size
            gates.add(
                StrataGate(
                    "No module dependency cycle",
                    if (cycles != 0) StrataGateStatus.Fail else StrataGateStatus.Pass,
                    if (cycles != 0) "$cycles ${if (cycles == 1) "cycle" else "cycles"}" else "None",
                    "Fails when modules depend on each other in a loop.",
                ),
            )
        }
        run {
            val worst = report.metrics.maxComplexity
            gates.add(
                StrataGate(
                    "No function above complexity 50",
                    when {
                        worst > 50 -> StrataGateStatus.Fail
                        worst > 30 -> StrataGateStatus.Warn
                        else -> StrataGateStatus.Pass
                    },
                    if (report.metrics.maxComplexityFunction.isEmpty()) worst.toString()
                    else "$worst (${report.metrics.maxComplexityFunction})",
                    "Warns above 30, fails above 50.",
                ),
            )
        }
        run {
            val name = "Debt in the most-changed files"
            val changed = snapshot.files.indices.filter { snapshot.files[it].commits > 0 }
            if (changed.isEmpty()) {
                gates.add(
                    StrataGate(
                        name, StrataGateStatus.Pass, "No history",
                        "Needs git history: the debt ratio of the 10 % most-changed files must stay below 20 %.",
                    ),
                )
            } else {
                val sorted = changed.sortedWith { a, b -> snapshot.files[b].commits.compareTo(snapshot.files[a].commits) }
                val take = max(1, ceil(sorted.size * 0.1).toInt())
                var debt = 0.0
                var effort = 0.0
                for (i in 0 until take) {
                    val f = sorted[i]
                    if (f < report.files.size) {
                        debt += report.files[f].debt
                        effort += report.files[f].effort
                    }
                }
                val ratio = if (effort > 0) debt / effort else 0.0
                gates.add(
                    StrataGate(
                        name,
                        when {
                            ratio >= 0.2 -> StrataGateStatus.Fail
                            ratio >= 0.1 -> StrataGateStatus.Warn
                            else -> StrataGateStatus.Pass
                        },
                        "${formatPercent(ratio)} in $take files",
                        "Debt ratio of the 10 % most-changed files: warns at 10 %, fails at 20 %.",
                    ),
                )
            }
        }
        return gates
    }

    fun buildReport(snapshot: InsightSnapshot, source: InsightModuleGraph): StrataReport {
        var graph = normalizeGraph(source)
        val files = snapshot.files
        val fileCount = files.size
        val moduleCount = graph.nodes.size
        if (graph.moduleOfFile.size != fileCount) {
            val moduleOfFile = IntArray(fileCount) { -1 }
            for (m in 0 until moduleCount) {
                for (f in graph.nodes[m].files) if (f in 0 until fileCount) moduleOfFile[f] = m
            }
            graph = InsightModuleGraph(graph.depth, graph.nodes, graph.edges, graph.cycles, moduleOfFile.toList())
        }
        val rules = evaluateRules(snapshot, graph)
        val fileScores = List(fileCount) { StrataScore() }
        val moduleScores = List(moduleCount) { StrataScore() }
        val fileViolations = List(fileCount) { ArrayList<StrataRef>() }
        val moduleViolations = List(moduleCount) { ArrayList<StrataRef>() }
        val total = StrataScore()
        val bySeverity = IntArray(4)
        val years = if (snapshot.historyDays > 0) snapshot.historyDays / 365.0 else 1.0
        val history = files.any { it.commits > 0 }
        fun interestFor(debt: Double, commits: Int): Double {
            if (commits <= 0 || debt <= 0) return 0.0
            val perYear = commits / max(0.1, years)
            return min(2.0 * debt, debt * 0.05 * perYear)
        }
        fun mark(score: StrataScore, v: StrataViolation) {
            score.debt += v.debt
            score.violations++
            val s = v.severity.ordinal
            if (score.worst < 0 || s < score.worst) score.worst = s
        }
        for ((r, rule) in rules.withIndex()) {
            for ((i, v) in rule.violations.withIndex()) {
                bySeverity[v.severity.ordinal]++
                if (rule.moduleScope || v.file < 0) {
                    if (v.module in 0 until moduleCount) {
                        moduleViolations[v.module].add(StrataRef(r, i))
                        mark(moduleScores[v.module], v)
                    }
                } else if (v.file < fileCount) {
                    fileViolations[v.file].add(StrataRef(r, i))
                    mark(fileScores[v.file], v)
                }
                total.debt += v.debt
                total.violations++
                val s = v.severity.ordinal
                if (total.worst < 0 || s < total.worst) total.worst = s
            }
        }
        val m = StrataMetrics()
        var complexitySum = 0.0
        var duplicated = 0
        for (f in 0 until fileCount) {
            val file = files[f]
            val score = fileScores[f]
            score.effort = effortMinutes(file.codeLines)
            score.ratio = if (score.effort > 0) score.debt / score.effort else if (score.debt > 0) 1.0 else 0.0
            score.rating = ratingFor(score.ratio)
            score.interest = interestFor(score.debt, file.commits)
            total.interest += score.interest
            m.codeLines += file.codeLines
            m.commentLines += file.commentLines
            m.types += file.types
            m.functions += file.functions.size
            duplicated += file.duplicatedLines
            if (file.test) m.testFiles++
            for (fn in file.functions) {
                complexitySum += fn.complexity
                if (fn.complexity > m.maxComplexity) {
                    m.maxComplexity = fn.complexity
                    m.maxComplexityFunction = fn.name
                    m.maxComplexityFile = f
                }
            }
        }
        for (i in 0 until moduleCount) {
            val node = graph.nodes[i]
            val score = moduleScores[i]
            val ownDebt = score.debt
            var codeLines = 0
            var commits = 0
            for (f in node.files) {
                if (f < 0 || f >= fileCount) continue
                val fileScore = fileScores[f]
                score.debt += fileScore.debt
                score.violations += fileScore.violations
                score.interest += fileScore.interest
                if (fileScore.worst >= 0 && (score.worst < 0 || fileScore.worst < score.worst)) score.worst = fileScore.worst
                codeLines += files[f].codeLines
                commits = max(commits, files[f].commits)
            }
            if (node.codeLines > 0) codeLines = node.codeLines
            val ownInterest = interestFor(ownDebt, if (node.commits > 0) node.commits else commits)
            score.interest += ownInterest
            total.interest += ownInterest
            score.effort = effortMinutes(codeLines)
            score.ratio = if (score.effort > 0) score.debt / score.effort else if (score.debt > 0) 1.0 else 0.0
            score.rating = ratingFor(score.ratio)
        }
        m.files = fileCount
        m.modules = moduleCount
        m.averageComplexity = if (m.functions != 0) complexitySum / m.functions else 0.0
        m.commentPercent = if (m.codeLines + m.commentLines > 0) 100.0 * m.commentLines / (m.codeLines + m.commentLines) else 0.0
        m.duplicatedPercent = if (m.codeLines > 0) min(100.0, 100.0 * duplicated / m.codeLines) else 0.0
        total.effort = effortMinutes(m.codeLines)
        total.ratio = if (total.effort > 0) total.debt / total.effort else 0.0
        total.rating = ratingFor(total.ratio)
        val partial = StrataReport(
            graph, rules, fileScores, moduleScores, fileViolations, moduleViolations, total, emptyList(), m,
            bySeverity.toList(), history,
        )
        val gates = evaluateGates(snapshot, partial)
        return StrataReport(
            graph, rules, fileScores, moduleScores, fileViolations, moduleViolations, total, gates, m,
            bySeverity.toList(), history,
        )
    }

    fun summaryLine(report: StrataReport): String {
        val parts = ArrayList<String>()
        parts.add("Rating ${ratingLetter(report.total.rating)}")
        parts.add("${formatDebt(report.total.debt)} debt")
        val failing = report.gatesWith(StrataGateStatus.Fail)
        val warning = report.gatesWith(StrataGateStatus.Warn)
        parts.add(
            when {
                failing != 0 -> "$failing ${if (failing == 1) "gate" else "gates"} failing"
                warning != 0 -> "$warning ${if (warning == 1) "gate" else "gates"} warning"
                else -> "all gates pass"
            },
        )
        return parts.joinToString(" · ")
    }

    fun buildModel(snapshot: InsightSnapshot, depth: Int): StrataModel {
        val graph = Insight.buildModuleGraph(snapshot, depth)
        val report = buildReport(snapshot, graph)
        return StrataModel(snapshot, report, StrataLayout.moduleLayout(report.graph), StrataLayout.matrixOrder(report.graph), depth)
    }
}
