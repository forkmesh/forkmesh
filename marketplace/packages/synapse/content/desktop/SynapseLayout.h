#pragma once

#include "RepoInsight.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QVector>
#include <QVector3D>

#include <functional>
#include <memory>

namespace forkmesh::synapse {

enum class Direction { Imports, Importers, Both };

struct BrainShape {
    float halfWidth = 96.0f;
    float halfHeightTop = 76.0f;
    float halfHeightBottom = 58.0f;
    float halfLength = 124.0f;
    float fissure = 15.0f;
    float innerShell = 0.4f;
    float outerShell = 0.96f;

    float radiusOf(const QVector3D &point) const;
    float outerAt(const QVector3D &point) const;
    QVector3D onShell(const QVector3D &direction, float radius) const;
    float boundingRadius() const { return halfLength; }
};

struct Lobe {
    QString name;
    QString path;
    QVector3D anchor;
    int hemisphere = 1;
    int layer = 0;
    QVector<int> nodes;
};

struct Node {
    int file = -1;
    int lobe = 0;
    int hemisphere = 1;
    float size = 1.0f;
    int fanIn = 0;
    int fanOut = 0;
    bool cycle = false;
    quint32 seed = 0;
};

struct Edge {
    int from = -1;
    int to = -1;
    bool cycle = false;
    quint32 seed = 0;
};

struct CycleInfo {
    int index = -1;
    QVector<int> files;
    QVector<int> edges;
    QVector<int> path;
};

struct Graph {
    insight::SnapshotPtr snapshot;
    bool includesIsolated = false;
    BrainShape shape;
    QVector<Node> nodes;
    QVector<Edge> edges;
    QVector<int> nodeOfFile;
    QList<Lobe> lobes;
    QVector<QVector<int>> outEdges;
    QVector<QVector<int>> inEdges;
    QList<CycleInfo> cycles;
    int imports = 0;
    float spacing = 10.0f;

    int nodeFor(int file) const
    {
        return file >= 0 && file < nodeOfFile.size() ? nodeOfFile.at(file) : -1;
    }
};

using GraphPtr = std::shared_ptr<const Graph>;

struct LayoutOptions {
    int iterations = 300;
    int barnesHutAbove = 1500;
    float theta = 0.85f;
    int publishIntervalMs = 45;
};

using PublishFn = std::function<void(const QVector<QVector3D> &, double)>;
using StopFn = std::function<bool()>;

struct Neighbourhood {
    QHash<int, int> imports;
    QHash<int, int> importers;

    bool contains(int file) const { return imports.contains(file) || importers.contains(file); }
    int size() const { return imports.size() + importers.size(); }
};

quint32 stableHash(const QString &text);
Graph buildGraph(const insight::SnapshotPtr &snapshot, bool includeIsolated);
QVector<QVector3D> seedPositions(const Graph &graph);
int layoutIterations(const Graph &graph, const LayoutOptions &options);
QVector<QVector3D> runLayout(const Graph &graph, const LayoutOptions &options = {},
                             const PublishFn &publish = {}, const StopFn &stop = {});
QVector<QVector3D> lobeCentres(const Graph &graph, const QVector<QVector3D> &positions);

QVector<int> cyclePath(const insight::Snapshot &snapshot, const QVector<int> &members);
QString cycleText(const insight::Snapshot &snapshot, const QVector<int> &path);
QList<CycleInfo> cycleInfos(const insight::Snapshot &snapshot);
QVector<int> orphans(const insight::Snapshot &snapshot);
QVector<int> leaves(const insight::Snapshot &snapshot);
QVector<int> mostImported(const insight::Snapshot &snapshot, int limit);
QVector<int> mostImporting(const insight::Snapshot &snapshot, int limit);
int mostConnected(const insight::Snapshot &snapshot);
int importCount(const insight::Snapshot &snapshot);
Neighbourhood neighbourhood(const insight::Snapshot &snapshot, int file, int hops,
                            Direction direction);
QString fileName(const QString &path);

}
