#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QByteArray;

namespace forkmesh {

struct SunburstNode {
    QString name;
    qint64 size = 0;
    int fileCount = 0;
    QList<SunburstNode> children;
};

constexpr int kSizeMapMaxDepth = 8;

struct DirectorySizeScanOptions {
    QSet<QString> pruned;
    int maxDepth = kSizeMapMaxDepth;
};

struct DirectorySizeScanResult {
    SunburstNode root;
    int unreadableDirs = 0;
    QStringList unreadableSample;
};

struct DirectorySizeScanProgressUpdate {
    int worker = 0;
    int workers = 1;
    QString path;
    bool idle = false;
    qint64 workerBytes = 0;
    int workerFiles = 0;
    qint64 bytes = 0;
    int files = 0;
};

using DirectorySizeScanProgress =
    std::function<void(const DirectorySizeScanProgressUpdate &update)>;

using DirectorySizeScanCancel = std::function<bool()>;

// Symlinks are never followed.
DirectorySizeScanResult
scanDirectorySizes(const QString &path, const DirectorySizeScanOptions &options,
                   const DirectorySizeScanProgress &progress = {},
                   const DirectorySizeScanCancel &canceled = {});

bool scanNeedsElevation(const QString &path, const QSet<QString> &pruned);

QSet<QString> mountPointsFromMountTable(const QByteArray &table);

QSet<QString> systemMountPoints();

// Elevated-helper protocol: JSON request, binary result on stdout.
QByteArray encodeScanRequest(const QString &path,
                             const DirectorySizeScanOptions &options);
bool decodeScanRequest(const QByteArray &payload, QString *path,
                       DirectorySizeScanOptions *options);
QByteArray encodeScanResult(const DirectorySizeScanResult &result);
bool decodeScanResult(const QByteArray &payload, DirectorySizeScanResult *result);

QByteArray encodeScanProgress(const DirectorySizeScanProgressUpdate &update);
bool decodeScanProgress(const QByteArray &line,
                        DirectorySizeScanProgressUpdate *update);

bool runningAsRoot();

} // namespace forkmesh
