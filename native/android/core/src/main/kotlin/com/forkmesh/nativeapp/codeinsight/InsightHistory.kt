package com.forkmesh.nativeapp.codeinsight

import kotlin.math.roundToLong

internal object History {
    private val fixPattern = QRegex.compile(
        "\\b(fix|fixes|fixed|fixing|bug|bugs|bugfix|hotfix|defect|crash|crashes|regression|broken|revert)\\b",
        caseless = true,
    )

    private fun asciiTrim(text: String): String = text.trim { it == ' ' || it in '\t'..'\r' }

    private fun qtInt(text: String): Int = asciiTrim(text).toIntOrNull() ?: 0

    fun parseGitLog(text: String): List<InsightCommit> {
        val commits = ArrayList<InsightCommit>()
        for (record in text.split('\u001e')) {
            if (asciiTrim(record).isEmpty()) continue
            val headerEnd = record.indexOf('\n')
            val header = if (headerEnd < 0) record else record.substring(0, headerEnd)
            val fields = header.split('\u001f')
            if (fields.size < 3) continue
            val files = ArrayList<InsightCommitFile>()
            if (headerEnd >= 0) {
                for (line in record.substring(headerEnd + 1).split('\n')) {
                    val parts = line.split('\t')
                    if (parts.size < 3) continue
                    val path = QText.trimmed(parts.subList(2, parts.size).joinToString("\t"))
                    val added = if (parts[0] == "-") null else qtInt(parts[0])
                    val deleted = if (parts[1] == "-") null else qtInt(parts[1])
                    files.add(InsightCommitFile(path, added, deleted))
                }
            }
            commits.add(
                InsightCommit(
                    hash = asciiTrim(fields[0]),
                    author = QText.trimmed(fields[1]),
                    time = asciiTrim(fields[2]).toLongOrNull() ?: 0L,
                    subject = if (fields.size > 3) fields[3] else "",
                    files = files,
                ),
            )
        }
        return commits
    }

    private class Accumulator {
        var commits = 0
        var recent = 0
        var fixes = 0
        var added = 0
        var deleted = 0
        var last = 0L
        var first = 0L
        val authorLines = HashMap<String, Int>()
        val authorCommits = HashMap<String, Int>()
    }

