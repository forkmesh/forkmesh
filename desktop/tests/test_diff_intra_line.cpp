#include "DiffIntraLine.h"

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

} // namespace

int main()
{
    using namespace forkmesh::diffintra;

    {
        const auto cells = intraLineCellsHtml(
            QStringLiteral("  <input name=\"createPr\" type=\"checkbox\" checked "
                           "class=\"h-4\" />"),
            QStringLiteral("  <input name=\"createPr\" type=\"checkbox\" "
                           "class=\"h-4\" />"),
            4096);
        expectTrue(cells.has_value(), "removed attribute is emphasized");
        if (cells) {
            expectEq(cells->oldHtml,
                     QStringLiteral("  &lt;input name=&quot;createPr&quot; "
                                    "type=&quot;checkbox&quot; <span "
                                    "class='delx'>checked </span>class=&quot;h-4&quot; /&gt;"),
                     "old side marks only the removed attribute");
            expectEq(cells->newHtml,
                     QStringLiteral("  &lt;input name=&quot;createPr&quot; "
                                    "type=&quot;checkbox&quot; class=&quot;h-4&quot; /&gt;"),
                     "new side has nothing to mark");
        }
    }

    {
        const auto cells = intraLineCellsHtml(QStringLiteral("return a + b;"),
                                              QStringLiteral("return a - b;"), 4096);
        expectTrue(cells.has_value(), "operator swap is emphasized");
        if (cells) {
            expectEq(cells->oldHtml,
                     QStringLiteral("return a <span class='delx'>+</span> b;"),
                     "old operator marked");
            expectEq(cells->newHtml,
                     QStringLiteral("return a <span class='addx'>-</span> b;"),
                     "new operator marked");
        }
    }

    {
        const auto cells = intraLineCellsHtml(QStringLiteral("int count = 1;"),
                                              QStringLiteral("int total = 2;"), 4096);
        expectTrue(cells.has_value(), "two separate changes");
        if (cells)
            expectEq(cells->newHtml,
                     QStringLiteral("int <span class='addx'>total</span> = "
                                    "<span class='addx'>2</span>;"),
                     "each changed word gets its own span");
    }

    {
        const auto changes = intraLineChanges(QStringLiteral("foo(a, b)"),
                                              QStringLiteral("foo(a, b, c)"));
        expectTrue(changes.has_value(), "insertion found");
        if (changes) {
            expectTrue(changes->oldRanges.isEmpty(), "nothing removed");
            expectTrue(changes->newRanges.size() == 1 &&
                           changes->newRanges.first() == Range{8, 11},
                       "inserted argument covers \", c\"");
        }
    }

    expectTrue(!intraLineCellsHtml(QStringLiteral("same"), QStringLiteral("same"), 4096),
               "identical lines are not emphasized");
    expectTrue(!intraLineCellsHtml(QString(), QStringLiteral("x"), 4096),
               "an empty side is not emphasized");
    expectTrue(!intraLineCellsHtml(QStringLiteral("alpha beta gamma"),
                                   QStringLiteral("one two three"), 4096),
               "a rewritten line is left whole");
    expectTrue(!intraLineCellsHtml(QString(300, QLatin1Char('a')) + QStringLiteral(" x"),
                                   QString(300, QLatin1Char('a')) + QStringLiteral(" y"),
                                   256),
               "lines over the HTML budget are left plain");

    QString longOld, longNew;
    for (int i = 0; i < 400; ++i) {
        longOld += QStringLiteral("w%1 ").arg(i);
        longNew += QStringLiteral("v%1 ").arg(i);
    }
    expectTrue(!intraLineChanges(longOld, longNew), "token grid cap holds");

    const auto cells = intraLineCellsHtml(QStringLiteral("a < b && c"),
                                          QStringLiteral("a <= b && c"), 4096);
    expectTrue(cells.has_value(), "escaping case emphasized");
    if (cells)
        expectEq(cells->newHtml,
                 QStringLiteral("a &lt;<span class='addx'>=</span> b &amp;&amp; c"),
                 "emphasized text stays escaped");

    if (failures == 0)
        std::printf("diff intra-line tests passed\n");
    return failures == 0 ? 0 : 1;
}
