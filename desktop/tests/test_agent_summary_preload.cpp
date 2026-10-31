#include "../src/AgentSummaryPreload.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdio>

namespace as = forkmesh::agentsummary;

namespace {

int g_failures = 0;

void check(bool ok, const char *what)
{
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

QByteArray readAll(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void testScript()
{
    const QByteArray script = as::script();
    check(script.startsWith("// SPDX-License-Identifier: MIT\n"),
          "the script keeps its MIT header");
    check(script.contains("\"[AgentSummary] Timer fired\""),
          "the script matches the summarizer log line");
    check(script.contains("\"agent_summary\""),
          "the script matches the summarizer query source");
    check(script.contains("2 ** 31 - 1"), "the inert timer never fires");
    check(script.contains("Bun.main.startsWith(\"/$bunfs/\")"),
          "the hook only installs inside the compiled Claude Code binary");
    check(script.endsWith("if (IN_COMPILED_BUN) install();\n"),
          "the script ends with its install call");
}

void testUsablePath()
{
    check(as::usablePath(QStringLiteral("/home/a/data/agents/no-agent-summary.js")),
          "a plain absolute path is usable");
    check(!as::usablePath(QStringLiteral("agents/no-agent-summary.js")),
          "a relative path is refused");
    check(!as::usablePath(
              QStringLiteral("/Users/a/Library/Application Support/ForkMesh/x.js")),
          "a path with a space is refused");
    check(!as::usablePath(QStringLiteral("/home/a/b\"c/x.js")),
          "a path with a quote is refused");
    check(!as::usablePath(QStringLiteral("/home/a/b\\c/x.js")),
          "a path with a backslash is refused");
}

void testWithPreload()
{
    const QString path = QStringLiteral("/d/no-agent-summary.js");
    check(as::withPreload(QString(), path) == QStringLiteral("--preload /d/no-agent-summary.js"),
          "an empty BUN_OPTIONS gets just the preload");
    check(as::withPreload(QStringLiteral("  --smol "), path) ==
              QStringLiteral("--smol --preload /d/no-agent-summary.js"),
          "existing options are kept in front");
    const QString once = as::withPreload(QStringLiteral("--smol"), path);
    check(as::withPreload(once, path) == once, "the preload is added once");
    check(as::withPreload(QStringLiteral("--preload /d/no-agent-summary.js.bak"), path) ==
              QStringLiteral("--preload /d/no-agent-summary.js.bak "
                             "--preload /d/no-agent-summary.js"),
          "a longer path with the same prefix is not mistaken for ours");
}

void testInstall()
{
    QTemporaryDir root;
    check(root.isValid(), "temporary folder");
    const QString dir = root.path() + QStringLiteral("/agents");
    const QString path = as::install(dir);
    check(path == QDir(dir).absoluteFilePath(as::kFileName), "installs into the folder");
    check(readAll(path) == as::script(), "the installed file is the script");

    QFile stale(path);
    check(stale.open(QIODevice::WriteOnly | QIODevice::Truncate), "open for overwrite");
    stale.write("stale");
    stale.close();
    check(as::install(dir) == path && readAll(path) == as::script(),
          "a changed file is rewritten");

    check(as::install(QString()).isEmpty(), "no folder, no install");
    const QString spaced = root.path() + QStringLiteral("/with space");
    check(as::install(spaced).isEmpty(), "a folder with a space is skipped");
    check(!QFileInfo::exists(spaced), "a skipped folder is not created");
}

void testApplyEnvironment()
{
    QTemporaryDir root;
    const QString spaced = root.path() + QStringLiteral("/with space");
    const QString plain = root.path() + QStringLiteral("/cache/agents");
    QProcessEnvironment env;
    env.insert(as::kOptionsVariable, QStringLiteral("--smol"));
    const bool applied = as::applyEnvironment(env, {spaced, plain});
#ifdef Q_OS_WIN
    check(!applied, "Windows launches are left alone");
    check(env.value(as::kOptionsVariable) == QStringLiteral("--smol"),
          "Windows BUN_OPTIONS is untouched");
#else
    const QString path = QDir(plain).absoluteFilePath(as::kFileName);
    check(applied, "the preload is applied");
    check(env.value(as::kOptionsVariable) ==
              QStringLiteral("--smol --preload ") + path,
          "falls back to the folder without a space");
    check(QFileInfo::exists(path), "the preload file exists before it is named");

    QProcessEnvironment none;
    check(!as::applyEnvironment(none, {spaced}), "nothing usable, nothing applied");
    check(!none.contains(as::kOptionsVariable),
          "BUN_OPTIONS is not set to a missing preload");
#endif
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testScript();
    testUsablePath();
    testWithPreload();
    testInstall();
    testApplyEnvironment();
    if (g_failures == 0)
        std::printf("agent summary preload tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
