#pragma once

#include "FleetAutoscaleModel.h"

#include <QFrame>
#include <QPointer>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

class FleetAutoscalePanel : public QFrame
{
public:
    explicit FleetAutoscalePanel(QWidget *parent = nullptr);
    ~FleetAutoscalePanel() override;

    void setScriptPath(const QString &path);
    QString scriptPath() const { return m_script; }
    void refresh();
    void applySettings(const forkmesh::autoscale::Settings &settings);
    forkmesh::autoscale::Settings settingsFromForm() const;
    void applyStatus(const forkmesh::autoscale::Status &status);
    QString statusText() const;
    int nodeRowCount(bool runnerPool) const;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct PoolWidgets {
        bool runner = false;
        QCheckBox *enabled = nullptr;
        QCheckBox *dryRun = nullptr;
        QSpinBox *min = nullptr;
        QSpinBox *max = nullptr;
        QSpinBox *upP95 = nullptr;
        QSpinBox *upLoopLag = nullptr;
        QDoubleSpinBox *upLoad = nullptr;
        QSpinBox *upRps = nullptr;
        QSpinBox *upFor = nullptr;
        QDoubleSpinBox *downFraction = nullptr;
        QSpinBox *downFor = nullptr;
        QSpinBox *upQueueWait = nullptr;
        QSpinBox *upQueueDepth = nullptr;
        QSpinBox *downIdle = nullptr;
        QLineEdit *snapshot = nullptr;
        QLabel *snapshotBuilt = nullptr;
        QSpinBox *cooldown = nullptr;
        QLineEdit *plan = nullptr;
        QLineEdit *region = nullptr;
        QDoubleSpinBox *cap = nullptr;
        QLabel *status = nullptr;
        QTableWidget *nodes = nullptr;
        QPushButton *scaleUp = nullptr;
        QPushButton *scaleDown = nullptr;
    };

    QWidget *buildPool(PoolWidgets &pool, const QString &title);
    void fillPool(PoolWidgets &pool, const forkmesh::autoscale::PoolSettings &settings);
    forkmesh::autoscale::PoolSettings readPool(const PoolWidgets &pool) const;
    void showPoolStatus(PoolWidgets &pool,
                        const forkmesh::autoscale::PoolStatus &status,
                        const forkmesh::autoscale::PoolSettings &settings);
    void save();
    void scale(bool runnerPool, bool up);
    void setBusy(bool busy);
    void appendLog(const QString &text);
    QString python() const;

    QString m_script;
    PoolWidgets m_app;
    PoolWidgets m_runner;
    QCheckBox *m_killSwitch = nullptr;
    QLabel *m_summary = nullptr;
    QLabel *m_settingsState = nullptr;
    QLabel *m_recent = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_refresh = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QTimer *m_timer = nullptr;
    QPointer<QProcess> m_statusProcess;
    QPointer<QProcess> m_actionProcess;
    bool m_formLoaded = false;
    bool m_formDirty = false;
};
