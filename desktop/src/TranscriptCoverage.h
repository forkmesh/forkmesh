#pragma once

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace forkmesh::transcriptcoverage {

struct Gap {
    QString key;
    QString label;
    int count = 0;
    QString sample;
};

bool isCodexToolItem(const QString &itemType);
QStringList eventGaps(const QJsonObject &event);
QStringList rawLineGaps(const QString &line);
QString gapLabel(const QString &key);

class Tracker
{
public:
    void clear();
    bool noteEvent(const QJsonObject &event);
    bool noteRawLine(const QString &line);
    bool noteRawLog(const QString &raw);
    bool isEmpty() const { return m_gaps.isEmpty(); }
    int kindCount() const { return int(m_gaps.size()); }
    int total() const;
    const QVector<Gap> &gaps() const { return m_gaps; }

private:
    bool add(const QStringList &keys, const QString &sample);

    QHash<QString, int> m_index;
    QVector<Gap> m_gaps;
};

QString summary(const QVector<Gap> &gaps, int limit = 8);
QString agentPrompt(const QVector<Gap> &gaps, const QString &sessionLabel);

}
