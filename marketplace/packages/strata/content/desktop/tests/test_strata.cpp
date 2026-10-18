#include "StrataFeature.h"
#include "StrataModel.h"
#include "StrataViews.h"
#include "Theme.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

#include <cmath>

using namespace forkmesh::strata;
using forkmesh::insight::FileInfo;
using forkmesh::insight::FunctionInfo;
using forkmesh::insight::ModuleEdge;
using forkmesh::insight::ModuleGraph;
using forkmesh::insight::ModuleNode;
using forkmesh::insight::Snapshot;

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

FileInfo makeFile(const QString &path, int codeLines)
{
    FileInfo file;
    file.path = path;
    file.language = QStringLiteral("C++");
    file.codeLines = codeLines;
    file.lines = codeLines + 10;
    file.commentLines = codeLines / 5;
    file.health = 9;
    return file;
}

FunctionInfo makeFunction(const QString &name, int line, int length, int complexity, int nesting = 1, int parameters = 1)
{
    FunctionInfo fn;
    fn.name = name;
    fn.line = line;
    fn.length = length;
    fn.complexity = complexity;
    fn.nesting = nesting;
    fn.parameters = parameters;
    return fn;
}

void link(Snapshot &snapshot, int from, int to)
{
    snapshot.files[from].dependsOn.append(to);
    snapshot.files[to].dependedOnBy.append(from);
}

void index(Snapshot &snapshot)
{
    snapshot.byPath.clear();
    for (int i = 0; i < snapshot.files.size(); ++i)
        snapshot.byPath.insert(snapshot.files[i].path, i);
}

ModuleGraph graphFor(const Snapshot &snapshot, const QStringList &modules)
{
    ModuleGraph graph;
    graph.depth = 1;
    graph.moduleOfFile.fill(-1, snapshot.files.size());
    for (const QString &name : modules) {
        ModuleNode node;
        node.path = name;
        node.name = name;
        graph.nodes.append(node);
    }
    for (int f = 0; f < snapshot.files.size(); ++f) {
        const QString top = snapshot.files[f].path.section(QLatin1Char('/'), 0, 0);
        const int m = modules.indexOf(top);
        if (m < 0)
            continue;
        graph.moduleOfFile[f] = m;
        ModuleNode &node = graph.nodes[m];
        node.files.append(f);
        node.codeLines += snapshot.files[f].codeLines;
        node.complexity += snapshot.files[f].complexity;
        node.functions += snapshot.files[f].functions.size();
        node.types += snapshot.files[f].types;
        node.abstractTypes += snapshot.files[f].abstractTypes;
    }
    QHash<qint64, int> edgeOf;
    for (int f = 0; f < snapshot.files.size(); ++f) {
        for (int d : snapshot.files[f].dependsOn) {
            const int a = graph.moduleOfFile[f];
            const int b = graph.moduleOfFile[d];
            if (a < 0 || b < 0)
                continue;
            if (a == b) {
                graph.nodes[a].internalEdges += 1;
                continue;
            }
            const qint64 key = qint64(a) * 1000 + b;
            if (!edgeOf.contains(key)) {
                edgeOf.insert(key, graph.edges.size());
                ModuleEdge edge;
                edge.from = a;
                edge.to = b;
                graph.edges.append(edge);
            }
            ModuleEdge &edge = graph.edges[edgeOf.value(key)];
            edge.weight += 1;
            edge.fileEdges.append(QPair<int, int>(f, d));
        }
    }
    QVector<QSet<int>> in(graph.nodes.size());
    QVector<QSet<int>> out(graph.nodes.size());
    for (const ModuleEdge &edge : graph.edges) {
        out[edge.from].insert(edge.to);
        in[edge.to].insert(edge.from);
    }
    for (int i = 0; i < graph.nodes.size(); ++i) {
        ModuleNode &node = graph.nodes[i];
        node.afferent = in[i].size();
        node.efferent = out[i].size();
        const int sum = node.afferent + node.efferent;
        node.instability = sum ? double(node.efferent) / sum : 0;
        node.abstractness = node.types ? double(node.abstractTypes) / node.types : 0;
        node.distance = std::abs(node.abstractness + node.instability - 1.0);
        node.health = 9;
    }
    return graph;
}

int ruleCount(const Report &report, const QString &id)
{
    const int r = report.ruleIndex(id);
    return r < 0 ? -1 : report.rules[r].violations.size();
}

const Violation *firstViolation(const Report &report, const QString &id)
{
    const int r = report.ruleIndex(id);
    if (r < 0 || report.rules[r].violations.isEmpty())
        return nullptr;
    return &report.rules[r].violations.first();
}

const GateResult *gate(const Report &report, const QString &name)
{
    for (const GateResult &g : report.gates) {
        if (g.name == name)
            return &g;
    }
    return nullptr;
}

Snapshot richSnapshot()
{
    Snapshot snapshot;
    snapshot.root = QStringLiteral("/tmp/strata-synthetic");
    snapshot.name = QStringLiteral("synthetic");
    snapshot.authors = {QStringLiteral("Author One"), QStringLiteral("Author Two")};
    snapshot.historyDays = 365;
    snapshot.files.append(makeFile(QStringLiteral("app/main.cpp"), 300));
    snapshot.files.append(makeFile(QStringLiteral("app/window.cpp"), 400));
    snapshot.files.append(makeFile(QStringLiteral("core/engine.cpp"), 2500));
    snapshot.files.append(makeFile(QStringLiteral("core/model.cpp"), 200));
    snapshot.files.append(makeFile(QStringLiteral("util/strings.cpp"), 400));
    snapshot.files.append(makeFile(QStringLiteral("api/interfaces.h"), 120));
    snapshot.files[0].functions = {makeFunction(QStringLiteral("main"), 10, 30, 3)};
    snapshot.files[2].functions = {makeFunction(QStringLiteral("Engine::run"), 40, 140, 26, 6, 8),
                                   makeFunction(QStringLiteral("Engine::step"), 300, 20, 12)};
    snapshot.files[2].complexity = 60;
    snapshot.files[2].maxComplexity = 26;
    snapshot.files[2].duplicatedLines = 120;
    snapshot.files[2].commentLines = 2;
    snapshot.files[2].commits = 40;
    snapshot.files[2].mainAuthor = QStringLiteral("Author One");
    snapshot.files[2].mainAuthorShare = 0.96;
    snapshot.files[2].authors = 2;
    snapshot.files[1].commits = 12;
    snapshot.files[1].formerAuthorShare = 0.9;
    snapshot.files[1].hotspot = 0.8;
    snapshot.files[4].types = 5;
    snapshot.files[5].types = 10;
    snapshot.files[5].abstractTypes = 9;
    for (FileInfo &file : snapshot.files) {
        if (file.maxComplexity == 0) {
            for (const FunctionInfo &fn : file.functions)
                file.maxComplexity = std::max(file.maxComplexity, fn.complexity);
        }
    }
    link(snapshot, 0, 1);
    link(snapshot, 1, 0);
    link(snapshot, 0, 2);
    link(snapshot, 1, 3);
    link(snapshot, 2, 1);
    link(snapshot, 2, 4);
    link(snapshot, 3, 4);
    link(snapshot, 5, 3);
    link(snapshot, 0, 4);
    index(snapshot);
    return snapshot;
}

