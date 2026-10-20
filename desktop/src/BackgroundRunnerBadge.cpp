#include "BackgroundRunnerBadge.h"

#include "AppPaths.h"
#include "BackgroundSettings.h"
#include "SingleInstance.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QSet>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace bg = forkmesh::background;

namespace {

constexpr int kRefreshMs = 60000;

QSet<BackgroundRunnerBadge *> &badges()
{
    static QSet<BackgroundRunnerBadge *> all;
    return all;
}

bool registered(const QString &project)
{
    const bg::StartManager manager = bg::StartManager::system();
    return manager.unitInstalled(project) ||
           QFileInfo::exists(manager.headlessEntryPath(project));
}

QIcon dot(const QString &severity, bool dark)
{
    QColor color;
    if (severity == QLatin1String("error"))
        color = QColor(dark ? "#f85149" : "#cf222e");
    else if (severity == QLatin1String("warn"))
        color = QColor(dark ? "#d29922" : "#9a6700");
    else
        color = QColor(dark ? "#3fb950" : "#1a7f37");
    color = Theme::remapColor(color);
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(8, 8) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(0, 0, 8, 8));
    return QIcon(pixmap);
}

}

BackgroundRunnerBadge::BackgroundRunnerBadge(std::function<void()> openSettings, QWidget *parent)
    : QPushButton(parent), m_openSettings(std::move(openSettings))
{
    setObjectName(QStringLiteral("backgroundRunnerBadge"));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setFixedHeight(26);
    setIconSize(QSize(8, 8));
    hide();
    badges().insert(this);
    connect(this, &QPushButton::clicked, this, [this] {
        refresh();
        showMenu();
    });
    if (!bg::supported())
        return;
    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, [this] { refresh(); });
    m_timer->start();
    QTimer::singleShot(5000, this, [this] { refresh(); });
}

BackgroundRunnerBadge::~BackgroundRunnerBadge()
{
    badges().remove(this);
}

void BackgroundRunnerBadge::refreshAll()
{
    for (BackgroundRunnerBadge *badge : std::as_const(badges()))
        badge->refresh();
}

void BackgroundRunnerBadge::refresh()
{
    if (!bg::supported() || m_refreshing)
        return;
    const QString project = forkmesh::paths::projectSlug();
    if (!registered(project)) {
        m_diagnostics = bg::RunnerDiagnostics();
        hide();
        return;
    }
    m_refreshing = true;
    const bool keep = bg::keepRunning();
    auto *watcher = new QFutureWatcher<bg::RunnerDiagnostics>(this);
    connect(watcher, &QFutureWatcher<bg::RunnerDiagnostics>::finished, this, [this, watcher] {
        bg::RunnerDiagnostics diagnostics = watcher->result();
        watcher->deleteLater();
        m_refreshing = false;
        apply(diagnostics);
    });
    watcher->setFuture(QtConcurrent::run([project, keep] {
        bg::RunnerDiagnostics diagnostics =
            bg::StartManager::system().diagnose(project, forkmesh::backgroundInstancePid());
        diagnostics.keepRunning = keep;
        return diagnostics;
    }));
}

void BackgroundRunnerBadge::apply(const bg::RunnerDiagnostics &diagnostics)
{
    m_diagnostics = diagnostics;
    const bg::RunnerBadge badge = bg::runnerBadge(diagnostics);
    setText(QLatin1Char(' ') + badge.text);
    setToolTip(badge.toolTip);
    setAccessibleName(badge.text);
    setIcon(dot(badge.severity, palette().color(QPalette::Window).lightness() < 128));
    if (property("severity").toString() != badge.severity) {
        setProperty("severity", badge.severity);
        style()->unpolish(this);
        style()->polish(this);
    }
    setVisible(badge.visible);
}

