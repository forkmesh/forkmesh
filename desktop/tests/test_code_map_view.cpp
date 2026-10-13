// The code map view (adhoc #187) is a plain QWidget with std::function
// callbacks, so it is driven directly and offscreen: it must paint both modes,
// ask for the text of the cells big enough to show it, zoom where it is
// clicked, hit-test the software 3D scene, and go quiet when nothing moves.
#include "CodeMapView.h"

#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QImage>
#include <QMouseEvent>

#include <cstdio>

using namespace forkmesh::codemap;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

QByteArray record(qint64 size, const char *path)
{
    return QByteArray("100644 blob 0123456789abcdef0123456789abcdef01234567 ")
        + QByteArray::number(size).rightJustified(7) + '\t' + path + '\0';
}

void settle(int milliseconds)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < milliseconds)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

void click(CodeMapView *view, const QPoint &at)
{
    QMouseEvent press(QEvent::MouseButtonPress, at, view->mapToGlobal(at), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, view->mapToGlobal(at), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view, &release);
}

// Pixels that differ from the top-left (background) one.
int paintedPixels(CodeMapView *view)
{
    const QImage image = view->grab().toImage().convertToFormat(QImage::Format_RGB32);
    const QRgb background = image.pixel(0, 0);
    int painted = 0;
    for (int y = 0; y < image.height(); y += 2)
        for (int x = 0; x < image.width(); x += 2)
            if (image.pixel(x, y) != background)
                ++painted;
    return painted;
}

