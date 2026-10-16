#include "EmberFeature.h"
#include "EmberMapView.h"
#include "EmberModel.h"
#include "RepoInsightBar.h"
#include "Theme.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QSet>
#include <QThread>

#include <cmath>

using namespace forkmesh::ember;
using forkmesh::insight::Coupling;
using forkmesh::insight::FileInfo;
using forkmesh::insight::Snapshot;
using forkmesh::insight::SnapshotPtr;

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition) {
        qInfo("PASS: %s", qPrintable(message));
    } else {
        qCritical("FAIL: %s", qPrintable(message));
        ++failures;
    }
}

void pump(int ms)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        if (condition())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return condition();
}

FileInfo file(const QString &path, int lines, double health, int commits = 0, double hotspot = 0,
              const QString &author = QString(), int fixes = 0, const QString &language = QStringLiteral("C++"))
{
    FileInfo f;
    f.path = path;
    f.language = language;
    f.codeLines = lines;
    f.lines = lines + lines / 5;
    f.health = health;
    f.commits = commits;
    f.recentCommits = commits / 3;
    f.hotspot = hotspot;
    f.mainAuthor = author;
    f.mainAuthorShare = author.isEmpty() ? 0 : 0.6;
    if (!author.isEmpty())
        f.topAuthors.append({author, commits});
    f.authors = author.isEmpty() ? 0 : 1;
    f.fixCommits = fixes;
    f.complexity = lines / 10;
    if (health < 8)
        f.healthIssues << QStringLiteral("Long function") << QStringLiteral("Deep nesting");
    forkmesh::insight::FunctionInfo fn;
    fn.name = QStringLiteral("run");
    fn.line = 12;
    fn.length = 40;
    fn.complexity = 9;
    f.functions.append(fn);
    return f;
}

std::shared_ptr<Snapshot> sample()
{
    auto s = std::make_shared<Snapshot>();
    s->root = QStringLiteral("/tmp/ember-sample");
    s->name = QStringLiteral("sample");
    s->git = true;
    s->historyDays = 365;
    s->commitsScanned = 400;
    s->files = {
        file(QStringLiteral("src/core/engine.cpp"), 4200, 2.5, 66, 1.0, QStringLiteral("Ada"), 14),
        file(QStringLiteral("src/core/engine.h"), 300, 7.0, 40, 0.4, QStringLiteral("Ada"), 3),
        file(QStringLiteral("src/core/util.cpp"), 800, 9.5, 5, 0.05, QStringLiteral("Bo"), 0),
        file(QStringLiteral("src/ui/window.cpp"), 2500, 5.0, 30, 0.6, QStringLiteral("Bo"), 6),
        file(QStringLiteral("src/ui/window.h"), 200, 9.0, 12, 0.1, QStringLiteral("Cy"), 0),
        file(QStringLiteral("src/ui/theme.qml"), 150, 9.8, 2, 0.01, QStringLiteral("Cy"), 0, QStringLiteral("QML")),
        file(QStringLiteral("tools/build.py"), 400, 3.5, 0, 0, QString(), 0, QStringLiteral("Python")),
        file(QStringLiteral("README.md"), 20, 10, 1, 0, QStringLiteral("Ada"), 0, QStringLiteral("Markdown")),
        file(QStringLiteral("deep/a/b/c/leaf.go"), 90, 8.5, 3, 0.02, QStringLiteral("Dee"), 1, QStringLiteral("Go")),
    };
    s->files[6].formerAuthorShare = 0.9;
    s->files[0].formerAuthorShare = 0.7;
    s->formerAuthors = {QStringLiteral("Dee")};
    s->authors = {QStringLiteral("Ada"), QStringLiteral("Bo"), QStringLiteral("Cy"), QStringLiteral("Dee")};
    for (int i = 0; i < s->files.size(); ++i)
        s->byPath.insert(s->files[i].path, i);
    s->couplings = {{0, 1, 30, 0.8}, {0, 3, 10, 0.35}, {3, 4, 9, 0.5}};
    return s;
}

