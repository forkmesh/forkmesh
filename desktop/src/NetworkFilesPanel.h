#pragma once

#include "NetworkShares.h"

#include <QHash>
#include <QIcon>
#include <QPointer>
#include <QTemporaryDir>
#include <QWidget>

#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace forkmesh::smb {

struct ClipboardItem {
    bool network = false;
    QString localPath;
    QString shareId;
    QString path;
    QString name;
    bool folder = false;
};

struct FileClipboard {
    QList<ClipboardItem> items;
    bool move = false;
};

FileClipboard &fileClipboard();

struct OpResult {
    bool ok = true;
    QString error;
    QList<Entry> entries;
    QList<ShareInfo> shares;
    QByteArray data;
    Entry entry;
    bool tooLarge = false;
    bool cancelled = false;
    QString localPath;
};

constexpr qint64 kTextOpenLimit = 4 * 1024 * 1024;

bool looksBinary(const QByteArray &head);
bool opensExternally(const QString &name);
QString uniqueLocalName(const QString &folder, const QString &name);

class NetworkFilesPanel : public QWidget
{
public:
    explicit NetworkFilesPanel(const QString &secretsPath, QWidget *parent = nullptr);
    ~NetworkFilesPanel() override;

    std::function<void(const QString &url, const QString &title, const QString &text, bool editable)> openText;
    std::function<void(const QString &message, bool error)> notify;
    std::function<QIcon(bool open)> folderIcon;
    std::function<QIcon(const QString &name)> fileIcon;
    std::function<void(QPushButton *, const QString &)> decorateButton;
    std::function<void()> clipboardChanged;
    std::function<void(const QString &)> statusChanged;

    void setTransportFactory(TransportFactory factory) { m_factory = std::move(factory); }
    void setPasswordPrompt(std::function<bool(const SavedShare &, QString *)> prompt) { m_passwordPrompt = std::move(prompt); }
    void setConfirm(std::function<bool(const QString &, const QString &)> confirm) { m_confirm = std::move(confirm); }
    void setTextPrompt(std::function<bool(const QString &, const QString &, QString *)> prompt) { m_textPrompt = std::move(prompt); }
    void setOpenExternal(std::function<void(const QString &)> open) { m_openExternal = std::move(open); }

    void activate();
    void reload();
    void openUrl(const QString &url, bool guest);
    void addShare(const SavedShare &share, const QString &password);
    void selectShare(const QString &id);
    void browse(const QString &path);
    void saveText(const QString &url, const QString &text, std::function<void(bool, const QString &)> done);
    void pasteIntoLocal(const QString &folder, std::function<void(bool)> done);
    void uploadLocal(const QStringList &paths, const QString &folder);
    void cancel();
    bool busy() const { return m_busy; }
    QString status() const;
    QString currentPath() const { return m_path; }
    SavedShare currentShare() const { return m_share; }
    QString currentUrl() const;
    ShareStore &store() { return m_store; }

    void newFolder(const QString &folder);
    void newFile(const QString &folder);
    void renameEntry(const QString &path);
    void deleteEntries(const QStringList &paths, const QList<bool> &folders);
    void copyEntries(const QStringList &paths, const QList<bool> &folders, bool move);
    void paste(const QString &folder);
    void openEntry(const QString &path, bool folder, qint64 size);
    void downloadEntry(const QString &path, bool folder, const QString &localFolder);

    QStringList visibleNames() const;
    QTreeWidget *tree() const { return m_tree; }

private:
    using Work = std::function<void(Client &, OpResult &, const Progress &)>;
    using Done = std::function<void(const OpResult &)>;

    void build();
    void rebuildShareList(bool open);
    std::shared_ptr<Connection> connection(const SavedShare &share, bool prompt);
    std::shared_ptr<Connection> connectionFor(const QString &url, QString *path);
    void post(const std::shared_ptr<Connection> &connection, const QString &label, Work work, Done done);
    void list(const QString &path, QTreeWidgetItem *parent);
    void fill(QTreeWidgetItem *parent, const QList<Entry> &entries);
    void applyFilter();
    void setBusy(bool busy, const QString &label = {});
    void setProgress(qint64 done, qint64 total);
    void setStatus(const QString &text, bool error = false);
    void showMenu(const QPoint &pos);
    void editCurrentShare();
    void removeCurrentShare();
    void showInfo(QTreeWidgetItem *item);
    void finish(const OpResult &result, const QString &success);
    bool ask(const QString &title, const QString &text);
    bool askText(const QString &title, const QString &label, QString *value);
    QString selectedFolder() const;
    QIcon iconFor(const Entry &entry) const;
    void decorate(QPushButton *button, const QString &icon);

    ShareStore m_store;
    TransportFactory m_factory;
    QHash<QString, std::shared_ptr<Connection>> m_connections;
    QHash<QString, QString> m_sessionPasswords;
    SavedShare m_share;
    QString m_path;
    bool m_busy = false;
    bool m_listed = false;
    int m_listGeneration = 0;
    QTemporaryDir m_temp;
    std::shared_ptr<Connection> m_activeConnection;
    std::function<bool(const SavedShare &, QString *)> m_passwordPrompt;
    std::function<bool(const QString &, const QString &)> m_confirm;
    std::function<bool(const QString &, const QString &, QString *)> m_textPrompt;
    std::function<void(const QString &)> m_openExternal;

    QComboBox *m_shares = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_editButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_upButton = nullptr;
    QPushButton *m_newFolderButton = nullptr;
    QPushButton *m_newFileButton = nullptr;
    QPushButton *m_uploadButton = nullptr;
    QPushButton *m_pasteButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QLabel *m_crumbs = nullptr;
    QLineEdit *m_filter = nullptr;
    QComboBox *m_sort = nullptr;
    QCheckBox *m_hidden = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_status = nullptr;
    QWidget *m_progressRow = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_cancelButton = nullptr;
};

}
