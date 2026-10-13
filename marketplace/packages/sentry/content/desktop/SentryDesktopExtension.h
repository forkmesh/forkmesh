#ifndef FORKMESH_SENTRY_DESKTOP_EXTENSION_H
#define FORKMESH_SENTRY_DESKTOP_EXTENSION_H

#include "DesktopPackageExtension.h"

#include <QObject>

class QAbstractButton;
class QButtonGroup;
class QLabel;
class QPushButton;
class QTextBrowser;

namespace forkmesh::sentry { struct Connection; }

class SentryClient;

class SentryDesktopExtension final
    : public QObject,
      public forkmesh::market::DesktopPackageExtension
{
public:
    QString packageId() const override;
    QList<forkmesh::market::DesktopLogCategory> logCategories() const override;
    QString logPageTitle() const override;
    QString logPageToolTip() const override;
    void activate(const forkmesh::market::DesktopPackageContext &context) override;
    void deactivate() override;
    QWidget *createLogPage(QWidget *parent) override;
    QWidget *logPage() const override { return m_logPage; }
    void refresh() override;
    QJsonObject remoteSnapshot() const override;

private:
    forkmesh::sentry::Connection storedConnection() const;
    void startClient();
    void showConnectionDialog();
    void disconnectAccount();
    void updateConnectButton(const QString &status = QString(),
                             bool connected = false, bool error = false);
    void setStatsPeriod(const QString &period);

    forkmesh::market::DesktopPackageContext m_context;
    QWidget *m_logPage = nullptr;
    QTextBrowser *m_logView = nullptr;
    QLabel *m_logStatus = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_browserButton = nullptr;
    QButtonGroup *m_periodGroup = nullptr;
    SentryClient *m_client = nullptr;
    bool m_active = false;
};

namespace forkmesh::market {
DesktopPackageExtension *createSentryDesktopExtension();
}

#endif
