#include "SynapseFeature.h"
#include "SynapseLayout.h"
#include "SynapseView.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QLineF>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QSet>
#include <QThread>

#include <cmath>

using namespace forkmesh::synapse;
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

struct Spec {
    QString path;
    QStringList imports;
};

SnapshotPtr makeSnapshot(const QList<Spec> &specs, const QList<QStringList> &cycles = {})
{
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->root = QStringLiteral("/tmp/synapse-test");
    snapshot->name = QStringLiteral("synapse-test");
    for (const Spec &spec : specs) {
        FileInfo file;
        file.path = spec.path;
        file.language = spec.path.endsWith(QLatin1String(".py")) ? QStringLiteral("Python") : QStringLiteral("C++");
        file.codeLines = 40 + int(stableHash(spec.path) % 400u);
        file.lines = file.codeLines + 10;
        file.health = 2.0 + double(stableHash(spec.path) % 80u) / 10.0;
        file.hotspot = double(stableHash(spec.path) % 100u) / 100.0;
        snapshot->byPath.insert(file.path, snapshot->files.size());
        snapshot->files.append(file);
    }
    for (int i = 0; i < specs.size(); ++i) {
        for (const QString &target : specs.at(i).imports) {
            const int to = snapshot->byPath.value(target, -1);
            if (to < 0) {
                snapshot->files[i].externalImports.append(target);
                continue;
            }
            snapshot->files[i].dependsOn.append(to);
            snapshot->files[to].dependedOnBy.append(i);
        }
    }
    for (const QStringList &cycle : cycles) {
        QVector<int> members;
        for (const QString &path : cycle) {
            const int index = snapshot->byPath.value(path);
            members.append(index);
            snapshot->files[index].cycle = snapshot->cycles.size();
        }
        snapshot->cycles.append(members);
    }
    return snapshot;
}

SnapshotPtr smallSnapshot()
{
    return makeSnapshot(
        {
            {QStringLiteral("core/a.cpp"), {QStringLiteral("core/b.cpp")}},
            {QStringLiteral("core/b.cpp"), {QStringLiteral("core/c.cpp")}},
            {QStringLiteral("core/c.cpp"), {QStringLiteral("core/a.cpp"), QStringLiteral("util/log.cpp")}},
            {QStringLiteral("util/log.cpp"), {}},
            {QStringLiteral("util/str.cpp"), {QStringLiteral("util/log.cpp"), QStringLiteral("fmt")}},
            {QStringLiteral("app/main.cpp"), {QStringLiteral("core/a.cpp"), QStringLiteral("util/str.cpp")}},
            {QStringLiteral("app/view.cpp"), {QStringLiteral("app/main.cpp")}},
            {QStringLiteral("docs/lonely.cpp"), {}},
        },
        {{QStringLiteral("core/a.cpp"), QStringLiteral("core/b.cpp"), QStringLiteral("core/c.cpp")}});
}

SnapshotPtr generatedSnapshot(int modules, int perModule, quint32 salt)
{
    QList<Spec> specs;
    for (int m = 0; m < modules; ++m) {
        for (int f = 0; f < perModule; ++f)
            specs.append({QStringLiteral("mod%1/sub/file%2.cpp").arg(m).arg(f), {}});
    }
    for (int i = 0; i < specs.size(); ++i) {
        const int module = i / perModule;
        const quint32 h = stableHash(QStringLiteral("%1-%2").arg(i).arg(salt));
        const int local = int(h % 3u) + 1;
        for (int k = 0; k < local; ++k) {
            const int target = module * perModule + int((h >> (4 + k * 5)) % quint32(perModule));
            if (target != i)
                specs[i].imports.append(specs.at(target).path);
        }
        if (h % 7u == 0 && modules > 1) {
            const int other = (module + 1 + int(h % quint32(modules - 1))) % modules;
            specs[i].imports.append(specs.at(other * perModule + int((h >> 9) % quint32(perModule))).path);
        }
    }
    return makeSnapshot(specs);
}

