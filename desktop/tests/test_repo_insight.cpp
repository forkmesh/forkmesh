#include "RepoInsight.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh::insight;

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition) {
        std::printf("PASS: %s\n", qPrintable(message));
    } else {
        ++failures;
        std::printf("FAIL: %s\n", qPrintable(message));
    }
}

const FunctionInfo *findFunction(const FileInfo &file, const QString &name)
{
    for (const FunctionInfo &fn : file.functions)
        if (fn.name == name || fn.name.endsWith(QLatin1String("::") + name))
            return &fn;
    return nullptr;
}

void testLanguages()
{
    check(languageForPath(QStringLiteral("src/a.cpp")) == QLatin1String("C++"), "cpp is C++");
    check(languageForPath(QStringLiteral("a/b.tsx")) == QLatin1String("TypeScript"), "tsx is TypeScript");
    check(languageForPath(QStringLiteral("x.py")) == QLatin1String("Python"), "py is Python");
    check(languageForPath(QStringLiteral("README.md")).isEmpty(), "markdown is not source");
    check(isTestPath(QStringLiteral("app/tests/test_x.py")), "tests folder is a test");
    check(isTestPath(QStringLiteral("src/foo.spec.ts")), "spec file is a test");
    check(isTestPath(QStringLiteral("Sources/FooTests.swift")), "XCTest file is a test");
    check(!isTestPath(QStringLiteral("src/contest.cpp")), "contest is not a test");
    check(isVendoredPath(QStringLiteral("web/node_modules/react/index.js")), "node_modules is vendored");
    check(isVendoredPath(QStringLiteral("public/app.min.js")), "minified is vendored");
    check(!isVendoredPath(QStringLiteral("src/vendorize.cpp")), "file name with vendor is not vendored");
}

void testCpp()
{
    const QByteArray source = R"(#include "foo.h"
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
)";
    const FileInfo file = measureSource(QStringLiteral("src/widget.cpp"), source);
    check(file.language == QLatin1String("C++"), "C++ language detected");
    check(file.commentLines == 1, QStringLiteral("one comment line (got %1)").arg(file.commentLines));
    check(file.imports.contains(QLatin1String("foo.h")) && file.imports.contains(QLatin1String("<vector>")),
          "includes extracted");
    const FunctionInfo *simple = findFunction(file, QStringLiteral("simple"));
    check(simple && simple->complexity == 1, "simple has complexity 1");
    const FunctionInfo *branchy = findFunction(file, QStringLiteral("branchy"));
    check(branchy != nullptr, "branchy found");
    if (branchy) {
        check(branchy->complexity == 10,
              QStringLiteral("branchy complexity 10 (got %1)").arg(branchy->complexity));
        check(branchy->parameters == 7, QStringLiteral("branchy has 7 parameters (got %1)").arg(branchy->parameters));
        check(branchy->nesting >= 5, QStringLiteral("branchy nesting >= 5 (got %1)").arg(branchy->nesting));
        check(branchy->line == 17, QStringLiteral("branchy starts on line 17 (got %1)").arg(branchy->line));
    }
    check(file.types == 1 && file.abstractTypes == 1, "abstract class counted");
}

void testPython()
{
    const QByteArray source = R"(import os, sys as system
from .models import User, Group
from ..core import util


class Repo(ABC):
    """Docstring
    spanning lines."""

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
)";
    const FileInfo file = measureSource(QStringLiteral("pkg/sub/repo.py"), source);
    check(file.imports.contains(QLatin1String("os")) && file.imports.contains(QLatin1String("sys")),
          "python plain imports");
    check(file.imports.contains(QLatin1String(".models")) && file.imports.contains(QLatin1String("?.models.User")),
          "python relative from-import with optional names");
    const FunctionInfo *find = findFunction(file, QStringLiteral("find"));
    check(find && find->complexity == 7, QStringLiteral("find complexity 7 (got %1)").arg(find ? find->complexity : -1));
    check(find && find->parameters == 2, "self is not a parameter");
    check(find && find->nesting == 2, QStringLiteral("find nesting 2 (got %1)").arg(find ? find->nesting : -1));
    const FunctionInfo *helper = findFunction(file, QStringLiteral("helper"));
    check(helper && helper->complexity == 2, "helper complexity 2");
    check(file.types == 1 && file.abstractTypes == 1, "ABC class is abstract");
    check(file.commentLines >= 2, QStringLiteral("docstring counted as comment (got %1)").arg(file.commentLines));
}

