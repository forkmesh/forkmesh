#include "DesignTokens.h"
#include "Theme.h"
#include "UiKit.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFrame>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>

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

    const auto updateBurstMs = [](QTableWidget &grid, const QString &label) {
        QElapsedTimer burst;
        burst.start();
        for (int i = 0; i < 20; ++i) {
            grid.item(i, 1)->setText(QStringLiteral("%1 value %2").arg(label).arg(i));
            grid.horizontalHeader()->sectionPosition(1);
            QApplication::processEvents();
        }
        return burst.elapsed();
    };
    const auto fillGrid = [](QTableWidget &grid) {
        grid.setProperty("dataTable", true);
        for (int row = 0; row < grid.rowCount(); ++row) {
            for (int column = 0; column < grid.columnCount(); ++column)
                grid.setItem(row, column,
                             new QTableWidgetItem(
                                 QStringLiteral("row %1 column %2 value").arg(row).arg(column)));
        }
        grid.resize(900, 400);
        grid.show();
        QApplication::processEvents();
        kit::syncStockWidgets();
        QApplication::processEvents();
    };
    QTableWidget grid(3000, 4);
    fillGrid(grid);
    QHeaderView *gridHeader = grid.horizontalHeader();
    check(gridHeader->sectionResizeMode(1) == QHeaderView::ResizeToContents,
          "a data table fits its columns to their contents");
    check(gridHeader->resizeContentsPrecision() == kit::kDataTableFitRows,
          "a data table fits its columns from a bounded number of rows");
    updateBurstMs(grid, QStringLiteral("warm"));
    const qint64 boundedMs = updateBurstMs(grid, QStringLiteral("bounded"));
    gridHeader->setResizeContentsPrecision(1000);
    const qint64 defaultMs = updateBurstMs(grid, QStringLiteral("default"));
    std::printf("20 cell updates on a 3,000-row data table: %lld ms fitting %d rows, "
                "%lld ms at Qt's default of 1000\n",
                static_cast<long long>(boundedMs), kit::kDataTableFitRows,
                static_cast<long long>(defaultMs));
    check(boundedMs < defaultMs,
          "cell updates on a long data table cost less with the bounded fit");
    QTableWidget narrow(10, 2);
    narrow.horizontalHeader()->setResizeContentsPrecision(20);
    fillGrid(narrow);
    check(narrow.horizontalHeader()->resizeContentsPrecision() == 20,
          "a data table keeps a smaller fit bound its page already chose");
    gridHeader->setResizeContentsPrecision(kit::kDataTableFitRows);

    for (const bool dark : {false, true}) {
        Theme::setActiveDesign(Theme::Design::Next, dark);
        qApp->setStyleSheet(Theme::nextStyleSheet(dark));
        QWidget host;
        host.resize(400, 300);
        host.show();
        QApplication::processEvents();
        for (const char *name : {"agentMetaPopup", "reactionPicker",
                                 "slashActionsPopup", "mentionPopup",
                                 "globalSearchPopup"}) {
            auto *popup = new QFrame(&host, Qt::Popup);
            popup->setObjectName(QString::fromLatin1(name));
            auto *layout = new QVBoxLayout(popup);
            layout->setContentsMargins(12, 10, 12, 10);
            layout->addWidget(new QLabel(QStringLiteral("Agent  Claude Code")));
            popup->adjustSize();
            popup->resize(240, 120);
            popup->show();
            QApplication::processEvents();
            const QImage image = popup->grab().toImage();
            const QColor inside = image.pixelColor(image.width() / 2,
                                                   image.height() - 4);
            const QByteArray what =
                QByteArray("a Next ") + (dark ? "dark " : "light ") + name +
                " paints a solid background inside its rounded corners";
            check(popup->testAttribute(Qt::WA_TranslucentBackground) &&
                      inside.alpha() == 255 &&
                      image.pixelColor(0, 0).alpha() < 255,
                  what.constData());
            popup->hide();
            delete popup;
        }
    }
    qApp->setStyleSheet(QString());

    Theme::setActiveDesign(Theme::Design::Classic, true);
    kit::syncStockWidgets();
    check(gridHeader->resizeContentsPrecision() == 1000 &&
              gridHeader->sectionResizeMode(1) == QHeaderView::Interactive,
          "leaving the Next design restores the column fit and resize mode");

    if (failures == 0)
        std::printf("\nAll UI kit tests passed.\n");
    else
        std::fprintf(stderr, "\n%d UI kit test(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
