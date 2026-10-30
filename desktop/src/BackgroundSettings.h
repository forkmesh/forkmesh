#pragma once

#include <QWidget>

class QCheckBox;
class QLabel;

namespace forkmesh::background {
struct StartStatus;
}

class BackgroundSettings : public QWidget
{
public:
    explicit BackgroundSettings(QWidget *parent = nullptr);
    ~BackgroundSettings() override;

    static void refreshAll();

private:
    void refresh();
    void applyStatus(const forkmesh::background::StartStatus &status, const QString &error);
    void setStartAtBoot(bool on);
    void setKeepRunning(bool on);
    void setSleepDisabled(bool on);
    void setRestartAllowed(bool on);

    QCheckBox *m_keepRunning = nullptr;
    QCheckBox *m_startAtBoot = nullptr;
    QCheckBox *m_noSleep = nullptr;
    QCheckBox *m_allowRestart = nullptr;
    QLabel *m_status = nullptr;
    bool m_busy = false;
    QString m_sleepError;
};