void testTypeScriptAndGo()
{
    const QByteArray ts = R"(import React, { useState } from 'react';
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
)";
    const FileInfo file = measureSource(QStringLiteral("web/src/app.ts"), ts);
    for (const char *spec : {"react", "./types", "../shared/util", "./lazy", "./legacy.js"})
        check(file.imports.contains(QLatin1String(spec)), QStringLiteral("ts import %1").arg(QLatin1String(spec)));
    const FunctionInfo *handler = findFunction(file, QStringLiteral("handler"));
    check(handler && handler->complexity == 4,
          QStringLiteral("handler complexity 4 (got %1)").arg(handler ? handler->complexity : -1));
    const FunctionInfo *plain = findFunction(file, QStringLiteral("plain"));
    check(plain && plain->complexity == 1, "optional parameter is not a ternary");

    const QByteArray wrapped = R"((function () {
  function inner(a) {
    if (a) { return 1; }
    return 0;
  }
  window.addEventListener('load', () => {
    const b = inner(2) || inner(3);
  });
})();
)";
    const FileInfo wrappedFile = measureSource(QStringLiteral("public/app.js"), wrapped);
    check(wrappedFile.functions.size() == 1 && wrappedFile.functions.first().name == QLatin1String("inner"),
          QStringLiteral("IIFE wrapper is not a function (got %1)").arg(wrappedFile.functions.size()));
    check(wrappedFile.complexity == 3, QStringLiteral("wrapper decisions count to the file (got %1)").arg(wrappedFile.complexity));

    const QByteArray go = R"(package server

import (
	"fmt"
	api "example.com/app/internal/api"
)

func (s *Server) Handle(w Writer, r *Request) error {
	if r == nil {
		return fmt.Errorf("nil")
	}
	switch r.Method {
	case "GET":
		return nil
	case "POST":
		return nil
	}
	return nil
}
)";
    const FileInfo goFile = measureSource(QStringLiteral("cmd/server/server.go"), go);
    check(goFile.imports.contains(QLatin1String("example.com/app/internal/api")), "go block import");
    const FunctionInfo *handle = findFunction(goFile, QStringLiteral("Handle"));
    check(handle && handle->complexity == 4,
          QStringLiteral("Go Handle complexity 4 (got %1)").arg(handle ? handle->complexity : -1));
    check(handle && handle->parameters == 2, "Go receiver is not a parameter");
}

Snapshot snapshotOf(const QList<QPair<QString, QByteArray>> &files)
{
    Snapshot snapshot;
    for (const auto &entry : files) {
        snapshot.byPath.insert(entry.first, int(snapshot.files.size()));
        snapshot.files.append(measureSource(entry.first, entry.second));
    }
    return snapshot;
}