std::shared_ptr<Snapshot> synthetic(int count)
{
    auto s = std::make_shared<Snapshot>();
    s->name = QStringLiteral("big");
    s->git = true;
    s->commitsScanned = 1000;
    quint32 seed = 7;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    s->files.reserve(count);
    for (int i = 0; i < count; ++i) {
        const QString path = QStringLiteral("m%1/p%2/q%3/f%4.cpp")
                                 .arg(next() % 12)
                                 .arg(next() % 9)
                                 .arg(next() % 15)
                                 .arg(i);
        FileInfo f;
        f.path = path;
        f.language = QStringLiteral("C++");
        f.codeLines = int(5 + next() % 3000);
        f.health = 1 + (next() % 90) / 10.0;
        f.commits = int(next() % 80);
        f.hotspot = (next() % 1000) / 1000.0;
        s->files.append(f);
        s->byPath.insert(path, i);
    }
    return s;
}

bool siblingsSeparate(const MapLayout &layout, double tolerance)
{
    for (const MapNode &node : layout.nodes) {
        for (int i = 0; i < node.children.size(); ++i) {
            const MapNode &a = layout.nodes[node.children[i]];
            for (int j = i + 1; j < node.children.size(); ++j) {
                const MapNode &b = layout.nodes[node.children[j]];
                if (std::hypot(a.x - b.x, a.y - b.y) < a.r + b.r - tolerance)
                    return false;
            }
        }
    }
    return true;
}

bool childrenInside(const MapLayout &layout, double tolerance)
{
    for (int i = 1; i < layout.nodes.size(); ++i) {
        const MapNode &c = layout.nodes[i];
        const MapNode &p = layout.nodes[c.parent];
        if (std::hypot(c.x - p.x, c.y - p.y) + c.r > p.r + tolerance)
            return false;
    }
    return true;
}

int distinctColors(const QImage &image)
{
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y += 4) {
        for (int x = 0; x < image.width(); x += 4)
            colors.insert(image.pixel(x, y));
    }
    return colors.size();
}

forkmesh::market::NativePackageHost fakeHost(const QString &root)
{
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("ember");
    host.packageName = QStringLiteral("Ember");
    if (!root.isEmpty()) {
        forkmesh::market::NativePackageRepository repo;
        repo.key = QStringLiteral("preview");
        repo.title = QFileInfo(root).fileName();
        repo.path = root;
        host.repositories = [repo] { return QList<forkmesh::market::NativePackageRepository>{repo}; };
        host.currentRepository = [repo] { return repo; };
    }
    return host;
}

int runPreview(QApplication &app, const QString &root, const QString &out)
{
    app.setPalette(Theme::applicationPalette(true));
    app.setStyleSheet(Theme::styleSheetForPalette(true, {}));
    EmberFeature feature(fakeHost(root));
    feature.resize(1600, 1000);
    feature.show();
    feature.refresh();
    const bool ok = waitFor([&] { return feature.ready() && !feature.bar()->busy(); }, 240000);
    if (!ok) {
        qCritical("Preview: the analysis did not finish in time");
        return 2;
    }
    pump(300);
    feature.preparePreview(qEnvironmentVariable("FORKMESH_INSIGHT_PREVIEW_VIEW"));
    const QString card = qEnvironmentVariable("FORKMESH_EMBER_PREVIEW_CARD");
    if (!card.isEmpty())
        feature.setCardTab(card.toInt());
    if (feature.cardTab() == 1)
        waitFor([] { return false; }, 6000);
    pump(800);
    feature.view()->finishAnimation();
    pump(200);
    const QImage image = feature.grab().toImage();
    if (!image.save(out)) {
        qCritical("Preview: could not write %s", qPrintable(out));
        return 3;
    }
    qInfo("Preview written to %s (%d files, selected %d)", qPrintable(out),
          feature.bar()->snapshot() ? int(feature.bar()->snapshot()->files.size()) : 0, feature.selectedFile());
    return 0;
}