QImage render(QWidget &widget)
{
    return widget.grab().toImage().convertToFormat(QImage::Format_ARGB32);
}

int distinctColours(const QImage &image)
{
    QSet<QRgb> colours;
    for (int y = 0; y < image.height(); y += 3) {
        for (int x = 0; x < image.width(); x += 3)
            colours.insert(image.pixel(x, y));
    }
    return colours.size();
}

bool pump(const std::function<bool()> &done, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(5);
    }
    return true;
}

forkmesh::market::NativePackageHost hostFor(const QString &root)
{
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("synapse");
    forkmesh::market::NativePackageRepository repository;
    repository.key = QStringLiteral("preview");
    repository.title = QFileInfo(root).fileName();
    repository.path = root;
    host.repositories = [repository] { return QList<forkmesh::market::NativePackageRepository>{repository}; };
    host.currentRepository = [repository] { return repository; };
    return host;
}

void testQueries()
{
    const SnapshotPtr snapshot = smallSnapshot();
    const Snapshot &s = *snapshot;
    const QVector<int> loop = cyclePath(s, s.cycles.first());
    check(loop.size() == 4 && loop.first() == loop.last(), QStringLiteral("cycle path closes on itself"));
    check(cycleText(s, loop) == QStringLiteral("a.cpp → b.cpp → c.cpp → a.cpp"),
          QStringLiteral("cycle text reads a → b → c → a (%1)").arg(cycleText(s, loop)));
    const QVector<int> lonely = orphans(s);
    check(lonely.size() == 1 && s.files.at(lonely.first()).path == QLatin1String("docs/lonely.cpp"),
          QStringLiteral("orphans are files with no imports either way"));
    const QVector<int> leaf = leaves(s);
    check(leaf.size() == 1 && s.files.at(leaf.first()).path == QLatin1String("util/log.cpp"),
          QStringLiteral("leaves import nothing but are imported"));
    const QVector<int> imported = mostImported(s, 2);
    check(imported.size() == 2 && s.files.at(imported.first()).dependedOnBy.size() == 2,
          QStringLiteral("most imported is ranked by importers"));
    check(s.files.at(mostImporting(s, 1).first()).dependsOn.size() == 2,
          QStringLiteral("most importing is ranked by imports"));
    check(importCount(s) == 8, QStringLiteral("import count counts repository imports only"));
    const int main = s.indexOf(QStringLiteral("app/main.cpp"));
    const Neighbourhood one = neighbourhood(s, main, 1, Direction::Both);
    check(one.imports.size() == 2 && one.importers.size() == 1,
          QStringLiteral("one hop: two imports and one importer"));
    const Neighbourhood two = neighbourhood(s, main, 2, Direction::Imports);
    check(two.imports.contains(s.indexOf(QStringLiteral("core/b.cpp")))
              && two.imports.value(s.indexOf(QStringLiteral("core/b.cpp"))) == 2
              && two.importers.isEmpty(),
          QStringLiteral("two hops follow imports only and record depth"));
    const Neighbourhood up = neighbourhood(s, s.indexOf(QStringLiteral("util/log.cpp")), 3, Direction::Importers);
    check(up.importers.contains(main) && up.imports.isEmpty(), QStringLiteral("importers walk upward"));
    check(mostConnected(s) >= 0, QStringLiteral("most connected file found"));

    const Graph graph = buildGraph(snapshot, false);
    check(graph.nodes.size() == 7, QStringLiteral("graph leaves isolated files out (%1)").arg(graph.nodes.size()));
    check(graph.edges.size() == 8, QStringLiteral("graph has one edge per import"));
    int cycleEdges = 0;
    for (const Edge &edge : graph.edges)
        cycleEdges += edge.cycle ? 1 : 0;
    check(cycleEdges == 3, QStringLiteral("three edges inside the cycle"));
    check(graph.cycles.size() == 1 && graph.cycles.first().edges.size() == 3,
          QStringLiteral("cycle info lists its edges"));
    check(graph.lobes.size() >= 3, QStringLiteral("files grouped into lobes (%1)").arg(graph.lobes.size()));
    const Graph withIsolated = buildGraph(snapshot, true);
    check(withIsolated.nodes.size() == 8, QStringLiteral("isolated files can be included"));
}

