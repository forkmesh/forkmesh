package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.codeinsight.InsightFixtures.snapshotOf
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import kotlin.coroutines.cancellation.CancellationException
import kotlin.math.abs

class InsightEngineTest {
    private fun findFunction(file: InsightFile, name: String): InsightFunction? =
        file.functions.firstOrNull { it.name == name || it.name.endsWith("::$name") }

    fun testLanguages() {
        assertEq("C++", Insight.languageForPath("src/a.cpp"))
        assertEq("TypeScript", Insight.languageForPath("a/b.tsx"))
        assertEq("Python", Insight.languageForPath("x.py"))
        assertEq("", Insight.languageForPath("README.md"))
        expect(Insight.isTestPath("app/tests/test_x.py"), "tests folder is a test")
        expect(Insight.isTestPath("src/foo.spec.ts"), "spec file is a test")
        expect(Insight.isTestPath("Sources/FooTests.swift"), "XCTest file is a test")
        expect(!Insight.isTestPath("src/contest.cpp"), "contest is not a test")
        expect(Insight.isVendoredPath("web/node_modules/react/index.js"), "node_modules is vendored")
        expect(Insight.isVendoredPath("public/app.min.js"), "minified is vendored")
        expect(!Insight.isVendoredPath("src/vendorize.cpp"), "file name with vendor is not vendored")
    }

    fun testCpp() {
        val source = """#include "foo.h"
#include <vector>
// comment line
namespace demo {

class Shape {
public:
    virtual ~Shape() = default;
    virtual double area() const = 0;
};

int simple(int a)
{
    return a + 1;
}

int Widget::branchy(int a, int b, int c, int d, int e, int f, int g)
{
    if (a > 0 && b > 0) {
        for (int i = 0; i < c; ++i) {
            if (i % 2 == 0) {
                while (d > 0) {
                    if (e || f) {
                        --d;
                    }
                }
            }
        }
    } else if (g) {
        return a ? b : c;
    }
    const char *text = "if (x) { y; }";
    return 0;
}

}
"""
        val file = Insight.measureSource("src/widget.cpp", source.toByteArray())
        assertEq("C++", file.language)
        assertEq(1, file.commentLines, "comment lines")
        expect("foo.h" in file.imports && "<vector>" in file.imports, "includes extracted")
        assertEq(1, findFunction(file, "simple")?.complexity, "simple complexity")
        val branchy = findFunction(file, "branchy") ?: throw AssertionError("branchy not found")
        assertEq(10, branchy.complexity, "branchy complexity")
        assertEq(7, branchy.parameters, "branchy parameters")
        expect(branchy.nesting >= 5, "branchy nesting >= 5 (got ${branchy.nesting})")
        assertEq(17, branchy.line, "branchy line")
        assertEq(1, file.types, "types")
        assertEq(1, file.abstractTypes, "abstract types")
    }

    fun testPython() {
        val source = """import os, sys as system
from .models import User, Group
from ..core import util


class Repo(ABC):
    ${"\"\"\""}Docstring
    spanning lines.${"\"\"\""}

    def find(self, name, deep=False):
        if not name or deep:
            return None
        for item in self.items:
            if item.name == name:
                return item
        return [x for x in self.items if x]


def helper(a, b):
    try:
        return a / b
    except ZeroDivisionError:
        return 0
"""
        val file = Insight.measureSource("pkg/sub/repo.py", source.toByteArray())
        expect("os" in file.imports && "sys" in file.imports, "python plain imports")
        expect(".models" in file.imports && "?.models.User" in file.imports, "python relative from-import with optional names")
        val find = findFunction(file, "find") ?: throw AssertionError("find not found")
        assertEq(7, find.complexity, "find complexity")
        assertEq(2, find.parameters, "self is not a parameter")
        assertEq(2, find.nesting, "find nesting")
        assertEq(2, findFunction(file, "helper")?.complexity, "helper complexity")
        assertEq(1, file.types)
        assertEq(1, file.abstractTypes)
        expect(file.commentLines >= 2, "docstring counted as comment (got ${file.commentLines})")
    }

