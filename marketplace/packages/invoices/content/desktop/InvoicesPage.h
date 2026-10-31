#pragma once

#include "NativePackageExtension.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>

class QComboBox;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QScrollArea;
class QSplitter;
class QStackedWidget;
class QTableWidget;
class QVBoxLayout;

namespace forkmesh::invoices {

class InvoicesPage final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler = std::function<void(bool, const QJsonObject &, const QString &)>;

    explicit InvoicesPage(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    int navigationBadge() const override;
    void prepareNavigationBadge() override;
    void themeChanged() override;

protected:
    void showEvent(QShowEvent *event) override;

private:
    void request(const QByteArray &method, const QString &path, const QJsonObject &body, ReplyHandler handler);
    QString organization() const;
    QString organizationPath(const QString &suffix = QString()) const;
    QJsonObject organizationRecord() const;
    bool installed() const;
    bool canManage() const;
    QString defaultCurrency() const;
    QJsonObject invoiceById(const QString &id) const;

    void refreshOrganizations();
    void loadOverview();
    void applyOverview(const QJsonObject &payload);
    void applyState();
    void arrange(bool controlTower);
    void applyStyle();
    void renderSummary();
    void renderList();
    void renderActivity();
    void openInvoice(const QString &id);
    void closeDetail();
    void renderDetail();
    void addDetailActions(QHBoxLayout *primary, QHBoxLayout *secondary);
    void invoiceChanged(const QJsonObject &invoice, const QString &notice);
    void loadContacts(std::function<void()> done);

    void installPlugin();
    void openEditor(const QJsonObject &invoice, const QJsonObject &seed);
    void openSendDialog();
    void openPaymentDialog();
    void runStatusAction(const QString &action);
    void deleteInvoice();
    void duplicateInvoice();
    void openSettings();
    void copyPublicLink();
    void openPublicLink();
    void openWebPage();
    void setStatus(const QString &text, bool error = false);

    forkmesh::market::NativePackageHost m_host;
    std::function<void()> m_searchIndexChanged;
    QLabel *m_status = nullptr;
    QComboBox *m_organization = nullptr;
    QPushButton *m_newButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_webButton = nullptr;
    QWidget *m_installPanel = nullptr;
    QLabel *m_installNote = nullptr;
    QPushButton *m_installButton = nullptr;
    QWidget *m_content = nullptr;
    QList<QFrame *> m_tiles;
    QList<QLabel *> m_tileNames;
    QList<QLabel *> m_tileValues;
    QList<QLabel *> m_tileDetails;
    QWidget *m_listPanel = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_filter = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_emptyNote = nullptr;
    QWidget *m_activityPanel = nullptr;
    QListWidget *m_activity = nullptr;
    QWidget *m_detailPanel = nullptr;
    QPushButton *m_backButton = nullptr;
    QScrollArea *m_detailScroll = nullptr;
    QWidget *m_detailActions = nullptr;
    QLabel *m_detailPlaceholder = nullptr;
    QStackedWidget *m_modeStack = nullptr;
    QSplitter *m_towerSplit = nullptr;
    QSplitter *m_towerLeft = nullptr;
    QStackedWidget *m_focusStack = nullptr;
    QSplitter *m_focusList = nullptr;
    QWidget *m_focusDetail = nullptr;

    QJsonArray m_organizations;
    QJsonObject m_overview;
    QJsonArray m_invoices;
    QJsonObject m_detail;
    QString m_detailId;
    QJsonArray m_contacts;
    bool m_contactsLoaded = false;
    bool m_loading = false;
    bool m_loaded = false;
    int m_serial = 0;
    int m_detailSerial = 0;
    int m_arranged = -1;
    QString m_hostQuery;
};

}
