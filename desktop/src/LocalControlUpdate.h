#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace forkmesh::localcontrol::update {

QString familyPrefix();
QString actionName();
QString subjectName();
QString actionSummary(int agentsRunning, const QString &version);

inline constexpr int kMaxBacklogLines = 300;
inline constexpr int kMaxBatchLines = 150;
inline constexpr int kMaxBatchChars = 7000;
inline constexpr int kMaxLineChars = 400;

QString cleanLine(const QString &line);

class ProgressFeed
{
public:
    void reset();
    void append(const QString &chunk);
    void noteStatus(const QString &status, bool isError);

    qint64 seq() const { return m_seq; }
    int run() const { return m_run; }
    QString status() const { return m_status; }
    QString error() const { return m_error; }
    bool relaunching() const { return m_relaunching; }

    QJsonArray backlog(int maxLines = kMaxBacklogLines) const;
    QJsonObject batchSince(qint64 sentSeq) const;

private:
    void push(const QString &line);

    QStringList m_lines;
    qint64 m_seq = 0;
    int m_run = 0;
    QString m_carry;
    QString m_status;
    QString m_error;
    bool m_relaunching = false;
};

struct RunState {
    bool running = false;
    QString kind;
    int percent = 0;
    QString phase;
    QString activity;
};

QJsonObject stateObject(const ProgressFeed &feed, const RunState &run);

} // namespace forkmesh::localcontrol::update
