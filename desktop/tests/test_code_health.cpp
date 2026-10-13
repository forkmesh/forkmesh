#include "CodeHealth.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh::codehealth;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool writeFile(const QString &path, const QString &text)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray bytes = text.toUtf8();
    return file.write(bytes) == bytes.size();
}

const Function *findFunction(const FileHealth &file, const char *name)
{
    for (const Function &fn : file.functions) {
        if (fn.name == QLatin1String(name))
            return &fn;
    }
    return nullptr;
}

bool hasFinding(const Function &fn, const char *kind)
{
    for (const Finding &finding : fn.findings) {
        if (finding.kind == QLatin1String(kind))
            return true;
    }
    return false;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    // --- stripNoise must never move a line ---------------------------------
    // The whole report's line numbers rest on this. An earlier version let an
    // unterminated literal swallow newlines and every span shifted.
    const QString tricky = QStringLiteral(
        "int a = 1; // brace } in a comment\n"
        "/* block { with braces }\n"
        "   still in the comment { */ int b = 2;\n"
        "QString s = \"a } brace \\\" inside a string {\";\n"
        "char c = '}';\n"
        "char q = '\\'';\n"
        "int unterminated = 3; // \"never closed\n"
        "int d = 4;\n");
    const QString clean = stripNoise(tricky);
    check(clean.count(QLatin1Char('\n')) == tricky.count(QLatin1Char('\n')),
          "stripNoise preserves the line count");
    const QStringList cleanLines = clean.split(QLatin1Char('\n'));
    const QStringList rawLines = tricky.split(QLatin1Char('\n'));
    check(cleanLines.size() == rawLines.size(),
          "stripNoise preserves every line");
    for (int i = 0; i < cleanLines.size(); ++i) {
        check(cleanLines.at(i).size() == rawLines.at(i).size(),
              "stripNoise preserves every column");
    }
    check(!clean.contains(QLatin1String("brace } in a comment")),
          "line comments are blanked");
    check(!clean.contains(QLatin1String("still in the comment")),
          "block comments are blanked across lines");
    check(clean.contains(QLatin1String("int b = 2;")),
          "code after a block comment survives");
    check(clean.contains(QLatin1String("int d = 4;")),
          "an unterminated literal does not eat the following line");
    check(clean.count(QLatin1Char('{')) == clean.count(QLatin1Char('}')),
          "braces balance once comments and literals are blanked");

    // --- function spans ----------------------------------------------------
    const QString source = QStringLiteral(
        "#include <QString>\n"
        "\n"
        "void Widget::paintRows()\n"
        "{\n"
        "    for (int i = 0; i < rows; ++i) {\n"
        "        cell(i)->setStyleSheet(rule);\n"
        "    }\n"
        "}\n"
        "\n"
        "void Widget::quiet()\n"
        "{\n"
        "    return;\n"
        "}\n");
    const QVector<Function> functions = scanFunctions(stripNoise(source));
    check(functions.size() == 2, "both definitions are found");
    if (functions.size() == 2) {
        check(functions.at(0).name == QLatin1String("Widget::paintRows"),
              "first function is named");
        check(functions.at(0).line == 3, "definition line is 1-based and exact");
        check(functions.at(0).loc == 6, "span runs to the closing brace");
        check(functions.at(1).line == 10, "second definition line is exact");
    }

    // A return type ending in `*` or `&` hugs the name. Missing these once hid
    // every pointer-returning page builder from the length score.
    const QVector<Function> pointers = scanFunctions(stripNoise(QStringLiteral(
        "QWidget *Page::build()\n"
        "{\n"
        "    return nullptr;\n"
        "}\n"
        "const QString &label()\n"
        "{\n"
        "    return text;\n"
        "}\n")));
    check(pointers.size() == 2, "pointer and reference returns are definitions");
    if (pointers.size() == 2) {
        check(pointers.at(0).name == QLatin1String("Page::build"),
              "a pointer-returning member is named");
        check(pointers.at(1).name == QLatin1String("label"),
              "a reference-returning free function is named");
    }

    // --- hazards, end to end ----------------------------------------------
    QTemporaryDir temp;
    check(temp.isValid(), "temporary directory");
    const QString root = temp.path();
    const QString src = QDir(root).filePath(QStringLiteral("src"));
    check(QDir().mkpath(src), "create source directory");
    check(writeFile(QDir(src).filePath(QStringLiteral("Widget.cpp")), source),
          "write the hazard fixture");

    const QString clean2 = QStringLiteral(
        "void Helper::tidy()\n"
        "{\n"
        "    int total = 0;\n"
        "    for (int i = 0; i < 4; ++i)\n"
        "        total += i;\n"
        "}\n");
    check(writeFile(QDir(src).filePath(QStringLiteral("Helper.cpp")), clean2),
          "write the clean fixture");

    Report report = analyse(root, {QStringLiteral("src")}, QString());
    check(report.files.size() == 2, "both files analysed");
    check(report.totalFunctions == 3, "functions counted across files");
    check(!report.cancelled, "an uncancelled scan does not report cancellation");

    const FileHealth *widget = nullptr;
    const FileHealth *helper = nullptr;
    for (const FileHealth &file : report.files) {
        if (file.display == QLatin1String("Widget.cpp"))
            widget = &file;
        if (file.display == QLatin1String("Helper.cpp"))
            helper = &file;
    }
    check(widget && helper, "both fixtures are present by name");
    if (widget) {
        const Function *paint = findFunction(*widget, "Widget::paintRows");
        check(paint != nullptr, "the hazardous function is reported");
        if (paint) {
            check(hasFinding(*paint, "stylesheet-in-loop"),
                  "setStyleSheet inside a loop is flagged");
            check(paint->loopDepth == 1, "a single loop is depth 1");
        }
        const Function *quiet = findFunction(*widget, "Widget::quiet");
        check(quiet && quiet->findings.isEmpty(),
              "a function with no loop carries no finding");
    }
    if (helper) {
        check(helper->findingCount == 0,
              "an ordinary counting loop is not flagged");
        check(helper->health == 100, "a clean small file scores 100");
    }
    if (widget && helper) {
        check(widget->health < helper->health,
              "the hazardous file ranks below the clean one");
        check(report.files.first().display == QLatin1String("Widget.cpp"),
              "files are ordered worst health first");
    }

    // --- loop spans, test-only code and acknowledgements -------------------
    QTemporaryDir precise;
    check(precise.isValid(), "second temporary directory");
    const QString preciseSrc = QDir(precise.path()).filePath(QStringLiteral("src"));
    check(QDir().mkpath(preciseSrc), "create the precision source directory");
    const QString spans = QStringLiteral(
        "void Rows::siblings()\n"
        "{\n"
        "    for (int i = 0; i < 4; ++i) {\n"
        "        for (int j = 0; j < 4; ++j)\n"
        "            total += j;\n"
        "        for (int k = 0; k < 4; ++k)\n"
        "            total += k;\n"
        "    }\n"
        "    label->setStyleSheet(rule);\n"
        "}\n"
        "\n"
        "void Rows::braceless()\n"
        "{\n"
        "    for (const auto &row : rows)\n"
        "        total += row;\n"
        "    if (ready) {\n"
        "        label->setStyleSheet(rule);\n"
        "    }\n"
        "}\n"
        "\n"
        "void Rows::nested()\n"
        "{\n"
        "    for (int i = 0; i < 4; ++i)\n"
        "        for (int j = 0; j < 4; ++j)\n"
        "            while (more(i, j))\n"
        "                step();\n"
        "}\n"
        "\n"
        "void Rows::onceOnly()\n"
        "{\n"
        "    for (const QString &name : dir.entryList())\n"
        "        use(name);\n"
        "    for (int i = 0; i < 3; ++i) {\n"
        "        static const QRegularExpression once(pattern);\n"
        "        use(once);\n"
        "    }\n"
        "}\n"
        "\n"
        "void Rows::everyPass()\n"
        "{\n"
        "    while (more(dir.entryList()))\n"
        "        step();\n"
        "    for (int i = 0; i < 3; ++i) {\n"
        "        QRegularExpression each(pattern);\n"
        "        use(each);\n"
        "    }\n"
        "}\n"
        "\n"
        "#ifdef FORKMESH_WINDOW_TESTS\n"
        "void Rows::testPump()\n"
        "{\n"
        "    while (!done)\n"
        "        QCoreApplication::processEvents();\n"
        "}\n"
        "#else\n"
        "void Rows::appPump()\n"
        "{\n"
        "    while (!done)\n"
        "        QCoreApplication::processEvents();\n"
        "}\n"
        "#endif\n"
        "\n"
        "// Joins the worker.\n"
        "// code-health: allow(blocking-wait) bounded 2 s wait on shutdown\n"
        "void Rows::stop()\n"
        "{\n"
        "    process.waitForFinished(2000);\n"
        "}\n"
        "\n"
        "void Rows::unexplained()\n"
        "{\n"
        "    // code-health: allow(blocking-wait)\n"
        "    process.waitForFinished(2000);\n"
        "}\n");
    check(writeFile(QDir(preciseSrc).filePath(QStringLiteral("Rows.cpp")), spans),
          "write the precision fixture");
    const Report exact = analyse(precise.path(), {QStringLiteral("src")}, QString());
    const FileHealth *rows = exact.files.isEmpty() ? nullptr : &exact.files.first();
    check(rows != nullptr, "the precision fixture is analysed");
    if (rows) {
        const Function *siblings = findFunction(*rows, "Rows::siblings");
        check(siblings && siblings->loopDepth == 2,
              "sibling loops inside one loop are depth 2, not 3");
        check(siblings && siblings->findings.isEmpty(),
              "a call after the loop is not flagged as inside it");
        const Function *braceless = findFunction(*rows, "Rows::braceless");
        check(braceless && braceless->loopDepth == 1 &&
                  braceless->findings.isEmpty(),
              "a braceless loop ends at its statement's semicolon");
        const Function *nested = findFunction(*rows, "Rows::nested");
        check(nested && nested->loopDepth == 3 &&
                  hasFinding(*nested, "deep-nested-loop"),
              "braceless loops still nest");
        const Function *onceOnly = findFunction(*rows, "Rows::onceOnly");
        check(onceOnly && onceOnly->findings.isEmpty(),
              "a range-for header and a static regex each run once");
        const Function *everyPass = findFunction(*rows, "Rows::everyPass");
        check(everyPass && hasFinding(*everyPass, "dir-walk-in-loop") &&
                  hasFinding(*everyPass, "regex-in-loop"),
              "a while condition and a local regex run every pass");
        check(findFunction(*rows, "Rows::testPump") == nullptr,
              "window-test-only code is not scanned");
        const Function *appPump = findFunction(*rows, "Rows::appPump");
        check(appPump && hasFinding(*appPump, "pump-in-loop"),
              "the #else branch of a window-test block is the app's and is scanned");
        const Function *stop = findFunction(*rows, "Rows::stop");
        check(stop && stop->findings.isEmpty(),
              "an acknowledgement with a reason clears its kind");
        const Function *unexplained = findFunction(*rows, "Rows::unexplained");
        check(unexplained && hasFinding(*unexplained, "blocking-wait"),
              "an acknowledgement without a reason clears nothing");
        check(rows->acknowledgedCount == 1 && exact.acknowledgedCount == 1,
              "acknowledged findings are counted, not hidden");
    }

    // --- progress and cancellation ----------------------------------------
    int seen = 0;
    Report counted = analyse(root, {QStringLiteral("src")}, QString(),
                             [&](int, int total, const QString &) {
                                 if (total > 0)
                                     ++seen;
                                 return true;
                             });
    check(seen >= counted.files.size(),
          "progress is reported at least once per file");

    int calls = 0;
    Report stopped = analyse(root, {QStringLiteral("src")}, QString(),
                             [&](int, int, const QString &) {
                                 ++calls;
                                 return false; // cancel immediately
                             });
    check(stopped.cancelled, "returning false cancels the scan");
    check(stopped.files.isEmpty(), "a cancelled scan yields no files");
    check(calls == 1, "cancellation stops before the second file");

    // --- stall attribution -------------------------------------------------
    // Only frames and "likely origin:" name code: prose once blamed main() for
    // a git branch called "main (forkmesh)". A record the watchdog judged
    // machine-wide starvation is blamed on nothing.
    QTemporaryDir diagnostics;
    check(diagnostics.isValid(), "diagnostics directory");
    check(writeFile(QDir(src).filePath(QStringLiteral("Shell.cpp")),
                    QStringLiteral(
                        "bool MainWindow::eventFilter(QObject *obj, QEvent *event)\n"
                        "{\n"
                        "    return handleFramelessResizeEvent(obj, event);\n"
                        "}\n"
                        "\n"
                        "bool MainWindow::handleFramelessResizeEvent(QObject *, QEvent *)\n"
                        "{\n"
                        "    return false;\n"
                        "}\n")),
          "write the event filter fixture");
    check(writeFile(
              QDir(diagnostics.path()).filePath(QStringLiteral("stalls.log")),
              QStringLiteral(
                  "===== STALL-1 BEGIN =====\n"
                  "2026-09-16T16:09:01.351  UI stalled ~1500 ms\n"
                  "blocking call: git rev-parse Widget::quiet (forkmesh)\n"
                  "./build/forkmesh(_ZN6Widget9paintRowsEv+0x10) [0x1]\n"
                  "===== STALL-1 END =====\n"
                  "===== STALL-2 BEGIN =====\n"
                  "2026-09-16T16:09:05.000  UI stalled ~2000 ms\n"
                  "verdict: machine-wide memory/io starvation. The frames\n"
                  "likely origin: Widget::quiet()\n"
                  "===== STALL-2 END =====\n"
                  "===== STALL-3 BEGIN =====\n"
                  "2026-10-06T20:15:29.505  UI stalled ~900 ms\n"
                  "likely origin: MainWindow::handleFramelessResizeEvent(QObject*, QEvent*) -> MainWindow::eventFilter(QObject*, QEvent*)\n"
                  "./build/forkmesh(_ZN10MainWindow26handleFramelessResizeEventEP7QObjectP6QEvent+0x29) [0x1]\n"
                  "./build/forkmesh(_ZN10MainWindow11eventFilterEP7QObjectP6QEvent+0x36e) [0x2]\n"
                  "./build/forkmesh(_ZN6Widget9paintRowsEv+0x10) [0x3]\n"
                  "===== STALL-3 END =====\n"
                  "===== STALL-4 BEGIN =====\n"
                  "2026-10-06T20:16:00.000  UI stalled ~700 ms\n"
                  "./build/forkmesh(_ZN10MainWindow26handleFramelessResizeEventEP7QObjectP6QEvent+0x29) [0x1]\n"
                  "./build/forkmesh(_ZN10MainWindow11eventFilterEP7QObjectP6QEvent+0x36e) [0x2]\n"
                  "===== STALL-4 END =====\n")),
          "write the stall fixture");
    Report stalls = analyse(root, {QStringLiteral("src")}, diagnostics.path());
    check(stalls.stallRecords == 4, "every stall record is read");
    check(stalls.stallStarvedMs == 2000,
          "a starvation verdict is counted apart");
    check(stalls.stallUnattributedMs == 700,
          "a record naming only event filter residue blames no function");
    for (const FileHealth &file : stalls.files) {
        if (file.display == QLatin1String("Shell.cpp")) {
            check(file.stallMs == 0,
                  "app-wide event filter frames are residue, not owners");
            continue;
        }
        if (file.display != QLatin1String("Widget.cpp"))
            continue;
        const Function *paint = findFunction(file, "Widget::paintRows");
        const Function *quiet = findFunction(file, "Widget::quiet");
        check(paint && paint->stallMs == 2400,
              "a mangled stack frame owns the stall, alone beside filter residue");
        check(quiet && quiet->stallMs == 0,
              "prose and starved records blame no function");
    }

    // --- component grouping ------------------------------------------------
    check(componentFor(QStringLiteral("desktop/src/MainWindowIssues.cpp")) ==
              QStringLiteral("UI · Issues"),
          "MainWindow pages group as UI pages with spaced names");
    check(componentFor(QStringLiteral("desktop/src/MainWindowRepoDetail.cpp")) ==
              QStringLiteral("UI · Repo Detail"),
          "camel case in a page name becomes words");
    check(componentFor(QStringLiteral("desktop/src/PullStore.cpp")) ==
              QLatin1String("Pulls"),
          "stores group by their subject");
    check(componentFor(QStringLiteral("desktop/src/Whatever.cpp")) ==
              QLatin1String("Core"),
          "anything unrecognised falls back to Core");

    if (failures == 0)
        std::fprintf(stdout, "test_code_health: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