void testFormatting()
{
    check(formatDebt(0) == QStringLiteral("0 min"), QStringLiteral("formatDebt 0 min"));
    check(formatDebt(45) == QStringLiteral("45 min"), QStringLiteral("formatDebt minutes"));
    check(formatDebt(200) == QStringLiteral("3 h 20 min"), QStringLiteral("formatDebt hours and minutes"));
    check(formatDebt(6 * 8 * 60 + 3 * 60) == QStringLiteral("6 d 3 h"), QStringLiteral("formatDebt uses 8 hour days"));
    check(formatDebt(16 * 60) == QStringLiteral("2 d"), QStringLiteral("formatDebt whole days"));
    check(std::abs(effortMinutes(1000) - 18 * 8 * 60) < 1e-9, QStringLiteral("18 man-days per 1000 lines of code"));
    check(ratingFor(0.0) == 0 && ratingFor(0.05) == 0, QStringLiteral("rating A up to 5 %"));
    check(ratingFor(0.051) == 1 && ratingFor(0.10) == 1, QStringLiteral("rating B up to 10 %"));
    check(ratingFor(0.2) == 2, QStringLiteral("rating C up to 20 %"));
    check(ratingFor(0.5) == 3, QStringLiteral("rating D up to 50 %"));
    check(ratingFor(0.51) == 4, QStringLiteral("rating E above 50 %"));
    check(ratingLetter(0) == QLatin1Char('A') && ratingLetter(4) == QLatin1Char('E'), QStringLiteral("rating letters"));
    check(ruleCatalog().size() >= 17, QStringLiteral("catalog has the default rules"));
}

