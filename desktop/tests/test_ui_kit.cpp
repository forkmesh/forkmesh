#include "DesignTokens.h"
#include "Theme.h"
#include "UiKit.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>

#include <cstdio>

using namespace forkmesh::ui;

namespace {

int failures = 0;

void check(bool ok, const char *what)
{
    if (ok) {
        std::printf("PASS: %s\n", what);
    } else {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    check(kit::cellDisplayText(QStringLiteral("short")) == QStringLiteral("short"),
          "a short cell keeps its text");
    const QString huge(2'000'000, QLatin1Char('x'));
    const QString capped = kit::cellDisplayText(huge);
    check(capped.size() == kit::kCellDisplayChars + 1 &&
              capped.endsWith(QChar(0x2026)),
          "a huge cell is cut to the display limit and ends with an ellipsis");
    QString emoji = QString(kit::kCellDisplayChars - 1, QLatin1Char('a')) +
                    QString::fromUtf8("\xF0\x9F\x98\x80") + QStringLiteral("tail");
    const QString emojiCut = kit::cellDisplayText(emoji);
    check(!emojiCut.at(emojiCut.size() - 2).isHighSurrogate(),
          "the cut never splits a surrogate pair");

    Theme::setActiveDesign(Theme::Design::Next, true);
    kit::installStockPolish();

    QTableWidget table(2, 1);
    table.setProperty("dataTable", true);
    table.setItem(0, 0, new QTableWidgetItem(huge));
    table.setItem(1, 0, new QTableWidgetItem(capped));
    table.resize(900, 300);
    table.show();
    QApplication::processEvents();
    kit::syncStockWidgets();
    QApplication::processEvents();

    QAbstractItemDelegate *delegate = table.itemDelegate();
    QStyleOptionViewItem option;
    option.initFrom(table.viewport());
    option.font = table.font();
    option.widget = &table;
    const QModelIndex hugeIndex = table.model()->index(0, 0);
    QStyledItemDelegate plain;
    QElapsedTimer timer;
    timer.start();
    const QSize plainHint = plain.sizeHint(option, hugeIndex);
    const qint64 plainMs = timer.restart();
    const QSize hugeHint = delegate->sizeHint(option, hugeIndex);
    const qint64 cappedMs = timer.restart();
    const QSize cappedHint = delegate->sizeHint(option, table.model()->index(1, 0));
    table.resizeColumnToContents(0);
    table.viewport()->grab();
    const qint64 paintMs = timer.elapsed();
    std::printf("2,000,000-character cell: uncapped size hint %lld ms (width %d), "
                "capped %lld ms (width %d), column fit and paint %lld ms\n",
                static_cast<long long>(plainMs), plainHint.width(),
                static_cast<long long>(cappedMs), hugeHint.width(),
                static_cast<long long>(paintMs));
    check(delegate && plainHint != hugeHint,
          "a data table measures cells through its own row delegate");
    check(hugeHint == cappedHint,
          "a 2,000,000-character cell is measured as its capped display text");

    if (failures == 0)
        std::printf("\nAll UI kit tests passed.\n");
    else
        std::fprintf(stderr, "\n%d UI kit test(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
