import Foundation
import XCTest
@testable import FMCodeInsight

struct FolderSource: InsightSource {
    var root: URL
    var name: String { root.lastPathComponent }
    var description: String { root.path }
    var git: Bool { false }
    var exists: Bool {
        var directory: ObjCBool = false
        return FileManager.default.fileExists(atPath: root.path, isDirectory: &directory) && directory.boolValue
    }

    func entries() throws -> [InsightEntry] {
        let base = root.standardizedFileURL.path + "/"
        var found: [InsightEntry] = []
        let keys: [URLResourceKey] = [.isRegularFileKey, .isDirectoryKey, .fileSizeKey, .isSymbolicLinkKey]
        guard let walker = FileManager.default.enumerator(at: root, includingPropertiesForKeys: keys) else { return [] }
        for case let url as URL in walker {
            let values = try url.resourceValues(forKeys: Set(keys))
            let name = url.lastPathComponent.lowercased()
            if values.isDirectory == true {
                if name.hasPrefix(".") || Languages.buildSegments.contains(name) || Languages.vendoredSegments.contains(name) {
                    walker.skipDescendants()
                }
                continue
            }
            guard values.isRegularFile == true, values.isSymbolicLink != true else { continue }
            let path = String(url.standardizedFileURL.path.dropFirst(base.count))
            found.append(InsightEntry(path: path, size: Int64(values.fileSize ?? 0)))
        }
        return found
    }

    func read(_ path: String, limit: Int64) throws -> [UInt8]? {
        let url = root.appendingPathComponent(path)
        guard let data = try? Data(contentsOf: url) else { return nil }
        return Int64(data.count) > limit ? [] : Array(data)
    }

    func history(_ options: InsightOptions, cancel: @escaping () -> Bool) throws -> [InsightCommit]? { nil }
}

final class InsightUnitTests: XCTestCase {
    func bytes(_ text: String) -> [UInt8] { Array(text.utf8) }

    func function(_ file: InsightFile, _ name: String) -> InsightFunction? {
        file.functions.first { $0.name == name || $0.name.hasSuffix("::" + name) }
    }

    func snapshot(_ files: [(String, String)]) -> InsightSnapshot {
        InsightSnapshot(files: files.map { Insight.measureSource($0.0, $0.1) })
    }

    func testLanguagesTestsAndVendoredPaths() {
        XCTAssertEqual(Insight.languageForPath("src/a.cpp"), "C++")
        XCTAssertEqual(Insight.languageForPath("a/b.tsx"), "TypeScript")
        XCTAssertEqual(Insight.languageForPath("x.py"), "Python")
        XCTAssertEqual(Insight.languageForPath("README.md"), "")
        XCTAssertTrue(Insight.isTestPath("app/tests/test_x.py"))
        XCTAssertTrue(Insight.isTestPath("src/foo.spec.ts"))
        XCTAssertTrue(Insight.isTestPath("Sources/FooTests.swift"))
        XCTAssertFalse(Insight.isTestPath("src/contest.cpp"))
        XCTAssertTrue(Insight.isVendoredPath("web/node_modules/react/index.js"))
        XCTAssertTrue(Insight.isVendoredPath("public/app.min.js"))
        XCTAssertFalse(Insight.isVendoredPath("src/vendorize.cpp"))
    }

    func testCppFunctionsComplexityAndAbstractClasses() {
        let source = """
        #include "foo.h"
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
        let file = Insight.measureSource("src/widget.cpp", source)
        XCTAssertEqual(file.language, "C++")
        XCTAssertEqual(file.commentLines, 1)
        XCTAssertTrue(file.imports.contains("foo.h") && file.imports.contains("<vector>"))
        XCTAssertEqual(function(file, "simple")?.complexity, 1)
        let branchy = function(file, "branchy")
        XCTAssertEqual(branchy?.complexity, 10)
        XCTAssertEqual(branchy?.parameters, 7)
        XCTAssertGreaterThanOrEqual(branchy?.nesting ?? 0, 5)
        XCTAssertEqual(branchy?.line, 17)
        XCTAssertEqual(file.types, 1)
        XCTAssertEqual(file.abstractTypes, 1)
    }

    func testPythonImportsDocstringsAndNesting() {
        let source = """
        import os, sys as system
        from .models import User, Group
        from ..core import util


        class Repo(ABC):
            \"\"\"Docstring
            spanning lines.\"\"\"

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
        let file = Insight.measureSource("pkg/sub/repo.py", source)
        XCTAssertTrue(file.imports.contains("os") && file.imports.contains("sys"))
        XCTAssertTrue(file.imports.contains(".models") && file.imports.contains("?.models.User"))
        let find = function(file, "find")
        XCTAssertEqual(find?.complexity, 7)
        XCTAssertEqual(find?.parameters, 2)
        XCTAssertEqual(find?.nesting, 2)
        XCTAssertEqual(function(file, "helper")?.complexity, 2)
        XCTAssertEqual(file.types, 1)
        XCTAssertEqual(file.abstractTypes, 1)
        XCTAssertGreaterThanOrEqual(file.commentLines, 2)
    }