void testRules()
{
    Snapshot snapshot = richSnapshot();
    ModuleGraph graph = graphFor(snapshot, {QStringLiteral("app"), QStringLiteral("core"), QStringLiteral("util"), QStringLiteral("api")});
    const Report report = buildReport(snapshot, graph);
    check(report.graph.cycles.size() == 1, QStringLiteral("normalizeGraph finds the app/core module cycle"));
    check(ruleCount(report, QStringLiteral("ARC01")) == 2, QStringLiteral("module cycle rule fires once per cycle edge"));
    const Violation *cycle = firstViolation(report, QStringLiteral("ARC01"));
    check(cycle && cycle->severity == Severity::Critical, QStringLiteral("module cycle is critical"));
    check(cycle && cycle->debt >= 30, QStringLiteral("module cycle carries debt"));
    check(ruleCount(report, QStringLiteral("ARC02")) == 3, QStringLiteral("file import cycle rule fires for each file in the cycle"));
    check(ruleCount(report, QStringLiteral("ARC04")) == 1, QStringLiteral("zone of pain fires for the concrete stable util module"));
    check(ruleCount(report, QStringLiteral("ARC05")) == 1, QStringLiteral("zone of uselessness fires for the abstract unused api module"));
    const Violation *complex = firstViolation(report, QStringLiteral("CS01"));
    check(ruleCount(report, QStringLiteral("CS01")) == 2, QStringLiteral("complex functions above 10 found"));
    check(complex && complex->severity == Severity::High && complex->line == 40, QStringLiteral("complexity above 20 is high, with the line"));
    check(ruleCount(report, QStringLiteral("CS02")) == 1, QStringLiteral("long function found"));
    check(ruleCount(report, QStringLiteral("CS03")) == 1, QStringLiteral("deep nesting found"));
    check(ruleCount(report, QStringLiteral("CS04")) == 1, QStringLiteral("too many parameters found"));
    check(ruleCount(report, QStringLiteral("CS05")) == 1, QStringLiteral("brain method found"));
    const Violation *large = firstViolation(report, QStringLiteral("MNT01"));
    check(large && large->severity == Severity::High, QStringLiteral("file above 2000 lines is a high large-file violation"));
    check(ruleCount(report, QStringLiteral("MNT02")) == 1, QStringLiteral("duplicated code found"));
    check(ruleCount(report, QStringLiteral("MNT03")) == 1, QStringLiteral("complex file without comments found"));
    check(ruleCount(report, QStringLiteral("MNT04")) == 1, QStringLiteral("knowledge island found"));
    check(ruleCount(report, QStringLiteral("MNT05")) == 1, QStringLiteral("former contributor hotspot found"));

    Snapshot scaled;
    scaled.files.append(makeFile(QStringLiteral("a/x.cpp"), 100));
    scaled.files.append(makeFile(QStringLiteral("a/y.cpp"), 100));
    scaled.files[0].functions = {makeFunction(QStringLiteral("f"), 1, 10, 12)};
    scaled.files[1].functions = {makeFunction(QStringLiteral("g"), 1, 10, 40)};
    index(scaled);
    const Report scaledReport = buildReport(scaled, graphFor(scaled, {QStringLiteral("a")}));
    const int r = scaledReport.ruleIndex(QStringLiteral("CS01"));
    double small = 0;
    double big = 0;
    for (const Violation &v : scaledReport.rules[r].violations)
        (v.file == 0 ? small : big) = v.debt;
    check(big > small && small > 0, QStringLiteral("debt grows with how far over the threshold"));

    Snapshot sdp;
    for (const QString &name : {QStringLiteral("s"), QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("v"),
                                QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")})
        sdp.files.append(makeFile(name + QStringLiteral("/one.cpp"), 100));
    link(sdp, 1, 0);
    link(sdp, 2, 0);
    link(sdp, 3, 0);
    link(sdp, 0, 4);
    link(sdp, 4, 5);
    link(sdp, 4, 6);
    link(sdp, 4, 7);
    index(sdp);
    const Report sdpReport = buildReport(sdp, graphFor(sdp, {QStringLiteral("s"), QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"),
                                                             QStringLiteral("v"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")}));
    const Violation *sdpViolation = firstViolation(sdpReport, QStringLiteral("ARC03"));
    check(ruleCount(sdpReport, QStringLiteral("ARC03")) == 1 && sdpViolation && sdpViolation->module == 0 && sdpViolation->peerModule == 4,
          QStringLiteral("stable module depending on a volatile one breaks the stable dependencies principle"));

    Snapshot hub;
    for (int i = 0; i < 25; ++i)
        hub.files.append(makeFile(QStringLiteral("h/f%1.cpp").arg(i), 50));
    for (int i = 1; i <= 12; ++i)
        link(hub, 0, i);
    for (int i = 13; i < 25; ++i)
        link(hub, i, 0);
    index(hub);
    const Report hubReport = buildReport(hub, graphFor(hub, {QStringLiteral("h")}));
    check(ruleCount(hubReport, QStringLiteral("DES01")) == 1, QStringLiteral("hub file found"));
}

void testDebtAndGates()
{
    Snapshot snapshot = richSnapshot();
    const ModuleGraph graph = graphFor(snapshot, {QStringLiteral("app"), QStringLiteral("core"), QStringLiteral("util"), QStringLiteral("api")});
    const Report report = buildReport(snapshot, graph);
    double sum = 0;
    for (const Rule &rule : report.rules) {
        for (const Violation &v : rule.violations)
            sum += v.debt;
    }
    check(std::abs(report.total.debt - sum) < 1e-6, QStringLiteral("total debt is the sum of violation debts"));
    double moduleSum = 0;
    for (const Score &score : report.modules)
        moduleSum += score.debt;
    check(std::abs(moduleSum - sum) < 1e-6, QStringLiteral("module debts add up to the total"));
    const int codeLines = 300 + 400 + 2500 + 200 + 400 + 120;
    check(report.metrics.codeLines == codeLines, QStringLiteral("metrics count lines of code"));
    check(std::abs(report.total.ratio - sum / effortMinutes(codeLines)) < 1e-9, QStringLiteral("debt ratio is debt over effort"));
    check(report.total.rating == ratingFor(report.total.ratio), QStringLiteral("rating follows the ratio"));
    check(report.total.interest > 0, QStringLiteral("annual interest with history"));
    check(report.files[2].interest <= 2 * report.files[2].debt, QStringLiteral("interest is capped"));
    check(report.bySeverity[0] == 2, QStringLiteral("critical count matches the cycle edges"));
    const GateResult *cycles = gate(report, QStringLiteral("No module dependency cycle"));
    check(cycles && cycles->status == GateStatus::Fail, QStringLiteral("cycle gate fails"));
    const GateResult *critical = gate(report, QStringLiteral("No critical rule violated"));
    check(critical && critical->status == GateStatus::Fail, QStringLiteral("critical gate fails"));
    const GateResult *complexity = gate(report, QStringLiteral("No function above complexity 50"));
    check(complexity && complexity->status == GateStatus::Pass, QStringLiteral("complexity 26 passes the 50 gate"));
    check(summaryLine(report).startsWith(QStringLiteral("Rating ")) && summaryLine(report).contains(QStringLiteral("failing")),
          QStringLiteral("summary line names rating and failing gates"));

    Snapshot clean;
    clean.files.append(makeFile(QStringLiteral("a/x.cpp"), 500));
    clean.files.append(makeFile(QStringLiteral("b/y.cpp"), 500));
    clean.files[0].functions = {makeFunction(QStringLiteral("f"), 1, 10, 3)};
    link(clean, 0, 1);
    index(clean);
    const Report cleanReport = buildReport(clean, graphFor(clean, {QStringLiteral("a"), QStringLiteral("b")}));
    check(cleanReport.total.debt == 0 && cleanReport.total.rating == 0, QStringLiteral("clean code is rated A with no debt"));
    check(cleanReport.gatesWith(GateStatus::Fail) == 0 && cleanReport.gatesWith(GateStatus::Warn) == 0, QStringLiteral("clean code passes every gate"));
    check(cleanReport.total.interest == 0, QStringLiteral("no interest without history"));
    check(summaryLine(cleanReport).contains(QStringLiteral("all gates pass")), QStringLiteral("summary says all gates pass"));

    Snapshot bad;
    bad.files.append(makeFile(QStringLiteral("a/x.cpp"), 40));
    bad.files[0].functions = {makeFunction(QStringLiteral("monster"), 1, 400, 70, 9, 12)};
    bad.files[0].commits = 20;
    index(bad);
    const Report badReport = buildReport(bad, graphFor(bad, {QStringLiteral("a")}));
    check(badReport.total.rating == 4, QStringLiteral("tiny file with a monster function is rated E"));
    const GateResult *rating = gate(badReport, QStringLiteral("Technical debt rating"));
    check(rating && rating->status == GateStatus::Fail, QStringLiteral("rating E fails the gate"));
    const GateResult *monster = gate(badReport, QStringLiteral("No function above complexity 50"));
    check(monster && monster->status == GateStatus::Fail, QStringLiteral("complexity 70 fails the gate"));
    const GateResult *hot = gate(badReport, QStringLiteral("Debt in the most-changed files"));
    check(hot && hot->status == GateStatus::Fail, QStringLiteral("debt in the most-changed files fails"));
}

bool overlapping(const GraphLayout &layout)
{
    for (int i = 0; i < layout.boxes.size(); ++i) {
        for (int j = i + 1; j < layout.boxes.size(); ++j) {
            if (layout.boxes[i].adjusted(1, 1, -1, -1).intersects(layout.boxes[j].adjusted(1, 1, -1, -1)))
                return true;
        }
    }
    return false;
}

void testLayout()
{
    LayoutInput input;
    const int n = 30;
    for (int i = 0; i < n; ++i)
        input.sizes.append(QSizeF(100 + (i % 4) * 30, 70 + (i % 3) * 20));
    for (int i = 0; i < n; ++i) {
        for (int k = 1; k <= 3; ++k) {
            const int j = i + k * 3 + (i % 5);
            if (j < n)
                input.links.append({i, j, 1 + (i + j) % 7, false});
        }
    }
    const GraphLayout layout = layeredLayout(input);
    check(!overlapping(layout), QStringLiteral("layered layout has no overlapping boxes"));
    bool downward = true;
    for (const LayoutLink &l : input.links) {
        if (layout.boxes[l.from].bottom() >= layout.boxes[l.to].top())
            downward = false;
    }
    check(downward, QStringLiteral("every edge points down from dependent to dependency"));
    check(layout.edges.size() == input.links.size(), QStringLiteral("every link is routed"));
    bool routed = true;
    for (const RoutedEdge &edge : layout.edges) {
        if (edge.points.size() < 2 || QLineF(edge.points.first(), QPointF(layout.boxes[edge.from].center().x(), layout.boxes[edge.from].bottom())).length() > layout.boxes[edge.from].width())
            routed = false;
    }
    check(routed, QStringLiteral("routes start at their source box"));
    const GraphLayout again = layeredLayout(input);
    check(again.boxes == layout.boxes, QStringLiteral("layout is deterministic"));
    check(layout.bounds.contains(layout.boxes.first()), QStringLiteral("bounds contain the boxes"));

    LayoutInput hinted;
    for (int i = 0; i < 6; ++i)
        hinted.sizes.append(QSizeF(120, 80));
    hinted.layers = {3, 2, 2, 1, 0, 0};
    hinted.links = {{0, 1, 1, false}, {0, 2, 1, false}, {1, 3, 1, false}, {2, 4, 1, false}, {3, 5, 1, false}};
    const GraphLayout layered = layeredLayout(hinted);
    bool respects = true;
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) {
            if (hinted.layers[i] > hinted.layers[j] && layered.boxes[i].top() >= layered.boxes[j].top())
                respects = false;
        }
    }
    check(respects, QStringLiteral("hinted layers are respected top to bottom"));
    check(layered.ranks[1] == layered.ranks[2], QStringLiteral("same layer shares a rank"));

    LayoutInput cyclic;
    for (int i = 0; i < 4; ++i)
        cyclic.sizes.append(QSizeF(120, 80));
    cyclic.links = {{0, 1, 2, true}, {1, 2, 1, true}, {2, 0, 3, true}, {2, 3, 1, false}};
    const GraphLayout cycleLayout = layeredLayout(cyclic);
    int upward = 0;
    for (const RoutedEdge &edge : cycleLayout.edges) {
        if (edge.points.size() >= 2 && edge.points.last().y() < edge.points.first().y())
            ++upward;
    }
    check(upward == 1, QStringLiteral("a cycle is broken at its lightest edge, drawn upward"));
    check(cycleLayout.ranks[3] > std::max({cycleLayout.ranks[0], cycleLayout.ranks[1], cycleLayout.ranks[2]}),
          QStringLiteral("dependency of the cycle sits below every member"));
    check(!overlapping(cycleLayout), QStringLiteral("cycle layout has no overlaps"));

    LayoutInput wide;
    wide.sizes.append(QSizeF(140, 80));
    for (int i = 1; i <= 40; ++i) {
        wide.sizes.append(QSizeF(120, 70));
        wide.links.append({0, i, 1, false});
    }
    const GraphLayout wrapped = layeredLayout(wide);
    check(wrapped.rankCount > 2, QStringLiteral("a very wide rank is split into several rows"));
    check(!overlapping(wrapped), QStringLiteral("wrapped rows do not overlap"));
    check(wrapped.bounds.width() < 40 * 160, QStringLiteral("wrapping keeps the graph from becoming a strip"));

    LayoutInput isolated;
    for (int i = 0; i < 5; ++i)
        isolated.sizes.append(QSizeF(100, 60));
    isolated.links = {{0, 1, 1, false}};
    const GraphLayout islands = layeredLayout(isolated);
    check(!overlapping(islands) && (islands.boxes[4].top() > islands.boxes[1].bottom() || islands.boxes[4].left() > islands.boxes[1].right()),
          QStringLiteral("isolated modules go below or beside the graph"));
}

void testMatrixAndTreemap()
{
    ModuleGraph graph;
    const QStringList names = {QStringLiteral("ui"), QStringLiteral("net"), QStringLiteral("core"), QStringLiteral("base"), QStringLiteral("io")};
    for (const QString &name : names) {
        ModuleNode node;
        node.name = name;
        node.path = name;
        graph.nodes.append(node);
    }
    const auto edge = [&graph](int a, int b) {
        ModuleEdge e;
        e.from = a;
        e.to = b;
        e.weight = 1;
        graph.edges.append(e);
    };
    edge(0, 1);
    edge(0, 2);
    edge(1, 4);
    edge(4, 1);
    edge(1, 2);
    edge(2, 3);
    edge(4, 3);
    normalizeGraph(graph);
    const QVector<int> order = matrixOrder(graph);
    check(order.size() == 5 && order.first() == 0 && order.last() == 3, QStringLiteral("matrix orders from top layer to foundation"));
    const int a = order.indexOf(1);
    const int b = order.indexOf(4);
    check(std::abs(a - b) == 1, QStringLiteral("cycle members are adjacent in the matrix"));
    const QVector<int> layers = moduleLayers(graph);
    check(layers[3] == 0 && layers[0] > layers[1], QStringLiteral("module layers computed from edges"));

    const QVector<double> weights = {50, 25, 12.5, 12.5, 0};
    const QRectF area(0, 0, 400, 200);
    const QVector<QRectF> rects = squarify(weights, area);
    bool proportional = true;
    bool inside = true;
    for (int i = 0; i < 4; ++i) {
        const double expected = weights[i] / 100.0 * area.width() * area.height();
        if (std::abs(rects[i].width() * rects[i].height() - expected) > 1.0)
            proportional = false;
        if (!area.adjusted(-0.01, -0.01, 0.01, 0.01).contains(rects[i]))
            inside = false;
    }
    check(proportional, QStringLiteral("treemap areas are proportional"));
    check(inside, QStringLiteral("treemap cells stay inside"));
    bool apart = true;
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            if (rects[i].adjusted(0.01, 0.01, -0.01, -0.01).intersects(rects[j].adjusted(0.01, 0.01, -0.01, -0.01)))
                apart = false;
        }
    }
    check(apart, QStringLiteral("treemap cells do not overlap"));
    check(rects[4].isEmpty(), QStringLiteral("zero weight gets no cell"));
}

void testDrillAndReport()
{
    Snapshot snapshot = richSnapshot();
    const ModuleGraph graph = graphFor(snapshot, {QStringLiteral("app"), QStringLiteral("core"), QStringLiteral("util"), QStringLiteral("api")});
    const Report report = buildReport(snapshot, graph);
    const DrillGraph drill = drillGraph(snapshot, report.graph, 1);
    int files = 0;
    int modules = 0;
    for (const DrillNode &node : drill.nodes)
        (node.module ? modules : files) += 1;
    check(files == 2, QStringLiteral("drill-down shows the module's files"));
    check(modules >= 2, QStringLiteral("drill-down collapses other modules"));
    check(!overlapping(drill.layout), QStringLiteral("drill-down layout has no overlaps"));

    const QString html = reportHtml(snapshot, report, QStringLiteral("synthetic"));
    check(html.contains(QStringLiteral("<h2 id=\"dashboard\">Dashboard</h2>")), QStringLiteral("report has the dashboard"));
    check(html.contains(QStringLiteral("Quality gates")) && html.contains(QStringLiteral("No module dependency cycle")),
          QStringLiteral("report has the quality gates"));
    check(html.contains(QStringLiteral("ARC01")) && html.contains(QStringLiteral("Avoid module dependency cycles")),
          QStringLiteral("report has the rules"));
    check(html.contains(QStringLiteral("<h2 id=\"modules\">Modules</h2>")) && html.contains(QStringLiteral("util")),
          QStringLiteral("report has the module table"));
    check(html.contains(QStringLiteral("core/engine.cpp:40")), QStringLiteral("report lists violations with lines"));
    check(!html.contains(QStringLiteral("<script")), QStringLiteral("report is self-contained without scripts"));
}

bool varied(const QImage &image)
{
    if (image.isNull())
        return false;
    QSet<QRgb> colors;
    for (int y = 0; y < image.height(); y += 7) {
        for (int x = 0; x < image.width(); x += 7) {
            colors.insert(image.pixel(x, y));
            if (colors.size() > 12)
                return true;
        }
    }
    return false;
}

void testViews()
{
    auto snapshot = std::make_shared<Snapshot>(richSnapshot());
    const ModuleGraph graph = graphFor(*snapshot, {QStringLiteral("app"), QStringLiteral("core"), QStringLiteral("util"), QStringLiteral("api")});
    auto model = std::make_shared<Model>();
    model->snapshot = snapshot;
    model->report = buildReport(*snapshot, graph);
    model->layout = moduleLayout(model->report.graph);
    model->order = matrixOrder(model->report.graph);

    QList<Selection> selected;
    ViewCallbacks callbacks;
    callbacks.select = [&selected](const Selection &s) { selected.append(s); };
    GraphView graphView;
    graphView.setCallbacks(callbacks);
    graphView.resize(900, 600);
    graphView.setModel(model);
    graphView.setSelection({SelectionKind::Module, 1});
    check(varied(graphView.grab().toImage()), QStringLiteral("graph view renders"));
    graphView.drillInto(1);
    check(graphView.drilledModule() == 1 && varied(graphView.grab().toImage()), QStringLiteral("graph drill-down renders"));
    graphView.drillOut();
    check(graphView.drilledModule() == -1, QStringLiteral("drill out returns to modules"));
    for (int c = 0; c < 4; ++c) {
        graphView.setColorBy(GraphView::ColorBy(c));
        graphView.grab();
    }
    MatrixView matrix;
    matrix.resize(700, 600);
    matrix.setModel(model);
    check(varied(matrix.grab().toImage()), QStringLiteral("matrix renders"));
    ScatterView scatter;
    scatter.resize(800, 600);
    scatter.setModel(model);
    scatter.setSelection({SelectionKind::Module, 2});
    check(varied(scatter.grab().toImage()), QStringLiteral("abstractness × instability renders"));
    MetricsView metrics;
    metrics.resize(800, 600);
    metrics.setModel(model);
    for (int c = 0; c < 4; ++c)
        metrics.setColorBy(MetricsView::ColorBy(c));
    metrics.setSizeBy(MetricsView::SizeBy::Complexity);
    check(varied(metrics.grab().toImage()), QStringLiteral("metrics treemap renders"));
    DashboardView dashboard;
    dashboard.resize(1200, 800);
    dashboard.setModel(model);
    check(varied(dashboard.grab().toImage()), QStringLiteral("dashboard renders"));
    RulesView rules;
    rules.resize(800, 600);
    rules.setModel(model);
    const int all = rules.visibleRules();
    rules.setFilter(QStringLiteral("engine.cpp"));
    const int filtered = rules.visibleRules();
    check(all >= 17 && filtered > 0 && filtered < all, QStringLiteral("rules filter narrows the tree"));
}


forkmesh::market::NativePackageHost hostFor(const QString &root)
{
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("strata");
    forkmesh::market::NativePackageRepository repository;
    repository.key = QStringLiteral("preview");
    repository.title = QFileInfo(root).fileName();
    repository.path = root;
    host.repositories = [repository] { return QList<forkmesh::market::NativePackageRepository>{repository}; };
    host.currentRepository = [repository] { return repository; };
    host.openRepositoryFile = [](const forkmesh::market::NativePackageRepository &, const QString &, int) {};
    return host;
}

bool waitFor(StrataFeature &feature, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        if (feature.settled())
            return true;
        QThread::msleep(10);
    }
    return false;
}

void pump(int ms)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
}