    fun testTypeScriptAndGo() {
        val ts = """import React, { useState } from 'react';
import type { Foo } from "./types";
export * from '../shared/util';
const lazy = () => import('./lazy');
const legacy = require('./legacy.js');

export const handler = async (event: Event): Promise<void> => {
  const x = event?.target ?? null;
  if (x && x.ok) {
    return;
  }
  const y = x ? 1 : 2;
};

export function plain(a?: number) {
  return a;
}
"""
        val file = Insight.measureSource("web/src/app.ts", ts.toByteArray())
        for (spec in listOf("react", "./types", "../shared/util", "./lazy", "./legacy.js")) expect(spec in file.imports, "ts import $spec")
        assertEq(4, findFunction(file, "handler")?.complexity, "handler complexity")
        assertEq(1, findFunction(file, "plain")?.complexity, "optional parameter is not a ternary")

        val wrapped = """(function () {
  function inner(a) {
    if (a) { return 1; }
    return 0;
  }
  window.addEventListener('load', () => {
    const b = inner(2) || inner(3);
  });
})();
"""
        val wrappedFile = Insight.measureSource("public/app.js", wrapped.toByteArray())
        assertEq(listOf("inner"), wrappedFile.functions.map { it.name }, "IIFE wrapper is not a function")
        assertEq(3, wrappedFile.complexity, "wrapper decisions count to the file")

        val go = "package server\n\nimport (\n\t\"fmt\"\n\tapi \"example.com/app/internal/api\"\n)\n\n" +
            "func (s *Server) Handle(w Writer, r *Request) error {\n\tif r == nil {\n\t\treturn fmt.Errorf(\"nil\")\n\t}\n" +
            "\tswitch r.Method {\n\tcase \"GET\":\n\t\treturn nil\n\tcase \"POST\":\n\t\treturn nil\n\t}\n\treturn nil\n}\n"
        val goFile = Insight.measureSource("cmd/server/server.go", go.toByteArray())
        expect("example.com/app/internal/api" in goFile.imports, "go block import")
        val handle = findFunction(goFile, "Handle") ?: throw AssertionError("Handle not found")
        assertEq(4, handle.complexity, "Go Handle complexity")
        assertEq(2, handle.parameters, "Go receiver is not a parameter")
    }

    fun testExtractImportsDropsOptionalNames() {
        assertEq(listOf(".models"), Insight.extractImports("Python", "from .models import User\n"))
        assertEq(listOf("<vector>", "a.h"), Insight.extractImports("C++", "#include <vector>\n// #include \"b.h\"\n#include \"a.h\"\n"))
        assertEq(emptyList<String>(), Insight.extractImports("Klingon", "import x"))
    }

    fun testResolution() {
        val snapshot = snapshotOf(
            "web/src/a.ts" to "import { b } from './b';\nimport x from 'lodash';\n",
            "web/src/b.ts" to "import { c } from './lib/c.js';\n",
            "web/src/lib/c.ts" to "import { a } from '../a';\n",
            "pkg/__init__.py" to "",
            "pkg/models.py" to "from . import util\nfrom pkg.views import render\n",
            "pkg/util.py" to "import json\n",
            "pkg/views.py" to "from .models import Model\n",
            "native/src/core.cpp" to "#include \"core.h\"\n#include <QString>\n",
            "native/include/core.h" to "#pragma once\n",
            "ios/Sources/Feed.swift" to "import UIKit\nstruct Feed { let item: FeedItem }\n",
            "ios/Sources/FeedItem.swift" to "struct FeedItem { let id: Int }\n",
            "rs/src/lib.rs" to "mod parser;\nuse crate::parser::Token;\n",
            "rs/src/parser.rs" to "pub struct Token;\n",
        )
        Insight.resolveDependencies(snapshot)
        fun depends(from: String, to: String): Boolean {
            val a = snapshot.indexOf(from)
            val b = snapshot.indexOf(to)
            return a >= 0 && b >= 0 && b in snapshot.files[a].dependsOn
        }
        expect(depends("web/src/a.ts", "web/src/b.ts"), "relative TS import resolves")
        expect(depends("web/src/b.ts", "web/src/lib/c.ts"), "ESM .js specifier resolves to .ts")
        expect(depends("web/src/lib/c.ts", "web/src/a.ts"), "parent relative import resolves")
        assertEq(listOf("lodash"), snapshot.files[snapshot.indexOf("web/src/a.ts")].externalImports, "package import is external")
        expect(depends("pkg/models.py", "pkg/util.py"), "python from . import module resolves")
        expect(depends("pkg/models.py", "pkg/views.py"), "python absolute package import resolves")
        expect(depends("pkg/views.py", "pkg/models.py"), "python relative module resolves")
        expect(!depends("pkg/views.py", "pkg/__init__.py"), "optional name does not add the package twice")
        expect(depends("native/src/core.cpp", "native/include/core.h"), "C++ include resolves by suffix")
        expect(depends("ios/Sources/Feed.swift", "ios/Sources/FeedItem.swift"), "Swift type reference resolves")
        expect(depends("rs/src/lib.rs", "rs/src/parser.rs"), "Rust mod resolves")
        val b = snapshot.indexOf("web/src/b.ts")
        expect(snapshot.indexOf("web/src/a.ts") in snapshot.files[b].dependedOnBy, "reverse edges filled")
        Insight.markCycles(snapshot)
        assertEq(2, snapshot.cycles.size, "cycles")
        assertEq(3, snapshot.cycles[0].size, "TS cycle has three files")
        assertEq(0, snapshot.files[b].cycle, "file knows its cycle")
    }

