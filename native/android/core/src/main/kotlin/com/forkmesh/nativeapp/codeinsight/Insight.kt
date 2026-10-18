package com.forkmesh.nativeapp.codeinsight

import java.text.NumberFormat
import java.util.concurrent.Callable
import java.util.concurrent.ExecutionException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import kotlin.coroutines.cancellation.CancellationException
import kotlin.math.roundToLong

object Insight {
    private const val CHUNK_SIZE = 1200

    private val goModule = QRegex.compile("^\\s*module\\s+(\\S+)", multiline = true)
    private val dartName = QRegex.compile("^name:\\s*([A-Za-z0-9_]+)", multiline = true)

    fun languageForPath(path: String): String = Languages.specFor(path).name

    fun isTestPath(path: String): Boolean = Languages.isTestPath(path)

    fun isVendoredPath(path: String): Boolean = Languages.isVendoredPath(path)

    fun measureSource(path: String, bytes: ByteArray): InsightFile = Measure.measure(path, bytes, false).info

    fun extractImports(language: String, text: String): List<String> {
        val spec = Languages.byName[language] ?: return emptyList()
        return Measure.importsFromStripped(language, Measure.stripSource(text, spec).noComments).filter { !it.startsWith('?') }
    }

    fun computeHealth(file: InsightFile): InsightHealth = Health.compute(file)

    fun healthBand(health: Double): HealthBand = when {
        health >= 8.0 -> HealthBand.Healthy
        health >= 4.0 -> HealthBand.Problematic
        else -> HealthBand.Unhealthy
    }

    fun stronglyConnected(adjacency: List<List<Int>>): List<List<Int>> = Graphs.stronglyConnected(adjacency)

    fun resolveDependencies(snapshot: InsightSnapshot) = Dependencies.resolve(snapshot)

    fun markCycles(snapshot: InsightSnapshot) = Graphs.markCycles(snapshot)

    fun markDuplication(snapshot: InsightSnapshot, texts: Map<String, ByteArray>) {
        val significant = ArrayList<IntArray>()
        val windows = ArrayList<LongArray>()
        for (file in snapshot.files) {
            val bytes = texts[file.path]
            if (bytes == null) {
                significant.add(IntArray(0))
                windows.add(LongArray(0))
                continue
            }
            val measured = Measure.measure(file.path, bytes, true)
            significant.add(measured.significant)
            windows.add(measured.windows)
        }
        Graphs.applyWindows(snapshot, significant, windows)
    }

    fun computeHotspots(snapshot: InsightSnapshot) = Graphs.computeHotspots(snapshot)

    fun parseGitLog(text: String): List<InsightCommit> = History.parseGitLog(text)

    fun applyHistory(snapshot: InsightSnapshot, commits: List<InsightCommit>, options: InsightOptions = InsightOptions(), now: Long) =
        History.apply(snapshot, commits, options, now)

    fun autoModuleDepth(snapshot: InsightSnapshot): Int = Graphs.autoModuleDepth(snapshot)

    fun modulePathFor(path: String, depth: Int): String = Graphs.modulePathFor(path, depth)

    fun buildModuleGraph(snapshot: InsightSnapshot, depth: Int = 0): InsightModuleGraph = Graphs.buildModuleGraph(snapshot, depth)

    fun sampleIndexes(count: Int, samples: Int = 16): List<Int> {
        val want = maxOf(2, samples)
        if (count <= want) return (0 until count).toList()
        val picks = ArrayList<Int>()
        for (k in 0 until want) {
            val index = (k.toDouble() * (count - 1).toDouble() / (want - 1).toDouble()).roundToLong().toInt()
            if (picks.isEmpty() || picks.last() != index) picks.add(index)
        }
        return picks
    }

