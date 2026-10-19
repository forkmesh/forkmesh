#pragma once

#include "NativePackageExtension.h"

#include "RunnersModel.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

class QFrame;
class QLabel;
class QPushButton;
class QScrollArea;
class QVBoxLayout;

namespace forkmesh::runnerspage {

class RunnersFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit RunnersFeature(const forkmesh::market::NativePackageHost &host,
                            QWidget *parent = nullptr);
    void refresh() override;
    int navigationBadge() const override;
    void prepareNavigationBadge() override;

private:
    void request(const QByteArray &method, const QString &path, const QJsonObject &body,
                 forkmesh::market::NativePackageReplyHandler handler);
    void loadAttention();
    void setBadge(int count);
    void render();
    void addState(QVBoxLayout *layout, const QString &title, const QString &body,
                  const char *name);
    QWidget *runnerWidget(const RunnerRow &row, bool first);
    QWidget *providerWidget(const RunnerRow &runner, const ProviderRow &provider);
    void runAction(RunnerAction action, const QString &runner);
    void confirmRevoke(const QString &runner);
    void changeAccount(const QString &runner, const QString &provider,
                       const AccountChoice &account, const QString &action);
    void confirmRemoveAccount(const QString &runner, const QString &provider,
                              const AccountChoice &account);
    void signIn(const QString &runner, const QString &provider);
    void openPage(const QString &path);
    void setStatus(const QString &text);
    void setError(const QString &text);

    forkmesh::market::NativePackageHost m_host;
    QLabel *m_status = nullptr;
    QFrame *m_error = nullptr;
    QLabel *m_errorText = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_add = nullptr;
    QScrollArea *m_scroll = nullptr;
    QJsonObject m_payload;
    QString m_busyRunner;
    QString m_notice;
    int m_badge = 0;
    bool m_loaded = false;
    bool m_loading = false;
    bool m_signedOut = false;
    bool m_attentionLoading = false;
    bool m_attentionLoaded = false;
};

}
