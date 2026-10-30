#pragma once

#include "BackgroundMode.h"

#include <QPointer>
#include <QPushButton>

#include <functional>

class QDialog;
class QPlainTextEdit;
class QTimer;

class BackgroundRunnerBadge : public QPushButton
{
public:
    explicit BackgroundRunnerBadge(std::function<void()> openSettings, QWidget *parent = nullptr);
    ~BackgroundRunnerBadge() override;

    static void refreshAll();
    void refresh();

private:
    void apply(const forkmesh::background::RunnerDiagnostics &diagnostics);
    void showMenu();
    void showDiagnostics();
    void loadReport();
    void turnOff();
    void closeToHeadless();

    std::function<void()> m_openSettings;
    forkmesh::background::RunnerDiagnostics m_diagnostics;
    QTimer *m_timer = nullptr;
    QPointer<QDialog> m_dialog;
    QPointer<QPlainTextEdit> m_report;
    bool m_refreshing = false;
    bool m_turningOff = false;
};