    fun testStronglyConnected() {
        val components = Insight.stronglyConnected(listOf(listOf(1), listOf(2), listOf(0, 3), emptyList(), listOf(9)))
        assertEq(listOf(listOf(3), listOf(0, 1, 2), listOf(4)), components)
    }

    private fun commitLog(vararg commits: Triple<String, Long, Pair<String, List<String>>>): String {
        val log = StringBuilder()
        for ((author, time, rest) in commits) {
            log.append('\u001e').append(time.toString(16)).append('\u001f').append(author).append('\u001f')
                .append(time).append('\u001f').append(rest.first).append("\n\n")
            for (file in rest.second) log.append("10\t2\t").append(file).append('\n')
        }
        return log.toString()
    }

    fun testHistoryAndHealth() {
        val snapshot = snapshotOf("a.cpp" to "int a() { return 1; }\n", "b.cpp" to "int b() { return 2; }\n", "c.cpp" to "int c() { return 3; }\n")
        val now = 1800000000L
        val day = 86400L
        val log = commitLog(
            Triple("Ada", now - 10 * day, "Add feature" to listOf("a.cpp", "b.cpp")),
            Triple("Ada", now - 20 * day, "Fix crash in parser" to listOf("a.cpp", "b.cpp")),
            Triple("Grace", now - 300 * day, "Refactor" to listOf("a.cpp", "b.cpp")),
            Triple("Grace", now - 310 * day, "Tweak" to listOf("a.cpp", "c.cpp")),
        )
        val commits = Insight.parseGitLog(log)
        assertEq(4, commits.size)
        assertEq(InsightCommitFile("b.cpp", 10, 2), commits[0].files[1])
        Insight.applyHistory(snapshot, commits, InsightOptions(), now)
        val a = snapshot.files[snapshot.indexOf("a.cpp")]
        assertEq(4, a.commits, "commits")
        assertEq(2, a.recentCommits, "recent commits")
        assertEq(1, a.fixCommits, "fix commits")
        assertEq(2, a.authors, "authors")
        assertEq(now - 10 * day, a.lastChanged, "last changed")
        expect(abs(a.formerAuthorShare - 0.5) < 1e-12, "half the code by former contributors")
        assertEq(listOf("Grace"), snapshot.formerAuthors)
        assertEq(1, snapshot.couplings.size, "couplings")
        assertEq(3, snapshot.couplings[0].shared)
        expect(abs(snapshot.couplings[0].degree - 3.0 / 3.5) < 1e-9, "coupling degree")
        val b = snapshot.indexOf("b.cpp")
        assertEq(b, snapshot.couplingsFor(b)[0].a, "couplingsFor puts the file first")
        Insight.computeHotspots(snapshot)
        assertEq(1.0, a.hotspot, "hottest file scores 1")

        val heavy = InsightFile(
            codeLines = 2400,
            functions = (0 until 4).map { InsightFunction("brain$it", 0, 150, 40, 6, 8) },
            maxComplexity = 40,
            complexity = 160,
            cycle = 0,
        )
        val health = Insight.computeHealth(heavy)
        assertEq(HealthBand.Unhealthy, Insight.healthBand(health.score), "heavy file is unhealthy (${health.score})")
        expect(health.issues.firstOrNull()?.startsWith("Brain methods: brain0 (complexity 40, 150 lines)") == true, "brain method reported: ${health.issues}")
        expect("Part of a circular import" in health.issues, "cycle reported")
        val clean = InsightFile(codeLines = 120, functions = listOf(InsightFunction(complexity = 3, length = 20)))
        assertEq(10.0, Insight.computeHealth(clean).score, "small clean file scores 10")
        assertEq("Problematic", Insight.healthBand(5.0).label)
    }

