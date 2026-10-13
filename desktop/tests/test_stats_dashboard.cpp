// StatsDashboard: grid packing, layout edits, persistence and painting.
// Runs offscreen (QT_QPA_PLATFORM=offscreen) like the onboarding suite.
#include "../src/StatsDashboard.h"
#include "../src/Theme.h"

#include <QApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QProcessEnvironment>

#include <cmath>
#include <cstdio>

namespace {

int failures = 0;

void check(bool condition, const QString &what)
{
    if (condition) {
        std::printf("ok   - %s\n", qPrintable(what));
        return;
    }
    ++failures;
    std::printf("FAIL - %s\n", qPrintable(what));
}

QVector<StatsTileCatalogEntry> catalog()
{
    return {
        {QStringLiteral("repos"), QStringLiteral("Repositories"),
         QStringLiteral("Workspace"), QStringLiteral("Repos on this node")},
        {QStringLiteral("issues"), QStringLiteral("Open issues"),
         QStringLiteral("Work"), QStringLiteral("Open issue count")},
        {QStringLiteral("tokens"), QStringLiteral("Tokens"),
         QStringLiteral("Agents"), QStringLiteral("Tokens spent")},
        {QStringLiteral("cost"), QStringLiteral("Spend"),
         QStringLiteral("Agents"), QStringLiteral("USD spent")},
        {QStringLiteral("peers"), QStringLiteral("Peers"),
         QStringLiteral("Network"), QStringLiteral("Connected peers")},
    };
}

StatsTileData provider(const QString &id)
{
    StatsTileData data;
    if (id == QLatin1String("repos")) {
        data.value = QStringLiteral("12");
        data.note = QStringLiteral("3 mirrored");
        data.series.labels = {QStringLiteral("Mon"), QStringLiteral("Tue"),
                              QStringLiteral("Wed"), QStringLiteral("Thu")};
        data.series.values = {1, 4, 2, 12};
        data.status = StatsTileStatus::Good;
        data.trend = StatsTrendPolarity::UpIsGood;
    } else if (id == QLatin1String("issues")) {
        data.value = QStringLiteral("7");
        data.series.categorical = true;
        data.series.labels = {QStringLiteral("P0"), QStringLiteral("P1"),
                              QStringLiteral("P2")};
        data.series.values = {2, 3, 2};
        data.series.tones = {StatsTileStatus::Critical, StatsTileStatus::Warning,
                             StatsTileStatus::None};
    } else if (id == QLatin1String("peers")) {
        data.available = false;
    } else if (id == QLatin1String("cost")) {
        data.value = QStringLiteral("$41.20");
        data.status = StatsTileStatus::Warning;
        data.trend = StatsTrendPolarity::DownIsGood;
        data.series.values = {10, 12, 18, 22, 30, 41};
    } else {
        data.value = QStringLiteral("1.2K");
    }
    return data;
}

bool overlaps(const QVector<QRect> &cells)
{
    for (int i = 0; i < cells.size(); ++i)
        for (int j = i + 1; j < cells.size(); ++j)
            if (cells.at(i).intersects(cells.at(j)))
                return true;
    return false;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // Packing: first-fit on a 3-wide grid.
    {
        QVector<StatsTilePlacement> layout{
            {QStringLiteral("a"), 2, 2}, {QStringLiteral("b"), 1, 1},
            {QStringLiteral("c"), 1, 1}, {QStringLiteral("d"), 2, 1},
            {QStringLiteral("e"), 1, 2}};
        const QVector<QRect> cells = StatsDashboard::packLayout(layout, 3);
        check(cells.size() == 5, QStringLiteral("packLayout returns one cell per tile"));
        check(cells.at(0) == QRect(0, 0, 2, 2), QStringLiteral("2x2 tile takes the top-left"));
        check(cells.at(1) == QRect(2, 0, 1, 1), QStringLiteral("1x1 fills beside it"));
        check(cells.at(2) == QRect(2, 1, 1, 1), QStringLiteral("next 1x1 fills the hole below"));
        check(cells.at(3) == QRect(0, 2, 2, 1), QStringLiteral("2x1 starts the next row"));
        check(cells.at(4) == QRect(2, 2, 1, 2), QStringLiteral("1x2 fills the remaining column"));
        check(!overlaps(cells), QStringLiteral("packed cells never overlap"));

        // A single column clamps wide tiles instead of dropping them.
        const QVector<QRect> narrow = StatsDashboard::packLayout(layout, 1);
        check(narrow.at(0) == QRect(0, 0, 1, 2) && narrow.at(3) == QRect(0, 4, 1, 1),
              QStringLiteral("one column clamps width and stacks tiles"));
        check(!overlaps(narrow), QStringLiteral("clamped cells never overlap"));
    }

    // JSON round trip, with out-of-range spans clamped and empty ids dropped.
    {
        QVector<StatsTilePlacement> layout{{QStringLiteral("repos"), 2, 1},
                                           {QStringLiteral("issues"), 1, 2}};
        const QJsonArray json = StatsDashboard::layoutToJson(layout);
        const QVector<StatsTilePlacement> back = StatsDashboard::layoutFromJson(json);
        check(back.size() == 2 && back.at(0).id == QLatin1String("repos") &&
                  back.at(0).width == 2 && back.at(1).height == 2,
              QStringLiteral("layout survives a JSON round trip"));
        const QJsonArray odd = QJsonDocument::fromJson(
            "[{\"id\":\"x\",\"w\":9,\"h\":0},{\"w\":1,\"h\":1}]").array();
        const QVector<StatsTilePlacement> clamped = StatsDashboard::layoutFromJson(odd);
        check(clamped.size() == 1 && clamped.at(0).width == 2 && clamped.at(0).height == 1,
              QStringLiteral("spans clamp to 1..2 and idless entries drop"));
    }

    // Number formatting.
    check(StatsDashboard::compactNumber(1284) == QLatin1String("1,284"),
          QStringLiteral("compactNumber keeps thousands separators under 10K"));
    check(StatsDashboard::compactNumber(12900) == QLatin1String("12.9K"),
          QStringLiteral("compactNumber compacts to K"));
    check(StatsDashboard::compactNumber(4'200'000) == QLatin1String("4.20M"),
          QStringLiteral("compactNumber compacts to M"));
    check(StatsDashboard::compactBytes(1536) == QLatin1String("1.5 KB"),
          QStringLiteral("compactBytes"));
    check(StatsDashboard::compactUsd(12.5) == QLatin1String("$12.50"),
          QStringLiteral("compactUsd keeps cents under $100"));
    check(StatsDashboard::compactDuration(3 * 3600 * 1000 + 5 * 60 * 1000) ==
              QLatin1String("3h 5m"),
          QStringLiteral("compactDuration"));

    // Trend: newer half of a time series against its older half.
    {
        StatsTileSeries rising;
        rising.values = {1, 4, 2, 12};
        double percent = 0;
        check(StatsDashboard::trendPercent(rising, &percent) && std::abs(percent - 180.0) < 0.01,
              QStringLiteral("trendPercent compares the newer half with the older half"));
        StatsTileSeries falling;
        falling.values = {10, 10, 7, 5, 5};
        check(StatsDashboard::trendPercent(falling, &percent) && std::abs(percent + 50.0) < 0.01,
              QStringLiteral("trendPercent skips the middle point of an odd series"));
        StatsTileSeries fromZero;
        fromZero.values = {0, 0, 3, 3};
        StatsTileSeries shortSeries;
        shortSeries.values = {1, 2, 3};
        StatsTileSeries bars = rising;
        bars.categorical = true;
        check(!StatsDashboard::trendPercent(fromZero, &percent) &&
                  !StatsDashboard::trendPercent(shortSeries, &percent) &&
                  !StatsDashboard::trendPercent(bars, &percent),
              QStringLiteral("no trend without a baseline, enough points, or a time axis"));
    }

    // Widget: add, remove, resize, move, persistence callback, painting.
    StatsDashboard board;
    board.setCatalog(catalog());
    board.setDataProvider(provider);
    board.setDefaultLayout({{QStringLiteral("repos"), 2, 1},
                            {QStringLiteral("issues"), 1, 1}});
    int changes = 0;
    board.layoutChanged = [&changes] { ++changes; };
    board.resize(3 * StatsDashboard::kMinSquareWidth + 2 * StatsDashboard::kGap + 10,
                 600);
    board.show();
    QApplication::processEvents();
    check(board.columns() == 3, QStringLiteral("three squares fit the test width"));
    check(board.tileIds().isEmpty(), QStringLiteral("board starts empty"));

    board.resetLayout();
    check(board.tileIds() == QStringList{QStringLiteral("repos"), QStringLiteral("issues")},
          QStringLiteral("reset installs the default layout"));
    check(changes == 1, QStringLiteral("reset reports a layout change"));
    check(board.tileValue(QStringLiteral("repos")) == QLatin1String("12"),
          QStringLiteral("tiles read their value from the provider"));

    check(board.addTile(QStringLiteral("tokens"), 1, 1), QStringLiteral("addTile adds a catalog stat"));
    check(!board.addTile(QStringLiteral("tokens")), QStringLiteral("a stat is placed once"));
    check(!board.addTile(QStringLiteral("nope")), QStringLiteral("unknown ids are refused"));
    check(!board.addTile(QStringLiteral("cost"), 3, 1), QStringLiteral("spans over two squares are refused"));
    check(board.addTile(QStringLiteral("peers"), 2, 2), QStringLiteral("four-square tiles add"));
    check(changes == 3, QStringLiteral("each add reports a layout change"));

    const QRect repos = board.tileRect(QStringLiteral("repos"));
    const QRect issues = board.tileRect(QStringLiteral("issues"));
    check(repos.width() > issues.width() * 1.9 && repos.height() == issues.height(),
          QStringLiteral("a 2x1 tile is twice as wide as a 1x1 on the same row"));
    check(issues.width() == issues.height() && issues.width() >= StatsDashboard::kMinSquareWidth,
          QStringLiteral("a 1x1 tile is a true square"));
    check(repos.width() == 2 * issues.width() + StatsDashboard::kGap,
          QStringLiteral("a 2x1 tile spans two squares and the gap"));
    check(repos.top() == issues.top() && issues.left() > repos.right(),
          QStringLiteral("tiles pack left to right"));
    const QRect peers = board.tileRect(QStringLiteral("peers"));
    check(peers.height() > issues.height() * 1.9, QStringLiteral("a 2x2 tile is two squares tall"));
    check(board.tileAt(repos.center()) == QLatin1String("repos"),
          QStringLiteral("tileAt maps a point to its tile"));
    check(board.contentHeight() >= peers.bottom(), QStringLiteral("content height covers the last tile"));

    check(board.resizeTile(QStringLiteral("issues"), 2, 2), QStringLiteral("resizeTile grows a tile"));
    check(board.tileRect(QStringLiteral("issues")).height() > issues.height() * 1.9,
          QStringLiteral("resized tile paints at its new size"));
    check(!board.resizeTile(QStringLiteral("issues"), 0, 1), QStringLiteral("resize rejects zero spans"));

    check(board.moveTile(QStringLiteral("peers"), 0), QStringLiteral("moveTile reorders"));
    check(board.tileIds().first() == QLatin1String("peers"),
          QStringLiteral("moved tile leads the order"));
    check(board.tileRect(QStringLiteral("peers")).topLeft() == QPoint(0, 0),
          QStringLiteral("the first tile packs at the origin"));

    // Drag: press on the last tile, drag onto the first, release.
    {
        const int before = changes;
        const QString last = board.tileIds().last();
        const QPoint from = board.tileRect(last).center();
        const QPoint to = board.tileRect(board.tileIds().first()).center();
        QMouseEvent press(QEvent::MouseButtonPress, from, board.mapToGlobal(from),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&board, &press);
        const QPoint mid = from + QPoint(40, 0);
        QMouseEvent move1(QEvent::MouseMove, mid, board.mapToGlobal(mid),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&board, &move1);
        QMouseEvent move2(QEvent::MouseMove, to, board.mapToGlobal(to),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&board, &move2);
        QMouseEvent release(QEvent::MouseButtonRelease, to, board.mapToGlobal(to),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&board, &release);
        check(board.tileIds().first() == last,
              QStringLiteral("dragging a tile onto the first slot moves it there"));
        check(changes == before + 1, QStringLiteral("a drag reports one layout change"));
    }

    // Persist and restore through JSON; unknown ids vanish on restore.
    {
        QVector<StatsTilePlacement> saved = board.layoutModel();
        saved.append({QStringLiteral("gone"), 1, 1});
        StatsDashboard restored;
        restored.setCatalog(catalog());
        restored.setDataProvider(provider);
        restored.setLayoutModel(StatsDashboard::layoutFromJson(
            StatsDashboard::layoutToJson(saved)));
        check(restored.tileIds() == board.tileIds(),
              QStringLiteral("restored layout matches, minus stats that no longer exist"));
    }

    // Remove via API and via the hover control.
    check(board.removeTile(QStringLiteral("tokens")), QStringLiteral("removeTile drops a tile"));
    check(!board.hasTile(QStringLiteral("tokens")), QStringLiteral("removed tile is gone"));
    {
        const QRect x = board.tileRemoveRect(QStringLiteral("issues"));
        check(x.isValid() && board.tileRect(QStringLiteral("issues")).contains(x),
              QStringLiteral("remove control sits inside its tile"));
        QMouseEvent press(QEvent::MouseButtonPress, x.center(), board.mapToGlobal(x.center()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&board, &press);
        check(!board.hasTile(QStringLiteral("issues")),
              QStringLiteral("clicking the X removes the tile"));
    }

    // Paint every tile shape once; the accent must land inside a plot.
    {
        board.addTile(QStringLiteral("issues"), 2, 2);
        board.addTile(QStringLiteral("tokens"), 1, 2);
        QApplication::processEvents();
        // Square cells stack taller than the test window: render the whole board.
        board.resize(board.width(), qMax(board.height(), board.contentHeight()));
        QApplication::processEvents();
        QImage image(board.size(), QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        board.render(&image);
        const QRect plot = board.tileRect(QStringLiteral("issues"));
        bool accent = false;
        for (int y = plot.top(); y < plot.bottom() && !accent; ++y)
            for (int x = plot.left(); x < plot.right(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (c.alpha() > 200 && c.blue() > 180 && c.red() < 120) {
                    accent = true;
                    break;
                }
            }
        check(accent, QStringLiteral("a four-square categorical tile paints accent bars"));
        bool critical = false;
        for (int y = plot.top(); y < plot.bottom() && !critical; ++y)
            for (int x = plot.left(); x < plot.right(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (c.alpha() > 200 && c.red() > 180 && c.green() < 90 && c.blue() < 90) {
                    critical = true;
                    break;
                }
            }
        check(critical, QStringLiteral("a bar toned critical paints in the status red"));
        const QRect good = board.tileRect(QStringLiteral("repos"));
        const QColor edge = image.pixelColor(good.left() + 2, good.center().y());
        check(board.tileStatus(QStringLiteral("repos")) == StatsTileStatus::Good &&
                  edge.green() > 130 && edge.red() < 60 && edge.blue() < 60,
              QStringLiteral("a tile operating well paints a green status edge"));

        QString clickedTile;
        int clickedPoint = -1;
        board.pointClicked = [&](const QString &id, int point) {
            clickedTile = id;
            clickedPoint = point;
        };
        const QPoint bar = board.tilePointPosition(QStringLiteral("issues"), 1);
        check(!bar.isNull(), QStringLiteral("a laid-out bar reports where it is drawn"));
        // The point is the bar's top edge; click just inside the bar.
        const QPoint inside = bar + QPoint(0, 3);
        QMouseEvent press(QEvent::MouseButtonPress, inside, board.mapToGlobal(inside),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&board, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, inside, board.mapToGlobal(inside),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&board, &release);
        check(clickedTile == QStringLiteral("issues") && clickedPoint == 1,
              QStringLiteral("clicking a bar reports its tile and point"));
        board.pointClicked = nullptr;
        check(board.tileValue(QStringLiteral("peers")).isEmpty(),
              QStringLiteral("an unavailable stat has no headline value"));
    }

    // A headline that changes between refreshes lights the update dot.
    {
        int reading = 1;
        StatsDashboard live;
        live.setCatalog(catalog());
        live.setDataProvider([&reading](const QString &id) {
            StatsTileData data;
            data.value = id == QLatin1String("repos") ? QString::number(reading)
                                                      : QStringLiteral("steady");
            return data;
        });
        live.setLayoutModel({{QStringLiteral("repos"), 1, 1},
                             {QStringLiteral("tokens"), 1, 1}});
        check(!live.tileRecentlyUpdated(QStringLiteral("repos")),
              QStringLiteral("a tile's first reading is not an update"));
        live.refresh();
        check(!live.tileRecentlyUpdated(QStringLiteral("repos")),
              QStringLiteral("an unchanged headline is not an update"));
        reading = 2;
        live.refresh();
        check(live.tileRecentlyUpdated(QStringLiteral("repos")) &&
                  !live.tileRecentlyUpdated(QStringLiteral("tokens")),
              QStringLiteral("only the tile whose headline changed shows the update dot"));
    }

    // Add menu lists only the stats not on the board.
    {
        QMenu *menu = board.buildAddMenu(&board);
        int enabled = 0;
        int total = 0;
        const auto count = [&](QMenu *m, auto &self) -> void {
            for (QAction *action : m->actions()) {
                if (action->menu()) {
                    self(action->menu(), self);
                } else if (action->isCheckable()) {
                    ++total;
                    if (action->isEnabled())
                        ++enabled;
                }
            }
        };
        count(menu, count);
        check(total == 5, QStringLiteral("add menu lists the whole catalog"));
        check(enabled == 5 - board.tileIds().size(),
              QStringLiteral("add menu enables only stats not yet placed"));
        delete menu;
    }

    // FORKMESH_STATS_PNG=<path> renders a themed board for eyeballing (light
    // and dark, side by side) without launching the app.
    const QString pngPath =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("FORKMESH_STATS_PNG"));
    if (!pngPath.isEmpty()) {
        const int width = 5 * StatsDashboard::kMinSquareWidth + 4 * StatsDashboard::kGap + 40;
        QImage sheet(width, 0, QImage::Format_ARGB32);
        QVector<QImage> renders;
        for (bool dark : {false, true}) {
            QApplication::setPalette(Theme::applicationPalette(dark));
            StatsDashboard demo;
            demo.setPalette(Theme::applicationPalette(dark));
            demo.setCatalog(catalog());
            demo.setDataProvider(provider);
            demo.setLayoutModel({{QStringLiteral("repos"), 2, 1},
                                 {QStringLiteral("issues"), 1, 1},
                                 {QStringLiteral("tokens"), 1, 1},
                                 {QStringLiteral("cost"), 1, 2},
                                 {QStringLiteral("peers"), 1, 1},
                                 {QStringLiteral("repos"), 1, 1}});
            demo.addTile(QStringLiteral("issues"));
            demo.resize(width - 40, 10);
            demo.resize(width - 40, demo.contentHeight());
            demo.show();
            QApplication::processEvents();
            QImage image(demo.width() + 40, demo.contentHeight() + 40, QImage::Format_ARGB32);
            image.fill(demo.palette().color(QPalette::Window));
            QPainter painter(&image);
            demo.render(&painter, QPoint(20, 20));
            painter.end();
            renders.append(image);
        }
        QImage out(renders.at(0).width(), renders.at(0).height() + renders.at(1).height(),
                   QImage::Format_ARGB32);
        QPainter painter(&out);
        painter.drawImage(0, 0, renders.at(0));
        painter.drawImage(0, renders.at(0).height(), renders.at(1));
        painter.end();
        check(out.save(pngPath), QStringLiteral("board render saved to ") + pngPath);
    }

    {
        // The Stats page lost every new tile after one restart: the upgrade
        // added them in memory, stamped a version, and never saved the board.
        const QVector<StatsTileCatalogEntry> cat = catalog();
        const QVector<StatsTilePlacement> defaults{
            {QStringLiteral("repos"), 2, 1}, {QStringLiteral("issues"), 1, 1},
            {QStringLiteral("tokens"), 1, 1}, {QStringLiteral("cost"), 1, 1},
            {QStringLiteral("peers"), 1, 1}};
        auto ids = [](const QVector<StatsTilePlacement> &layout) {
            QStringList out;
            for (const StatsTilePlacement &placement : layout)
                out.append(placement.id);
            return out;
        };

        const StatsDashboard::LayoutMerge first = StatsDashboard::mergeSavedLayout(
            {}, false, defaults, cat, {});
        check(ids(first.layout) == ids(defaults) && first.changed &&
                  first.offered.size() == cat.size(),
              QStringLiteral("a first launch starts from the defaults and records them as offered"));

        const QVector<StatsTilePlacement> oldBoard{
            {QStringLiteral("cost"), 2, 1}, {QStringLiteral("repos"), 1, 1}};
        const StatsDashboard::LayoutMerge grown = StatsDashboard::mergeSavedLayout(
            oldBoard, true, defaults, cat, {});
        check(ids(grown.layout) ==
                  QStringList({QStringLiteral("cost"), QStringLiteral("repos"),
                               QStringLiteral("issues"), QStringLiteral("tokens"),
                               QStringLiteral("peers")}) &&
                  grown.layout.at(0).width == 2 && grown.changed,
              QStringLiteral("a board saved before the catalog grew keeps its order and gains every new tile"));

        const StatsDashboard::LayoutMerge again = StatsDashboard::mergeSavedLayout(
            grown.layout, true, defaults, cat, grown.offered);
        check(ids(again.layout) == ids(grown.layout) && !again.changed,
              QStringLiteral("merging a saved, fully offered board changes nothing"));

        QVector<StatsTilePlacement> trimmed = grown.layout;
        trimmed.removeIf([](const StatsTilePlacement &placement) {
            return placement.id == QLatin1String("tokens");
        });
        const StatsDashboard::LayoutMerge kept = StatsDashboard::mergeSavedLayout(
            trimmed, true, defaults, cat, grown.offered);
        check(!ids(kept.layout).contains(QStringLiteral("tokens")) && !kept.changed,
              QStringLiteral("a tile the user removed is not offered again"));

        const StatsDashboard::LayoutMerge emptied = StatsDashboard::mergeSavedLayout(
            {}, true, defaults, cat, grown.offered);
        check(emptied.layout.isEmpty() && !emptied.changed,
              QStringLiteral("a deliberately emptied board stays empty"));

        QVector<StatsTilePlacement> foreign = grown.layout;
        foreign.append({QStringLiteral("from-a-newer-build"), 1, 2});
        const StatsDashboard::LayoutMerge preserved = StatsDashboard::mergeSavedLayout(
            foreign, true, defaults, cat, grown.offered);
        check(ids(preserved.layout).contains(QStringLiteral("from-a-newer-build")) &&
                  preserved.layout.last().height == 2,
              QStringLiteral("a tile this build does not know is kept, not erased"));

        StatsDashboard shown;
        shown.setCatalog(cat);
        shown.setLayoutModel(preserved.layout);
        check(!shown.tileIds().contains(QStringLiteral("from-a-newer-build")) &&
                  shown.tileIds().size() == grown.layout.size(),
              QStringLiteral("an unknown tile is hidden from the board while it stays saved"));
    }

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