void testLayout()
{
    const SnapshotPtr snapshot = generatedSnapshot(6, 40, 1);
    const Graph graph = buildGraph(snapshot, false);
    LayoutOptions options;
    options.iterations = 160;
    int publishes = 0;
    double last = -1;
    const QVector<QVector3D> first = runLayout(graph, options, [&](const QVector<QVector3D> &, double progress) {
        ++publishes;
        last = progress;
    });
    const QVector<QVector3D> second = runLayout(graph, options);
    check(first == second, QStringLiteral("layout is deterministic"));
    check(publishes >= 2 && last == 1.0, QStringLiteral("layout publishes progress and finishes at 1"));
    bool inside = true;
    bool sides = true;
    for (int i = 0; i < first.size(); ++i) {
        inside = inside && graph.shape.radiusOf(first.at(i)) <= 1.0f;
        sides = sides && first.at(i).x() * float(graph.nodes.at(i).hemisphere) > 0;
    }
    check(inside, QStringLiteral("nodes stay inside the brain"));
    check(sides, QStringLiteral("nodes stay in their hemisphere"));
    const QVector<QVector3D> centres = lobeCentres(graph, first);
    float spread = 0;
    int counted = 0;
    for (int i = 0; i < first.size(); ++i) {
        spread += (first.at(i) - centres.at(graph.nodes.at(i).lobe)).length();
        ++counted;
    }
    spread /= float(std::max(1, counted));
    float apart = 1e9f;
    for (int a = 0; a < centres.size(); ++a)
        for (int b = a + 1; b < centres.size(); ++b)
            apart = std::min(apart, (centres.at(a) - centres.at(b)).length());
    check(apart > spread * 0.8f, QStringLiteral("lobes separate (closest centres %1, spread %2)").arg(apart).arg(spread));

    const SnapshotPtr big = generatedSnapshot(8, 220, 2);
    const Graph bigGraph = buildGraph(big, false);
    LayoutOptions fast;
    fast.iterations = 24;
    const QVector<QVector3D> b1 = runLayout(bigGraph, fast);
    const QVector<QVector3D> b2 = runLayout(bigGraph, fast);
    bool bigInside = true;
    for (const QVector3D &p : b1)
        bigInside = bigInside && bigGraph.shape.radiusOf(p) <= 1.0f && std::isfinite(p.x());
    check(bigGraph.nodes.size() > fast.barnesHutAbove, QStringLiteral("large graph uses Barnes-Hut (%1 nodes)").arg(bigGraph.nodes.size()));
    check(b1 == b2 && bigInside, QStringLiteral("Barnes-Hut layout is deterministic and bounded"));
    bool cancelled = false;
    runLayout(bigGraph, fast, {}, [&] {
        cancelled = true;
        return true;
    });
    check(cancelled, QStringLiteral("layout honours cancel"));
}