void testPacking()
{
    QVector<PackCircle> circles;
    quint32 seed = 11;
    for (int i = 0; i < 300; ++i) {
        seed = seed * 1664525u + 1013904223u;
        circles.append({0, 0, 1 + (seed >> 8) % 40});
    }
    const double r = packSiblings(circles);
    bool apart = true;
    bool inside = true;
    for (int i = 0; i < circles.size(); ++i) {
        inside = inside && std::hypot(circles[i].x, circles[i].y) + circles[i].r <= r + 1e-6;
        for (int j = i + 1; j < circles.size(); ++j)
            apart = apart && std::hypot(circles[i].x - circles[j].x, circles[i].y - circles[j].y)
                                 >= circles[i].r + circles[j].r - 1e-6;
    }
    check(apart, QStringLiteral("packSiblings: 300 circles never overlap"));
    check(inside, QStringLiteral("packSiblings: the returned radius encloses every circle"));
    double area = 0;
    for (const PackCircle &c : circles)
        area += c.r * c.r;
    check(area / (r * r) > 0.55, QStringLiteral("packSiblings: packing is dense (%1 of the enclosing area)")
                                      .arg(area / (r * r), 0, 'f', 2));

    QVector<PackCircle> two = {{0, 0, 3}, {0, 0, 5}};
    const double r2 = packSiblings(two);
    check(std::abs(r2 - 8) < 1e-9 && std::abs(two[0].x + two[1].x - 2) < 20,
          QStringLiteral("packSiblings: two circles side by side, radius 8"));
    const PackCircle e = encloseCircles({{-1, 0, 1}, {1, 0, 1}, {0, 0.5, 0.2}});
    check(std::abs(e.r - 2) < 1e-6 && std::abs(e.x) < 1e-6, QStringLiteral("encloseCircles: smallest enclosing circle"));
}

