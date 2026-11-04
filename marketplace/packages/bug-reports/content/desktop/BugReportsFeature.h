#pragma once

#include "NativePackageExtension.h"

#include "BugReportsModel.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QVBoxLayout;

namespace forkmesh::bugreports {

enum class Tab { Report, Mine, Reported };

class BugReportsFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit BugReportsFeature(const forkmesh::market::NativePackageHost &host,
                               QWidget *parent = nullptr);
    void refresh() override;
    void activate() override;
    int navigationBadge() const override;
    void prepareNavigationBadge() override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query,
                                                               int limit) const override;
    void activateSearchHit(const QString &id) override;
    void prepareSearch() override;

    void showTab(Tab tab);
    Tab currentTab() const { return m_tab; }
    bool admin() const { return m_admin; }

private:
    void request(const QByteArray &method, const QString &path, const QJsonObject &body,
                 forkmesh::market::NativePackageReplyHandler handler);
    QWidget *buildReportPane();
    void submit();
    void loadMine();
    void loadInbox();
    void setBadge(int count);
    void renderMine();
    void renderInbox();
    void renderTabs();
    void addState(QVBoxLayout *layout, const QString &title, const QString &body);
    QWidget *reportCard(const BugReport &report, bool inbox);
    void confirmFixed(const QString &id);
    void setInboxStatus(const QString &id, const QString &status);
    void setInboxFilter(const QString &status);
    void setNotice(QLabel *label, const QString &text, const QString &tone);

    forkmesh::market::NativePackageHost m_host;
    Tab m_tab = Tab::Report;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_tabReport = nullptr;
    QPushButton *m_tabMine = nullptr;
    QPushButton *m_tabReported = nullptr;
    QStackedWidget *m_stack = nullptr;
    QPlainTextEdit *m_message = nullptr;
    QComboBox *m_severity = nullptr;
    QLineEdit *m_contact = nullptr;
    QPushButton *m_send = nullptr;
    QLabel *m_reportNotice = nullptr;
    QLabel *m_mineNotice = nullptr;
    QLabel *m_inboxNotice = nullptr;
    QComboBox *m_inboxFilter = nullptr;
    QScrollArea *m_mineScroll = nullptr;
    QScrollArea *m_inboxScroll = nullptr;
    ReportList m_mine;
    ReportList m_inbox;
    QString m_inboxStatus = QStringLiteral("new");
    QString m_busyReport;
    QString m_mineError;
    QString m_inboxError;
    int m_badge = 0;
    bool m_sending = false;
    bool m_mineLoading = false;
    bool m_mineLoaded = false;
    bool m_inboxLoading = false;
    bool m_inboxLoaded = false;
    bool m_admin = false;
    bool m_signedOut = false;
};

}