void writeFile(const QString &path, const QByteArray &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(text);
}

void testPage()
{
    QTemporaryDir dir;
    check(dir.isValid(), QStringLiteral("temporary repository created"));
    const QString root = dir.path();
    writeFile(root + QStringLiteral("/app/main.cpp"),
              "#include \"core/engine.h\"\n#include \"util/text.h\"\nint main(int argc, char **argv)\n{\n    if (argc > 1) {\n        return run(argv[1]);\n    }\n    return 0;\n}\n");
    writeFile(root + QStringLiteral("/core/engine.h"), "#pragma once\n#include \"util/text.h\"\nint run(const char *name);\n");
    writeFile(root + QStringLiteral("/core/engine.cpp"),
              "#include \"core/engine.h\"\n#include \"app/main.h\"\nint run(const char *name)\n{\n    int total = 0;\n    for (int i = 0; i < 10; ++i) {\n        if (i % 2 == 0 && name) {\n            total += i;\n        } else if (i % 3 == 0) {\n            total -= i;\n        }\n    }\n    return total;\n}\n");
    writeFile(root + QStringLiteral("/app/main.h"), "#pragma once\n#include \"core/engine.h\"\n");
    writeFile(root + QStringLiteral("/util/text.h"), "#pragma once\nint length(const char *text);\n");
    writeFile(root + QStringLiteral("/util/text.cpp"), "#include \"util/text.h\"\nint length(const char *text)\n{\n    int n = 0;\n    while (text && text[n])\n        ++n;\n    return n;\n}\n");

    StrataFeature feature(hostFor(root));
    feature.resize(1400, 900);
    feature.show();
    feature.refresh();
    const bool ready = waitFor(feature, 60000);
    check(ready && feature.model() && !feature.model()->snapshot->files.isEmpty(), QStringLiteral("page analyzes a repository with the engine"));
    if (!ready || !feature.model())
        return;
    check(!feature.model()->report.graph.nodes.isEmpty(), QStringLiteral("page builds modules"));
    for (int view = 0; view < StrataFeature::ViewCount; ++view) {
        feature.showView(view);
        pump(30);
        check(varied(feature.grab().toImage()), QStringLiteral("page renders view %1").arg(view));
    }
    const auto hits = feature.searchHits(QStringLiteral("engine"), 10);
    check(!hits.isEmpty(), QStringLiteral("search finds files"));
    if (!hits.isEmpty()) {
        feature.activateSearchHit(hits.last().id);
        check(feature.selection().kind != SelectionKind::None, QStringLiteral("activating a search hit selects it"));
    }
    feature.select({SelectionKind::Module, 0});
    check(feature.detailsHtml().contains(QStringLiteral("Instability")), QStringLiteral("details show module metrics"));
    feature.setHostSearchQuery(QStringLiteral("util"));
    feature.setHostSearchQuery(QString());
    const QString reportPath = dir.filePath(QStringLiteral("report.html"));
    check(feature.writeReport(reportPath) && QFileInfo(reportPath).size() > 1000, QStringLiteral("report export writes a file"));
    feature.setModuleDepth(2);
    check(waitFor(feature, 30000), QStringLiteral("changing module depth rebuilds the graph"));
}