    fun apply(snapshot: InsightSnapshot, log: List<InsightCommit>, options: InsightOptions, now: Long) {
        val perFile = HashMap<Int, Accumulator>()
        val authorLast = HashMap<String, Long>()
        val authorTotals = HashMap<String, Int>()
        val pairs = HashMap<Long, Int>()
        val recentCutoff = now - 90 * 86400L
        val formerCutoff = now - 180 * 86400L
        var commits = 0
        var earliest = 0L
        var latest = 0L
        for (commit in log) {
            val author = commit.author
            val time = commit.time
            commits++
            earliest = if (earliest == 0L) time else minOf(earliest, time)
            latest = maxOf(latest, time)
            authorLast[author] = maxOf(authorLast[author] ?: 0L, time)
            authorTotals[author] = (authorTotals[author] ?: 0) + 1
            val fix = fixPattern.found(commit.subject)
            val touched = ArrayList<Int>()
            for (entry in commit.files) {
                val index = snapshot.byPath[entry.path] ?: continue
                val added = entry.added ?: 0
                val deleted = entry.deleted ?: 0
                val acc = perFile.getOrPut(index) { Accumulator() }
                acc.commits++
                if (time >= recentCutoff) acc.recent++
                if (fix) acc.fixes++
                acc.added += added
                acc.deleted += deleted
                acc.last = maxOf(acc.last, time)
                acc.first = if (acc.first == 0L) time else minOf(acc.first, time)
                acc.authorLines[author] = (acc.authorLines[author] ?: 0) + added
                acc.authorCommits[author] = (acc.authorCommits[author] ?: 0) + 1
                if (index !in touched) touched.add(index)
            }
            if (touched.size >= 2 && touched.size <= options.maxCommitFiles) {
                touched.sort()
                for (a in touched.indices) for (b in a + 1 until touched.size) {
                    val key = (touched[a].toLong() shl 32) or (touched[b].toLong() and 0xffffffffL)
                    pairs[key] = (pairs[key] ?: 0) + 1
                }
            }
        }
        val former = authorLast.filterValues { it < formerCutoff }.keys
        for ((index, acc) in perFile) {
            val file = snapshot.files[index]
            file.commits = acc.commits
            file.recentCommits = acc.recent
            file.fixCommits = acc.fixes
            file.linesAdded = acc.added
            file.linesDeleted = acc.deleted
            file.lastChanged = acc.last
            file.firstChanged = acc.first
            file.authors = acc.authorCommits.size
            var totalLines = 0
            var formerLines = 0
            var main = ""
            var mainLines = -1
            var mainCommits = -1
            for ((name, count) in acc.authorCommits) {
                val lines = acc.authorLines[name] ?: 0
                totalLines += lines
                if (name in former) formerLines += lines
                if (lines > mainLines || (lines == mainLines && count > mainCommits) ||
                    (lines == mainLines && count == mainCommits && name < main)
                ) {
                    main = name
                    mainLines = lines
                    mainCommits = count
                }
            }
            file.mainAuthor = main
            if (totalLines > 0) {
                file.mainAuthorShare = mainLines.toDouble() / totalLines.toDouble()
                file.formerAuthorShare = formerLines.toDouble() / totalLines.toDouble()
            } else if (acc.commits > 0) {
                file.mainAuthorShare = mainCommits.toDouble() / acc.commits.toDouble()
                val formerCommits = acc.authorCommits.filterKeys { it in former }.values.sum()
                file.formerAuthorShare = formerCommits.toDouble() / acc.commits.toDouble()
            }
            file.topAuthors = acc.authorCommits.entries
                .map { InsightAuthorCount(it.key, it.value) }
                .sortedWith(compareByDescending<InsightAuthorCount> { it.commits }.thenBy { it.name })
                .take(5)
        }
        val couplings = ArrayList<InsightCoupling>()
        for ((key, shared) in pairs) {
            if (shared < options.minSharedCommits) continue
            val a = (key ushr 32).toInt()
            val b = (key and 0xffffffffL).toInt()
            val average = (snapshot.files[a].commits + snapshot.files[b].commits) / 2.0
            if (average <= 0) continue
            val degree = (shared / average).coerceIn(0.0, 1.0)
            if (degree < options.minCouplingDegree) continue
            couplings.add(InsightCoupling(a, b, shared, degree))
        }
        couplings.sortWith(
            compareByDescending<InsightCoupling> { it.degree }.thenByDescending { it.shared }.thenBy { it.a }.thenBy { it.b },
        )
        snapshot.couplings = if (couplings.size > 20000) couplings.subList(0, 20000).toList() else couplings
        val everyone = authorTotals.entries.sortedWith(compareByDescending<Map.Entry<String, Int>> { it.value }.thenBy { it.key })
        snapshot.authors = everyone.map { it.key }
        snapshot.formerAuthors = everyone.map { it.key }.filter { it in former }
        snapshot.commitsScanned = commits
        snapshot.historyStart = earliest
        snapshot.historyEnd = latest
        snapshot.historyDays = if (commits > 0) {
            val span = maxOf(1L, (now - earliest + 86399) / 86400)
            minOf(span, options.historyDays.toLong()).toInt()
        } else {
            0
        }
    }
}

internal object Health {
    fun roundHalfAway(value: Double): Double =
        if (value >= 0) value.roundToLong().toDouble() else -(-value).roundToLong().toDouble()

    private fun listed(names: List<String>): String {
        var text = names.take(3).joinToString(", ")
        if (names.size > 3) text += " and ${names.size - 3} more"
        return text
    }

