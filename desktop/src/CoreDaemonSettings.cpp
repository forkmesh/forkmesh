#include "CoreDaemonSettings.h"

#include "CoreClient.h"

#include <QCheckBox>
#include <QDir>
#include <QLabel>
#include <QLayoutItem>
#include <QMessageBox>
#include <QStringList>
#include <QVBoxLayout>

namespace {

QString exitText(const QJsonObject &lastExit)
{
    const QString kind = lastExit.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("crash"))
        return QStringLiteral("recovered after an unclean stop");
    if (kind == QLatin1String("clean"))
        return QStringLiteral("last stop was clean");
    return QString();
}

QString permissionsText(const QJsonArray &permissions)
{
    QStringList lines;
    for (const QJsonValue &value : permissions) {
        const QJsonObject permission = value.toObject();
        const QString label = permission.value(QStringLiteral("label")).toString();
        const QString risk = permission.value(QStringLiteral("risk")).toString();
        lines << QStringLiteral("• %1%2").arg(
            label.isEmpty() ? permission.value(QStringLiteral("id")).toString() : label,
            risk.isEmpty() ? QString() : QStringLiteral(" (%1 risk)").arg(risk));
    }
    return lines.isEmpty() ? QStringLiteral("• no extra permissions; no network access")
                           : lines.join(QLatin1Char('\n'));
}

QString stateText(const QJsonObject &plugin)
{
    const QString state = plugin.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("running"))
        return QStringLiteral("running");
    if (state == QLatin1String("stopped"))
        return plugin.value(QStringLiteral("start")).toString() == QLatin1String("boot")
                   ? QStringLiteral("starting")
                   : QStringLiteral("starts when used");
    if (state == QLatin1String("needs_consent"))
        return QStringLiteral("updated; asks for new permissions");
    if (state == QLatin1String("untrusted"))
        return QStringLiteral("signed by an untrusted publisher");
    if (state == QLatin1String("invalid"))
        return plugin.value(QStringLiteral("problem")).toString();
    if (state == QLatin1String("failed"))
        return QStringLiteral("stopped after repeated crashes");
    return QStringLiteral("off");
}

}

CoreDaemonSettings::CoreDaemonSettings(CoreClient *client, QWidget *parent)
    : QWidget(parent), m_client(client)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_background = new QCheckBox(QStringLiteral("Keep the background engine running after ForkMesh closes"));
    m_background->setToolTip(QStringLiteral(
        "forkmesh-core keeps phone connections, host jobs, terminals, agents and background "
        "services running while the window is closed. When off it stops two minutes after the "
        "window closes, once its work is done."));
    m_autostart = new QCheckBox(QStringLiteral("Start the background engine when the computer starts"));
    m_autostart->setToolTip(QStringLiteral(
        "Registers forkmesh-core with the system so background work resumes after a restart "
        "without opening the window. On Linux it is a systemd user unit with lingering turned on, "
        "so it starts at boot before anyone logs in; on macOS (a login item) and Windows (the Run "
        "key) it starts when you sign in."));
    m_headless = new QCheckBox(QStringLiteral("Run ForkMesh without a window while none is open"));
    m_headless->setToolTip(QStringLiteral(
        "While the background engine runs and no window is open, it starts ForkMesh without a "
        "window, so your phone can list, start and prompt agents and use everything else the "
        "desktop does. Opening ForkMesh takes over from it. Linux only."));
    m_headless->hide();
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("modeHint"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_servicesLabel = new QLabel(QStringLiteral("Background services"));
    m_servicesLabel->setObjectName(QStringLiteral("modeHint"));
    m_services = new QVBoxLayout;
    m_services->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_background);
    layout->addWidget(m_autostart);
    layout->addWidget(m_headless);
    layout->addWidget(m_status);
    layout->addWidget(m_servicesLabel);
    layout->addLayout(m_services);
    m_servicesLabel->hide();

    connect(m_background, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_applying)
            setDaemon({{QStringLiteral("background"), on}});
    });
    connect(m_autostart, &QCheckBox::toggled, this, [this](bool on) {
        if (m_applying)
            return;
        QJsonObject params{{QStringLiteral("autostart"), on}};
        if (on && m_client)
            params.insert(QStringLiteral("binary"), QDir::cleanPath(m_client->binaryPath()));
        setDaemon(params);
    });
    connect(m_headless, &QCheckBox::toggled, this, [this](bool on) {
        if (!m_applying)
            setDaemon({{QStringLiteral("headlessDesktop"), on}});
    });
    if (m_client) {
        connect(m_client, &CoreClient::connectedChanged, this, [this](bool) { refresh(); });
        connect(m_client, &CoreClient::coreEvent, this,
                [this](const QString &event, const QJsonObject &data) {
                    if (event == QLatin1String("core.daemon.changed"))
                        applyState(data);
                    else if (event == QLatin1String("core.plugins.changed"))
                        applyPlugins(data.value(QStringLiteral("plugins")).toArray());
                });
    }
    refresh();
}

void CoreDaemonSettings::refresh()
{
    const bool connected = m_client && m_client->isConnected() &&
                           m_client->hasNativeRoute(QStringLiteral("core.daemon.get"));
    m_background->setEnabled(connected);
    m_autostart->setEnabled(connected);
    m_headless->setEnabled(connected);
    if (!connected) {
        m_status->setText(m_client ? QStringLiteral("The background engine is not running.")
                                   : QStringLiteral("The background engine is turned off."));
        applyPlugins({});
        return;
    }
    QPointer<CoreDaemonSettings> self(this);
    m_client->request(QStringLiteral("core.daemon.get"), {},
                      [self](bool ok, const QJsonValue &result, const QString &) {
                          if (self && ok)
                              self->applyState(result.toObject());
                      });
    if (m_client->hasNativeRoute(QStringLiteral("core.plugins.list")))
        m_client->request(QStringLiteral("core.plugins.list"), {},
                          [self](bool ok, const QJsonValue &result, const QString &) {
                              if (self && ok)
                                  self->applyPlugins(
                                      result.toObject().value(QStringLiteral("plugins")).toArray());
                          });
}