void testLayout()
{
    const auto s = sample();
    const MapLayout layout = buildLayout(*s);
    check(layout.nodes.size() > s->files.size(), QStringLiteral("layout: folders and files"));
    bool mapped = true;
    for (int i = 0; i < s->files.size(); ++i) {
        const int n = layout.fileNode(i);
        mapped = mapped && n > 0 && layout.nodes[n].file == i && layout.nodes[n].path == s->files[i].path;
    }
    check(mapped, QStringLiteral("layout: every file has its node"));
    check(childrenInside(layout, 1e-6), QStringLiteral("layout: children lie inside their parent"));
    check(siblingsSeparate(layout, 1e-6), QStringLiteral("layout: siblings never overlap"));
    const int leaf = layout.fileNode(8);
    const MapNode &chainFolder = layout.nodes[layout.nodes[leaf].parent];
    check(chainFolder.name == QStringLiteral("deep/a/b/c") && chainFolder.path == QStringLiteral("deep/a/b/c"),
          QStringLiteral("layout: single-folder chains collapse into one circle (%1)").arg(chainFolder.name));
    check(layout.nodes[0].codeLines == 4200 + 300 + 800 + 2500 + 200 + 150 + 400 + 20 + 90,
          QStringLiteral("layout: the root adds up every line"));
    const int engine = layout.fileNode(0);
    const int util = layout.fileNode(2);
    check(layout.nodes[engine].r > layout.nodes[util].r, QStringLiteral("layout: bigger files get bigger circles"));
    const int readme = layout.fileNode(7);
    check(layout.nodes[readme].r >= std::sqrt(12.0) - 1e-9, QStringLiteral("layout: tiny files keep a clickable size"));
    check(layout.nodeAt(QPointF(layout.nodes[engine].x, layout.nodes[engine].y)) == engine,
          QStringLiteral("layout: nodeAt finds the file under a point"));
    check(layout.nodeAt(QPointF(1e9, 1e9)) == -1, QStringLiteral("layout: nodeAt outside the map finds nothing"));
    const QVector<int> chain = layout.chain(engine);
    check(chain.size() == 4 && chain.first() == 0 && layout.nodes[chain[1]].name == QStringLiteral("src")
              && layout.nodes[chain[2]].name == QStringLiteral("core") && chain.last() == engine,
          QStringLiteral("layout: chain runs from the root to the file"));
    const MapLayout again = buildLayout(*s);
    bool same = again.nodes.size() == layout.nodes.size();
    for (int i = 0; same && i < layout.nodes.size(); ++i)
        same = layout.nodes[i].x == again.nodes[i].x && layout.nodes[i].y == again.nodes[i].y
               && layout.nodes[i].r == again.nodes[i].r;
    check(same, QStringLiteral("layout: deterministic"));
    const double coreHealth = layout.nodes[layout.nodes[engine].parent].health;
    check(coreHealth > 2.5 && coreHealth < 4.5, QStringLiteral("layout: folder health is weighted by lines (%1)").arg(coreHealth));

    const auto big = synthetic(20000);
    QElapsedTimer clock;
    clock.start();
    const MapLayout large = buildLayout(*big);
    const qint64 ms = clock.elapsed();
    check(ms < 1000, QStringLiteral("layout: 20,000 files packed in %1 ms").arg(ms));
    check(childrenInside(large, 1e-6) && siblingsSeparate(large, 1e-6),
          QStringLiteral("layout: 20,000-file layout has no overlaps and stays nested"));
    const MapLayout large2 = buildLayout(*big);
    check(large2.nodes.size() == large.nodes.size() && large2.nodes.last().x == large.nodes.last().x,
          QStringLiteral("layout: 20,000-file layout is deterministic"));
}

void testViewMath()
{
    const auto s = sample();
    const MapLayout layout = buildLayout(*s);
    const QSizeF viewport(1000, 800);
    const ViewTransform root = fitTransform(layout.nodes[0], viewport, 18);
    check(std::abs(layout.nodes[0].r * root.k - 382) < 1e-6, QStringLiteral("view: the root fits the shorter side"));
    const int engine = layout.fileNode(0);
    const ViewTransform folder = fitTransform(layout.nodes[layout.nodes[engine].parent], viewport);
    const ViewTransform start = interpolateView(root, folder, 0, viewport);
    const ViewTransform end = interpolateView(root, folder, 1, viewport);
    const ViewTransform mid = interpolateView(root, folder, 0.5, viewport);
    check(std::abs(start.k - root.k) < 1e-9 && std::abs(end.k - folder.k) < 1e-9 && end.cx == folder.cx,
          QStringLiteral("view: zoom interpolation starts and ends on its targets"));
    check(mid.k > 0 && std::isfinite(mid.cx), QStringLiteral("view: zoom interpolation stays finite"));
    check(easeInOut(0) == 0 && easeInOut(1) == 1 && std::abs(easeInOut(0.5) - 0.5) < 1e-9,
          QStringLiteral("view: easing"));
    const int focus = readableFocus(layout, engine, viewport, 26);
    check(focus >= 0 && layout.isAncestor(focus, engine) && layout.isFolder(focus),
          QStringLiteral("view: readable focus is a folder holding the file"));
    const ViewTransform at = fitTransform(layout.nodes[focus], viewport);
    check(layout.nodes[engine].r * at.k >= 26, QStringLiteral("view: the file is readable at that focus"));
}