int preview(const QString &root, const QString &out)
{
    QApplication::setPalette(Theme::applicationPalette(true));
    qApp->setStyleSheet(Theme::styleSheetForPalette(true, {}));
    StrataFeature feature(hostFor(root));
    feature.resize(1600, 1000);
    feature.show();
    feature.refresh();
    if (!waitFor(feature, 240000) || !feature.model()) {
        qCritical("preview: the analysis did not finish");
        return 2;
    }
    const QString depth = qEnvironmentVariable("FORKMESH_INSIGHT_PREVIEW_DEPTH");
    if (!depth.isEmpty()) {
        feature.setModuleDepth(depth.toInt());
        waitFor(feature, 60000);
    }
    const ModelPtr model = feature.model();
    const auto &graph = model->report.graph;
    int best = -1;
    int bestScore = -1;
    for (int m = 0; m < graph.nodes.size(); ++m) {
        int degree = 0;
        for (const ModuleEdge &edge : graph.edges) {
            if (edge.from == m || edge.to == m)
                ++degree;
        }
        const int score = (graph.nodes[m].cycle >= 0 ? 1000 : 0) + degree;
        if (score > bestScore) {
            bestScore = score;
            best = m;
        }
    }
    const QString view = qEnvironmentVariable("FORKMESH_INSIGHT_PREVIEW_VIEW").toLower();
    int index = StrataFeature::Graph;
    if (view == QLatin1String("dashboard"))
        index = StrataFeature::Dashboard;
    else if (view == QLatin1String("matrix"))
        index = StrataFeature::Matrix;
    else if (view == QLatin1String("scatter") || view == QLatin1String("ai") || view == QLatin1String("abstractness"))
        index = StrataFeature::Scatter;
    else if (view == QLatin1String("metrics"))
        index = StrataFeature::Metrics;
    else if (view == QLatin1String("rules"))
        index = StrataFeature::Rules;
    feature.showView(index);
    pump(100);
    if (view == QLatin1String("drill") && best >= 0)
        feature.graphView()->drillInto(best);
    if (best >= 0 && index != StrataFeature::Dashboard && index != StrataFeature::Rules)
        feature.select({SelectionKind::Module, best});
    if (index == StrataFeature::Matrix && graph.edges.size() > 0) {
        for (const ModuleEdge &edge : graph.edges) {
            if (edge.inCycle) {
                feature.showCell(edge.from, edge.to);
                break;
            }
        }
    }
    pump(900);
    const QImage image = feature.grab().toImage();
    if (!image.save(out)) {
        qCritical("preview: could not save %s", qPrintable(out));
        return 3;
    }
    qInfo("preview: saved %s (%d modules, %s)", qPrintable(out), int(graph.nodes.size()), qPrintable(summaryLine(model->report)));
    return 0;
}

