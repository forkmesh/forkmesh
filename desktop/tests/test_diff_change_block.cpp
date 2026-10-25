#include "DiffChangeBlock.h"

#include <cstdio>

namespace {

int failures = 0;

void expectEq(const QString &actual, const QString &expected, const char *what)
{
    if (actual == expected)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s\n  expected: %s\n  actual:   %s\n", what,
                 qPrintable(expected), qPrintable(actual));
}

void expectTrue(bool ok, const char *what)
{
    if (ok)
        return;
    ++failures;
    std::fprintf(stderr, "FAIL %s\n", what);
}

const QString kFilePatch = QStringLiteral(
    "diff --git a/src/a.txt b/src/a.txt\n"
    "index 1111111..2222222 100644\n"
    "--- a/src/a.txt\n"
    "+++ b/src/a.txt\n"
    "@@ -1,12 +1,13 @@\n"
    " one\n"
    "-two\n"
    "+TWO\n"
    " three\n"
    " four\n"
    " five\n"
    " six\n"
    " seven\n"
    "-eight\n"
    "+EIGHT\n"
    "+EIGHT-AND-A-HALF\n"
    " nine\n"
    " ten\n"
    " eleven\n"
    " twelve\n");

} // namespace

int main()
{
    using namespace forkmesh::diffblock;

    {
        const QString patch =
            kFilePatch +
            QStringLiteral("diff --git a/b.txt b/b.txt\n"
                           "--- a/b.txt\n"
                           "+++ b/b.txt\n"
                           "@@ -1 +1 @@\n"
                           "-x\n"
                           "+y\n");
        const QStringList sections = splitFilePatches(QStringLiteral("junk\n") + patch);
        expectTrue(sections.size() == 2, "two file sections");
        if (sections.size() == 2) {
            expectEq(sections.at(0), kFilePatch, "first section is verbatim");
            expectEq(filePatchPath(sections.at(0)), QStringLiteral("src/a.txt"),
                     "first section path");
            expectEq(filePatchPath(sections.at(1)), QStringLiteral("b.txt"),
                     "second section path");
        }
    }

    expectEq(changeBlockPatch(kFilePatch, 2, 2),
             QStringLiteral("diff --git a/src/a.txt b/src/a.txt\n"
                            "--- a/src/a.txt\n"
                            "+++ b/src/a.txt\n"
                            "@@ -1,5 +1,5 @@\n"
                            " one\n"
                            "-two\n"
                            "+TWO\n"
                            " three\n"
                            " four\n"
                            " five\n"),
             "first block keeps its own context");

    expectEq(changeBlockPatch(kFilePatch, 8, 8),
             QStringLiteral("diff --git a/src/a.txt b/src/a.txt\n"
                            "--- a/src/a.txt\n"
                            "+++ b/src/a.txt\n"
                            "@@ -5,7 +5,8 @@\n"
                            " five\n"
                            " six\n"
                            " seven\n"
                            "-eight\n"
                            "+EIGHT\n"
                            "+EIGHT-AND-A-HALF\n"
                            " nine\n"
                            " ten\n"
                            " eleven\n"),
             "second block leaves the first one out");

    expectTrue(changeBlockPatch(kFilePatch, 3, 3).isEmpty(),
               "a start that is not a block finds nothing");

    expectEq(changeBlockPatch(QStringLiteral("diff --git a/c b/c\n"
                                             "--- a/c\n"
                                             "+++ b/c\n"
                                             "@@ -1,2 +1,2 @@\n"
                                             " keep\n"
                                             "-old\n"
                                             "\\ No newline at end of file\n"
                                             "+new\n"
                                             "\\ No newline at end of file\n"),
                              2, 2),
             QStringLiteral("diff --git a/c b/c\n"
                            "--- a/c\n"
                            "+++ b/c\n"
                            "@@ -1,2 +1,2 @@\n"
                            " keep\n"
                            "-old\n"
                            "\\ No newline at end of file\n"
                            "+new\n"
                            "\\ No newline at end of file\n"),
             "a missing final newline stays inside the block");

    expectTrue(changeBlockPatch(QStringLiteral("diff --git a/i.png b/i.png\n"
                                               "Binary files a/i.png and b/i.png differ\n"),
                                1, 1)
                   .isEmpty(),
               "binary files have no blocks");

    if (failures == 0)
        std::printf("diff change block tests passed\n");
    return failures == 0 ? 0 : 1;
}
