#include "PromptDrop.h"
#include "WorkspaceOpenIn.h"
#include "WorkspaceShortcuts.h"
#include "WorktreeInclude.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>

#include <cstdio>

namespace {

int failures = 0;

void expect(bool condition, const char *what)
{
    if (condition)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", what);
}

void expectList(const QStringList &actual, const QStringList &expected, const char *what)
{
    if (actual == expected)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s\n  expected: %s\n  actual:   %s\n", what,
                 qPrintable(expected.join(QStringLiteral(", "))),
                 qPrintable(actual.join(QStringLiteral(", "))));
}

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool git(const QString &dir, const QStringList &args)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.start(QStringLiteral("git"), args);
    return process.waitForFinished(30000) && process.exitCode() == 0;
}

void shortcutTable()
{
    using namespace forkmesh::workspaceshortcuts;
    const QList<Binding> all = bindings();
    QSet<QString> keys;
    QSet<int> actions;
    for (const Binding &binding : all) {
        const QString text = binding.keys.toString(QKeySequence::PortableText);
        expect(!text.isEmpty(), "every shortcut has keys");
        expect(!binding.label.isEmpty(), "every shortcut has a label");
        expect(!keys.contains(text), "no two shortcuts share keys");
        keys.insert(text);
        actions.insert(int(binding.action));
    }
    expect(actions.size() == all.size(), "each action is listed once");
    expect(actions.contains(int(Action::ShowShortcuts)), "the sheet lists itself");
    expect(keysText(QKeySequence(Qt::SHIFT | Qt::Key_Backtab)).contains(QLatin1String("Tab")) &&
               !keysText(QKeySequence(Qt::SHIFT | Qt::Key_Backtab))
                    .contains(QLatin1String("Backtab")),
           "Backtab reads as Shift+Tab");
}

void editorDetection()
{
    using namespace forkmesh::openin;
    const QHash<QString, QString> onPath = {
        {QStringLiteral("vscodium"), QStringLiteral("/usr/bin/vscodium")},
        {QStringLiteral("zeditor"), QStringLiteral("/usr/bin/zeditor")},
        {QStringLiteral("zed"), QStringLiteral("/opt/zed/bin/zed")},
    };
    const QList<Editor> found =
        availableEditors([&onPath](const QString &name) { return onPath.value(name); });
    QStringList labels;
    QStringList programs;
    for (const Editor &editor : found) {
        labels << editor.label;
        programs << editor.program;
    }
    expectList(labels, {QStringLiteral("VSCodium"), QStringLiteral("Zed")},
               "only editors on PATH, in menu order");
    expectList(programs,
               {QStringLiteral("/usr/bin/vscodium"), QStringLiteral("/opt/zed/bin/zed")},
               "first program name found wins");
    expect(availableEditors({}).isEmpty(), "no finder, no editors");
}

void dropPlan(const QString &root)
{
    using namespace forkmesh::promptdrop;
    const QString repo = root + QStringLiteral("/repo");
    writeFile(repo + QStringLiteral("/src/main.cpp"), "int main() {}\n");
    writeFile(repo + QStringLiteral("/docs/my notes.md"), "x\n");
    writeFile(repo + QStringLiteral("/shot.png"), "png");
    writeFile(root + QStringLiteral("/outside.txt"), "x\n");
    QDir().mkpath(repo + QStringLiteral("/assets"));
    const auto media = [](const QString &path) { return path.endsWith(QLatin1String(".png")); };

    const Plan result = plan(
        {repo + QStringLiteral("/src/main.cpp"), repo + QStringLiteral("/shot.png"),
         repo + QStringLiteral("/assets"), repo + QStringLiteral("/docs/my notes.md"),
         root + QStringLiteral("/outside.txt"), repo + QStringLiteral("/src/main.cpp"),
         repo + QStringLiteral("/missing.txt")},
        {QString(), root + QStringLiteral("/elsewhere"), repo}, media);
    expectList(result.attachments, {repo + QStringLiteral("/shot.png")},
               "images become attachments");
    expectList(result.mentions,
               {QStringLiteral("@src/main.cpp"), QStringLiteral("@assets/"),
                QStringLiteral("@\"docs/my notes.md\""),
                QStringLiteral("@") + root + QStringLiteral("/outside.txt")},
               "files and folders become mentions, relative inside the repo");
    expect(insertion(result, QStringLiteral("look at")).startsWith(QLatin1String(" @src")),
           "a space separates the mention from earlier text");
    expect(insertion(result, QString()).startsWith(QLatin1String("@src")),
           "no leading space in an empty prompt");
    expect(insertion(Plan{}, QStringLiteral("x")).isEmpty(), "nothing to insert");
    expectList(localPaths({QUrl(QStringLiteral("https://example.com/a")),
                           QUrl::fromLocalFile(QStringLiteral("/tmp/a/../b.txt"))}),
               {QStringLiteral("/tmp/b.txt")}, "only local files are dropped");
}

