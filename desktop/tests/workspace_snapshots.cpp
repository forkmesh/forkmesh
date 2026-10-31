#include <QtWidgets>

#include "AgentStore.h"
#include "AppViewMode.h"
#include "ClaudeTranscriptView.h"
#include "DesignTokens.h"
#include "MainWindow.h"
#include "GitLayout.h"
#include "MainWindowNextAccess.h"
#include "SessionSidePanels.h"
#include "WorkspaceLayout.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>

namespace {

bool git(const QString &dir, const QStringList &args)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.start(QStringLiteral("git"), args);
    return process.waitForFinished(15000) && process.exitCode() == 0;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

QJsonObject turn(const QString &role, const QJsonObject &block)
{
    return QJsonObject{
        {QStringLiteral("type"), role},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("role"), role},
                     {QStringLiteral("content"), QJsonArray{block}}}}};
}

QJsonObject text(const QString &body)
{
    return turn(QStringLiteral("assistant"),
                QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                            {QStringLiteral("text"), body}});
}

QJsonObject toolUse(const QString &id, const QString &name,
                    const QJsonObject &input)
{
    return turn(QStringLiteral("assistant"),
                QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_use")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("name"), name},
                            {QStringLiteral("input"), input}});
}

QJsonObject toolResult(const QString &id, const QString &content)
{
    return turn(QStringLiteral("user"),
                QJsonObject{{QStringLiteral("type"), QStringLiteral("tool_result")},
                            {QStringLiteral("tool_use_id"), id},
                            {QStringLiteral("content"), content},
                            {QStringLiteral("is_error"), false}});
}

QList<QJsonObject> demoTranscript()
{
    QList<QJsonObject> events;
    events.append(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("_local_user")},
        {QStringLiteral("text"),
         QStringLiteral("The invoice total ignores the discount when the "
                        "quantity is above ten. Find it and fix it, with a "
                        "test.")}});
    events.append(text(QStringLiteral(
        "I'll read the pricing module first, then reproduce the problem with "
        "a test before changing anything.")));
    events.append(toolUse(QStringLiteral("toolu_ws_1"), QStringLiteral("Read"),
                          QJsonObject{{QStringLiteral("file_path"),
                                       QStringLiteral("src/pricing.py")}}));
    events.append(toolResult(
        QStringLiteral("toolu_ws_1"),
        QStringLiteral("1\tdef total(quantity, unit_price, discount=0.0):\n"
                       "2\t    if quantity > 10:\n"
                       "3\t        return quantity * unit_price\n"
                       "4\t    return quantity * unit_price * (1 - discount)\n")));
    events.append(text(QStringLiteral(
        "Found it. The branch for more than ten items returns early and never "
        "applies `discount`. The fix is to compute the subtotal once and "
        "apply the discount on every path.")));
    events.append(toolUse(
        QStringLiteral("toolu_ws_2"), QStringLiteral("Edit"),
        QJsonObject{
            {QStringLiteral("file_path"), QStringLiteral("src/pricing.py")},
            {QStringLiteral("old_string"),
             QStringLiteral("    if quantity > 10:\n"
                            "        return quantity * unit_price\n"
                            "    return quantity * unit_price * (1 - discount)\n")},
            {QStringLiteral("new_string"),
             QStringLiteral("    subtotal = quantity * unit_price\n"
                            "    return subtotal * (1 - discount)\n")}}));
    events.append(toolResult(QStringLiteral("toolu_ws_2"),
                             QStringLiteral("The file src/pricing.py has been "
                                            "updated.")));
    events.append(toolUse(
        QStringLiteral("toolu_ws_3"), QStringLiteral("Bash"),
        QJsonObject{{QStringLiteral("command"),
                     QStringLiteral("python -m pytest tests/test_pricing.py -q")},
                    {QStringLiteral("description"),
                     QStringLiteral("Run the pricing tests")}}));
    events.append(toolResult(QStringLiteral("toolu_ws_3"),
                             QStringLiteral("....                          "
                                            "[100%]\n4 passed in 0.08s\n")));
    events.append(text(QStringLiteral(
        "The discount now applies to every quantity.\n\n"
        "- **Cause:** the early return for orders above ten items skipped the "
        "discount.\n"
        "- **Fix:** one subtotal, one discount, in `src/pricing.py`.\n"
        "- **Test:** `tests/test_pricing.py` covers eleven items at 20% off; "
        "all four tests pass.")));
    events.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                              {QStringLiteral("subtype"), QStringLiteral("success")},
                              {QStringLiteral("num_turns"), 4}});
    return events;
}

}

