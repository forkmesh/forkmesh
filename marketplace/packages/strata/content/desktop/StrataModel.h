#pragma once

#include "RepoInsight.h"

#include <QList>
#include <QPair>
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QVector>

#include <memory>

namespace forkmesh::strata {

enum class Severity { Critical, High, Medium, Low };
enum class Category { Architecture, Design, CodeSmells, Maintainability };
enum class GateStatus { Pass, Warn, Fail };

struct Violation {
    int file = -1;
    int module = -1;
    int line = 0;
    Severity severity = Severity::Medium;
    QString message;
    double debt = 0;
    int peerModule = -1;
};

struct Rule {
    QString id;
    QString title;
    Category category = Category::CodeSmells;
    Severity severity = Severity::Medium;
    QString description;
    QString howToFix;
    QList<Violation> violations;
    double debt = 0;
    bool moduleScope = false;
    int worst() const;
};

struct GateResult {
    QString name;
    GateStatus status = GateStatus::Pass;
    QString measured;
    QString detail;
};

struct Score {
    double debt = 0;
    double effort = 0;
    double ratio = 0;
    int rating = 0;
    double interest = 0;
    int violations = 0;
    int worst = -1;
};

struct Metrics {
    int codeLines = 0;
    int commentLines = 0;
    int files = 0;
    int modules = 0;
    int types = 0;
    int functions = 0;
    double averageComplexity = 0;
    int maxComplexity = 0;
    QString maxComplexityFunction;
    int maxComplexityFile = -1;
    double commentPercent = 0;
    double duplicatedPercent = 0;
    int testFiles = 0;
};

using ViolationRef = QPair<int, int>;

struct Report {
    insight::ModuleGraph graph;
    QList<Rule> rules;
    QVector<Score> files;
    QVector<Score> modules;
    QVector<QVector<ViolationRef>> fileViolations;
    QVector<QVector<ViolationRef>> moduleViolations;
    Score total;
    QList<GateResult> gates;
    Metrics metrics;
    int bySeverity[4] = {0, 0, 0, 0};
    bool history = false;

    int ruleIndex(const QString &id) const;
    int gatesWith(GateStatus status) const;
    const Violation &violation(const ViolationRef &ref) const;
    QVector<ViolationRef> worstForModule(int module, int limit) const;
};

double effortMinutes(int codeLines);
int ratingFor(double ratio);
QChar ratingLetter(int rating);
QString severityName(Severity severity);
QString categoryName(Category category);
QString gateStatusName(GateStatus status);
QString formatDebt(double minutes);
QString formatPercent(double ratio);

QList<Rule> ruleCatalog();
QList<QVector<int>> components(const QList<QVector<int>> &adjacency);
void normalizeGraph(insight::ModuleGraph &graph);
QList<Rule> evaluateRules(const insight::Snapshot &snapshot, const insight::ModuleGraph &graph);
QList<GateResult> evaluateGates(const insight::Snapshot &snapshot, const Report &report);
Report buildReport(const insight::Snapshot &snapshot, const insight::ModuleGraph &graph);
QString summaryLine(const Report &report);

struct LayoutLink {
    int from = -1;
    int to = -1;
    int weight = 1;
    bool cycle = false;
};

struct LayoutInput {
    QVector<QSizeF> sizes;
    QVector<int> layers;
    QList<LayoutLink> links;
    double layerGap = 96;
    double nodeGap = 40;
    int maxPerRank = 0;
};

struct RoutedEdge {
    int from = -1;
    int to = -1;
    int weight = 1;
    bool cycle = false;
    bool flat = false;
    QVector<QPointF> points;
};

struct GraphLayout {
    QVector<QRectF> boxes;
    QVector<int> ranks;
    QList<RoutedEdge> edges;
    QRectF bounds;
    int crossings = 0;
    int rankCount = 0;
};

GraphLayout layeredLayout(const LayoutInput &input);
QSizeF moduleBoxSize(int codeLines);
QVector<int> moduleLayers(const insight::ModuleGraph &graph);
GraphLayout moduleLayout(const insight::ModuleGraph &graph);

struct DrillNode {
    bool module = false;
    bool dependent = false;
    int index = -1;
    int weight = 0;
};

struct DrillGraph {
    int module = -1;
    QVector<DrillNode> nodes;
    QList<LayoutLink> links;
    GraphLayout layout;
    int hiddenFiles = 0;
};

DrillGraph drillGraph(const insight::Snapshot &snapshot, const insight::ModuleGraph &graph,
                      int module, int maxFiles = 120);

QVector<int> matrixOrder(const insight::ModuleGraph &graph);
QVector<QRectF> squarify(const QVector<double> &weights, const QRectF &rect);
QString reportHtml(const insight::Snapshot &snapshot, const Report &report, const QString &title);

struct Model {
    insight::SnapshotPtr snapshot;
    Report report;
    GraphLayout layout;
    QVector<int> order;
    int depth = 0;
};

using ModelPtr = std::shared_ptr<const Model>;

ModelPtr buildModel(const insight::SnapshotPtr &snapshot, int depth);

}