    fun trend(path: String, versions: List<InsightVersion>): List<InsightTrendPoint> = versions.map { version ->
        val info = measureSource(path, version.bytes)
        InsightTrendPoint(
            time = version.time,
            commit = version.commit,
            author = version.author,
            codeLines = info.codeLines,
            complexity = info.complexity,
            maxComplexity = info.maxComplexity,
            health = Health.compute(info).score,
        )
    }

    fun analyze(
        source: InsightSource,
        options: InsightOptions = InsightOptions(),
        now: Long = System.currentTimeMillis() / 1000,
        progress: (String, Double) -> Unit = { _, _ -> },
        isCancelled: () -> Boolean = { false },
    ): InsightSnapshot {
        val started = System.nanoTime()
        val name = source.name
        val snapshot = InsightSnapshot(source = source.description, name = name, analyzedAt = now)
        val report = { text: String, fraction: Double -> progress(text, fraction.coerceIn(0.0, 1.0)) }
        val checkCancelled = { if (isCancelled()) throw CancellationException("Analysis of $name was canceled.") }
        val elapsed = { (System.nanoTime() - started) / 1_000_000 }
        report("Listing files in $name…", 0.01)
        snapshot.git = source.git
        if (source.git) {
            snapshot.head = source.head
            snapshot.branch = source.branch
        }
        val seen = HashSet<String>()
        var all = source.entries().filter { seen.add(it.path) }
        if (!source.git || all.none { languageForPath(it.path).isNotEmpty() }) {
            all = all.filter { !Languages.isSkippedFolderPath(it.path) }.take(400_001)
            snapshot.git = false
        }
        val history = snapshot.git
        checkCancelled()
        var skipped = 0
        val listed = ArrayList<InsightEntry>()
        for (entry in all) {
            if (languageForPath(entry.path).isEmpty()) continue
            if (isVendoredPath(entry.path) || entry.size > options.maxFileBytes) {
                skipped++
                continue
            }
            listed.add(entry)
        }
        listed.sortBy { it.path }
        var sources: List<InsightEntry> = listed
        if (sources.size > options.maxFiles) {
            val format = NumberFormat.getIntegerInstance()
            snapshot.warning = "Analyzed the first ${format.format(options.maxFiles)} of ${format.format(sources.size)} source files"
            sources = sources.subList(0, options.maxFiles)
        }
        snapshot.packageRoots = readPackageRoots(source, all)
        if (sources.isEmpty()) {
            snapshot.error = "No source code found in $name."
            snapshot.elapsedMs = elapsed()
            return snapshot
        }
        val files = ArrayList<InsightFile>()
        val significant = ArrayList<IntArray>()
        val windows = ArrayList<LongArray>()
        val workers = maxOf(2, Runtime.getRuntime().availableProcessors() - 1)
        val pool = Executors.newFixedThreadPool(workers) { runnable -> Thread(runnable, "InsightMeasure").apply { isDaemon = true } }
        try {
            val format = NumberFormat.getIntegerInstance()
            var start = 0
            while (start < sources.size) {
                checkCancelled()
                val chunk = sources.subList(start, minOf(start + CHUNK_SIZE, sources.size))
                report(
                    "Reading ${format.format(minOf(start + CHUNK_SIZE, sources.size))} of ${format.format(sources.size)} files…",
                    0.04 + 0.6 * start.toDouble() / sources.size.toDouble(),
                )
                for (measured in measureChunk(pool, workers, chunk, source, options.maxFileBytes, isCancelled)) {
                    if (measured.skip) {
                        skipped++
                        continue
                    }
                    files.add(measured.info)
                    significant.add(measured.significant)
                    windows.add(measured.windows)
                }
                start += CHUNK_SIZE
            }
        } finally {
            pool.shutdownNow()
        }
        checkCancelled()
        snapshot.files = files
        snapshot.skippedFiles = skipped
        applyHeaderLanguage(files)
        checkCancelled()
        report("Connecting imports…", 0.66)
        resolveDependencies(snapshot)
        markCycles(snapshot)
        report("Looking for duplicated code…", 0.7)
        Graphs.applyWindows(snapshot, significant, windows)
        significant.clear()
        windows.clear()
        if (history && options.history) {
            checkCancelled()
            report("Reading git history…", 0.74)
            val commits = source.history(options, isCancelled)
            checkCancelled()
            report("Finding files that change together…", 0.9)
            applyHistory(snapshot, commits.orEmpty(), options, now)
        }
        report("Scoring code health…", 0.95)
        for (file in snapshot.files) {
            val health = Health.compute(file)
            file.health = health.score
            file.healthIssues = health.issues
        }
        computeHotspots(snapshot)
        snapshot.elapsedMs = elapsed()
        report("Done", 1.0)
        return snapshot
    }

