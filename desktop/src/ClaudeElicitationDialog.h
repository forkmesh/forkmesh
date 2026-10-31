#pragma once

#include "ClaudeControlProtocol.h"

#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QString>

class QLabel;
class QWidget;

class ClaudeElicitationDialog : public QDialog
{
    Q_OBJECT
public:
    ClaudeElicitationDialog(const QString &sessionTitle, const QJsonObject &requestEvent,
                            QWidget *parent = nullptr);

    QString promptKey() const { return m_key; }
    QMap<QString, QString> values() const;
    void setValueForTest(const QString &name, const QString &value);
    void finishForTest(const QString &action) { finishWith(action); }

signals:
    void answered(const QString &key, const QString &action, const QJsonObject &content);

private:
    void finishWith(const QString &action);

    QString m_key;
    bool m_urlMode = false;
    QList<forkmesh::claudecontrol::ElicitationField> m_fields;
    QList<QWidget *> m_editors;
    QLabel *m_error = nullptr;
    bool m_finished = false;
};