QJsonValue pathOf(const Snapshot &snapshot, int file)
{
    return file >= 0 && file < snapshot.files.size() ? QJsonValue(snapshot.files[file].path) : QJsonValue();
}

QJsonValue moduleOf(const ModuleGraph &graph, int module)
{
    return module >= 0 && module < graph.nodes.size() ? QJsonValue(graph.nodes[module].path) : QJsonValue();
}

QJsonObject scoreJson(const Score &score)
{
    return QJsonObject{{QStringLiteral("debt"), score.debt},
                       {QStringLiteral("effort"), score.effort},
                       {QStringLiteral("ratio"), score.ratio},
                       {QStringLiteral("rating"), score.rating},
                       {QStringLiteral("interest"), score.interest},
                       {QStringLiteral("violations"), score.violations},
                       {QStringLiteral("worst"), score.worst}};
}

QJsonArray rectJson(const QRectF &rect)
{
    return QJsonArray{rect.x(), rect.y(), rect.width(), rect.height()};
}

QJsonObject layoutJson(const GraphLayout &layout)
{
    QJsonArray boxes;
    for (const QRectF &box : layout.boxes)
        boxes.append(rectJson(box));
    QJsonArray ranks;
    for (int rank : layout.ranks)
        ranks.append(rank);
    QJsonArray edges;
    for (const RoutedEdge &edge : layout.edges) {
        QJsonArray points;
        for (const QPointF &p : edge.points)
            points.append(QJsonArray{p.x(), p.y()});
        edges.append(QJsonObject{{QStringLiteral("from"), edge.from},
                                 {QStringLiteral("to"), edge.to},
                                 {QStringLiteral("weight"), edge.weight},
                                 {QStringLiteral("cycle"), edge.cycle},
                                 {QStringLiteral("flat"), edge.flat},
                                 {QStringLiteral("points"), points}});
    }
    return QJsonObject{{QStringLiteral("boxes"), boxes},
                       {QStringLiteral("ranks"), ranks},
                       {QStringLiteral("edges"), edges},
                       {QStringLiteral("bounds"), rectJson(layout.bounds)},
                       {QStringLiteral("crossings"), layout.crossings},
                       {QStringLiteral("rankCount"), layout.rankCount}};
}

QJsonObject treemapJson(const Snapshot &snapshot, const ModuleGraph &graph, int sizeBy, const QRectF &area)
{
    const auto sizeOf = [&snapshot, sizeBy](int f) -> double {
        const FileInfo &info = snapshot.files[f];
        if (sizeBy == 1)
            return info.codeLines > 0 ? std::max(1, info.complexity) : 0;
        if (sizeBy == 2)
            return info.codeLines > 0 ? std::max<int>(1, info.functions.size()) : 0;
        return info.codeLines;
    };
    QVector<double> moduleWeights(graph.nodes.size(), 0);
    for (int m = 0; m < graph.nodes.size(); ++m) {
        for (int f : graph.nodes[m].files)
            moduleWeights[m] += sizeOf(f);
    }
    const QVector<QRectF> moduleRects = squarify(moduleWeights, area);
    QVector<QRectF> fileRects(snapshot.files.size());
    for (int m = 0; m < graph.nodes.size(); ++m) {
        const QRectF box = moduleRects[m];
        if (box.width() < 2 || box.height() < 2)
            continue;
        const double header = box.height() > 44 && box.width() > 70 ? 17 : 0;
        const QRectF inner = box.adjusted(2, 2 + header, -2, -2);
        QVector<double> weights;
        for (int f : graph.nodes[m].files)
            weights.append(sizeOf(f));
        const QVector<QRectF> cells = squarify(weights, inner);
        for (int k = 0; k < cells.size(); ++k)
            fileRects[graph.nodes[m].files[k]] = cells[k];
    }
    QJsonArray modules;
    for (const QRectF &rect : moduleRects)
        modules.append(rectJson(rect));
    QJsonArray files;
    for (const QRectF &rect : fileRects)
        files.append(rectJson(rect));
    return QJsonObject{{QStringLiteral("modules"), modules}, {QStringLiteral("files"), files}};
}