    private fun plural(count: Int): String = if (count == 1) "" else "s"

    fun compute(file: InsightFile): InsightHealth {
        var score = 10.0
        val reasons = ArrayList<String>()
        var brainCount = 0
        var brain = 0.0
        var complex = 0.0
        var longFunctions = 0.0
        var arguments = 0.0
        val brainNames = ArrayList<String>()
        val complexNames = ArrayList<String>()
        val longNames = ArrayList<String>()
        val argumentNames = ArrayList<String>()
        var deepest: InsightFunction? = null
        for (fn in file.functions) {
            val isBrain = fn.length >= 70 && fn.complexity >= 15
            if (isBrain) {
                brainCount++
                brain += 1.5
                brainNames.add("${fn.name} (complexity ${fn.complexity}, ${fn.length} lines)")
            } else if (fn.complexity > 10) {
                complex += 0.35
                complexNames.add("${fn.name} (${fn.complexity})")
            }
            if (!isBrain && fn.length > 70) {
                longFunctions += 0.25
                longNames.add("${fn.name} (${fn.length} lines)")
            }
            if (fn.parameters > 5) {
                arguments += 0.2
                argumentNames.add("${fn.name} (${fn.parameters})")
            }
            if (deepest == null || fn.nesting > deepest.nesting) deepest = fn
        }
        if (brainCount > 0) {
            score -= minOf(3.5, brain)
            reasons.add("Brain method${plural(brainCount)}: ${listed(brainNames)}")
        }
        if (complexNames.isNotEmpty()) {
            score -= minOf(2.5, complex)
            reasons.add("Complex function${plural(complexNames.size)}: ${listed(complexNames)}")
        }
        if (file.maxComplexity > 50) {
            score -= 1.0
            reasons.add("Extremely complex function (complexity ${file.maxComplexity})")
        } else if (file.maxComplexity > 25) {
            score -= 0.5
        }
        if (deepest != null && deepest.nesting >= 4) {
            score -= if (deepest.nesting >= 6) 1.25 else 0.5
            reasons.add("Deeply nested logic: depth ${deepest.nesting} in ${deepest.name}")
        }
        if (longNames.isNotEmpty()) {
            score -= minOf(1.5, longFunctions)
            reasons.add("Large function${plural(longNames.size)}: ${listed(longNames)}")
        }
        if (argumentNames.isNotEmpty()) {
            score -= minOf(1.0, arguments)
            reasons.add("Many arguments: ${listed(argumentNames)}")
        }
        if (file.codeLines > 800) {
            score -= minOf(2.0, (file.codeLines - 800) / 1000.0 + 0.5)
            reasons.add("Large file: ${file.codeLines} lines of code")
        }
        if (file.functions.size > 75) {
            score -= 0.75
            reasons.add("Low cohesion: ${file.functions.size} functions in one file")
        }
        if (file.codeLines > 0) {
            val share = file.duplicatedLines.toDouble() / file.codeLines.toDouble()
            if (share > 0.1 && file.duplicatedLines >= 12) {
                score -= minOf(1.5, share * 5.0)
                reasons.add("Code duplication: ${file.duplicatedLines} lines (${roundHalfAway(share * 100).toInt()} %)")
            }
        }
        if (file.codeLines > 100 && file.complexity.toDouble() / file.codeLines.toDouble() > 0.25) {
            score -= 0.5
            reasons.add("Complex conditionals: ${file.complexity} decisions in ${file.codeLines} lines")
        }
        if (file.dependsOn.size > 25) {
            score -= 0.5
            reasons.add("Many dependencies: imports ${file.dependsOn.size} files")
        }
        if (file.cycle >= 0) {
            score -= 0.5
            reasons.add("Part of a circular import")
        }
        score = (roundHalfAway(score * 10.0) / 10.0).coerceIn(1.0, 10.0)
        return InsightHealth(score, reasons)
    }
}
