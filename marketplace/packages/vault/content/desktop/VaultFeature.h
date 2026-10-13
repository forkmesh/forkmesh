// Password Vault: the desktop rail page. Screen logic only; the keys, the
// items, and every rule about them live in VaultClient and VaultCrypto.
#pragma once

#include "NativePackageExtension.h"
#include "VaultClient.h"

#include <QList>
#include <QString>

#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QTimer;

namespace forkmesh::vault {

class VaultFeature : public forkmesh::market::NativePackageWidget
{
public:
    explicit VaultFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~VaultFeature() override;

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    // A locked vault has nothing to search and nothing to warm: the top
    // search box must never be what prompts for a master password.
    void prepareSearch() override {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum Panel { LoadingPanel, SetupPanel, UnlockPanel, AppPanel };

    void buildSetupPanel(QWidget *panel);
    void buildUnlockPanel(QWidget *panel);
    void buildAppPanel(QWidget *panel);
    void showPanel(Panel panel);
    void setStatus(const QString &text, bool error = false);
    void setBusy(bool busy);
    QString explain(const QString &code) const;

    void load();
    void createVault();
    void unlockVault();
    void recoverVault();
    void enter();
    void lockVault(const QString &note = QString());
    void touch();
    void reload(std::function<void()> then = {});
    void renderItems();
    QString selectedId() const;

    void editItem(const QString &id);
    void deleteItem(const QString &id);
    void shareItem(const QString &id);
    void copyField(const QString &id, const char *field, const QString &label);
    void openSite(const QString &id);
    void showRecoveryCode(const QString &code);
    void openSettings();
    bool newPasswordFrom(QLineEdit *first, QLineEdit *second, QString *password);

    std::function<void()> m_searchIndexChanged;
    std::unique_ptr<VaultClient> m_client;
    QStackedWidget *m_stack = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_lockButton = nullptr;
    QPushButton *m_settingsButton = nullptr;
    QLineEdit *m_setupPassword = nullptr;
    QLineEdit *m_setupConfirm = nullptr;
    QPushButton *m_setupButton = nullptr;
    QLineEdit *m_unlockPassword = nullptr;
    QPushButton *m_unlockButton = nullptr;
    QLineEdit *m_search = nullptr;
    QComboBox *m_filter = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel *m_empty = nullptr;
    QList<QPushButton *> m_rowButtons;
    QTimer *m_idleTimer = nullptr;
    QTimer *m_clipboardTimer = nullptr;
    QString m_clipboardValue;
    QString m_hostQuery;
    int m_autoLockMinutes = 10;
    bool m_busy = false;
    bool m_loaded = false;
};

}  // namespace forkmesh::vault