void testResolution()
{
    Snapshot snapshot = snapshotOf({
        {QStringLiteral("web/src/a.ts"), "import { b } from './b';\nimport x from 'lodash';\n"},
        {QStringLiteral("web/src/b.ts"), "import { c } from './lib/c.js';\n"},
        {QStringLiteral("web/src/lib/c.ts"), "import { a } from '../a';\n"},
        {QStringLiteral("pkg/__init__.py"), ""},
        {QStringLiteral("pkg/models.py"), "from . import util\nfrom pkg.views import render\n"},
        {QStringLiteral("pkg/util.py"), "import json\n"},
        {QStringLiteral("pkg/views.py"), "from .models import Model\n"},
        {QStringLiteral("native/src/core.cpp"), "#include \"core.h\"\n#include <QString>\n"},
        {QStringLiteral("native/include/core.h"), "#pragma once\n"},
        {QStringLiteral("ios/Sources/Feed.swift"), "import UIKit\nstruct Feed { let item: FeedItem }\n"},
        {QStringLiteral("ios/Sources/FeedItem.swift"), "struct FeedItem { let id: Int }\n"},
        {QStringLiteral("rs/src/lib.rs"), "mod parser;\nuse crate::parser::Token;\n"},
        {QStringLiteral("rs/src/parser.rs"), "pub struct Token;\n"},
    });
    resolveDependencies(snapshot);
    const auto depends = [&snapshot](const char *from, const char *to) {
        const int a = snapshot.indexOf(QLatin1String(from));
        const int b = snapshot.indexOf(QLatin1String(to));
        return a >= 0 && b >= 0 && snapshot.files.at(a).dependsOn.contains(b);
    };
    check(depends("web/src/a.ts", "web/src/b.ts"), "relative TS import resolves");
    check(depends("web/src/b.ts", "web/src/lib/c.ts"), "ESM .js specifier resolves to .ts");
    check(depends("web/src/lib/c.ts", "web/src/a.ts"), "parent relative import resolves");
    check(snapshot.files.at(snapshot.indexOf(QStringLiteral("web/src/a.ts"))).externalImports ==
              QStringList{QStringLiteral("lodash")},
          "package import is external");
    check(depends("pkg/models.py", "pkg/util.py"), "python from . import module resolves");
    check(depends("pkg/models.py", "pkg/views.py"), "python absolute package import resolves");
    check(depends("pkg/views.py", "pkg/models.py"), "python relative module resolves");
    check(!depends("pkg/views.py", "pkg/__init__.py"), "optional name does not add the package twice");
    check(depends("native/src/core.cpp", "native/include/core.h"), "C++ include resolves by suffix");
    check(depends("ios/Sources/Feed.swift", "ios/Sources/FeedItem.swift"), "Swift type reference resolves");
    check(depends("rs/src/lib.rs", "rs/src/parser.rs"), "Rust mod resolves");
    const int b = snapshot.indexOf(QStringLiteral("web/src/b.ts"));
    check(snapshot.files.at(b).dependedOnBy.contains(snapshot.indexOf(QStringLiteral("web/src/a.ts"))),
          "reverse edges filled");
    markCycles(snapshot);
    check(snapshot.cycles.size() == 2, QStringLiteral("two cycles (got %1)").arg(snapshot.cycles.size()));
    check(snapshot.cycles.value(0).size() == 3, "TS cycle has three files");
    check(snapshot.files.at(b).cycle == 0, "file knows its cycle");
}

void testHistoryAndHealth()
{
    Snapshot snapshot = snapshotOf({
        {QStringLiteral("a.cpp"), "int a() { return 1; }\n"},
        {QStringLiteral("b.cpp"), "int b() { return 2; }\n"},
        {QStringLiteral("c.cpp"), "int c() { return 3; }\n"},
    });
    const qint64 now = 1800000000;
    const qint64 day = 86400;
    QByteArray log;
    const auto commit = [&log](const char *author, qint64 time, const char *subject,
                               std::initializer_list<const char *> files) {
        log += "\x1e";
        log += QByteArray::number(qHash(QByteArray::number(time)), 16);
        log += "\x1f";
        log += author;
        log += "\x1f";
        log += QByteArray::number(time);
        log += "\x1f";
        log += subject;
        log += "\n\n";
        for (const char *file : files) {
            log += "10\t2\t";
            log += file;
            log += "\n";
        }
    };
    commit("Ada", now - 10 * day, "Add feature", {"a.cpp", "b.cpp"});
    commit("Ada", now - 20 * day, "Fix crash in parser", {"a.cpp", "b.cpp"});
    commit("Grace", now - 300 * day, "Refactor", {"a.cpp", "b.cpp"});
    commit("Grace", now - 310 * day, "Tweak", {"a.cpp", "c.cpp"});
    Options options;
    applyHistory(snapshot, log, options, now);
    const FileInfo &a = snapshot.files.at(snapshot.indexOf(QStringLiteral("a.cpp")));
    check(a.commits == 4, "a has four commits");
    check(a.recentCommits == 2, "two recent commits");
    check(a.fixCommits == 1, "one fix commit");
    check(a.authors == 2, "two authors");
    check(a.lastChanged == now - 10 * day, "last changed");
    check(qFuzzyCompare(a.formerAuthorShare, 0.5), "half the code by former contributors");
    check(snapshot.formerAuthors == QStringList{QStringLiteral("Grace")}, "Grace is a former author");
    check(snapshot.couplings.size() == 1, QStringLiteral("one coupling (got %1)").arg(snapshot.couplings.size()));
    if (!snapshot.couplings.isEmpty()) {
        const Coupling &c = snapshot.couplings.first();
        check(c.shared == 3, "a and b share three commits");
        check(std::abs(c.degree - 3.0 / 3.5) < 1e-9, "coupling degree");
    }
    check(snapshot.couplingsFor(snapshot.indexOf(QStringLiteral("b.cpp"))).value(0).a ==
              snapshot.indexOf(QStringLiteral("b.cpp")),
          "couplingsFor puts the file first");
    computeHotspots(snapshot);
    check(a.hotspot == 0.0 || snapshot.files.at(0).hotspot == 1.0, "hottest file scores 1");

    FileInfo heavy;
    heavy.codeLines = 2400;
    for (int i = 0; i < 4; ++i) {
        FunctionInfo fn;
        fn.name = QStringLiteral("brain%1").arg(i);
        fn.length = 150;
        fn.complexity = 40;
        fn.nesting = 6;
        fn.parameters = 8;
        heavy.functions.append(fn);
    }
    heavy.maxComplexity = 40;
    heavy.complexity = 160;
    heavy.cycle = 0;
    QStringList issues;
    const double health = computeHealth(heavy, &issues);
    check(healthBand(health) == HealthBand::Unhealthy, QStringLiteral("heavy file is unhealthy (%1)").arg(health));
    check(!issues.isEmpty() && issues.first().startsWith(QLatin1String("Brain method")), "brain method reported");
    FileInfo clean;
    clean.codeLines = 120;
    FunctionInfo small;
    small.complexity = 3;
    small.length = 20;
    clean.functions.append(small);
    check(computeHealth(clean, nullptr) == 10.0, "small clean file scores 10");
}