void testView()
{
    const SnapshotPtr snapshot = smallSnapshot();
    auto graph = std::make_shared<Graph>(buildGraph(snapshot, false));
    const QVector<QVector3D> positions = runLayout(*graph);
    SynapseView view;
    view.resize(800, 600);
    view.setAutoRotate(false);
    view.setGraph(graph, positions);
    view.setPositions(positions, 1.0);
    view.fit(false);
    check(!view.gpuRendering(), QStringLiteral("offscreen view paints in software"));
    int exact = 0;
    bool covered = true;
    int onScreen = 0;
    for (const Node &node : graph->nodes) {
        const QPointF at = view.screenPosition(node.file);
        if (std::isnan(at.x()))
            continue;
        ++onScreen;
        const int hit = view.pick(at);
        if (hit == node.file) {
            ++exact;
            continue;
        }
        const QPointF other = hit >= 0 ? view.screenPosition(hit) : QPointF(1e9, 1e9);
        covered = covered && QLineF(other, at).length() < 40;
    }
    check(onScreen == graph->nodes.size(), QStringLiteral("fit shows every node (%1)").arg(onScreen));
    check(exact >= onScreen - 1 && covered,
          QStringLiteral("picking finds the node under the cursor or the one in front of it (%1 of %2)").arg(exact).arg(onScreen));
    check(view.pick(QPointF(2, 2)) == -1, QStringLiteral("picking empty space finds nothing"));
    const QImage image = view.renderSoftware(QSize(640, 400));
    check(distinctColours(image) > 300, QStringLiteral("software render is not blank (%1 colours)").arg(distinctColours(image)));
    const int main = snapshot->indexOf(QStringLiteral("app/main.cpp"));
    view.selectFile(main, false);
    view.setShowMode(ShowMode::Neighbourhood);
    check(view.visibleNodeCount() == 4, QStringLiteral("neighbourhood shows the file and its neighbours (%1)").arg(view.visibleNodeCount()));
    view.setDepth(3);
    check(view.visibleNodeCount() == 7, QStringLiteral("deeper neighbourhood reaches further (%1)").arg(view.visibleNodeCount()));
    view.clearSelection();
    view.setShowMode(ShowMode::Cycles);
    check(view.visibleNodeCount() == 3, QStringLiteral("cycle mode shows only the loop"));
    view.setShowMode(ShowMode::Everything);
    view.setIsolatedCycle(0);
    check(view.visibleNodeCount() == 3, QStringLiteral("isolating a cycle shows its files"));
    view.setShowMode(ShowMode::Everything);
    view.setSearchQuery(QStringLiteral("util/"));
    const QImage searched = view.renderSoftware(QSize(640, 400));
    check(distinctColours(searched) > 200, QStringLiteral("search highlight renders"));
    for (ColourBy mode : {ColourBy::Language, ColourBy::Health, ColourBy::Hotspot, ColourBy::Module}) {
        view.setColourBy(mode);
        check(distinctColours(view.renderSoftware(QSize(320, 240))) > 100,
              QStringLiteral("colour mode %1 renders").arg(int(mode)));
    }
    SynapseView empty;
    empty.resize(400, 300);
    check(distinctColours(empty.renderSoftware(QSize(400, 300))) > 20, QStringLiteral("empty state renders a message"));
}

void testFeature()
{
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("synapse");
    SynapseFeature feature(host);
    feature.resize(1280, 800);
    feature.show();
    const SnapshotPtr snapshot = generatedSnapshot(5, 30, 3);
    feature.showSnapshot(snapshot);
    check(pump([&] { return !feature.layoutRunning() && feature.view()->settled(); }, 60000),
          QStringLiteral("layout finishes on a worker"));
    check(feature.summary().contains(QStringLiteral("imports")), QStringLiteral("summary line set: %1").arg(feature.summary()));
    const QList<forkmesh::market::NativePackageSearchHit> hits = feature.searchHits(QStringLiteral("mod2/"), 5);
    check(hits.size() == 5, QStringLiteral("search hits come from the snapshot"));
    feature.activateSearchHit(hits.first().id);
    check(feature.view()->selectedFile() == snapshot->indexOf(hits.first().id),
          QStringLiteral("activating a hit selects the file"));
    feature.setHostSearchQuery(QStringLiteral("mod1"));
    feature.setShowIndex(2);
    check(pump([&] { return !feature.layoutRunning(); }, 60000), QStringLiteral("orphan layout finishes"));
    feature.setShowIndex(0);
    pump([&] { return !feature.layoutRunning(); }, 60000);
    feature.showcase();
    QCoreApplication::processEvents();
    const QImage page = render(feature);
    check(distinctColours(page) > 400, QStringLiteral("page renders offscreen (%1 colours)").arg(distinctColours(page)));
    feature.showSnapshot(smallSnapshot());
    check(pump([&] { return !feature.layoutRunning() && feature.view()->settled(); }, 60000),
          QStringLiteral("a new snapshot replaces the old one"));
    feature.isolateCycle(0);
    check(feature.view()->isolatedCycle() == 0, QStringLiteral("cycle list isolates the loop"));
    QCoreApplication::processEvents();
    check(distinctColours(render(feature)) > 200, QStringLiteral("cycle view renders"));
}

