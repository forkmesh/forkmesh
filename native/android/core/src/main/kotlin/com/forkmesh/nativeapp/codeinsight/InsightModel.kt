package com.forkmesh.nativeapp.codeinsight

data class InsightFunction(
    var name: String = "",
    var line: Int = 0,
    var length: Int = 0,
    var complexity: Int = 1,
    var nesting: Int = 0,
    var parameters: Int = 0,
)

data class InsightAuthorCount(val name: String, val commits: Int)

enum class HealthBand(val label: String) {
    Healthy("Healthy"),
    Problematic("Problematic"),
    Unhealthy("Unhealthy"),
}

data class InsightHealth(val score: Double, val issues: List<String>)

data class InsightFile(
    var path: String = "",
    var language: String = "",
    var bytes: Long = 0,
    var lines: Int = 0,
    var codeLines: Int = 0,
    var commentLines: Int = 0,
    var blankLines: Int = 0,
    var types: Int = 0,
    var abstractTypes: Int = 0,
    var complexity: Int = 0,
    var maxComplexity: Int = 0,
    var maxNesting: Int = 0,
    var duplicatedLines: Int = 0,
    var test: Boolean = false,
    var functions: List<InsightFunction> = emptyList(),
    var imports: List<String> = emptyList(),
    var externalImports: List<String> = emptyList(),
    var declares: List<String> = emptyList(),
    var references: List<String> = emptyList(),
    var dependsOn: List<Int> = emptyList(),
    var dependedOnBy: List<Int> = emptyList(),
    var commits: Int = 0,
    var recentCommits: Int = 0,
    var authors: Int = 0,
    var fixCommits: Int = 0,
    var linesAdded: Int = 0,
    var linesDeleted: Int = 0,
    var lastChanged: Long = 0,
    var firstChanged: Long = 0,
    var mainAuthor: String = "",
    var mainAuthorShare: Double = 0.0,
    var formerAuthorShare: Double = 0.0,
    var topAuthors: List<InsightAuthorCount> = emptyList(),
    var health: Double = 10.0,
    var healthIssues: List<String> = emptyList(),
    var hotspot: Double = 0.0,
    var cycle: Int = -1,
) {
    fun deepCopy(): InsightFile = copy(functions = functions.map { it.copy() })
}

data class InsightCoupling(val a: Int, val b: Int, val shared: Int, val degree: Double)

data class InsightOptions(
    val history: Boolean = true,
    val historyDays: Int = 365,
    val maxFiles: Int = 30000,
    val maxCommits: Int = 8000,
    val maxCommitFiles: Int = 40,
    val minSharedCommits: Int = 3,
    val minCouplingDegree: Double = 0.25,
    val maxFileBytes: Long = 2L * 1024 * 1024,
)

data class InsightCommitFile(val path: String, val added: Int?, val deleted: Int?)

data class InsightCommit(
    val hash: String,
    val author: String,
    val time: Long,
    val subject: String,
    val files: List<InsightCommitFile>,
)

data class InsightModuleNode(
    var path: String = "",
    var name: String = "",
    var files: List<Int> = emptyList(),
    var codeLines: Int = 0,
    var complexity: Int = 0,
    var functions: Int = 0,
    var types: Int = 0,
    var abstractTypes: Int = 0,
    var afferent: Int = 0,
    var efferent: Int = 0,
    var internalEdges: Int = 0,
    var instability: Double = 0.0,
    var abstractness: Double = 0.0,
    var distance: Double = 0.0,
    var cohesion: Double = 0.0,
    var health: Double = 10.0,
    var commits: Int = 0,
    var cycle: Int = -1,
    var layer: Int = 0,
)

data class InsightModuleEdge(
    var from: Int = -1,
    var to: Int = -1,
    var weight: Int = 0,
    var inCycle: Boolean = false,
    var fileEdges: List<Pair<Int, Int>> = emptyList(),
)

data class InsightModuleGraph(
    val depth: Int,
    val nodes: List<InsightModuleNode>,
    val edges: List<InsightModuleEdge>,
    val cycles: List<List<Int>>,
    val moduleOfFile: List<Int>,
) {
    fun edgeIndex(from: Int, to: Int): Int = edges.indexOfFirst { it.from == from && it.to == to }
}

data class InsightTrendPoint(
    val time: Long,
    val commit: String,
    val author: String,
    val codeLines: Int,
    val complexity: Int,
    val maxComplexity: Int,
    val health: Double,
)

class InsightVersion(val time: Long, val commit: String, val author: String, val bytes: ByteArray)

data class InsightEntry(val path: String, val size: Long = -1)

interface InsightSource {
    val name: String
    val description: String
    val head: String
    val branch: String
    val git: Boolean

    fun entries(): List<InsightEntry>

    fun read(path: String, limit: Long): ByteArray?

    fun history(options: InsightOptions, isCancelled: () -> Boolean): List<InsightCommit>?
}

class InsightSnapshot(
    var source: String = "",
    var name: String = "",
    var head: String = "",
    var branch: String = "",
    var analyzedAt: Long = 0,
    var elapsedMs: Long = 0,
    var git: Boolean = false,
    var historyDays: Int = 0,
    var commitsScanned: Int = 0,
    var historyStart: Long = 0,
    var historyEnd: Long = 0,
    files: List<InsightFile> = emptyList(),
    var packageRoots: Map<String, String> = emptyMap(),
    var couplings: List<InsightCoupling> = emptyList(),
    var cycles: List<List<Int>> = emptyList(),
    var authors: List<String> = emptyList(),
    var formerAuthors: List<String> = emptyList(),
    var skippedFiles: Int = 0,
    var warning: String = "",
    var error: String = "",
) {
    var files: List<InsightFile> = files
        set(value) {
            field = value
            byPath = indexPaths(value)
        }

    var byPath: Map<String, Int> = indexPaths(files)
        private set

    fun indexOf(path: String): Int = byPath[path] ?: -1

    val totalCodeLines: Int
        get() = files.sumOf { it.codeLines.toLong() }.coerceAtMost(Int.MAX_VALUE.toLong()).toInt()

    val totalComplexity: Int
        get() = files.sumOf { it.complexity.toLong() }.coerceAtMost(Int.MAX_VALUE.toLong()).toInt()

    fun couplingsFor(file: Int): List<InsightCoupling> = couplings
        .mapNotNull {
            when (file) {
                it.a -> it
                it.b -> it.copy(a = it.b, b = it.a)
                else -> null
            }
        }
        .sortedWith(compareByDescending<InsightCoupling> { it.degree }.thenByDescending { it.shared })

    fun deepCopy(): InsightSnapshot = InsightSnapshot(
        source, name, head, branch, analyzedAt, elapsedMs, git, historyDays, commitsScanned,
        historyStart, historyEnd, files.map { it.deepCopy() }, LinkedHashMap(packageRoots), couplings,
        cycles, authors, formerAuthors, skippedFiles, warning, error,
    )

    private companion object {
        fun indexPaths(files: List<InsightFile>): Map<String, Int> {
            val map = HashMap<String, Int>(files.size * 2)
            files.forEachIndexed { index, file -> map[file.path] = index }
            return map
        }
    }
}
