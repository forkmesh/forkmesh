#pragma once

#include "NativePackageExtension.h"
#include "SecureDriveStore.h"

#include <QList>
#include <QPointer>
#include <QString>

#include <functional>
#include <memory>

class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QFutureWatcherBase;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTimer;
class QTreeWidget;
class QWidget;

namespace forkmesh::securedrive {

class EditorWindow;

class SecureDriveFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit SecureDriveFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~SecureDriveFeature() override;

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    void prepareSearch() override {}

    struct Outcome
    {
        bool ok = false;
        QString code;
        QString text;
        QByteArray bytes;
    };

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    enum Panel { SetupPanel, UnlockPanel, MainPanel };

    struct ExternalCopy
    {
        QString slot;
        QString fileId;
        QString name;
        QString path;
        QByteArray digest;
    };

    QWidget *buildSetupPanel();
    QWidget *buildUnlockPanel();
    QWidget *buildMainPanel();
    QWidget *buildFilesTab();
    QWidget *buildActivityTab();
    QWidget *buildDevicesTab();
    void showPanel(Panel panel);
    void setStatus(const QString &text, bool error = false);
    QString explain(const QString &code) const;
    void run(const QString &busyText, std::function<Outcome()> work,
             std::function<void(const Outcome &)> done);
    bool busy() const { return m_busy; }
    void touch();

    void createDevice();
    void unlock();
    void lock(const QString &note = QString());
    void selectDrive(const QString &slot);
    void scanCurrent(bool quiet);
    void poll();
    void createDrive();
    void joinDrive();
    void forgetDrive();
    void changePassphrase();

    void renderDrives();
    void renderAll();
    void renderBanner();
    void renderFiles();
    void renderActivity();
    void renderDevices();

    QString selectedFileId() const;
    void newFile(const QString &kind);
    void addFiles(const QStringList &paths);
    void openFile(const QString &fileId, int version = -1);
    void openExternally(const QString &fileId);
    void saveCopy(const QString &fileId);
    void showVersions(const QString &fileId);
    void renameFile(const QString &fileId);
    void deleteFile(const QString &fileId);
    void saveExternalCopies();
    void closeExternalCopies();
    void wipeExternalFolder();
    void exportActivity();
    void invite();
    void approveRequest(const QString &object);
    void denyRequest(const QString &object);
    void removeDevice(const QString &deviceId);
    void cancelInvites();
    void closeEditors(bool discard);
    QString externalFolder() const;

    QString m_root;
    std::unique_ptr<LocalState> m_state;
    std::unique_ptr<Drive> m_drive;
    QList<QPointer<EditorWindow>> m_editors;
    QList<ExternalCopy> m_external;
    QFutureWatcherBase *m_watcher = nullptr;
    bool m_busy = false;
    bool m_lockPending = false;
    QString m_lockNote;
    QString m_hostQuery;
    std::function<void()> m_searchIndexChanged;

    QStackedWidget *m_stack = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_lockButton = nullptr;
    QLineEdit *m_setupName = nullptr;
    QLineEdit *m_setupPassphrase = nullptr;
    QLineEdit *m_setupConfirm = nullptr;
    QLineEdit *m_unlockPassphrase = nullptr;
    QComboBox *m_drivePicker = nullptr;
    QLabel *m_banner = nullptr;
    QStackedWidget *m_driveStack = nullptr;
    QLabel *m_pendingText = nullptr;
    QLabel *m_pendingSafety = nullptr;
    QTabWidget *m_tabs = nullptr;
    QLineEdit *m_fileFilter = nullptr;
    QTreeWidget *m_files = nullptr;
    QList<QPushButton *> m_fileButtons;
    QPushButton *m_newButton = nullptr;
    QPushButton *m_addButton = nullptr;
    QWidget *m_externalBar = nullptr;
    QLabel *m_externalText = nullptr;
    QLineEdit *m_activityFilter = nullptr;
    QTreeWidget *m_activity = nullptr;
    QWidget *m_requestsBox = nullptr;
    QTreeWidget *m_requests = nullptr;
    QTreeWidget *m_devices = nullptr;
    QPushButton *m_inviteButton = nullptr;
    QPushButton *m_removeDeviceButton = nullptr;
    QLabel *m_inviteNote = nullptr;
    QPushButton *m_cancelInvitesButton = nullptr;
    QLabel *m_ownSafety = nullptr;
    QTimer *m_idleTimer = nullptr;
    QTimer *m_pollTimer = nullptr;
};

}  // namespace forkmesh::securedrive