void testModules()
{
    Snapshot snapshot = snapshotOf({
        {QStringLiteral("core/a.ts"), "export interface Port {}\n"},
        {QStringLiteral("core/b.ts"), "import { Port } from './a';\n"},
        {QStringLiteral("ui/view.ts"), "import { b } from '../core/b';\nimport { s } from '../store/s';\n"},
        {QStringLiteral("store/s.ts"), "import { v } from '../ui/view';\nimport { a } from '../core/a';\n"},
        {QStringLiteral("main.ts"), "import { v } from './ui/view';\n"},
    });
    resolveDependencies(snapshot);
    markCycles(snapshot);
    const ModuleGraph graph = buildModuleGraph(snapshot, 1);
    check(graph.nodes.size() == 4, QStringLiteral("four modules (got %1)").arg(graph.nodes.size()));
    int core = -1;
    int ui = -1;
    int root = -1;
    for (int i = 0; i < graph.nodes.size(); ++i) {
        if (graph.nodes.at(i).path == QLatin1String("core"))
            core = i;
        if (graph.nodes.at(i).path == QLatin1String("ui"))
            ui = i;
        if (graph.nodes.at(i).path.isEmpty())
            root = i;
    }
    check(core >= 0 && graph.nodes.at(core).layer == 0, "core is the foundation layer");
    check(core >= 0 && graph.nodes.at(core).instability == 0.0, "core is fully stable");
    check(core >= 0 && graph.nodes.at(core).abstractness == 1.0, "core is abstract");
    check(ui >= 0 && graph.nodes.at(ui).cycle >= 0, "ui is in a module cycle");
    check(root >= 0 && graph.nodes.at(root).layer == 2, "root module sits on top");
    check(graph.cycles.size() == 1, "one module cycle");
    const int edge = graph.edgeIndex(ui, core);
    check(edge >= 0 && graph.edges.at(edge).weight == 1 && !graph.edges.at(edge).inCycle, "ui uses core");
    check(core >= 0 && graph.nodes.at(core).internalEdges == 1, "core internal edge");
}

