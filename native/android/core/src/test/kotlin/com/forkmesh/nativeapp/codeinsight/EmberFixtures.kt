package com.forkmesh.nativeapp.codeinsight

object EmberFixtures {
    fun synthetic(count: Int): InsightSnapshot {
        var seed = 7L
        fun next(): Long {
            seed = (seed * 1664525L + 1013904223L) and 0xffffffffL
            return seed shr 8
        }
        val files = ArrayList<InsightFile>(count)
        for (i in 0 until count) {
            val m = next() % 12
            val p = next() % 9
            val q = next() % 15
            val f = InsightFile(path = "m$m/p$p/q$q/f$i.cpp", language = "C++")
            f.codeLines = (5 + next() % 3000).toInt()
            f.health = 1 + (next() % 90) / 10.0
            f.commits = (next() % 80).toInt()
            f.hotspot = (next() % 1000) / 1000.0
            files.add(f)
        }
        return InsightSnapshot(name = "big", git = true, commitsScanned = 1000, files = files)
    }

    private fun file(
        path: String,
        lines: Int,
        health: Double,
        commits: Int = 0,
        hotspot: Double = 0.0,
        author: String = "",
        fixes: Int = 0,
        language: String = "C++",
    ): InsightFile = InsightFile(
        path = path,
        language = language,
        codeLines = lines,
        lines = lines + lines / 5,
        health = health,
        commits = commits,
        recentCommits = commits / 3,
        hotspot = hotspot,
        mainAuthor = author,
        mainAuthorShare = if (author.isEmpty()) 0.0 else 0.6,
        topAuthors = if (author.isEmpty()) emptyList() else listOf(InsightAuthorCount(author, commits)),
        authors = if (author.isEmpty()) 0 else 1,
        fixCommits = fixes,
        complexity = lines / 10,
        healthIssues = if (health < 8) listOf("Long function", "Deep nesting") else emptyList(),
        functions = listOf(InsightFunction("run", 12, 40, 9, 2, 1)),
    )

    fun sample(): InsightSnapshot {
        val files = listOf(
            file("src/core/engine.cpp", 4200, 2.5, 66, 1.0, "Ada", 14),
            file("src/core/engine.h", 300, 7.0, 40, 0.4, "Ada", 3),
            file("src/core/util.cpp", 800, 9.5, 5, 0.05, "Bo", 0),
            file("src/ui/window.cpp", 2500, 5.0, 30, 0.6, "Bo", 6),
            file("src/ui/window.h", 200, 9.0, 12, 0.1, "Cy", 0),
            file("src/ui/theme.qml", 150, 9.8, 2, 0.01, "Cy", 0, "QML"),
            file("tools/build.py", 400, 3.5, 0, 0.0, "", 0, "Python"),
            file("README.md", 20, 10.0, 1, 0.0, "Ada", 0, "Markdown"),
            file("deep/a/b/c/leaf.go", 90, 8.5, 3, 0.02, "Dee", 1, "Go"),
        )
        files[6].formerAuthorShare = 0.9
        files[0].formerAuthorShare = 0.7
        return InsightSnapshot(
            name = "sample",
            git = true,
            historyDays = 365,
            commitsScanned = 400,
            files = files,
            couplings = listOf(InsightCoupling(0, 1, 30, 0.8), InsightCoupling(0, 3, 10, 0.35), InsightCoupling(3, 4, 9, 0.5)),
            authors = listOf("Ada", "Bo", "Cy", "Dee"),
            formerAuthors = listOf("Dee"),
        )
    }
}
