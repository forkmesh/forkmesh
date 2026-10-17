#pragma once

#include "RepoInsight.h"

#include <QColor>
#include <QList>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace forkmesh::ember {

struct PackCircle {
    double x = 0;
    double y = 0;
    double r = 0;
};

double packSiblings(QVector<PackCircle> &circles);
PackCircle encloseCircles(const QVector<PackCircle> &circles);

struct MapNode {
    QString name;
    QString path;
    int parent = -1;
    int depth = 0;
    int file = -1;
    QVector<int> children;
    double x = 0;
    double y = 0;
    double r = 0;
    qint64 codeLines = 0;
    int files = 0;
    double health = 10;
    int commits = 0;
};

struct MapLayout {
    QVector<MapNode> nodes;
    QVector<int> nodeOfFile;
    int maxDepth = 0;

    bool isEmpty() const { return nodes.isEmpty(); }
    bool isFolder(int node) const;
    bool contains(int node, const QPointF &point) const;
    int nodeAt(const QPointF &point) const;
    QVector<int> chain(int node) const;
    bool isAncestor(int ancestor, int node) const;
    int fileNode(int file) const;
};

using MapLayoutPtr = std::shared_ptr<const MapLayout>;

struct LayoutOptions {
    double minLines = 12;
    double padding = 2.5;
    double rimShare = 0.035;
    bool collapseChains = true;
};

MapLayout buildLayout(const insight::Snapshot &snapshot, const LayoutOptions &options = {});

struct ViewTransform {
    double cx = 0;
    double cy = 0;
    double k = 1;
};

ViewTransform fitTransform(const MapNode &node, const QSizeF &viewport, double margin = 18);
ViewTransform interpolateView(const ViewTransform &from, const ViewTransform &to, double t,
                              const QSizeF &viewport);
double easeInOut(double t);
int readableFocus(const MapLayout &layout, int node, const QSizeF &viewport,
                  double minRadius = 48);

enum class Tab { Hotspots, RefactoringTargets, CodeHealth, ChangeCoupling, Knowledge, Defects,
                 Languages };

QList<Tab> allTabs();
QString tabTitle(Tab tab);
bool tabNeedsHistory(Tab tab);
bool hasHistory(const insight::Snapshot &snapshot);

struct Filters {
    double minHealth = 1;
    double maxHealth = 10;
    int minCommits = 0;
    QString author;
    QString search;

    bool active() const;
};

bool matchesFilters(const insight::FileInfo &file, const Filters &filters);

struct CouplingRef {
    int file = -1;
    int shared = 0;
    double degree = 0;
};

using CouplingIndex = QVector<QVector<CouplingRef>>;

CouplingIndex buildCouplingIndex(const insight::Snapshot &snapshot);

struct Target {
    int file = -1;
    int rank = 0;
    double score = 0;
    QStringList reasons;
};

double targetScore(const insight::FileInfo &file);
QList<Target> refactoringTargets(const insight::Snapshot &snapshot, const CouplingIndex &coupling,
                                 int limit = 15);

struct LegendEntry {
    QString label;
    QColor color;
    bool ring = false;
};

struct FileStyle {
    QColor color;
    bool dimmed = false;
    double glow = 0;
    int rank = 0;
    bool marked = false;
};

struct TabColoring {
    Tab tab = Tab::CodeHealth;
    QVector<FileStyle> files;
    QList<LegendEntry> legend;
    bool scale = false;
    QColor scaleLow;
    QColor scaleHigh;
    QString scaleLowLabel;
    QString scaleHighLabel;
    QString note;
    QList<Target> targets;
    int glowing = 0;
    int matching = 0;
};

QColor bandColor(insight::HealthBand band);
QColor healthColor(double health);
QColor neutralColor();
QColor couplingLinkColor();
QColor mix(const QColor &a, const QColor &b, double t);
const QList<QColor> &categoricalPalette();
QStringList topAuthors(const insight::Snapshot &snapshot, int limit);
QStringList topLanguages(const insight::Snapshot &snapshot, int limit);

TabColoring colorize(const insight::Snapshot &snapshot, Tab tab, const Filters &filters,
                     const CouplingIndex &coupling);

struct Summary {
    int unhealthy = 0;
    int problematic = 0;
    int healthy = 0;
    qint64 unhealthyLines = 0;
    qint64 problematicLines = 0;
    qint64 healthyLines = 0;
    int topHotspot = -1;
    QString text;
};

Summary summarize(const insight::Snapshot &snapshot);

QString fileNameOf(const QString &path);
QString folderOf(const QString &path);
QString spanText(int days);
QString agoText(qint64 when, qint64 now);
QString percentText(double share);

} // namespace forkmesh::ember