QJsonObject modelJson(const Snapshot &snapshot, int depth)
{
    const ModelPtr model = buildModel(std::make_shared<Snapshot>(snapshot), depth);
    const Report &report = model->report;
    const ModuleGraph &graph = report.graph;
    QJsonArray cycles;
    for (const QVector<int> &cycle : graph.cycles) {
        QJsonArray members;
        for (int m : cycle)
            members.append(graph.nodes[m].path);
        cycles.append(members);
    }
    QJsonArray edges;
    for (const ModuleEdge &edge : graph.edges)
        edges.append(QJsonObject{{QStringLiteral("from"), graph.nodes[edge.from].path},
                                 {QStringLiteral("to"), graph.nodes[edge.to].path},
                                 {QStringLiteral("weight"), edge.weight},
                                 {QStringLiteral("inCycle"), edge.inCycle}});
    QJsonArray nodes;
    for (const ModuleNode &node : graph.nodes)
        nodes.append(QJsonObject{{QStringLiteral("path"), node.path},
                                 {QStringLiteral("cycle"), node.cycle},
                                 {QStringLiteral("afferent"), node.afferent},
                                 {QStringLiteral("efferent"), node.efferent},
                                 {QStringLiteral("instability"), node.instability},
                                 {QStringLiteral("abstractness"), node.abstractness},
                                 {QStringLiteral("distance"), node.distance}});
    QJsonArray rules;
    for (const Rule &rule : report.rules) {
        QJsonArray violations;
        for (const Violation &v : rule.violations)
            violations.append(QJsonObject{{QStringLiteral("file"), pathOf(snapshot, v.file)},
                                          {QStringLiteral("module"), moduleOf(graph, v.module)},
                                          {QStringLiteral("peerModule"), moduleOf(graph, v.peerModule)},
                                          {QStringLiteral("line"), v.line},
                                          {QStringLiteral("severity"), severityName(v.severity)},
                                          {QStringLiteral("message"), v.message},
                                          {QStringLiteral("debt"), v.debt}});
        rules.append(QJsonObject{{QStringLiteral("id"), rule.id},
                                 {QStringLiteral("title"), rule.title},
                                 {QStringLiteral("category"), categoryName(rule.category)},
                                 {QStringLiteral("severity"), severityName(rule.severity)},
                                 {QStringLiteral("moduleScope"), rule.moduleScope},
                                 {QStringLiteral("debt"), rule.debt},
                                 {QStringLiteral("worst"), rule.worst()},
                                 {QStringLiteral("violations"), violations}});
    }
    QJsonArray files;
    for (int f = 0; f < snapshot.files.size(); ++f) {
        QJsonObject score = scoreJson(report.files[f]);
        score.insert(QStringLiteral("path"), snapshot.files[f].path);
        QJsonArray refs;
        for (const ViolationRef &ref : report.fileViolations[f])
            refs.append(QJsonArray{report.rules[ref.first].id, ref.second});
        score.insert(QStringLiteral("refs"), refs);
        files.append(score);
    }
    QJsonArray modules;
    for (int m = 0; m < graph.nodes.size(); ++m) {
        QJsonObject score = scoreJson(report.modules[m]);
        score.insert(QStringLiteral("path"), graph.nodes[m].path);
        QJsonArray refs;
        for (const ViolationRef &ref : report.moduleViolations[m])
            refs.append(QJsonArray{report.rules[ref.first].id, ref.second});
        score.insert(QStringLiteral("refs"), refs);
        QJsonArray worst;
        for (const ViolationRef &ref : report.worstForModule(m, 6))
            worst.append(QJsonArray{report.rules[ref.first].id, ref.second});
        score.insert(QStringLiteral("worstRefs"), worst);
        modules.append(score);
    }
    QJsonArray gates;
    for (const GateResult &gate : report.gates)
        gates.append(QJsonObject{{QStringLiteral("name"), gate.name},
                                 {QStringLiteral("status"), gateStatusName(gate.status)},
                                 {QStringLiteral("measured"), gate.measured},
                                 {QStringLiteral("detail"), gate.detail}});
    const Metrics &m = report.metrics;
    const QJsonObject metrics{{QStringLiteral("codeLines"), m.codeLines},
                              {QStringLiteral("commentLines"), m.commentLines},
                              {QStringLiteral("files"), m.files},
                              {QStringLiteral("modules"), m.modules},
                              {QStringLiteral("types"), m.types},
                              {QStringLiteral("functions"), m.functions},
                              {QStringLiteral("averageComplexity"), m.averageComplexity},
                              {QStringLiteral("maxComplexity"), m.maxComplexity},
                              {QStringLiteral("maxComplexityFunction"), m.maxComplexityFunction},
                              {QStringLiteral("maxComplexityFile"), pathOf(snapshot, m.maxComplexityFile)},
                              {QStringLiteral("commentPercent"), m.commentPercent},
                              {QStringLiteral("duplicatedPercent"), m.duplicatedPercent},
                              {QStringLiteral("testFiles"), m.testFiles}};
    QJsonArray order;
    for (int i : model->order)
        order.append(graph.nodes[i].path);
    QJsonArray layers;
    for (int layer : moduleLayers(graph))
        layers.append(layer);
    QJsonArray drills;
    for (int module = 0; module < graph.nodes.size(); ++module) {
        const DrillGraph drill = drillGraph(snapshot, graph, module);
        QJsonArray drillNodes;
        for (const DrillNode &node : drill.nodes)
            drillNodes.append(QJsonObject{{QStringLiteral("module"), node.module},
                                          {QStringLiteral("dependent"), node.dependent},
                                          {QStringLiteral("name"), node.module ? moduleOf(graph, node.index) : pathOf(snapshot, node.index)},
                                          {QStringLiteral("weight"), node.weight}});
        QJsonArray links;
        for (const LayoutLink &link : drill.links)
            links.append(QJsonArray{link.from, link.to, link.weight, link.cycle});
        drills.append(QJsonObject{{QStringLiteral("module"), graph.nodes[module].path},
                                  {QStringLiteral("hiddenFiles"), drill.hiddenFiles},
                                  {QStringLiteral("nodes"), drillNodes},
                                  {QStringLiteral("links"), links},
                                  {QStringLiteral("layout"), layoutJson(drill.layout)}});
    }
    QJsonArray boxSizes;
    for (int lines : {0, 1, 100, 1000, 5000, 20000, 100000}) {
        const QSizeF size = moduleBoxSize(lines);
        boxSizes.append(QJsonArray{lines, size.width(), size.height()});
    }
    const QRectF area(14, 6, 972, 648);
    return QJsonObject{{QStringLiteral("depth"), graph.depth},
                       {QStringLiteral("summary"), summaryLine(report)},
                       {QStringLiteral("history"), report.history},
                       {QStringLiteral("total"), scoreJson(report.total)},
                       {QStringLiteral("metrics"), metrics},
                       {QStringLiteral("bySeverity"), QJsonArray{report.bySeverity[0], report.bySeverity[1], report.bySeverity[2], report.bySeverity[3]}},
                       {QStringLiteral("cycles"), cycles},
                       {QStringLiteral("nodes"), nodes},
                       {QStringLiteral("edges"), edges},
                       {QStringLiteral("rules"), rules},
                       {QStringLiteral("files"), files},
                       {QStringLiteral("modules"), modules},
                       {QStringLiteral("gates"), gates},
                       {QStringLiteral("order"), order},
                       {QStringLiteral("layers"), layers},
                       {QStringLiteral("layout"), layoutJson(model->layout)},
                       {QStringLiteral("drill"), drills},
                       {QStringLiteral("boxSizes"), boxSizes},
                       {QStringLiteral("treemapLines"), treemapJson(snapshot, graph, 0, area)},
                       {QStringLiteral("treemapComplexity"), treemapJson(snapshot, graph, 1, area)},
                       {QStringLiteral("treemapFunctions"), treemapJson(snapshot, graph, 2, area)},
                       {QStringLiteral("report"), reportHtml(snapshot, report, QStringLiteral("fixture <repo>"))}};
}

bool gitIn(const QString &dir, const QStringList &args)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.start(QStringLiteral("git"), args);
    return process.waitForFinished(60000) && process.exitCode() == 0;
}