int find(const Tree &tree, const char *path)
{
    for (int i = 0; i < tree.nodes.size(); ++i)
        if (tree.nodes.at(i).path == QLatin1String(path))
            return i;
    return -1;
}

} // namespace

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    CodeMapView view;
    view.resize(900, 600);
    view.show();
    settle(30);
    view.grab(); // an empty view must paint its placeholder without a tree

    QStringList requested;
    int focusChanges = 0;
    QString opened;
    view.setOnFocusChanged([&](int) { ++focusChanges; });
    view.setOnOpenFile([&](const QString &path) { opened = path; });
    view.setOnWantContent([&](const QStringList &paths) {
        requested += paths;
        QString source;
        for (int i = 0; i < 120; ++i)
            source += QStringLiteral("    const value_%1 = compute(%1); // step\n").arg(i);
        const QByteArray bytes = source.toUtf8();
        const QByteArray binary("\x89PNG\0\0binary", 12);
        QImage red(40, 20, QImage::Format_RGB32);
        red.fill(Qt::red);
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        red.save(&buffer, "PNG");
        for (const QString &path : paths) {
            if (path.endsWith(QLatin1String("logo.png")))
                view.setContent(path, &png);
            else if (path.endsWith(QLatin1String("broken.png")))
                view.setContent(path, &binary);
            else if (path.endsWith(QLatin1String(".dat")))
                view.setContent(path, &binary);
            else if (path.endsWith(QLatin1String("gone.js")))
                view.setContent(path, nullptr);
            else
                view.setContent(path, &bytes);
        }
        view.finishContentBatch();
    });

    QByteArray listing;
    listing += record(30000, "src/engine.js");
    listing += record(20000, "src/render.js");
    listing += record(9000, "src/gone.js");
    listing += record(8000, "src/blob.dat");
    listing += record(15000, "docs/guide.md");
    listing += record(10000, "docs/logo.png");
    listing += record(6000, "docs/broken.png");
    listing += record(7000, "docs/bundle.zip");
    listing += record(5000, "README.md");
    view.setTree(buildTree(listing, QStringLiteral("fixture")));
    check(focusChanges >= 1, "a new tree announces the root as the focus");
    settle(400);

    // Flat view: painted, and the cells big enough to read asked for their text.
    check(paintedPixels(&view) > 5000, "the flat view paints the treemap");
    check(requested.contains(QStringLiteral("src/engine.js"))
              && requested.contains(QStringLiteral("README.md")),
          "visible text files are requested");
    check(!requested.contains(QStringLiteral("docs/bundle.zip")),
          "a binary without a picture is never read");
    const int zip = find(view.tree(), "docs/bundle.zip");
    check(view.tree().nodes.at(zip).contentState == 3, "and shows its type card instead");
    check(requested.contains(QStringLiteral("docs/logo.png")), "an image is read for its picture");
    check(view.stats().previews == 1, "the image decodes into a preview");
    check(view.tree().nodes.at(find(view.tree(), "docs/broken.png")).contentState == 3,
          "an image that does not decode falls back to its type card");
    {
        const QImage shot = view.grab().toImage().convertToFormat(QImage::Format_RGB32);
        int red = 0;
        for (int y = 0; y < shot.height(); ++y)
            for (int x = 0; x < shot.width(); ++x)
                if (shot.pixel(x, y) == qRgb(255, 0, 0))
                    ++red;
        check(red > 500, "the image is drawn inside its cell");
    }
    check(view.stats().loadedFiles == 4, "engine, render, guide and README are loaded");
    const int dat = find(view.tree(), "src/blob.dat");
    const int gone = find(view.tree(), "src/gone.js");
    check(view.tree().nodes.at(dat).contentState == 3, "NUL bytes mark a blob unavailable");
    check(view.tree().nodes.at(gone).contentState == 3, "a missing blob is unavailable, not pending");
    const int before = requested.size();
    view.grab();
    settle(300);
    check(requested.size() == before, "nothing is requested twice");

    // Click to zoom: src is the largest directory, so it is laid out first.
    const int src = find(view.tree(), "src");
    const double homeScale = view.stats().scale;
    const QPoint insideSrc = view.screenRect(src).center().toPoint();
    check(view.nodeAt(insideSrc) == find(view.tree(), "src/engine.js")
              || view.tree().nodes.at(view.nodeAt(insideSrc)).path.startsWith(QLatin1String("src")),
          "hit testing finds a node under src");
    click(&view, view.screenRect(find(view.tree(), "README.md")).center().toPoint());
    settle(700);
    check(view.focusNode() == find(view.tree(), "README.md"), "clicking a cell focuses it");
    check(view.stats().scale > homeScale * 1.5, "and zooms in on it");
    check(view.zoomOut(), "Esc-style zoom out steps to the parent");
    settle(700);
    check(view.focusNode() == 0 && !view.zoomOut(), "at the root there is nowhere further out");

    // Search.
    check(view.setSearch(QStringLiteral("SRC/")) == 4, "search is case-insensitive over paths");
    view.visitMatch(false);
    settle(700);
    check(view.focusNode() == find(view.tree(), "src/engine.js"), "Enter visits the largest match first");
    view.setSearch(QString());

    // 3D view.
    view.goTo(0, false);
    view.setMode(CodeMapView::Mode::City);
    settle(400);
    check(paintedPixels(&view) > 5000, "the 3D view paints the city");
    const CodeMapView::Stats city = view.stats();
    check(city.boxesDrawn >= 9, "every folder and file is a box with a top face");
    check(city.texturesDrawn >= 1, "loaded files carry their source on top");
    check(city.labelsDrawn >= 1, "folders are labelled");
    int hits = 0;
    for (int x = 50; x < 900; x += 50)
        for (int y = 50; y < 600; y += 50)
            if (view.nodeAt(QPoint(x, y)) >= 0)
                ++hits;
    check(hits > 10, "the painted faces are hit-testable");
    view.goToPath(QStringLiteral("src/engine.js"));
    settle(900);
    check(view.focusNode() == find(view.tree(), "src/engine.js"), "flying to a path focuses it");
    check(view.nodeAt(QPoint(450, 300)) == find(view.tree(), "src/engine.js"),
          "the flown-to tile ends up under the centre of the view");
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, QPoint(450, 300), view.mapToGlobal(QPoint(450, 300)),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&view, &doubleClick);
    check(opened == QLatin1String("src/engine.js"), "double-clicking a file asks the owner to open it");

    // An idle view must stop painting.
    settle(600);
    const quint64 frames = view.stats().frames;
    settle(500);
    check(view.stats().frames == frames, "an idle view does not repaint");

    // Back to flat keeps the focus; an empty tree must not crash either mode.
    view.setMode(CodeMapView::Mode::Flat);
    settle(100);
    check(view.focusNode() == find(view.tree(), "src/engine.js"), "switching modes keeps the focus");
    view.setTree(Tree());
    view.grab();
    view.setMode(CodeMapView::Mode::City);
    view.grab();
    click(&view, QPoint(100, 100));

    if (failures == 0)
        std::fprintf(stdout, "test_code_map_view: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
