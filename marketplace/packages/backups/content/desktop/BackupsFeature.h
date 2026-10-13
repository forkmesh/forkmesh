#pragma once

#include "BackupHostFeed.h"
#include "BackupShelf.h"
#include "NativePackageExtension.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>

class QCheckBox;
class QLabel;
class QListWidget;
class QProcess;
class QPushButton;
class QSpinBox;
class QTreeWidget;

namespace forkmesh::backups {

class BackupsFeature final : public forkmesh::market::NativePackageWidget,
                             public forkmesh::backupfeed::Listener
{
public:
    explicit BackupsFeature(const forkmesh::market::NativePackageHost &host,
                            QWidget *parent = nullptr);
    ~BackupsFeature() override;

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    void prepareSearch() override;

    void backupStatus(const QString &text, bool error) override;

    static Locations desktopLocations();

private:
    void buildPage();
    void syncControls();
    void renderShelves();
    void renderItems();
    void updateActions();
    void measureFolders();
    void setStatus(const QString &text, bool error = false);
    const BackupItem *selectedItem() const;
    void selectItem(const QString &id);
    QHash<QString, QString> loadLabels() const;
    void saveLabels(const QHash<QString, QString> &labels);
    QString labelsKey() const;

    void keepSelected();
    void releaseSelected();
    void labelSelected();
    void verifySelected();
    void restoreSelected();
    void copySelected();
    void revealSelected();
    void deleteSelected();

    QString m_packageId;
    std::function<void()> m_searchIndexChanged;
    Locations m_locations;
    QList<BackupItem> m_items;
    QSet<QString> m_leaving;
    QString m_query;
    int m_shelfFilter = -1;
    quint64 m_generation = 0;
    bool m_loaded = false;
    bool m_syncing = false;

    QLabel *m_health = nullptr;
    QLabel *m_shelfNote = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_hourly = nullptr;
    QSpinBox *m_keep = nullptr;
    QPushButton *m_backUpNow = nullptr;
    QListWidget *m_shelves = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_keepButton = nullptr;
    QPushButton *m_releaseButton = nullptr;
    QPushButton *m_labelButton = nullptr;
    QPushButton *m_verifyButton = nullptr;
    QPushButton *m_restoreButton = nullptr;
    QPushButton *m_copyButton = nullptr;
    QPushButton *m_revealButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPointer<QProcess> m_verify;
    bool m_copying = false;
};

} // namespace forkmesh::backups