int writeGolden(const QString &fixture)
{
    QTemporaryDir temp;
    const QString repo = temp.path() + QStringLiteral("/repo");
    QDirIterator it(fixture + QStringLiteral("/repo"), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString source = it.next();
        const QString target = repo + source.mid(fixture.size() + 5);
        QDir().mkpath(QFileInfo(target).absolutePath());
        QFile::copy(source, target);
    }
    qputenv("GIT_CEILING_DIRECTORIES", temp.path().toUtf8());
    qputenv("GIT_AUTHOR_NAME", "Fixture");
    qputenv("GIT_AUTHOR_EMAIL", "fixture@example.com");
    qputenv("GIT_COMMITTER_NAME", "Fixture");
    qputenv("GIT_COMMITTER_EMAIL", "fixture@example.com");
    if (!gitIn(repo, {QStringLiteral("init"), QStringLiteral("-q")}) || !gitIn(repo, {QStringLiteral("add"), QStringLiteral("-A")})
        || !gitIn(repo, {QStringLiteral("commit"), QStringLiteral("-q"), QStringLiteral("-m"), QStringLiteral("fixture")})) {
        std::fprintf(stderr, "git setup failed\n");
        return 1;
    }
    forkmesh::insight::Options options;
    options.history = false;
    const forkmesh::insight::SnapshotPtr base = forkmesh::insight::analyze(repo, options);
    if (!base || !base->error.isEmpty()) {
        std::fprintf(stderr, "analysis failed\n");
        return 1;
    }
    QFile logFile(fixture + QStringLiteral("/history.log"));
    logFile.open(QIODevice::ReadOnly);
    Snapshot history = *base;
    forkmesh::insight::applyHistory(history, logFile.readAll(), forkmesh::insight::Options(), 1792000000);
    for (FileInfo &file : history.files) {
        QStringList issues;
        file.health = forkmesh::insight::computeHealth(file, &issues);
        file.healthIssues = issues;
    }
    forkmesh::insight::computeHotspots(history);
    history.head = QStringLiteral("0000000000000000000000000000000000000000");
    history.branch = QStringLiteral("main");
    history.analyzedAt = 0;
    Snapshot plain = *base;
    plain.head = history.head;
    plain.branch = history.branch;
    plain.analyzedAt = 0;
    QJsonArray debts;
    for (double minutes : {0.0, 0.4, 0.5, 45.0, 59.6, 60.0, 200.0, 480.0, 660.0, 960.0, 2940.0, 48000.0, 480000.0, 4800000.0})
        debts.append(QJsonArray{minutes, formatDebt(minutes)});
    QJsonArray percents;
    for (double ratio : {0.0, 0.0004, 0.001, 0.0123, 0.05, 0.0995, 0.1, 0.12345, 0.5, 1.0, 12.3456})
        percents.append(QJsonArray{ratio, formatPercent(ratio)});
    LayoutInput mesh;
    for (int i = 0; i < 30; ++i)
        mesh.sizes.append(QSizeF(100 + (i % 4) * 30, 70 + (i % 3) * 20));
    for (int i = 0; i < 30; ++i) {
        for (int k = 1; k <= 3; ++k) {
            const int j = i + k * 3 + (i % 5);
            if (j < 30)
                mesh.links.append({i, j, 1 + (i + j) % 7, false});
        }
    }
    LayoutInput hinted;
    for (int i = 0; i < 6; ++i)
        hinted.sizes.append(QSizeF(120, 80));
    hinted.layers = {3, 2, 2, 1, 0, 0};
    hinted.links = {{0, 1, 1, false}, {0, 2, 1, false}, {1, 3, 1, false}, {2, 4, 1, false}, {3, 5, 1, false}};
    LayoutInput cyclic;
    for (int i = 0; i < 7; ++i)
        cyclic.sizes.append(QSizeF(120 + i * 7, 80));
    cyclic.links = {{0, 1, 2, true}, {1, 2, 1, true}, {2, 0, 3, true}, {2, 3, 1, false}, {3, 4, 2, true}, {4, 5, 1, true}, {5, 3, 4, true}, {4, 3, 1, true}, {0, 6, 1, false}};
    LayoutInput wide;
    wide.sizes.append(QSizeF(140, 80));
    for (int i = 1; i <= 40; ++i) {
        wide.sizes.append(QSizeF(120, 70));
        wide.links.append({0, i, 1, false});
    }
    LayoutInput isolated;
    for (int i = 0; i < 5; ++i)
        isolated.sizes.append(QSizeF(100, 60));
    isolated.links = {{0, 1, 1, false}};
    LayoutInput tall;
    for (int i = 0; i < 12; ++i)
        tall.sizes.append(QSizeF(110, 70));
    for (int i = 0; i + 1 < 8; ++i)
        tall.links.append({i, i + 1, 1, false});
    LayoutInput lonely;
    for (int i = 0; i < 9; ++i)
        lonely.sizes.append(QSizeF(90 + i * 10, 60));
    QJsonArray squares;
    for (const QRectF &rect : squarify({50, 25, 12.5, 12.5, 0, 3, 7.25, 1}, QRectF(3, 5, 400, 200)))
        squares.append(rectJson(rect));
    const QJsonObject layouts{{QStringLiteral("mesh"), layoutJson(layeredLayout(mesh))},
                              {QStringLiteral("hinted"), layoutJson(layeredLayout(hinted))},
                              {QStringLiteral("cyclic"), layoutJson(layeredLayout(cyclic))},
                              {QStringLiteral("wide"), layoutJson(layeredLayout(wide))},
                              {QStringLiteral("isolated"), layoutJson(layeredLayout(isolated))},
                              {QStringLiteral("tall"), layoutJson(layeredLayout(tall))},
                              {QStringLiteral("lonely"), layoutJson(layeredLayout(lonely))},
                              {QStringLiteral("squarify"), squares}};
    const QJsonObject root{{QStringLiteral("now"), 1792000000},
                           {QStringLiteral("formatDebt"), debts},
                           {QStringLiteral("formatPercent"), percents},
                           {QStringLiteral("layouts"), layouts},
                           {QStringLiteral("auto"), modelJson(history, 0)},
                           {QStringLiteral("depth2"), modelJson(history, 2)},
                           {QStringLiteral("plain"), modelJson(plain, 0)},
                           {QStringLiteral("synthetic"), modelJson(richSnapshot(), 1)}};
    const QByteArray json = QJsonDocument(root).toJson(QJsonDocument::Compact) + "\n";
    std::fwrite(json.constData(), 1, size_t(json.size()), stdout);
    return 0;
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QString golden = qEnvironmentVariable("FORKMESH_STRATA_GOLDEN");
    if (!golden.isEmpty())
        return writeGolden(QDir(golden).absolutePath());
    const QString root = qEnvironmentVariable("FORKMESH_INSIGHT_PREVIEW_ROOT");
    const QString out = qEnvironmentVariable("FORKMESH_INSIGHT_PREVIEW_OUT");
    if (!root.isEmpty() && !out.isEmpty())
        return preview(root, out);
    testFormatting();
    testRules();
    testDebtAndGates();
    testLayout();
    testMatrixAndTreemap();
    testDrillAndReport();
    testViews();
    testPage();
    if (failures)
        qCritical("%d check(s) failed", failures);
    else
        qInfo("All Strata checks passed");
    return failures ? 1 : 0;
}