    fun testHealthIssueTexts() {
        val file = InsightFile(
            codeLines = 900,
            complexity = 400,
            maxComplexity = 60,
            duplicatedLines = 200,
            functions = listOf(
                InsightFunction("a", 1, 80, 12, 4, 6),
                InsightFunction("b", 1, 10, 11, 1, 1),
            ),
        )
        val health = Insight.computeHealth(file)
        assertEq(
            listOf(
                "Complex functions: a (12), b (11)",
                "Extremely complex function (complexity 60)",
                "Deeply nested logic: depth 4 in a",
                "Large function: a (80 lines)",
                "Many arguments: a (6)",
                "Large file: 900 lines of code",
                "Code duplication: 200 lines (22 %)",
                "Complex conditionals: 400 decisions in 900 lines",
            ),
            health.issues,
        )
        assertEq(5.1, health.score)
    }

    fun testModules() {
        val snapshot = snapshotOf(
            "core/a.ts" to "export interface Port {}\n",
            "core/b.ts" to "import { Port } from './a';\n",
            "ui/view.ts" to "import { b } from '../core/b';\nimport { s } from '../store/s';\n",
            "store/s.ts" to "import { v } from '../ui/view';\nimport { a } from '../core/a';\n",
            "main.ts" to "import { v } from './ui/view';\n",
        )
        Insight.resolveDependencies(snapshot)
        Insight.markCycles(snapshot)
        val graph = Insight.buildModuleGraph(snapshot, 1)
        assertEq(4, graph.nodes.size, "modules")
        val core = graph.nodes.indexOfFirst { it.path == "core" }
        val ui = graph.nodes.indexOfFirst { it.path == "ui" }
        val root = graph.nodes.indexOfFirst { it.path.isEmpty() }
        assertEq(0, graph.nodes[core].layer, "core is the foundation layer")
        assertEq(0.0, graph.nodes[core].instability, "core is fully stable")
        assertEq(1.0, graph.nodes[core].abstractness, "core is abstract")
        expect(graph.nodes[ui].cycle >= 0, "ui is in a module cycle")
        assertEq(2, graph.nodes[root].layer, "root module sits on top")
        assertEq("(root files)", graph.nodes[root].name)
        assertEq(1, graph.cycles.size, "one module cycle")
        val edge = graph.edgeIndex(ui, core)
        expect(edge >= 0 && graph.edges[edge].weight == 1 && !graph.edges[edge].inCycle, "ui uses core")
        assertEq(1, graph.nodes[core].internalEdges, "core internal edge")
        assertEq("a/b", Insight.modulePathFor("a/b/c/d.ts", 2))
        assertEq("", Insight.modulePathFor("d.ts", 3))
    }

    fun testAnalyzeFolder() {
        val source = MemorySource(
            mapOf(
                "src/main.js" to "const u = require('./util/u');\nfunction main() { if (u) { return 1; } }\n".toByteArray(),
                "src/util/u.js" to "module.exports = function u(a) { return a && a.b; };\n".toByteArray(),
                "node_modules/lib/x.js" to "function x() {}\n".toByteArray(),
                "build/out.js" to "function y() {}\n".toByteArray(),
                "notes.md" to "# hi\n".toByteArray(),
            ),
            git = false,
        )
        val steps = ArrayList<Pair<String, Double>>()
        val snapshot = Insight.analyze(source, progress = { text, fraction -> steps.add(text to fraction) })
        assertEq("", snapshot.error)
        assertEq(2, snapshot.files.size, "two source files")
        assertEq(0, snapshot.skippedFiles, "skipped folders are not listed")
        assertEq(1, snapshot.files[snapshot.indexOf("src/main.js")].dependsOn.size, "folder import resolved")
        expect(!snapshot.git, "plain folder has no git")
        assertEq("memory:fixture", snapshot.source)
        assertEq(
            listOf(
                "Listing files in fixture…", "Reading 2 of 2 files…", "Connecting imports…",
                "Looking for duplicated code…", "Scoring code health…", "Done",
            ),
            steps.map { it.first },
        )
        expect(steps.zipWithNext().all { it.first.second <= it.second.second }, "progress only moves forward")
    }

    fun testGitSourceWithoutCodeFallsBackToFolderRules() {
        val source = MemorySource(mapOf("notes.txt" to "x = 1\n".toByteArray(), "README.md" to "hi\n".toByteArray()))
        val snapshot = Insight.analyze(source)
        expect(!snapshot.git, "git turned off")
        assertEq("No source code found in fixture.", snapshot.error)
    }