void includeList()
{
    using namespace forkmesh::worktreeinclude;
    expectList(copyList({QStringLiteral(".env"), QStringLiteral("config/local.json"),
                         QStringLiteral("notes.txt"), QStringLiteral("build/.env"),
                         QStringLiteral("../escape"), QStringLiteral(".env")},
                        {QStringLiteral(".env"), QStringLiteral("build/"),
                         QStringLiteral("config/local.json")}),
               {QStringLiteral(".env"), QStringLiteral("config/local.json"),
                QStringLiteral("build/.env")},
               "only listed files that git ignores, once each");
}

void includeCopy(const QString &root)
{
    using namespace forkmesh::worktreeinclude;
    const QString repo = root + QStringLiteral("/include-repo");
    const QString tree = root + QStringLiteral("/include-tree");
    QDir().mkpath(repo);
    QDir().mkpath(tree);
    expect(!apply(repo, tree).configured, "no .worktreeinclude, nothing to do");
    if (!git(repo, {QStringLiteral("init"), QStringLiteral("-q")})) {
        expect(false, "git init");
        return;
    }
    writeFile(repo + QStringLiteral("/.gitignore"), ".env\n*.local\nsecrets/\n");
    writeFile(repo + QStringLiteral("/.worktreeinclude"), ".env\nsecrets/\nREADME.md\n");
    writeFile(repo + QStringLiteral("/.env"), "TOKEN=1\n");
    writeFile(repo + QStringLiteral("/app/.env"), "TOKEN=2\n");
    writeFile(repo + QStringLiteral("/secrets/key.pem"), "key\n");
    writeFile(repo + QStringLiteral("/settings.local"), "ignored, not listed\n");
    writeFile(repo + QStringLiteral("/README.md"), "listed, not ignored\n");
    writeFile(tree + QStringLiteral("/app/.env"), "kept\n");

    const Result result = apply(repo, tree);
    expect(result.configured, "a .worktreeinclude is read");
    expect(result.errors.isEmpty(), "copy reports no errors");
    expect(result.copied == 2, "two new files copied");
    expect(result.existing == 1, "a file already in the worktree is left alone");
    expect(readFile(tree + QStringLiteral("/.env")) == "TOKEN=1\n", ".env copied");
    expect(readFile(tree + QStringLiteral("/secrets/key.pem")) == "key\n",
           "files under a listed folder copied");
    expect(readFile(tree + QStringLiteral("/app/.env")) == "kept\n",
           "existing worktree file not overwritten");
    expect(!QFileInfo::exists(tree + QStringLiteral("/settings.local")),
           "ignored files not listed are not copied");
    expect(!QFileInfo::exists(tree + QStringLiteral("/README.md")),
           "listed files git does not ignore are not copied");
}

}

int main(int argc, char **argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QTemporaryDir root;
    if (!root.isValid()) {
        std::fprintf(stderr, "FAIL temporary directory\n");
        return 1;
    }
    qputenv("GIT_CONFIG_GLOBAL", "/dev/null");
    qputenv("GIT_CONFIG_NOSYSTEM", "1");
    qputenv("GIT_CEILING_DIRECTORIES", QFile::encodeName(root.path()));
    qputenv("HOME", QFile::encodeName(root.path()));
    shortcutTable();
    editorDetection();
    dropPlan(root.path());
    includeList();
    includeCopy(root.path());
    if (failures == 0)
        std::printf("workspace parity: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