void testColoring()
{
    const auto s = sample();
    const CouplingIndex coupling = buildCouplingIndex(*s);
    check(coupling[0].size() == 2 && coupling[0].first().file == 1 && coupling[0].first().degree == 0.8,
          QStringLiteral("coupling: index sorted by degree"));
    check(coupling[4].size() == 1 && coupling[4].first().file == 3, QStringLiteral("coupling: index is symmetric"));

    const TabColoring health = colorize(*s, Tab::CodeHealth, {}, coupling);
    check(health.files[0].color == bandColor(forkmesh::insight::HealthBand::Unhealthy)
              && health.files[3].color == bandColor(forkmesh::insight::HealthBand::Problematic)
              && health.files[2].color == bandColor(forkmesh::insight::HealthBand::Healthy),
          QStringLiteral("code health: red, yellow and green bands"));
    check(health.legend.size() == 3 && health.matching == s->files.size(), QStringLiteral("code health: legend and nothing dimmed"));

    const TabColoring hot = colorize(*s, Tab::Hotspots, {}, coupling);
    check(hot.files[0].color.greenF() < hot.files[3].color.greenF() && hot.files[3].color.greenF() < hot.files[2].color.greenF()
              && hot.files[0].color.hsvHue() < hot.files[2].color.hsvHue(),
          QStringLiteral("hotspots: the most changed file is the reddest"));
    check(hot.files[6].color == neutralColor(), QStringLiteral("hotspots: unchanged files stay grey"));
    check(hot.files[0].glow > 0 && hot.files[2].glow == 0 && hot.glowing >= 1,
          QStringLiteral("hotspots: top hotspots glow"));
    check(hot.scale, QStringLiteral("hotspots: has a scale legend"));

    const QList<Target> targets = refactoringTargets(*s, coupling, 15);
    check(targets.size() >= 3 && targets[0].file == 0 && targets[1].file == 3 && targets[2].file == 1,
          QStringLiteral("refactoring: ordered by hotspot × (10 − health)"));
    bool ranks = true;
    for (int i = 0; i < targets.size(); ++i)
        ranks = ranks && targets[i].rank == i + 1 && (i == 0 || targets[i - 1].score >= targets[i].score);
    check(ranks, QStringLiteral("refactoring: ranks are 1..n by descending score"));
    check(!targets[0].reasons.isEmpty() && targets[0].reasons.join(' ').contains(QStringLiteral("66")),
          QStringLiteral("refactoring: reasons mention the commits"));
    bool noHealthy = true;
    for (const Target &t : targets)
        noHealthy = noHealthy && s->files[t.file].commits > 0;
    check(noHealthy && targetScore(s->files[7]) == 0, QStringLiteral("refactoring: perfect or unchanged files are no target"));
    const TabColoring refactor = colorize(*s, Tab::RefactoringTargets, {}, coupling);
    check(refactor.files[0].rank == 1 && !refactor.files[0].dimmed && refactor.files[6].dimmed,
          QStringLiteral("refactoring: targets ranked, everything else dimmed"));
    check(refactor.targets.size() == targets.size(), QStringLiteral("refactoring: coloring carries the ranked list"));

    const TabColoring couple = colorize(*s, Tab::ChangeCoupling, {}, coupling);
    check(couple.files[0].color != neutralColor() && couple.files[2].color == neutralColor(),
          QStringLiteral("change coupling: coupled files coloured, others grey"));
    check(couple.files[0].color.greenF() > couple.files[3].color.greenF() - 1e-9,
          QStringLiteral("change coupling: stronger coupling is brighter"));

    const TabColoring knowledge = colorize(*s, Tab::Knowledge, {}, coupling);
    check(knowledge.files[0].color == categoricalPalette()[0], QStringLiteral("knowledge: the top author gets the first colour"));
    check(!categoricalPalette().contains(knowledge.files[6].color) && knowledge.files[6].marked && knowledge.files[0].marked
              && !knowledge.files[3].marked,
          QStringLiteral("knowledge: former contributors are marked"));
    bool formerLegend = false;
    for (const LegendEntry &entry : knowledge.legend)
        formerLegend = formerLegend || entry.label == QStringLiteral("Dee (former)");
    check(formerLegend, QStringLiteral("knowledge: legend flags former authors"));

    const TabColoring defects = colorize(*s, Tab::Defects, {}, coupling);
    check(defects.files[0].color.redF() > 0.7 && defects.files[2].color == neutralColor()
              && defects.files[0].color.greenF() < defects.files[3].color.greenF(),
          QStringLiteral("defects: more fixes, deeper red"));

    const TabColoring languages = colorize(*s, Tab::Languages, {}, coupling);
    check(languages.files[0].color == categoricalPalette()[0] && languages.files[6].color != languages.files[0].color
              && languages.legend.first().label == QStringLiteral("C++"),
          QStringLiteral("languages: categorical colours with C++ first"));

    auto plain = std::make_shared<Snapshot>(*s);
    plain->git = false;
    plain->commitsScanned = 0;
    for (FileInfo &f : plain->files)
        f.commits = 0;
    const TabColoring noHistory = colorize(*plain, Tab::Hotspots, {}, coupling);
    check(!noHistory.note.isEmpty() && noHistory.note.contains(QStringLiteral("git history")),
          QStringLiteral("no history: hotspot tabs explain they need git history"));
    check(colorize(*plain, Tab::CodeHealth, {}, coupling).note.isEmpty()
              && colorize(*plain, Tab::Languages, {}, coupling).note.isEmpty(),
          QStringLiteral("no history: code health and languages still work"));

    const Summary summary = summarize(*s);
    check(summary.unhealthy == 2 && summary.problematic == 2 && summary.topHotspot == 0
              && summary.text.contains(QStringLiteral("top hotspot src/core/engine.cpp")),
          QStringLiteral("summary: %1").arg(summary.text));
}

