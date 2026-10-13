#pragma once

#include "CoreNodeLogs.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QtGlobal>

#include <functional>

class QProcess;
class QTimer;

namespace forkmesh {
namespace deploylogs {

struct DeployLine {
    QString text;
    QString category;
};

struct TailedLine {
    QString directoryKind;
    QString fileKind;
    QString raw;
};

inline constexpr int kFilesPerDirectory = 8;
inline constexpr qint64 kMaximumReadBytes = 256 * 1024;
inline constexpr qint64 kDedupeWindowMs = 10 * 60 * 1000;

QString autodeployDirectory();
QString opsDirectory();
QString fileKind(const QString &directoryKind, const QString &fileName);
QString withoutStamp(const QString &line);
bool looksFailed(const QString &line);
QString lineCategory(const QString &line, int priority = 6);
DeployLine fileLine(const TailedLine &line);
QStringList journalUnits();
QString journalRole(const corelogs::JournalEntry &entry);
QStringList journalArguments(const QString &afterCursor);
DeployLine journalLine(const corelogs::JournalEntry &entry);

class FileTailer
{
public:
    explicit FileTailer(const QList<QPair<QString, QString>> &directories)
        : m_directories(directories)
    {
    }
    QList<TailedLine> poll();

private:
    QList<QPair<QString, QString>> m_directories;
    QHash<QString, qint64> m_offsets;
    bool m_primed = false;
};

class CrossSourceDedupe
{
public:
    bool admit(const QString &source, const QString &line, qint64 nowMs);

private:
    struct Seen {
        QString source;
        int count = 0;
        qint64 lastMs = 0;
    };
    void expire(qint64 nowMs);
    QHash<QString, Seen> m_seen;
    qint64 m_expiredMs = 0;
};

class DeployLogFollower : public QObject
{
public:
    using Sink = std::function<void(const QString &text, const QString &category)>;
    DeployLogFollower(const QString &cursorPath, Sink sink, QObject *parent = nullptr);
    ~DeployLogFollower() override;
    void start();
    void stop();

private:
    void pollFiles();
    void deliverFileLines(const QList<TailedLine> &lines);
    void launchJournal();
    void readJournal();
    void saveCursor(bool force);

    QString m_cursorPath;
    Sink m_sink;
    FileTailer m_tailer;
    CrossSourceDedupe m_dedupe;
    QPointer<QTimer> m_timer;
    QPointer<QProcess> m_journal;
    QByteArray m_journalTail;
    QString m_cursor;
    QString m_cursorWritten;
    qint64 m_cursorSavedMs = 0;
    bool m_cursorDirty = false;
    bool m_cursorSaving = false;
    bool m_pollBusy = false;
    bool m_running = false;
    quint64 m_generation = 0;
    QThreadPool m_io;
};

} // namespace deploylogs
} // namespace forkmesh