int runWorkspaceSnapshots(MainWindow &window)
{
    using forkmesh::ui::AppViewMode;
    int failures = 0;
    const auto check = [&failures](bool condition, const QString &what) {
        if (condition) {
            qInfo("PASS: %s", qPrintable(what));
        } else {
            qCritical("FAIL: %s", qPrintable(what));
            ++failures;
        }
    };
    const auto settle = [&window](int passes = 6) {
        for (int pass = 0; pass < passes; ++pass) {
            QApplication::processEvents();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            QThread::msleep(20);
        }
        window.testClearPageNotices();
        QApplication::processEvents();
    };

    const QString outRoot = qEnvironmentVariable(
        "FORKMESH_UI_SNAPSHOT_DIR",
        QDir::current().filePath(QStringLiteral("ui-snapshots")));
    QSettings settings;
    const QVariant savedDesign = settings.value(QStringLiteral("app/design"));
    const QVariant savedTheme = settings.value(QStringLiteral("app/theme"));

    QTemporaryDir tree;
    const QString root = tree.path();
    const bool repoReady =
        tree.isValid() &&
        git(root, {QStringLiteral("init"), QStringLiteral("-q"),
                   QStringLiteral("-b"), QStringLiteral("main")}) &&
        git(root, {QStringLiteral("config"), QStringLiteral("user.email"),
                   QStringLiteral("tests@forkmesh.local")}) &&
        git(root, {QStringLiteral("config"), QStringLiteral("user.name"),
                   QStringLiteral("ForkMesh Tests")}) &&
        writeFile(root + QStringLiteral("/src/pricing.py"),
                  "def total(quantity, unit_price, discount=0.0):\n"
                  "    if quantity > 10:\n"
                  "        return quantity * unit_price\n"
                  "    return quantity * unit_price * (1 - discount)\n") &&
        writeFile(root + QStringLiteral("/tests/test_pricing.py"),
                  "from src.pricing import total\n\n\n"
                  "def test_small_order():\n"
                  "    assert total(2, 5.0) == 10.0\n") &&
        git(root, {QStringLiteral("add"), QStringLiteral(".")}) &&
        git(root, {QStringLiteral("commit"), QStringLiteral("-qm"),
                   QStringLiteral("seed")}) &&
        git(root, {QStringLiteral("checkout"), QStringLiteral("-q"),
                   QStringLiteral("-b"), QStringLiteral("agent/9301-invoice")}) &&
        writeFile(root + QStringLiteral("/src/pricing.py"),
                  "def total(quantity, unit_price, discount=0.0):\n"
                  "    subtotal = quantity * unit_price\n"
                  "    return subtotal * (1 - discount)\n") &&
        writeFile(root + QStringLiteral("/tests/test_pricing.py"),
                  "from src.pricing import total\n\n\n"
                  "def test_small_order():\n"
                  "    assert total(2, 5.0) == 10.0\n\n\n"
                  "def test_large_order_keeps_discount():\n"
                  "    assert total(11, 10.0, discount=0.2) == 88.0\n") &&
        git(root, {QStringLiteral("add"), QStringLiteral(".")}) &&
        git(root, {QStringLiteral("commit"), QStringLiteral("-qm"),
                   QStringLiteral("Apply the discount to every quantity")});
    check(repoReady, QStringLiteral("workspace fixture repository written"));

    window.testEnableSessionStartBypass(true);
    window.testSetSetupInputs(QStringLiteral("qa-tester"), QString());
    window.testStartSession();
    window.resize(1440, 900);
    window.show();
    settle();
    const int repoIndex = window.testAddLocalRepository(
        QStringLiteral("forkmesh"), QStringLiteral("workspace-demo"), root);
    check(repoIndex >= 0 && window.testOpenRepository(repoIndex),
          QStringLiteral("the workspace fixture repository opens"));
    settle();

    struct Row {
        int id;
        const char *title;
        QString status;
        int minutesAgo;
    };
    const Row rows[] = {
        {9301, "Invoice total ignores the discount above ten items",
         AgentStatus::Running, 2},
        {9302, "Add pagination to the audit log endpoint", AgentStatus::Success, 38},
        {9303, "Flaky upload test on Windows runners", AgentStatus::Failed, 95},
        {9304, "Rename the billing settings page", AgentStatus::Stopped, 180},
        {9305, "Draft release notes for 1.3", AgentStatus::Queued, 5},
        {9306, "Migrate the session store to SQLite", AgentStatus::Success, 1500},
    };
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const Row &row : rows) {
        AgentSession session;
        session.id = row.id;
        session.owner = QStringLiteral("forkmesh");
        session.name = QStringLiteral("workspace-demo");
        session.provider = QStringLiteral("claude-code");
        session.model = QStringLiteral("claude-opus-5-5");
        session.issueTitle = QString::fromLatin1(row.title);
        session.prompt = session.issueTitle;
        session.status = row.status;
        session.directCheckout = row.id != 9301;
        if (row.id == 9301) {
            session.branchName = QStringLiteral("agent/9301-invoice");
            session.baseBranch = QStringLiteral("main");
        }
        session.createdAtMs = now - qint64(row.minutesAgo) * 60 * 1000;
        window.testAddAgentSession(session);
    }
    window.testSetStreamWorktree(9301, root);
    window.testSetAgentTranscriptEvents(9301, demoTranscript());
    window.testOpenAgentsOverview();
    settle();
    window.testRefreshAgentTable();
    settle();
    window.testRefreshAgentDiffStats();
    settle(40);

    QWidget *listPane =
        window.findChild<QWidget *>(QStringLiteral("agentsListPane"));
    QTableWidget *table =
        listPane ? listPane->findChild<QTableWidget *>(QStringLiteral("issueTable"))
                 : nullptr;
    const auto select = [table](int sessionId) {
        if (!table)
            return false;
        for (int row = 0; row < table->rowCount(); ++row) {
            QTableWidgetItem *item = table->item(row, 0);
            if (item && item->data(Qt::UserRole).toInt() == sessionId) {
                table->clearSelection();
                table->selectRow(row);
                return true;
            }
        }
        return false;
    };
    const auto grab = [&window, &settle](const QString &dir, const QString &name,
                                         const QSize &size = QSize(1440, 900)) {
        if (window.size() != size) {
            window.resize(size);
            settle();
        }
        settle();
        QDir().mkpath(dir);
        return window.grab().save(QDir(dir).filePath(name + QStringLiteral(".png")));
    };
    const auto named = [&window](const char *name) {
        return window.findChild<QWidget *>(QLatin1String(name));
    };
    const auto press = [&window, &settle](const char *name) {
        auto *button = window.findChild<QPushButton *>(QLatin1String(name));
        if (!button)
            return false;
        button->click();
        settle();
        return true;
    };
    const auto shown = [&window](QWidget *widget) {
        return widget && widget->isVisibleTo(&window);
    };

    struct Variant {
        const char *design;
        const char *theme;
    };
    const Variant variants[] = {
        {"classic", "dark"}, {"next", "dark"}, {"next", "light"}};
    int saved = 0;
    const auto alertStack = [&window](const QString &dir, const QString &name,
                                      QWidget *below = nullptr,
                                      int *belowTop = nullptr) {
        for (int i = 0; i < 16; ++i)
            window.testFlashMessage(QStringLiteral("Mirror %1 caught up").arg(i),
                                    i % 2 == 0);
        for (int pass = 0; pass < 40; ++pass) {
            QApplication::processEvents();
            QThread::msleep(20);
        }
        QWidget *area = window.findChild<QWidget *>(QStringLiteral("pageNoticeArea"));
        const QRect stack = area && area->isVisible()
                                ? QRect(area->mapTo(&window, QPoint()), area->size())
                                : QRect();
        if (below && belowTop)
            *belowTop = below->mapTo(&window, QPoint()).y();
        QDir().mkpath(dir);
        window.grab().save(QDir(dir).filePath(name + QStringLiteral(".png")));
        window.testClearPageNotices();
        QApplication::processEvents();
        return stack;
    };
    for (const Variant &variant : variants) {
        const bool next = QLatin1String(variant.design) == QLatin1String("next");
        const QString label = QStringLiteral("%1-%2").arg(
            QLatin1String(variant.design), QLatin1String(variant.theme));
        const QString dir = QDir(outRoot).filePath(label);
        window.testSetAppViewMode(AppViewMode::Standard);
        settings.setValue(QStringLiteral("app/design"),
                          QLatin1String(variant.design));
        settings.setValue(QStringLiteral("app/theme"), QLatin1String(variant.theme));
        MainWindow::applyTheme();
        settle();
        check(Theme::isNext() == next,
              QStringLiteral("%1 is the active design").arg(label));

        window.testSetAppViewMode(AppViewMode::Workspace);
        settle();
        check(select(9301), QStringLiteral("the running session is in the list (%1)")
                                .arg(label));
        ClaudeTranscriptView *transcript =
            window.findChild<ClaudeTranscriptView *>();
        QElapsedTimer render;
        render.start();
        while (transcript && transcript->verticalScrollBar()->maximum() == 0 &&
               render.elapsed() < 2500)
            settle(2);
        settle();

        QWidget *sidebar = named("workspaceSidebarHeader");
        QWidget *header = named("workspaceHeader");
        QWidget *panelBar = named("workspacePanelBar");
        QWidget *dock = window.testWorkspaceTerminalDock();
        QSplitter *split = window.testAgentWorkspaceSplit();
        QWidget *toolbar = nullptr;
        if (auto *startAll = window.findChild<QPushButton *>(
                QStringLiteral("agentStartAllButton")))
            toolbar = startAll->parentWidget();
        QWidget *codePage = named("agentCodePage");
        QWidget *codeSplit = named("agentCodeSplit");
        QWidget *pill = named("agentStatusPill");
        QWidget *detail = named("agentsDetailPane");

        check(sidebar && header && panelBar && dock && split && toolbar &&
                  codePage && codeSplit && pill && detail,
              QStringLiteral("the workspace pieces exist (%1)").arg(label));
        if (!sidebar || !header || !panelBar || !dock || !split || !toolbar ||
            !codePage || !codeSplit || !pill || !detail)
            continue;
        check(split->count() == 3 && split->widget(2) == dock && shown(dock) &&
                  window.testWorkspaceTerminal() &&
                  dock->isAncestorOf(window.testWorkspaceTerminal()),
              QStringLiteral("Workspace keeps three panes with the terminal in "
                             "the third (%1)")
                  .arg(label));
        check(shown(sidebar) == next && shown(header) == next &&
                  shown(panelBar) == next && shown(toolbar) == !next,
              QStringLiteral("the new session list header, conversation header "
                             "and panel tabs show only in the new design (%1)")
                  .arg(label));
        const bool pillInside =
            detail->rect().intersects(QRect(pill->pos(), pill->size()));
        check(pillInside == !next,
              QStringLiteral("the classic action row is laid out only in the "
                             "classic design (%1)")
                  .arg(label));
        if (grab(dir, QStringLiteral("workspace-session")))
            ++saved;
        int headerTop = -1;
        const QRect alerts = alertStack(dir, QStringLiteral("workspace-alerts"),
                                        header, &headerTop);
        check(alerts.isValid() &&
                  (!header || (headerTop >= 0 && alerts.bottom() < headerTop)),
              QStringLiteral("a burst of notices sits in the layout above the "
                             "conversation header instead of over it (%1)")
                  .arg(label));
        if (next) {
            const QString edge = QStringLiteral("border-left: 2px solid");
            const bool plain = !qApp->styleSheet().contains(edge);
            settings.setValue(QLatin1String(Theme::kEdgeIndicatorSetting), true);
            MainWindow::applyTheme();
            settle();
            const bool edged = qApp->styleSheet().contains(edge);
            alertStack(dir, QStringLiteral("workspace-alerts-edges"));
            settings.remove(QLatin1String(Theme::kEdgeIndicatorSetting));
            MainWindow::applyTheme();
            settle();
            check(plain && edged && !qApp->styleSheet().contains(edge),
                  QStringLiteral("inline notices wear a coloured left edge only "
                                 "when edge indicators are on (%1)")
                      .arg(label));
        }

        if (!next) {
            check(codePage->parentWidget() == codeSplit,
                  QStringLiteral("the live diff stays beside the transcript in "
                                 "the classic design"));
            QWidget *classicWrapper = named("promptWrapper");
            auto *classicSend =
                classicWrapper ? classicWrapper->findChild<QPushButton *>(
                                     QStringLiteral("quickAddSendIcon"))
                               : nullptr;
            check(!shown(named("workspaceSend")) && classicSend &&
                      shown(classicSend) &&
                      !(table && table->property("workspaceCards").toBool()),
                  QStringLiteral("the classic design keeps its send buttons and "
                                 "list rows"));
            window.testSetAppViewMode(AppViewMode::Standard);
            settle();
            check(!shown(dock) && shown(toolbar),
                  QStringLiteral("Standard view hides the terminal pane in the "
                                 "classic design"));
            if (grab(dir, QStringLiteral("agents-standard")))
                ++saved;
            continue;
        }

        const int sidebarWidth = split->sizes().value(0);
        check(sidebarWidth >= 240 && sidebarWidth <= 340,
              QStringLiteral("the session list is a sidebar (%1 px, %2)")
                  .arg(sidebarWidth)
                  .arg(label));
        check(table && table->rowCount() == 6 && table->rowHeight(0) >= 70 &&
                  table->property("workspaceCards").toBool(),
              QStringLiteral("each session is a three-line card (%1 px, %2)")
                  .arg(table ? table->rowHeight(0) : 0)
                  .arg(label));
        QWidget *changesBar = named("workspaceChangesBar");
        QElapsedTimer stats;
        stats.start();
        while (changesBar && !shown(changesBar) && stats.elapsed() < 6000)
            settle(10);
        check(shown(changesBar),
              QStringLiteral("the changed-files card shows under a session "
                             "whose branch has edits (%1)")
                  .arg(label));
        QWidget *send = named("workspaceSend");
        QWidget *wrapper = named("promptWrapper");
        auto *oldSend = wrapper ? wrapper->findChild<QPushButton *>(
                                      QStringLiteral("quickAddSendIcon"))
                                : nullptr;
        check(shown(send) && wrapper && oldSend &&
                  !wrapper->rect().intersects(
                      QRect(oldSend->mapTo(wrapper, QPoint(0, 0)),
                            oldSend->size())),
              QStringLiteral("the docked composer has one round send button "
                             "(%1)")
                  .arg(label));

        check(press("workspaceToggleChanges") && shown(codePage) &&
                  dock->isAncestorOf(codePage) && shown(dock) &&
                  !shown(changesBar),
              QStringLiteral("Show diff opens the live diff in the side panel "
                             "and folds the card away (%1)")
                  .arg(label));
        settle(60);
        if (grab(dir, QStringLiteral("workspace-changes")))
            ++saved;

        check(press("workspaceToggleBrowser") && !shown(codePage) &&
                  shown(named("workspaceBrowserPane")),
              QStringLiteral("Browser replaces the diff in the side panel (%1)")
                  .arg(label));
        if (grab(dir, QStringLiteral("workspace-browser")))
            ++saved;

        check(press("workspaceToggleBrowser") && !shown(dock),
              QStringLiteral("pressing the open pane's button closes the side "
                             "panel (%1)")
                  .arg(label));
        if (grab(dir, QStringLiteral("workspace-conversation")))
            ++saved;

        check(press("workspaceToggleDetails") && shown(dock) &&
                  shown(named("workspaceDetailsHost")),
              QStringLiteral("Details shows the session panels in the side "
                             "panel (%1)")
                  .arg(label));
        if (grab(dir, QStringLiteral("workspace-details")))
            ++saved;
        {
            forkmesh::ui::SessionSidePanels panels;
            panels.resize(320, 1500);
            panels.show();
            forkmesh::ui::SessionSidePanels::Details details;
            details.session = true;
            details.machine = QStringLiteral("desk");
            details.machineNote = QStringLiteral("this computer");
            details.machineIcon = QStringLiteral("device-desktop");
            details.branch = QStringLiteral("agent/9301-invoice");
            details.agent = QStringLiteral("Claude Code · Opus 5.5");
            details.contextUsed = 127500;
            details.contextWindow = 1000000;
            details.promptTokens = 412000;
            details.completionTokens = 38000;
            details.totalTokens = 450000;
            details.turns = 14;
            details.costUsd = 3.42;
            details.remoteAvailable = true;
            details.remotePhones = {QStringLiteral("Pixel 9")};
            details.remoteLocal = true;
            bool toggled = false;
            panels.setRemoteActions([&toggled](bool on) { toggled = !on; }, [] {});
            panels.setDetails(details);
            settle();
            QStringList rows;
            for (QLabel *swatch : panels.findChildren<QLabel *>(
                     QStringLiteral("sessionsDetailSwatch")))
                if (swatch->isVisible())
                    rows << swatch->parentWidget()
                                ->findChild<QLabel *>(QStringLiteral("sessionsDetailText"))
                                ->text();
            check(rows == QStringList{QStringLiteral("Used"), QStringLiteral("Free space"),
                                      QStringLiteral("Input tokens"),
                                      QStringLiteral("Output tokens"), QStringLiteral("Turns"),
                                      QStringLiteral("Cost")},
                  QStringLiteral("the Context card lists coloured rows (%1): %2")
                      .arg(label, rows.join(QStringLiteral(", "))));
            auto *used = panels.findChild<QLabel *>(QStringLiteral("sessionsContextUsed"));
            auto *status = panels.findChild<QLabel *>(QStringLiteral("sessionsRemoteStatus"));
            auto *remote = panels.findChild<QAbstractButton *>(
                QStringLiteral("sessionsRemoteSwitch"));
            check(used && used->text() == QStringLiteral("13% used") && status &&
                      status->text().contains(QStringLiteral("Pixel 9")) &&
                      status->text().contains(QStringLiteral("on this network")) && remote &&
                      remote->isChecked() && remote->isEnabled(),
                  QStringLiteral("the Remote control switch shows the connected phone (%1)")
                      .arg(label));
            QDir().mkpath(dir);
            if (panels.grab().save(
                    QDir(dir).filePath(QStringLiteral("session-panels-phone.png"))))
                ++saved;
            if (remote)
                remote->click();
            check(toggled, QStringLiteral("clicking Remote control asks to turn it off (%1)")
                               .arg(label));
            details.remotePhones.clear();
            panels.setDetails(details);
            settle();
            check(status && status->text().contains(QStringLiteral("Connect a phone")),
                  QStringLiteral("with no phone the panel says how to connect one (%1)")
                      .arg(label));
            details.contextUsed = 960000;
            details.remoteOn = false;
            panels.setDetails(details);
            settle();
            check(remote && !remote->isChecked() && status &&
                      status->text().startsWith(QStringLiteral("Off.")),
                  QStringLiteral("Remote control off says phones are refused (%1)")
                      .arg(label));
            if (panels.grab().save(
                    QDir(dir).filePath(QStringLiteral("session-panels-no-phone.png"))))
                ++saved;
        }

        check(press("workspaceToggleTerminal") && shown(dock) &&
                  shown(window.testWorkspaceTerminal()),
              QStringLiteral("Terminal brings the side panel back (%1)").arg(label));
        if (grab(dir, QStringLiteral("workspace-narrow"), QSize(1100, 720)))
            ++saved;
        const QWidget *more = named("workspaceSessionMore");
        check(more && shown(header) &&
                  header->rect().contains(
                      QRect(more->mapTo(header, QPoint(0, 0)), more->size())),
              QStringLiteral("the conversation header keeps its controls inside "
                             "a narrow window (%1)")
                  .arg(label));
        window.resize(1440, 900);
        settle();

        check(press("workspaceNewSession") &&
                  window.testSelectedAgentSessionId() < 0 &&
                  shown(named("workspaceEmpty")),
              QStringLiteral("New session clears the selection and shows the "
                             "empty conversation (%1)")
                  .arg(label));
        if (grab(dir, QStringLiteral("workspace-new")))
            ++saved;

        window.testSetAppViewMode(AppViewMode::Standard);
        settle();
        select(9302);
        settle(20);
        check(!shown(dock) && shown(sidebar) && shown(header),
              QStringLiteral("Standard view keeps the new Agents layout without "
                             "the side panel (%1)")
                  .arg(label));
        if (grab(dir, QStringLiteral("agents-standard")))
            ++saved;
    }
    {
        using forkmesh::ui::NextAccess;
        const QString dir = QDir(outRoot).filePath(QStringLiteral("next-dark"));
        window.testSetAppViewMode(AppViewMode::Standard);
        settings.setValue(QStringLiteral("app/design"), QStringLiteral("next"));
        settings.setValue(QStringLiteral("app/theme"), QStringLiteral("dark"));
        MainWindow::applyTheme();
        settle();
        window.testSetAppViewMode(AppViewMode::Workspace);
        settle();
        select(9301);
        settle(20);
        QWidget *sidebar = named("workspaceSidebarHeader");
        QWidget *header = named("workspaceHeader");
        QWidget *codePage = named("agentCodePage");
        QWidget *codeSplit = named("agentCodeSplit");
        QWidget *dock = window.testWorkspaceTerminalDock();
        QWidget *toolbar = nullptr;
        if (auto *startAll = window.findChild<QPushButton *>(
                QStringLiteral("agentStartAllButton")))
            toolbar = startAll->parentWidget();
        check(!forkmesh::ui::workspaceLayoutClassic() && shown(sidebar) &&
                  shown(header),
              QStringLiteral("the Focus layout is the default in the new "
                             "interface"));

        forkmesh::ui::setLayoutMode(forkmesh::ui::LayoutMode::ControlTower);
        NextAccess::syncWorkspace(window);
        settle(20);
        check(Theme::isNext() && !shown(sidebar) && !shown(header) &&
                  shown(toolbar) && !shown(named("workspaceSend")) &&
                  !shown(named("workspacePanelBar")) && shown(dock) &&
                  codePage && codePage->parentWidget() == codeSplit &&
                  !(table && table->property("workspaceCards").toBool()),
              QStringLiteral("the Control Tower layout brings the earlier "
                             "Agents page back inside the new interface"));
        check(settings.value(QStringLiteral("app/layoutMode")).toString() ==
                      QLatin1String("controlTower") &&
                  forkmesh::ui::gitLayoutCompact(),
              QStringLiteral("the Control Tower choice is saved and covers the "
                             "Git page too"));
        if (grab(dir, QStringLiteral("workspace-classic-layout")))
            ++saved;
        const auto towerPanels = [codeSplit]() -> QWidget * {
            return codeSplit ? codeSplit->findChild<QWidget *>(
                                   QStringLiteral("sessionsDetails"),
                                   Qt::FindDirectChildrenOnly)
                             : nullptr;
        };
        const auto subSessionsCard = [&towerPanels]() -> QWidget * {
            QWidget *panels = towerPanels();
            if (!panels)
                return nullptr;
            for (QFrame *card : panels->findChildren<QFrame *>(
                     QStringLiteral("sessionsDetailCard")))
                if (card->property("sessionsPanel").toString() ==
                    QLatin1String("subsessions"))
                    return card;
            return nullptr;
        };
        window.testSetAppViewMode(AppViewMode::Standard);
        settle();
        select(9301);
        settle(20);
        check(shown(towerPanels()) && shown(subSessionsCard()) &&
                  !named("agentSubSessionStrip"),
              QStringLiteral("Control Tower shows the side panels, Sub-sessions "
                             "among them, beside the transcript instead of a "
                             "strip above it"));
        if (grab(dir, QStringLiteral("agents-control-tower")))
            ++saved;
        window.testSetAppViewMode(AppViewMode::Workspace);
        settle(20);

        forkmesh::ui::setLayoutMode(forkmesh::ui::LayoutMode::Focus);
        NextAccess::syncWorkspace(window);
        settle(20);
        check(shown(sidebar) && shown(header) && !shown(toolbar) &&
                  shown(named("workspaceSend")) && codePage &&
                  dock->isAncestorOf(codePage) &&
                  table && table->property("workspaceCards").toBool(),
              QStringLiteral("switching back to Focus restores the new layout"));
        check(!shown(towerPanels()),
              QStringLiteral("Focus hides the Control Tower side panels and "
                             "keeps them in its Details tab"));
        if (grab(dir, QStringLiteral("workspace-new-layout-again")))
            ++saved;
    }
    check(saved > 0, QStringLiteral("workspace snapshots were written (%1 files "
                                    "in %2)")
                         .arg(saved)
                         .arg(outRoot));

    window.testSetAppViewMode(AppViewMode::Standard);
    const auto restore = [&settings](const QString &key, const QVariant &value) {
        if (value.isValid())
            settings.setValue(key, value);
        else
            settings.remove(key);
    };
    forkmesh::ui::setLayoutMode(forkmesh::ui::LayoutMode::Focus);
    restore(QStringLiteral("app/design"), savedDesign);
    restore(QStringLiteral("app/theme"), savedTheme);
    MainWindow::applyTheme();
    settle();
    return failures;
}