    private fun measureChunk(
        pool: ExecutorService,
        workers: Int,
        chunk: List<InsightEntry>,
        source: InsightSource,
        limit: Long,
        isCancelled: () -> Boolean,
    ): List<Measured> {
        val results = arrayOfNulls<Measured>(chunk.size)
        val next = AtomicInteger(0)
        val task = Callable {
            while (true) {
                val at = next.getAndIncrement()
                if (at >= chunk.size) break
                val entry = chunk[at]
                if (isCancelled()) {
                    results[at] = Measured(InsightFile(path = entry.path), skip = true)
                    continue
                }
                results[at] = measureEntry(entry, source, limit)
            }
        }
        val futures = pool.invokeAll(List(minOf(workers, chunk.size)) { task })
        for (future in futures) {
            try {
                future.get()
            } catch (failure: ExecutionException) {
                throw failure.cause ?: failure
            }
        }
        return results.map { it!! }
    }

    private fun measureEntry(entry: InsightEntry, source: InsightSource, limit: Long): Measured {
        val read = source.read(entry.path, limit)
        val bytes = if (read == null || read.size > limit) ByteArray(0) else read
        if (bytes.isEmpty() || Measure.binaryText(bytes) || Measure.generatedText(bytes) || Measure.minifiedText(bytes)) {
            return Measured(InsightFile(path = entry.path), skip = true)
        }
        return Measure.measure(entry.path, bytes, true)
    }

    private fun applyHeaderLanguage(files: List<InsightFile>) {
        var cpp = 0
        var c = 0
        var objc = 0
        for (file in files) {
            if (file.language == "C++") {
                cpp++
            } else if (file.language == "C" && !file.path.endsWith(".h")) {
                c++
            } else if (file.language == "Objective-C") {
                objc++
            }
        }
        val headerLanguage = if (objc > cpp && objc > c) "Objective-C" else if (cpp >= c && cpp > 0) "C++" else ""
        if (headerLanguage.isEmpty()) return
        for (file in files) if (file.path.endsWith(".h") && file.language == "C") file.language = headerLanguage
    }

    private fun readPackageRoots(source: InsightSource, all: List<InsightEntry>): Map<String, String> {
        val manifests = ArrayList<InsightEntry>()
        for (entry in all) {
            val fileName = Paths.fileNameOf(entry.path)
            if ((fileName == "go.mod" || fileName == "pubspec.yaml") && !isVendoredPath(entry.path)) manifests.add(entry)
            if (manifests.size > 200) break
        }
        val roots = LinkedHashMap<String, String>()
        val limit = 1L shl 20
        for (entry in manifests) {
            val read = source.read(entry.path, limit)
            val bytes = if (read == null || read.size > limit) ByteArray(0) else read
            val text = String(bytes, Charsets.UTF_8)
            val dir = Paths.dirOf(entry.path)
            if (entry.path.endsWith("go.mod")) {
                goModule.firstMatch(text)?.let { roots["go:" + it.group(1).replace("\"", "")] = dir }
            } else {
                dartName.firstMatch(text)?.let { roots["dart:" + it.group(1)] = dir }
            }
        }
        return roots
    }
}