    func testTypeScriptAndGo() {
        let ts = """
        import React, { useState } from 'react';
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
        let file = Insight.measureSource("web/src/app.ts", ts)
        for spec in ["react", "./types", "../shared/util", "./lazy", "./legacy.js"] {
            XCTAssertTrue(file.imports.contains(spec), spec)
        }
        XCTAssertEqual(function(file, "handler")?.complexity, 4)
        XCTAssertEqual(function(file, "plain")?.complexity, 1)

        let wrapped = """
        (function () {
          function inner(a) {
            if (a) { return 1; }
            return 0;
          }
          window.addEventListener('load', () => {
            const b = inner(2) || inner(3);
          });
        })();

        """
        let wrappedFile = Insight.measureSource("public/app.js", wrapped)
        XCTAssertEqual(wrappedFile.functions.map(\.name), ["inner"])
        XCTAssertEqual(wrappedFile.complexity, 3)

        let go = """
        package server

        import (
        \t"fmt"
        \tapi "example.com/app/internal/api"
        )

        func (s *Server) Handle(w Writer, r *Request) error {
        \tif r == nil {
        \t\treturn fmt.Errorf("nil")
        \t}
        \tswitch r.Method {
        \tcase "GET":
        \t\treturn nil
        \tcase "POST":
        \t\treturn nil
        \t}
        \treturn nil
        }

        """
        let goFile = Insight.measureSource("cmd/server/server.go", go)
        XCTAssertTrue(goFile.imports.contains("example.com/app/internal/api"))
        let handle = function(goFile, "Handle")
        XCTAssertEqual(handle?.complexity, 4)
        XCTAssertEqual(handle?.parameters, 2)
    }

    func testExtractImportsDropsOptionalNames() {
        XCTAssertEqual(Insight.extractImports("Python", "from .models import User\nimport os\n"), ["os", ".models"])
        XCTAssertEqual(Insight.extractImports("Markdown", "import x"), [])
    }

    func testResolutionAndCycles() {
        var s = snapshot([
            ("web/src/a.ts", "import { b } from './b';\nimport x from 'lodash';\n"),
            ("web/src/b.ts", "import { c } from './lib/c.js';\n"),
            ("web/src/lib/c.ts", "import { a } from '../a';\n"),
            ("pkg/__init__.py", ""),
            ("pkg/models.py", "from . import util\nfrom pkg.views import render\n"),
            ("pkg/util.py", "import json\n"),
            ("pkg/views.py", "from .models import Model\n"),
            ("native/src/core.cpp", "#include \"core.h\"\n#include <QString>\n"),
            ("native/include/core.h", "#pragma once\n"),
            ("ios/Sources/Feed.swift", "import UIKit\nstruct Feed { let item: FeedItem }\n"),
            ("ios/Sources/FeedItem.swift", "struct FeedItem { let id: Int }\n"),
            ("rs/src/lib.rs", "mod parser;\nuse crate::parser::Token;\n"),
            ("rs/src/parser.rs", "pub struct Token;\n"),
        ])
        Insight.resolveDependencies(&s)
        func depends(_ from: String, _ to: String) -> Bool {
            let a = s.indexOf(from)
            let b = s.indexOf(to)
            return a >= 0 && b >= 0 && s.files[a].dependsOn.contains(b)
        }
        XCTAssertTrue(depends("web/src/a.ts", "web/src/b.ts"))
        XCTAssertTrue(depends("web/src/b.ts", "web/src/lib/c.ts"))
        XCTAssertTrue(depends("web/src/lib/c.ts", "web/src/a.ts"))
        XCTAssertEqual(s.files[s.indexOf("web/src/a.ts")].externalImports, ["lodash"])
        XCTAssertTrue(depends("pkg/models.py", "pkg/util.py"))
        XCTAssertTrue(depends("pkg/models.py", "pkg/views.py"))
        XCTAssertTrue(depends("pkg/views.py", "pkg/models.py"))
        XCTAssertFalse(depends("pkg/views.py", "pkg/__init__.py"))
        XCTAssertTrue(depends("native/src/core.cpp", "native/include/core.h"))
        XCTAssertTrue(depends("ios/Sources/Feed.swift", "ios/Sources/FeedItem.swift"))
        XCTAssertTrue(depends("rs/src/lib.rs", "rs/src/parser.rs"))
        let b = s.indexOf("web/src/b.ts")
        XCTAssertTrue(s.files[b].dependedOnBy.contains(s.indexOf("web/src/a.ts")))
        Insight.markCycles(&s)
        XCTAssertEqual(s.cycles.count, 2)
        XCTAssertEqual(s.cycles.first?.count, 3)
        XCTAssertEqual(s.files[b].cycle, 0)
    }

    func testStronglyConnectedComponents() {
        let components = Insight.stronglyConnected([[1], [2], [0, 3], [], [4, 9]])
        XCTAssertEqual(components, [[3], [0, 1, 2], [4]])
    }

    func testHistoryCouplingsAndHotspots() {
        var s = snapshot([
            ("a.cpp", "int a() { return 1; }\n"),
            ("b.cpp", "int b() { return 2; }\n"),
            ("c.cpp", "int c() { return 3; }\n"),
        ])
        let now: Int64 = 1_800_000_000
        let day: Int64 = 86400
        var log = ""
        func commit(_ author: String, _ time: Int64, _ subject: String, _ files: [String]) {
            log += "\u{1e}h\(time)\u{1f}\(author)\u{1f}\(time)\u{1f}\(subject)\n\n"
            for file in files { log += "10\t2\t\(file)\n" }
        }
        commit("Ada", now - 10 * day, "Add feature", ["a.cpp", "b.cpp"])
        commit("Ada", now - 20 * day, "Fix crash in parser", ["a.cpp", "b.cpp"])
        commit("Grace", now - 300 * day, "Refactor", ["a.cpp", "b.cpp"])
        commit("Grace", now - 310 * day, "Tweak", ["a.cpp", "c.cpp"])
        let commits = Insight.parseGitLog(log)
        XCTAssertEqual(commits.count, 4)
        XCTAssertEqual(commits[1].files, [InsightCommitFile(path: "a.cpp", added: 10, deleted: 2), InsightCommitFile(path: "b.cpp", added: 10, deleted: 2)])
        Insight.applyHistory(&s, commits, InsightOptions(), now: now)
        let a = s.files[s.indexOf("a.cpp")]
        XCTAssertEqual(a.commits, 4)
        XCTAssertEqual(a.recentCommits, 2)
        XCTAssertEqual(a.fixCommits, 1)
        XCTAssertEqual(a.authors, 2)
        XCTAssertEqual(a.lastChanged, now - 10 * day)
        XCTAssertEqual(a.formerAuthorShare, 0.5, accuracy: 1e-12)
        XCTAssertEqual(s.formerAuthors, ["Grace"])
        XCTAssertEqual(s.couplings.count, 1)
        XCTAssertEqual(s.couplings.first?.shared, 3)
        XCTAssertEqual(s.couplings.first?.degree ?? 0, 3.0 / 3.5, accuracy: 1e-9)
        XCTAssertEqual(s.couplingsFor(s.indexOf("b.cpp")).first?.a, s.indexOf("b.cpp"))
        Insight.computeHotspots(&s)
        XCTAssertEqual(s.files[0].hotspot, 1.0)
    }

    func testBinaryNumstatCountsAsZero() {
        let commits = Insight.parseGitLog("\u{1e}c1\u{1f}Ann\u{1f}100\u{1f}Add\n\n-\t-\tlogo.png\n3\t1\tname with\ttab.c\n")
        XCTAssertEqual(commits.first?.files, [InsightCommitFile(path: "logo.png", added: nil, deleted: nil), InsightCommitFile(path: "name with\ttab.c", added: 3, deleted: 1)])
    }

    func testHealthScoring() {
        var heavy = InsightFile()
        heavy.codeLines = 2400
        for i in 0..<4 {
            heavy.functions.append(InsightFunction(name: "brain\(i)", line: 1, length: 150, complexity: 40, nesting: 6, parameters: 8))
        }
        heavy.maxComplexity = 40
        heavy.complexity = 160
        heavy.cycle = 0
        let result = Insight.computeHealth(heavy)
        XCTAssertEqual(Insight.healthBand(result.health), .unhealthy)
        XCTAssertEqual(result.issues.first, "Brain methods: brain0 (complexity 40, 150 lines), brain1 (complexity 40, 150 lines), brain2 (complexity 40, 150 lines) and 1 more")
        XCTAssertTrue(result.issues.contains("Large file: 2400 lines of code"))
        XCTAssertTrue(result.issues.contains("Part of a circular import"))
        var clean = InsightFile()
        clean.codeLines = 120
        clean.functions.append(InsightFunction(length: 20, complexity: 3))
        XCTAssertEqual(Insight.computeHealth(clean).health, 10.0)
        XCTAssertEqual(HealthBand.problematic.name, "Problematic")
        XCTAssertEqual(Insight.healthBand(7.9), .problematic)
        XCTAssertEqual(Insight.healthBand(8.0), .healthy)
    }

    func testModuleGraph() {
        var s = snapshot([
            ("core/a.ts", "export interface Port {}\n"),
            ("core/b.ts", "import { Port } from './a';\n"),
            ("ui/view.ts", "import { b } from '../core/b';\nimport { s } from '../store/s';\n"),
            ("store/s.ts", "import { v } from '../ui/view';\nimport { a } from '../core/a';\n"),
            ("main.ts", "import { v } from './ui/view';\n"),
        ])
        Insight.resolveDependencies(&s)
        Insight.markCycles(&s)
        let graph = Insight.buildModuleGraph(s, depth: 1)
        XCTAssertEqual(graph.nodes.count, 4)
        let core = graph.nodes.firstIndex { $0.path == "core" } ?? -1
        let ui = graph.nodes.firstIndex { $0.path == "ui" } ?? -1
        let root = graph.nodes.firstIndex { $0.path.isEmpty } ?? -1
        XCTAssertEqual(graph.nodes[core].layer, 0)
        XCTAssertEqual(graph.nodes[core].instability, 0.0)
        XCTAssertEqual(graph.nodes[core].abstractness, 1.0)
        XCTAssertGreaterThanOrEqual(graph.nodes[ui].cycle, 0)
        XCTAssertEqual(graph.nodes[root].layer, 2)
        XCTAssertEqual(graph.nodes[root].name, "(root files)")
        XCTAssertEqual(graph.cycles.count, 1)
        let edge = graph.edgeIndex(ui, core)
        XCTAssertGreaterThanOrEqual(edge, 0)
        XCTAssertEqual(graph.edges[edge].weight, 1)
        XCTAssertFalse(graph.edges[edge].inCycle)
        XCTAssertEqual(graph.nodes[core].internalEdges, 1)
        XCTAssertEqual(Insight.modulePathFor("a/b/c.ts", depth: 2), "a/b")
        XCTAssertEqual(Insight.modulePathFor("c.ts", depth: 2), "")
    }

    func testTrendMeasuresEachVersion() {
        let versions = [
            InsightVersion(time: 1, commit: "a", author: "Ann", bytes: bytes("int f() { return 1; }\n")),
            InsightVersion(time: 2, commit: "b", author: "Bob", bytes: bytes("int f(int a) {\n  if (a) { return 1; }\n  return 0;\n}\n")),
        ]
        let points = Insight.trend("x.cpp", versions: versions)
        XCTAssertEqual(points.map(\.complexity), [1, 2])
        XCTAssertEqual(points.map(\.codeLines), [1, 4])
        XCTAssertEqual(points.map(\.commit), ["a", "b"])
        XCTAssertEqual(Insight.trendPicks(commitCount: 3), [2, 1, 0])
        XCTAssertEqual(Insight.trendPicks(commitCount: 5, samples: 3), [4, 2, 0])
    }

    func testFolderAnalysisSkipsVendoredAndCountsSkipped() throws {
        let source = MemorySource(files: [
            ("src/main.js", bytes("const u = require('./util/u');\nfunction main() { if (u) { return 1; } }\n")),
            ("src/util/u.js", bytes("module.exports = function u(a) { return a && a.b; };\n")),
            ("node_modules/lib/x.js", bytes("function x() {}\n")),
            ("notes.md", bytes("# hi\n")),
            ("empty.py", []),
        ], git: false)
        var texts: [String] = []
        let snapshot = try Insight.analyze(source: source, progress: { text, _ in texts.append(text) })
        XCTAssertEqual(snapshot.error, "")
        XCTAssertEqual(snapshot.files.map(\.path), ["src/main.js", "src/util/u.js"])
        XCTAssertEqual(snapshot.files[snapshot.indexOf("src/main.js")].dependsOn.count, 1)
        XCTAssertEqual(snapshot.skippedFiles, 2)
        XCTAssertFalse(snapshot.git)
        XCTAssertEqual(texts, ["Listing files in repo…", "Reading 3 of 3 files…", "Connecting imports…", "Looking for duplicated code…", "Scoring code health…", "Done"])
    }

    func testEmptyAndMissingSourcesReportTheDesktopErrors() throws {
        let empty = MemorySource(files: [("notes.md", bytes("# hi\n"))])
        XCTAssertEqual(try Insight.analyze(source: empty).error, "No source code found in repo.")
        var missing = MemorySource(files: [])
        missing.exists = false
        XCTAssertEqual(try Insight.analyze(source: missing).error, "The folder memory:repo does not exist on this computer.")
        let folder = FolderSource(root: URL(fileURLWithPath: "/nonexistent/forkmesh-insight"))
        XCTAssertEqual(try Insight.analyze(source: folder).error, "The folder /nonexistent/forkmesh-insight does not exist on this computer.")
    }

    func testCancellationThrows() throws {
        let source = MemorySource(files: try Fixture.repoFiles(), commits: try Fixture.history())
        XCTAssertThrowsError(try Insight.analyze(source: source, isCancelled: { true })) { error in
            XCTAssertTrue(error is CancellationError)
        }
        var cancelled = false
        var seen: [String] = []
        XCTAssertThrowsError(try Insight.analyze(source: source, progress: { text, _ in
            seen.append(text)
            if text.hasPrefix("Reading") { cancelled = true }
        }, isCancelled: { cancelled })) { error in
            XCTAssertTrue(error is CancellationError)
        }
        XCTAssertFalse(seen.contains("Connecting imports…"))
    }

    func testMaxFilesCapWarns() throws {
        let source = MemorySource(files: (0..<3).map { ("f\($0).js", bytes("function f\($0)() {}\n")) }, git: false)
        var options = InsightOptions()
        options.maxFiles = 2
        let snapshot = try Insight.analyze(source: source, options: options)
        XCTAssertEqual(snapshot.files.count, 2)
        XCTAssertEqual(snapshot.warning, "Analyzed the first 2 of 3 source files")
    }

    func testFixtureTwentyTimesIsFast() throws {
        let source = MemorySource(files: try Fixture.repoFiles(), commits: try Fixture.history())
        let started = Date()
        for _ in 0..<20 { _ = try Insight.analyze(source: source, now: 1_792_000_000) }
        let seconds = Date().timeIntervalSince(started)
        print("FMCodeInsight: fixture analysed 20 times in \(String(format: "%.3f", seconds)) s")
        XCTAssertLessThan(seconds, 10)
    }

    func testRealTreeWhenAsked() throws {
        guard let root = ProcessInfo.processInfo.environment["FM_INSIGHT_ROOT"] else { return }
        let source = FolderSource(root: URL(fileURLWithPath: root))
        let started = Date()
        let snapshot = try Insight.analyze(source: source)
        let seconds = Date().timeIntervalSince(started)
        let edges = snapshot.files.reduce(0) { $0 + $1.dependsOn.count }
        let functions = snapshot.files.reduce(0) { $0 + $1.functions.count }
        print("FMCodeInsight: \(root): \(snapshot.files.count) files, \(snapshot.skippedFiles) skipped, \(edges) edges, \(functions) functions, \(snapshot.cycles.count) cycles in \(String(format: "%.2f", seconds)) s")
        XCTAssertEqual(snapshot.error, "")
    }
}
