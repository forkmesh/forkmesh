#pragma once

#include "NativePackageExtension.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

class QLabel;
class QPushButton;
class QStackedWidget;

namespace forkmesh::spreadsheets {

class SpreadsheetsPage final : public forkmesh::market::NativePackageWidget
{
public:
    explicit SpreadsheetsPage(const forkmesh::market::NativePackageHost &host,
                              QWidget *parent = nullptr);
    ~SpreadsheetsPage() override;

    void refresh() override;
    void prepareNavigationBadge() override;
    void themeChanged() override;
    bool acceptsHostCommand(const QString &command) const override;
    bool hostCommand(const QString &command, const QJsonObject &args) override;

protected:
    void showEvent(QShowEvent *event) override;

private:
    QUrl pageUrl(const QString &path) const;
    void sync(bool create);
    void requestSession();
    void build(const QUrl &origin, const QString &account, const QString &token,
               const QByteArray &session);
    void drop();
    void load(const QString &path);
    void decide(const QUrl &url, bool linkClicked, bool mainFrame);
    void openLink(const QUrl &url);
    void openInBrowser();
    void showMessage(const QString &title, const QString &body, bool offerBrowser,
                     bool offerReload);
    QString askSavePath(const QString &title, const QString &name, const QString &filter);
    void saved(const QString &path, bool ok);

    forkmesh::market::NativePackageHost m_host;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_message = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_body = nullptr;
    QPushButton *m_reload = nullptr;
    QPushButton *m_browser = nullptr;
    QWidget *m_view = nullptr;
    QObject *m_profile = nullptr;
    QUrl m_origin;
    QByteArray m_session;
    QString m_pending;
    bool m_sessionPending = false;
};

}