void CoreDaemonSettings::applyState(const QJsonObject &state)
{
    m_applying = true;
    m_background->setChecked(state.value(QStringLiteral("background")).toBool());
    const QJsonObject autostart = state.value(QStringLiteral("autostart")).toObject();
    m_autostart->setChecked(autostart.value(QStringLiteral("enabled")).toBool());
    const QJsonObject headless = state.value(QStringLiteral("headlessDesktop")).toObject();
    m_headless->setChecked(headless.value(QStringLiteral("enabled")).toBool());
    m_headless->setVisible(headless.value(QStringLiteral("supported")).toBool());
    m_applying = false;
    QStringList parts;
    if (m_client)
        parts << QStringLiteral("forkmesh-core %1 (pid %2)").arg(m_client->version()).arg(m_client->pid());
    if (state.value(QStringLiteral("backgroundFlag")).toBool())
        parts << QStringLiteral("started at login");
    const QString exit = exitText(state.value(QStringLiteral("lastExit")).toObject());
    if (!exit.isEmpty())
        parts << exit;
    QString text = parts.join(QStringLiteral(" · "));
    if (autostart.value(QStringLiteral("enabled")).toBool()) {
        const bool atBoot = autostart.value(QStringLiteral("atBoot")).toBool();
        const QString mechanism = autostart.value(QStringLiteral("mechanism")).toString();
        text += QStringLiteral("\n%1 through %2:\n%3")
                    .arg(atBoot ? QStringLiteral("Starts when the computer starts")
                                : QStringLiteral("Starts when you log in"),
                         mechanism, autostart.value(QStringLiteral("location")).toString());
        if (!atBoot && mechanism == QLatin1String("systemd-user"))
            text += QStringLiteral("\nTo start it before you log in, run: loginctl enable-linger");
    }
    m_status->setText(text);
}

void CoreDaemonSettings::applyPlugins(const QJsonArray &plugins)
{
    while (QLayoutItem *item = m_services->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    m_servicesLabel->setVisible(!plugins.isEmpty());
    for (const QJsonValue &value : plugins) {
        const QJsonObject plugin = value.toObject();
        const QString state = plugin.value(QStringLiteral("state")).toString();
        auto *check = new QCheckBox(QStringLiteral("%1 — %2")
                                        .arg(plugin.value(QStringLiteral("name")).toString(),
                                             stateText(plugin)));
        check->setChecked(plugin.value(QStringLiteral("enabled")).toBool() &&
                          state != QLatin1String("needs_consent"));
        check->setEnabled(state != QLatin1String("invalid") && state != QLatin1String("untrusted"));
        check->setToolTip(QStringLiteral("%1\n\nPermissions:\n%2")
                              .arg(plugin.value(QStringLiteral("summary")).toString(),
                                   permissionsText(plugin.value(QStringLiteral("permissions")).toArray())));
        connect(check, &QCheckBox::toggled, this,
                [this, plugin](bool on) { togglePlugin(plugin, on); });
        m_services->addWidget(check);
    }
}

void CoreDaemonSettings::setDaemon(const QJsonObject &params)
{
    if (!m_client)
        return;
    QPointer<CoreDaemonSettings> self(this);
    m_client->request(QStringLiteral("core.daemon.set"), params,
                      [self](bool ok, const QJsonValue &result, const QString &error) {
                          if (!self)
                              return;
                          if (!ok) {
                              QMessageBox::warning(self, QStringLiteral("Background engine"),
                                                   QStringLiteral("The change was not saved: %1").arg(error));
                              self->refresh();
                              return;
                          }
                          self->applyState(result.toObject());
                      });
}

void CoreDaemonSettings::togglePlugin(const QJsonObject &plugin, bool enable)
{
    if (!m_client)
        return;
    const QString id = plugin.value(QStringLiteral("id")).toString();
    if (enable) {
        const QString question =
            QStringLiteral("Run %1 %2 in the background?\n\nIt runs as a separate process with its own "
                           "data folder and can use:\n%3\n\nSigned by %4.")
                .arg(plugin.value(QStringLiteral("name")).toString(),
                     plugin.value(QStringLiteral("version")).toString(),
                     permissionsText(plugin.value(QStringLiteral("permissions")).toArray()),
                     plugin.value(QStringLiteral("signer")).toString().left(16) + QStringLiteral("…"));
        if (QMessageBox::question(this, QStringLiteral("Background service"), question) != QMessageBox::Yes) {
            refresh();
            return;
        }
    }
    QPointer<CoreDaemonSettings> self(this);
    m_client->request(enable ? QStringLiteral("core.plugins.enable") : QStringLiteral("core.plugins.disable"),
                      {{QStringLiteral("id"), id}},
                      [self](bool ok, const QJsonValue &result, const QString &error) {
                          if (!self)
                              return;
                          if (!ok)
                              QMessageBox::warning(self, QStringLiteral("Background service"),
                                                   QStringLiteral("The service did not change: %1").arg(error));
                          self->applyPlugins(result.toObject().value(QStringLiteral("plugins")).toArray());
                          if (!ok)
                              self->refresh();
                      });
}