int preview(const QString &root, const QString &out)
{
    qApp->setPalette(Theme::applicationPalette(true));
    qApp->setStyleSheet(Theme::styleSheetForPalette(true, {}));
    SynapseFeature feature(hostFor(QDir::cleanPath(root)));
    feature.resize(1600, 1000);
    feature.show();
    feature.refresh();
    const bool ready = pump([&] {
        return feature.snapshot() && !feature.layoutRunning() && feature.view()->settled();
    }, 240000);
    if (!ready && !feature.snapshot()) {
        qCritical("preview: the snapshot did not arrive");
        return 1;
    }
    pump([&] { return !feature.layoutRunning() && feature.view()->settled(); }, 240000);
    feature.showcase();
    const QString view = qEnvironmentVariable("FORKMESH_INSIGHT_PREVIEW_VIEW").toLower();
    if (view == QLatin1String("language") || view == QLatin1String("health") || view == QLatin1String("hotspot")) {
        const ColourBy mode = view == QLatin1String("language") ? ColourBy::Language
                              : view == QLatin1String("health") ? ColourBy::Health
                                                                : ColourBy::Hotspot;
        feature.view()->setColourBy(mode);
    } else if (view == QLatin1String("cycles")) {
        feature.isolateCycle(0);
    } else if (view == QLatin1String("overview")) {
        feature.selectFile(-1, false);
    }
    if (qEnvironmentVariableIsSet("FORKMESH_SYNAPSE_PREVIEW_YAW")) {
        Camera camera = feature.view()->camera();
        camera.yaw = qEnvironmentVariable("FORKMESH_SYNAPSE_PREVIEW_YAW").toFloat();
        camera.pitch = qEnvironmentVariable("FORKMESH_SYNAPSE_PREVIEW_PITCH", QStringLiteral("0.34")).toFloat();
        feature.view()->setCamera(camera);
        feature.view()->fit(false);
    }
    pump([] { return false; }, 400);
    QElapsedTimer frameTimer;
    frameTimer.start();
    for (int i = 0; i < 5; ++i)
        feature.view()->renderSoftware(feature.view()->size());
    qInfo("preview: software frame %lld ms", frameTimer.elapsed() / 5);
    const QImage image = render(feature);
    if (!image.save(out)) {
        qCritical("preview: could not write %s", qPrintable(out));
        return 1;
    }
    qInfo("preview: wrote %s (%s, %d nodes, %d edges drawn)", qPrintable(out),
          feature.view()->gpuRendering() ? "OpenGL" : "software",
          feature.view()->graph() ? int(feature.view()->graph()->nodes.size()) : 0,
          feature.view()->drawnEdgeCount());
    return 0;
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QString root = qEnvironmentVariable("FORKMESH_INSIGHT_PREVIEW_ROOT");
    const QString out = qEnvironmentVariable("FORKMESH_INSIGHT_PREVIEW_OUT");
    if (!root.isEmpty() && !out.isEmpty())
        return preview(root, out);
    testQueries();
    testLayout();
    testView();
    testFeature();
    if (failures == 0)
        qInfo("All Synapse tests passed");
    return failures == 0 ? 0 : 1;
}