void BackgroundRunnerBadge::showMenu()
{
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    const bg::RunnerBadge badge = bg::runnerBadge(m_diagnostics);
    QAction *header = menu.addAction(badge.visible
                                         ? QStringLiteral("ForkMesh without a window: %1")
                                               .arg(badge.text.section(QStringLiteral(": "), 1))
                                         : QStringLiteral("ForkMesh without a window"));
    header->setEnabled(false);
    menu.addSeparator();
    connect(menu.addAction(QStringLiteral("Diagnostics…")), &QAction::triggered, this,
            [this] { showDiagnostics(); });
    if (m_openSettings)
        connect(menu.addAction(QStringLiteral("Startup settings…")), &QAction::triggered, this,
                [this] { m_openSettings(); });
    menu.addSeparator();
    QAction *off = menu.addAction(QStringLiteral("Turn off start at boot"));
    off->setToolTip(QStringLiteral(
        "Stops ForkMesh from starting without a window when the computer starts, and removes "
        "its unit and tray entry. Turn it on again in Settings → General → Startup."));
    off->setEnabled(!m_turningOff);
    connect(off, &QAction::triggered, this, [this] { turnOff(); });
    const QSize hint = menu.sizeHint();
    menu.exec(mapToGlobal(QPoint(width() - hint.width(), height())));
}

void BackgroundRunnerBadge::showDiagnostics()
{
    if (m_dialog) {
        m_dialog->raise();
        m_dialog->activateWindow();
        loadReport();
        return;
    }
    auto *dialog = new QDialog(window());
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("ForkMesh without a window"));
    dialog->resize(720, 520);
    auto *column = new QVBoxLayout(dialog);
    auto *report = new QPlainTextEdit;
    report->setReadOnly(true);
    report->setLineWrapMode(QPlainTextEdit::NoWrap);
    report->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    column->addWidget(report);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QPushButton *reload = buttons->addButton(QStringLiteral("Refresh"),
                                             QDialogButtonBox::ActionRole);
    QPushButton *copy = buttons->addButton(QStringLiteral("Copy"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(reload, &QPushButton::clicked, this, [this] { loadReport(); });
    connect(copy, &QPushButton::clicked, report,
            [report] { QApplication::clipboard()->setText(report->toPlainText()); });
    column->addWidget(buttons);
    m_dialog = dialog;
    m_report = report;
    loadReport();
    dialog->show();
}

void BackgroundRunnerBadge::loadReport()
{
    if (!m_report)
        return;
    m_report->setPlainText(QStringLiteral("Checking…"));
    const QString project = forkmesh::paths::projectSlug();
    const bool keep = bg::keepRunning();
    auto *watcher = new QFutureWatcher<bg::RunnerDiagnostics>(this);
    connect(watcher, &QFutureWatcher<bg::RunnerDiagnostics>::finished, this, [this, watcher] {
        const bg::RunnerDiagnostics diagnostics = watcher->result();
        watcher->deleteLater();
        if (m_report)
            m_report->setPlainText(bg::runnerReport(diagnostics));
        if (!m_refreshing)
            apply(diagnostics);
    });
    watcher->setFuture(QtConcurrent::run([project, keep] {
        bg::RunnerDiagnostics diagnostics = bg::StartManager::system().diagnose(
            project, forkmesh::backgroundInstancePid(), true);
        diagnostics.keepRunning = keep;
        return diagnostics;
    }));
}

void BackgroundRunnerBadge::turnOff()
{
    if (m_turningOff)
        return;
    const auto answer = QMessageBox::question(
        window(), QStringLiteral("Turn off start at boot"),
        QStringLiteral("ForkMesh will no longer start without a window when the computer "
                       "starts. You can turn it on again in Settings → General → Startup."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    m_turningOff = true;
    setEnabled(false);
    const QString project = forkmesh::paths::projectSlug();
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        const QString error = watcher->result();
        watcher->deleteLater();
        m_turningOff = false;
        setEnabled(true);
        if (!error.isEmpty())
            QMessageBox::warning(window(), QStringLiteral("Turn off start at boot"),
                                 QStringLiteral("Could not turn it off completely: %1").arg(error));
        BackgroundSettings::refreshAll();
        refreshAll();
        loadReport();
    });
    watcher->setFuture(QtConcurrent::run([project] {
        QString error;
        if (!bg::StartManager::system().turnOff(project, &error) && error.isEmpty())
            error = QStringLiteral("unknown error");
        return error;
    }));
}