void testFilters()
{
    const auto s = sample();
    const CouplingIndex coupling = buildCouplingIndex(*s);
    Filters range;
    range.minHealth = 4;
    range.maxHealth = 8;
    const TabColoring a = colorize(*s, Tab::CodeHealth, range, coupling);
    check(!a.files[3].dimmed && !a.files[1].dimmed && a.files[0].dimmed && a.files[2].dimmed && a.matching == 2,
          QStringLiteral("filters: health range dims files outside it"));
    Filters commits;
    commits.minCommits = 20;
    check(colorize(*s, Tab::CodeHealth, commits, coupling).matching == 3, QStringLiteral("filters: commit threshold"));
    Filters author;
    author.author = QStringLiteral("Bo");
    const TabColoring b = colorize(*s, Tab::CodeHealth, author, coupling);
    check(!b.files[2].dimmed && !b.files[3].dimmed && b.files[0].dimmed && b.matching == 2,
          QStringLiteral("filters: author"));
    Filters search;
    search.search = QStringLiteral("WINDOW");
    check(colorize(*s, Tab::CodeHealth, search, coupling).matching == 2, QStringLiteral("filters: case-insensitive filename search"));
    check(search.active() && !Filters().active(), QStringLiteral("filters: active flag"));
    check(colorize(*s, Tab::CodeHealth, search, coupling).files.size() == s->files.size(),
          QStringLiteral("filters: non-matching files are dimmed, not removed"));
}

void testText()
{
    check(spanText(365) == QStringLiteral("1 year") && spanText(90) == QStringLiteral("3 months"), QStringLiteral("text: spans"));
    const qint64 now = 1'800'000'000;
    check(agoText(now - 61 * 86400, now) == QStringLiteral("2 months ago") && agoText(now, now) == QStringLiteral("today"),
          QStringLiteral("text: ago"));
    check(fileNameOf(QStringLiteral("a/b/c.cpp")) == QStringLiteral("c.cpp") && folderOf(QStringLiteral("a/b/c.cpp")) == QStringLiteral("a/b"),
          QStringLiteral("text: path parts"));
    check(percentText(0.126) == QStringLiteral("13 %"), QStringLiteral("text: percent"));
}

