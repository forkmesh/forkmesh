#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QWidget>

class CoreClient;
class QCheckBox;
class QLabel;
class QVBoxLayout;

class CoreDaemonSettings : public QWidget
{
public:
    explicit CoreDaemonSettings(CoreClient *client, QWidget *parent = nullptr);

private:
    void refresh();
    void applyState(const QJsonObject &state);
    void applyPlugins(const QJsonArray &plugins);
    void setDaemon(const QJsonObject &params);
    void togglePlugin(const QJsonObject &plugin, bool enable);

    QPointer<CoreClient> m_client;
    QCheckBox *m_background = nullptr;
    QCheckBox *m_autostart = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_servicesLabel = nullptr;
    QVBoxLayout *m_services = nullptr;
    bool m_applying = false;
};
