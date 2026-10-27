#pragma once

#include "AgentTaskList.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::sessioninsights {

struct Entry {
    QString key;
    QString label;
    QString detail;
    QString icon;
    int count = 0;
    int added = 0;
    int removed = 0;
    bool created = false;
    quint64 touched = 0;
};

struct Background {
    QString id;
    QString label;
    QString kind;
    QString status;
    quint64 touched = 0;

    bool running() const { return status == QLatin1String("running"); }
};

struct ToolCount {
    QString name;
    int calls = 0;
    int errors = 0;
};

class Collector
{
public:
    void clear();
    bool ingest(const QJsonObject &event);

    QList<Entry> outputs() const;
    QList<Entry> sources() const;
    QList<Background> background() const;
    QList<ToolCount> tools() const;
    const agenttasks::Tracker &tasks() const { return m_tasks; }
    int toolCalls() const { return m_calls; }
    int toolErrors() const { return m_errors; }
    int runningBackground() const;

private:
    struct Pending {
        QString name;
        QString list;
        QString key;
        int added = 0;
        int removed = 0;
        bool background = false;
    };

    bool applyToolUse(const QString &id, const QString &name, const QJsonObject &input);
    bool applyToolResult(const QString &id, const QString &text, bool isError);
    bool applyNotifications(const QString &text);
    void touchOutput(const QString &id, const QString &path, const QString &name,
                     int added, int removed, bool created);
    void touchSource(const QString &key, const QString &label, const QString &detail,
                     const QString &icon);

    agenttasks::Tracker m_tasks;
    QHash<QString, Entry> m_outputs;
    QHash<QString, Entry> m_sources;
    QHash<QString, Background> m_background;
    QHash<QString, ToolCount> m_tools;
    QHash<QString, Pending> m_pending;
    QHash<QString, QString> m_shells;
    quint64 m_clock = 0;
    int m_calls = 0;
    int m_errors = 0;
};

QString fileName(const QString &path);
QString hostOf(const QString &url);

QStringList defaultCardOrder();
QStringList normalizedCardOrder(const QStringList &saved);
QStringList movedCard(const QStringList &order, const QString &key, int toIndex);

}