void testMapView()
{
    const auto s = sample();
    auto layout = std::make_shared<const MapLayout>(buildLayout(*s));
    auto coupling = std::make_shared<const CouplingIndex>(buildCouplingIndex(*s));
    EmberMapView view;
    view.resize(900, 700);
    view.show();
    int focusEvents = 0;
    int selected = -2;
    view.setOnFocusChanged([&](int) { ++focusEvents; });
    view.setOnSelect([&](int f) { selected = f; });
    view.setData(s, layout, coupling);
    view.setColoring(colorize(*s, Tab::CodeHealth, {}, *coupling));
    check(view.focus() == 0 && view.focusChain() == QVector<int>{0}, QStringLiteral("map: starts on the whole repository"));
    const int engine = layout->fileNode(0);
    const int core = layout->nodes[engine].parent;
    const int src = layout->nodes[core].parent;
    const QPointF srcCenter = view.toScreen(QPointF(layout->nodes[src].x, layout->nodes[src].y));
    const QPointF emptyInSrc = view.toScreen(QPointF(layout->nodes[src].x, layout->nodes[src].y - layout->nodes[src].r * 0.97));
    Q_UNUSED(srcCenter);
    view.clickAt(emptyInSrc);
    view.finishAnimation();
    check(view.focus() == src, QStringLiteral("map: a click on a folder zooms into it"));
    check(view.focusChain() == QVector<int>({0, src}), QStringLiteral("map: breadcrumb chain follows the zoom"));
    check(std::abs(view.screenRadius(src) - (350 - 18)) < 1e-6, QStringLiteral("map: the focused folder fills the view"));
    view.focusNode(core, false);
    check(view.focusChain() == QVector<int>({0, src, core}), QStringLiteral("map: deeper breadcrumb"));
    view.zoomOut();
    view.finishAnimation();
    check(view.focus() == src, QStringLiteral("map: zoom out goes up one level"));
    const QPointF enginePoint = view.toScreen(QPointF(layout->nodes[engine].x, layout->nodes[engine].y));
    view.clickAt(enginePoint);
    check(selected == 0 && view.selectedFile() == 0, QStringLiteral("map: a click on a file selects it"));
    view.grab();
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&view, &esc);
    check(view.selectedFile() == -1 && selected == -1, QStringLiteral("map: Esc clears the selection first"));
    QKeyEvent esc2(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&view, &esc2);
    view.finishAnimation();
    check(view.focus() == 0, QStringLiteral("map: then Esc zooms out"));
    view.setSelectedFile(8, true);
    view.finishAnimation();
    const int leaf = layout->fileNode(8);
    check(view.screenRadius(leaf) >= 20, QStringLiteral("map: selecting a tiny file zooms until it is readable"));
    check(focusEvents >= 4, QStringLiteral("map: focus changes are reported"));
    const QImage image = view.grab().toImage();
    check(distinctColors(image) > 30, QStringLiteral("map: renders a non-blank picture"));
    check(view.labelsDrawn() > 0, QStringLiteral("map: draws file labels when circles are large enough"));
    view.setColoring(colorize(*s, Tab::Hotspots, {}, *coupling));
    view.grab();
    check(view.circlesDrawn() > 0, QStringLiteral("map: hotspot view paints circles"));
}

