#include "BinarySize.h"
#include "SourceDir.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh::binarysize;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

void testFamilies()
{
    check(familyOf(QStringLiteral("QList<QString>::append(QString const&)")) ==
              QLatin1String("QList"),
          "a template member groups under its class");
    check(familyOf(QStringLiteral("void std::__introsort_loop<int*, long>(int*, int*, long)")) ==
              QLatin1String("std::__introsort_loop"),
          "the return type is skipped and std keeps two levels");
    check(familyOf(QStringLiteral("vtable for QHash<QString, int>")) == QLatin1String("QHash"),
          "vtable prefixes are stripped");
    check(familyOf(QStringLiteral("forkmesh::codehealth::analyse(QString const&)")) ==
              QLatin1String("forkmesh::codehealth"),
          "forkmesh keeps its sub-namespace");
    check(familyOf(QStringLiteral("(anonymous namespace)::Builder::child(int)")) ==
              QLatin1String("Builder"),
          "anonymous namespaces are skipped");
    check(familyOf(QStringLiteral("main")) == QLatin1String("(free functions)"),
          "a free function has no family of its own");
    check(shortNameOf(QStringLiteral(
              "QHashPrivate::Data<QHashPrivate::Node<QString, int>>::rehash(unsigned long)")) ==
              QLatin1String("Data::rehash"),
          "a short name keeps the class and member without arguments");
    check(shortNameOf(QStringLiteral("vtable for QHash<QString, int>")) ==
              QLatin1String("vtable for QHash"),
          "a short name keeps what kind of table it is");
    check(shortNameOf(QStringLiteral("main")) == QLatin1String("main"),
          "a plain name is its own short name");
}

void testSourcePaths()
{
    check(sourcePathOf(QStringLiteral("/b/CMakeFiles/forkmesh.dir/src/Foo.cpp.o"), QString()) ==
              QLatin1String("src/Foo.cpp"),
          "object paths map back to their source");
    check(sourcePathOf(
              QStringLiteral("/b/CMakeFiles/forkmesh.dir/home/u/co/marketplace/x/A.cpp.o"),
              QStringLiteral("/home/u/co")) == QLatin1String("marketplace/x/A.cpp"),
          "sources outside the build folder lose the checkout prefix");
    check(sourcePathOf(QStringLiteral("/b/.qt/rcc/qrc_icons.o"), QString()) ==
              QLatin1String("qrc_icons"),
          "objects outside CMakeFiles keep their file name");
}

void testLinkedObjects()
{
    QTemporaryDir dir;
    check(dir.isValid(), "temporary directory");
    writeFile(dir.filePath(QStringLiteral("CMakeFiles/app.dir/src/a.cpp.o")), "x");
    writeFile(dir.filePath(QStringLiteral("CMakeFiles/app.dir/src/b c.cpp.o")), "x");
    writeFile(dir.filePath(QStringLiteral("CMakeFiles/app.dir/src/stray.cpp.o")), "x");
    writeFile(dir.filePath(QStringLiteral("build.ninja")),
              "build app-tests: LINK CMakeFiles/app.dir/src/stray.cpp.o\n"
              "build app: CXX_EXECUTABLE_LINKER CMakeFiles/app.dir/src/a.cpp.o "
              "CMakeFiles/app.dir/src/b$ c.cpp.o -lfoo | libdep.so || order\n");
    const QStringList objects = linkedObjects(dir.path(), QStringLiteral("app"));
    check(objects.size() == 2, "only the target's link line is read");
    check(objects.value(1).endsWith(QLatin1String("src/b c.cpp.o")),
          "ninja escapes are undone");

    QFile::remove(dir.filePath(QStringLiteral("build.ninja")));
    check(linkedObjects(dir.path(), QStringLiteral("app")).size() == 3,
          "without a link line every object in the target folder counts");
}

void checkSums(const Report &report)
{
    bool balanced = true;
    for (const Node &node : report.nodes) {
        if (node.children.isEmpty())
            continue;
        qint64 sum = 0;
        for (int child : node.children)
            sum += report.nodes.at(child).bytes;
        balanced = balanced && sum == node.bytes;
    }
    check(balanced, "every group is the sum of its children");
}

bool hasNode(const Report &report, const QString &name, int category)
{
    for (const Node &node : report.nodes) {
        if (node.name == name && node.category == category && node.bytes > 0)
            return true;
    }
    return false;
}

void testOwnBinary()
{
#if defined(Q_OS_LINUX)
    Options options;
    options.binaryPath = QCoreApplication::applicationFilePath();
    options.buildDir = QCoreApplication::applicationDirPath();
    options.targetName = QFileInfo(options.binaryPath).completeBaseName();
    options.sourceRoot = forkmesh::sourceDir();
    int calls = 0;
    const Report report = analyse(options, [&](int done, int total) {
        ++calls;
        return done <= total;
    });
    check(report.error.isEmpty(), "the test binary is readable");
    check(report.fileSize == QFileInfo(options.binaryPath).size(), "file size is recorded");
    check(!report.nodes.isEmpty() && report.nodes.at(0).bytes == report.fileSize,
          "the whole file is accounted for");
    check(calls > 0, "progress is reported");
    checkSums(report);
    if (report.objectCount > 0 && report.fromSymtab) {
        check(hasNode(report, QStringLiteral("test_binary_size.cpp"), AppCode),
              "this test's own source shows up as app code");
        check(hasNode(report, QStringLiteral("BinarySize.cpp"), AppCode),
              "the analyser shows up as app code");
    }

    Options capped = options;
    capped.symbolsPerGroup = 1;
    const Report small = analyse(capped);
    check(!small.nodes.isEmpty() && small.nodes.at(0).bytes == report.fileSize,
          "folding small symbols keeps the total");
    check(small.nodes.size() < report.nodes.size(), "folding small symbols drops leaves");

    const Report cancelled = analyse(options, [](int, int) { return false; });
    check(cancelled.error == QLatin1String("cancelled"), "a scan can be cancelled");
#endif
    Options missing;
    missing.binaryPath = QStringLiteral("/nonexistent/binary");
    check(!analyse(missing).error.isEmpty(), "a missing binary is an error, not a crash");
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testFamilies();
    testSourcePaths();
    testLinkedObjects();
    testOwnBinary();
    if (failures == 0)
        std::printf("binary size tests passed\n");
    return failures == 0 ? 0 : 1;
}