    fun testEmptySourceReportsDesktopError() {
        val snapshot = Insight.analyze(MemorySource(emptyMap(), name = "empty-repo"))
        assertEq("No source code found in empty-repo.", snapshot.error)
        assertEq(0, snapshot.files.size)
    }

    fun testSkipsEmptyBinaryGeneratedAndMinified() {
        val minified = ("var a=1;".repeat(3000)).toByteArray()
        val source = MemorySource(
            mapOf(
                "a.py" to "def f():\n    return 1\n".toByteArray(),
                "empty.py" to ByteArray(0),
                "blob.c" to byteArrayOf(105, 110, 116, 0, 1),
                "gen.ts" to "// Code generated by protoc. DO NOT EDIT.\nexport const x = 1;\n".toByteArray(),
                "big.js" to minified,
            ),
        )
        val snapshot = Insight.analyze(source, InsightOptions(history = false))
        assertEq(listOf("a.py"), snapshot.files.map { it.path })
        assertEq(4, snapshot.skippedFiles)
    }

    fun testCancellationThrows() {
        var calls = 0
        assertFails<CancellationException> {
            Insight.analyze(MemorySource(InsightFixtures.repoFiles), isCancelled = { ++calls > 1 })
        }
        assertFails<CancellationException> {
            Insight.analyze(MemorySource(InsightFixtures.repoFiles), isCancelled = { true })
        }
    }

    fun testMaxFilesWarning() {
        val snapshot = Insight.analyze(MemorySource(InsightFixtures.repoFiles), InsightOptions(history = false, maxFiles = 5))
        expect(snapshot.files.size in 1..5, "at most five files measured (${snapshot.files.size})")
        expect(snapshot.warning.startsWith("Analyzed the first 5 of "), snapshot.warning)
    }

    fun testTrendAndSamples() {
        val versions = listOf(
            InsightVersion(100, "c1", "Ada", "def f(a):\n    return a\n".toByteArray()),
            InsightVersion(200, "c2", "Ada", "def f(a):\n    if a:\n        return 1\n    return 0\n".toByteArray()),
        )
        val points = Insight.trend("x.py", versions)
        assertEq(listOf(1, 2), points.map { it.complexity })
        assertEq(listOf(2, 4), points.map { it.codeLines })
        assertEq(listOf(10.0, 10.0), points.map { it.health })
        assertEq("c2", points[1].commit)
        assertEq(listOf(0, 1, 2), Insight.sampleIndexes(3))
        val picks = Insight.sampleIndexes(400)
        assertEq(16, picks.size)
        assertEq(0, picks.first())
        assertEq(399, picks.last())
    }

    fun testMarkDuplication() {
        val block = (1..8).joinToString("\n") { "    total_$it = compute(value_$it, factor)" }
        val first = "def one():\n$block\n"
        val second = "def two():\n$block\n"
        val snapshot = snapshotOf("a.py" to first, "b.py" to second, "c.py" to "def three():\n    return 3\n")
        Insight.markDuplication(snapshot, mapOf("a.py" to first.toByteArray(), "b.py" to second.toByteArray(), "c.py" to "def three():\n    return 3\n".toByteArray()))
        assertEq(listOf(8, 8, 0), snapshot.files.map { it.duplicatedLines })
    }

    fun testFixturePerformance() {
        val files = InsightFixtures.repoFiles
        val options = InsightOptions(history = false)
        Insight.analyze(MemorySource(files), options)
        val started = System.nanoTime()
        repeat(20) { Insight.analyze(MemorySource(files), options) }
        val ms = (System.nanoTime() - started) / 1_000_000
        println("    codeinsight: fixture analysed 20 times in $ms ms")
        expect(ms < 3000, "20 fixture runs took $ms ms")
    }

    fun testRealTreeTiming() {
        val root = System.getenv("FORKMESH_INSIGHT_ROOT")?.let(::File) ?: File(InsightFixtures.worktree, "desktop/src")
        if (!root.isDirectory) return
        val started = System.nanoTime()
        val snapshot = Insight.analyze(FolderSource(root), InsightOptions(history = false))
        val ms = (System.nanoTime() - started) / 1_000_000
        val functions = snapshot.files.sumOf { it.functions.size }
        val edges = snapshot.files.sumOf { it.dependsOn.size }
        println(
            "    codeinsight: ${root.path}: ${snapshot.files.size} files, ${snapshot.totalCodeLines} code lines, " +
                "$functions functions, $edges edges, ${snapshot.cycles.size} cycles, skipped ${snapshot.skippedFiles}, $ms ms",
        )
        assertEq("", snapshot.error)
    }
}