void testMapPerformance()
{
    const auto big = synthetic(20000);
    auto layout = std::make_shared<const MapLayout>(buildLayout(*big));
    auto coupling = std::make_shared<const CouplingIndex>(buildCouplingIndex(*big));
    EmberMapView view;
    view.resize(1200, 900);
    view.show();
    view.setData(big, layout, coupling);
    view.setColoring(colorize(*big, Tab::Hotspots, {}, *coupling));
    QElapsedTimer clock;
    clock.start();
    view.grab();
    const qint64 full = clock.elapsed();
    check(full < 400, QStringLiteral("map: full-quality frame of 20,000 files in %1 ms").arg(full));
    view.zoomBy(1.6);
    clock.restart();
    for (int i = 0; i < 5; ++i)
        view.grab();
    const double fast = clock.elapsed() / 5.0;
    check(view.animating() && fast < 40, QStringLiteral("map: animated frame of 20,000 files in %1 ms").arg(fast, 0, 'f', 1));
    view.finishAnimation();
    view.focusNode(layout->nodes[0].children.first(), false);
    clock.restart();
    view.grab();
    check(clock.elapsed() < 400, QStringLiteral("map: zoomed-in frame in %1 ms").arg(clock.elapsed()));
}

void testFeature()
{
    forkmesh::ember::EmberFeature feature(fakeHost(QString()));
    feature.resize(1400, 900);
    feature.show();
    const auto s = sample();
    feature.showSnapshot(s);
    check(waitFor([&] { return feature.ready(); }, 5000), QStringLiteral("feature: the layout is built on a worker"));
    check(feature.tab() == Tab::Hotspots, QStringLiteral("feature: opens on Hotspots when there is history"));
    feature.setTab(Tab::RefactoringTargets);
    check(feature.coloring().tab == Tab::RefactoringTargets && feature.coloring().targets.size() >= 3,
          QStringLiteral("feature: refactoring tab carries the ranked list"));
    feature.selectFile(0, true);
    feature.view()->finishAnimation();
    check(feature.selectedFile() == 0 && feature.crumbText().endsWith(QStringLiteral("engine.cpp")),
          QStringLiteral("feature: breadcrumb ends with the file (%1)").arg(feature.crumbText()));
    const auto hits = feature.searchHits(QStringLiteral("window"), 10);
    check(hits.size() == 2 && hits.first().id.startsWith(QStringLiteral("file:src/ui/window")),
          QStringLiteral("feature: top search finds files"));
    feature.activateSearchHit(QStringLiteral("file:src/ui/window.h"));
    check(feature.selectedFile() == 4, QStringLiteral("feature: activating a hit selects the file"));
    feature.setHostSearchQuery(QStringLiteral("engine"));
    pump(150);
    check(feature.coloring().matching == 2, QStringLiteral("feature: the top search box filters the map"));
    feature.setCardTab(2);
    pump(50);
    const QImage image = feature.grab().toImage();
    check(!image.isNull() && distinctColors(image) > 40, QStringLiteral("feature: page renders offscreen"));

    auto plain = std::make_shared<Snapshot>(*s);
    plain->git = false;
    plain->commitsScanned = 0;
    for (FileInfo &f : plain->files)
        f.commits = 0;
    forkmesh::ember::EmberFeature second(fakeHost(QString()));
    second.resize(1200, 800);
    second.show();
    second.showSnapshot(plain);
    waitFor([&] { return second.ready(); }, 5000);
    check(second.tab() == Tab::CodeHealth, QStringLiteral("feature: falls back to Code health without history"));
    check(distinctColors(second.grab().toImage()) > 30, QStringLiteral("feature: no-history page renders"));
}

}

int main(int argc, char **argv)
{
    const QString previewRoot = qEnvironmentVariable("FORKMESH_INSIGHT_PREVIEW_ROOT");
    const QString previewOut = qEnvironmentVariable("FORKMESH_INSIGHT_PREVIEW_OUT");
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (!previewRoot.isEmpty() && !previewOut.isEmpty())
        return runPreview(app, previewRoot, previewOut);

    testPacking();
    testLayout();
    testViewMath();
    testColoring();
    testFilters();
    testText();
    testMapView();
    testMapPerformance();
    testFeature();

    if (failures > 0) {
        qCritical("%d check(s) failed", failures);
        return 1;
    }
    qInfo("All Ember checks passed");
    return 0;
}
