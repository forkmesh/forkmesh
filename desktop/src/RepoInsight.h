#pragma once

#include <QByteArray>
#include <QFuture>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <memory>

namespace forkmesh::insight {

struct FunctionInfo {
    QString name;
    int line = 0;
    int length = 0;
    int complexity = 1;
    int nesting = 0;
    int parameters = 0;
};

enum class HealthBand { Healthy, Problematic, Unhealthy };

struct FileInfo {
    QString path;
    QString language;
    qint64 bytes = 0;
    int lines = 0;
    int codeLines = 0;
    int commentLines = 0;
    int blankLines = 0;
    int types = 0;
    int abstractTypes = 0;
    int complexity = 0;
    int maxComplexity = 0;
    int maxNesting = 0;
    int duplicatedLines = 0;
    bool test = false;
    QList<FunctionInfo> functions;
    QStringList imports;
    QStringList externalImports;
    QStringList declares;
    QStringList references;
    QVector<int> dependsOn;
    QVector<int> dependedOnBy;
    int commits = 0;
    int recentCommits = 0;
    int authors = 0;
    int fixCommits = 0;
    int linesAdded = 0;
    int linesDeleted = 0;
    qint64 lastChanged = 0;
    qint64 firstChanged = 0;
    QString mainAuthor;
    double mainAuthorShare = 0;
    double formerAuthorShare = 0;
    QList<QPair<QString, int>> topAuthors;
    double health = 10;
    QStringList healthIssues;
    double hotspot = 0;
    int cycle = -1;
};

struct Coupling {
    int a = -1;
    int b = -1;
    int shared = 0;
    double degree = 0;
};

struct Snapshot {
    QString root;
    QString name;
    QString head;
    QString branch;
    qint64 analyzedAt = 0;
    qint64 elapsedMs = 0;
    bool git = false;
    int historyDays = 0;
    int commitsScanned = 0;
    qint64 historyStart = 0;
    qint64 historyEnd = 0;
    QList<FileInfo> files;
    QHash<QString, int> byPath;
    QHash<QString, QString> packageRoots;
    QList<Coupling> couplings;
    QList<QVector<int>> cycles;
    QStringList authors;
    QStringList formerAuthors;
    int skippedFiles = 0;
    QString warning;
    QString error;

    int indexOf(const QString &path) const { return byPath.value(path, -1); }
    int totalCodeLines() const;
    int totalComplexity() const;
    QList<Coupling> couplingsFor(int file) const;
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

struct Options {
    bool history = true;
    int historyDays = 365;
    int maxFiles = 30000;
    int maxCommits = 8000;
    int maxCommitFiles = 40;
    int minSharedCommits = 3;
    double minCouplingDegree = 0.25;
    qint64 maxFileBytes = 2 * 1024 * 1024;
};

using ProgressFn = std::function<void(const QString &, double)>;
using CancelFn = std::function<bool()>;

struct ModuleNode {
    QString path;
    QString name;
    QVector<int> files;
    int codeLines = 0;
    int complexity = 0;
    int functions = 0;
    int types = 0;
    int abstractTypes = 0;
    int afferent = 0;
    int efferent = 0;
    int internalEdges = 0;
    double instability = 0;
    double abstractness = 0;
    double distance = 0;
    double cohesion = 0;
    double health = 10;
    int commits = 0;
    int cycle = -1;
    int layer = 0;
};

struct ModuleEdge {
    int from = -1;
    int to = -1;
    int weight = 0;
    bool inCycle = false;
    QVector<QPair<int, int>> fileEdges;
};

struct ModuleGraph {
    int depth = 1;
    QList<ModuleNode> nodes;
    QList<ModuleEdge> edges;
    QList<QVector<int>> cycles;
    QVector<int> moduleOfFile;

    int edgeIndex(int from, int to) const;
};

struct TrendPoint {
    qint64 time = 0;
    QString commit;
    QString author;
    int codeLines = 0;
    int complexity = 0;
    int maxComplexity = 0;
    double health = 10;
};

QString languageForPath(const QString &path);
bool isTestPath(const QString &path);
bool isVendoredPath(const QString &path);
FileInfo measureSource(const QString &path, const QByteArray &bytes);
QStringList extractImports(const QString &language, const QString &text);
double computeHealth(const FileInfo &file, QStringList *issues = nullptr);
HealthBand healthBand(double health);
QString healthBandName(HealthBand band);
void resolveDependencies(Snapshot &snapshot);
QList<QVector<int>> stronglyConnected(const QList<QVector<int>> &adjacency);
void markCycles(Snapshot &snapshot);
void markDuplication(Snapshot &snapshot, const QHash<QString, QByteArray> &texts);
void computeHotspots(Snapshot &snapshot);
void applyHistory(Snapshot &snapshot, const QByteArray &gitLog, const Options &options,
                  qint64 now);
int autoModuleDepth(const Snapshot &snapshot);
QString modulePathFor(const QString &filePath, int depth);
ModuleGraph buildModuleGraph(const Snapshot &snapshot, int depth = 0);

SnapshotPtr analyze(const QString &root, const Options &options = {},
                    const ProgressFn &progress = {}, const CancelFn &cancel = {});
QList<TrendPoint> complexityTrend(const QString &root, const QString &path, int samples = 16,
                                  const CancelFn &cancel = {});

class Analysis
{
public:
    static QFuture<SnapshotPtr> request(const QString &root, bool force = false);
    static SnapshotPtr cached(const QString &root);
    static bool running(const QString &root);
    static bool fresh(const QString &root);
    static void cancel(const QString &root);
};

} // namespace forkmesh::insight