void testAnalyzeFolder()
{
    QTemporaryDir dir;
    QDir root(dir.path());
    root.mkpath(QStringLiteral("src/util"));
    root.mkpath(QStringLiteral("node_modules/lib"));
    const auto write = [&root](const QString &path, const QByteArray &bytes) {
        QFile file(root.filePath(path));
        file.open(QIODevice::WriteOnly);
        file.write(bytes);
    };
    write(QStringLiteral("src/main.js"), "const u = require('./util/u');\nfunction main() { if (u) { return 1; } }\n");
    write(QStringLiteral("src/util/u.js"), "module.exports = function u(a) { return a && a.b; };\n");
    write(QStringLiteral("node_modules/lib/x.js"), "function x() {}\n");
    write(QStringLiteral("notes.md"), "# hi\n");
    const SnapshotPtr snapshot = analyze(dir.path());
    check(snapshot && snapshot->error.isEmpty(), "folder analysis succeeds");
    if (!snapshot)
        return;
    check(snapshot->files.size() == 2, QStringLiteral("two source files (got %1)").arg(snapshot->files.size()));
    const int main = snapshot->indexOf(QStringLiteral("src/main.js"));
    check(main >= 0 && snapshot->files.at(main).dependsOn.size() == 1, "folder import resolved");
    check(!snapshot->git, "plain folder has no git");
}

void runOn(const QString &root)
{
    QElapsedTimer timer;
    timer.start();
    const SnapshotPtr snapshot = analyze(root, Options(), [](const QString &text, double fraction) {
        std::printf("  %3d%% %s\n", int(fraction * 100), qPrintable(text));
        std::fflush(stdout);
    });
    if (!snapshot) {
        std::printf("canceled\n");
        return;
    }
    int edges = 0;
    int functions = 0;
    QHash<QString, int> languages;
    QHash<QString, int> unresolved;
    for (const FileInfo &file : snapshot->files) {
        edges += int(file.dependsOn.size());
        functions += int(file.functions.size());
        ++languages[file.language];
        unresolved[file.language] += int(file.externalImports.size());
    }
    std::printf("%s\nerror: %s\nfiles %lld, edges %d, functions %d, cycles %lld, couplings %lld, commits %d, %lld ms\n",
                qPrintable(snapshot->name), qPrintable(snapshot->error), qlonglong(snapshot->files.size()),
                edges, functions, qlonglong(snapshot->cycles.size()), qlonglong(snapshot->couplings.size()),
                snapshot->commitsScanned, qlonglong(timer.elapsed()));
    for (auto it = languages.cbegin(); it != languages.cend(); ++it)
        std::printf("  %-12s %6d files, %6d external imports\n", qPrintable(it.key()), it.value(),
                    unresolved.value(it.key()));
    QList<FileInfo> worst = snapshot->files;
    std::sort(worst.begin(), worst.end(), [](const FileInfo &a, const FileInfo &b) { return a.health < b.health; });
    for (int i = 0; i < qMin<int>(8, worst.size()); ++i)
        std::printf("  health %4.1f  hot %.2f  %s  [%s]\n", worst.at(i).health, worst.at(i).hotspot,
                    qPrintable(worst.at(i).path), qPrintable(worst.at(i).healthIssues.join(QStringLiteral("; ")).left(160)));
    int bands[3] = {0, 0, 0};
    QHash<QString, QStringList> internal;
    for (const FileInfo &file : snapshot->files) {
        ++bands[int(healthBand(file.health))];
        for (const QString &spec : file.externalImports) {
            const bool local = spec.startsWith(QLatin1Char('.')) || spec.contains(QLatin1String("forkmesh")) ||
                               (file.language == QLatin1String("C++") && !spec.startsWith(QLatin1Char('<')));
            if (local && internal[file.language].size() < 6)
                internal[file.language].append(file.path + QStringLiteral(" -> ") + spec);
        }
    }
    std::printf("healthy %d, problematic %d, unhealthy %d\n", bands[0], bands[1], bands[2]);
    for (auto it = internal.cbegin(); it != internal.cend(); ++it)
        for (const QString &line : it.value())
            std::printf("  unresolved %s: %s\n", qPrintable(it.key()), qPrintable(line));
    const ModuleGraph graph = buildModuleGraph(*snapshot);
    std::printf("modules at depth %d: %lld, module edges %lld, module cycles %lld\n", graph.depth,
                qlonglong(graph.nodes.size()), qlonglong(graph.edges.size()), qlonglong(graph.cycles.size()));
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString root = qEnvironmentVariable("FORKMESH_INSIGHT_ROOT");
    if (!root.isEmpty()) {
        runOn(root);
        return 0;
    }
    testLanguages();
    testCpp();
    testPython();
    testTypeScriptAndGo();
    testResolution();
    testHistoryAndHealth();
    testModules();
    testAnalyzeFolder();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
