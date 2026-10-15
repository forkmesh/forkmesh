#pragma once

#include "CleanupPlan.h"
#include "CleanupScanner.h"
#include "NativePackageExtension.h"

#include <QHash>
#include <QList>
#include <QPointer>
#include <QSet>
#include <QString>

#include <functional>

class QCheckBox;
class QLabel;
class QProcess;
class QPushButton;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

namespace forkmesh::cleanup {

class CleanupFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit CleanupFeature(const forkmesh::market::NativePackageHost &host,
                            QWidget *parent = nullptr);
    ~CleanupFeature() override;

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    void prepareSearch() override;

    static Context desktopContext();
    static QString logPath();

private:
    void buildPage();
    void cancelScan();
    void startNativeScan(quint64 generation, const Context &context);
    void applyScan(const ScanResult &result);
    void render();
    bool visible(const Item &item) const;
    void updateSummary();
    void updateButtons();
    void setStatus(const QString &text, bool error = false);
    const Item *itemById(const QString &id) const;
    QList<Item> checkedItems() const;
    void checkOrphaned();
    void clearChecks();
    void openSelectedFolder();
    void deleteChecked();
    bool confirmDeletion(const QList<Item> &items, QList<Item> *chosen,
                         QHash<QString, Choice> *choices);
    void runPlan(const Plan &plan);
    void removeNextWorktree();
    void finishDeletion();
    void completeDeletion(bool logged);
    void trimCcache();

    QString m_packageId;
    std::function<void()> m_searchIndexChanged;
    QList<Item> m_items;
    QStringList m_notes;
    QSet<QString> m_checked;
    QString m_query;
    qint64 m_scannedAtMs = 0;
    quint64 m_generation = 0;
    CancelFlag m_cancel;
    bool m_scanning = false;
    bool m_deleting = false;
    bool m_rendering = false;

    QList<Step> m_pendingWorktrees;
    QList<Outcome> m_outcomes;
    ProcessSnapshot m_deleteProcesses;
    int m_planSkipped = 0;
    bool m_coreRecheck = false;
    QHash<QString, QString> m_coreRefusals;
    qsizetype m_coreLogged = -1;
    bool m_coreLogOk = true;

    QLabel *m_summary = nullptr;
    QLabel *m_status = nullptr;
    QCheckBox *m_orphanedOnly = nullptr;
    QSpinBox *m_olderDays = nullptr;
    QSpinBox *m_largerMb = nullptr;
    QTreeWidget *m_tree = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_selectOrphaned = nullptr;
    QPushButton *m_clearButton = nullptr;
    QPushButton *m_openButton = nullptr;
    QPushButton *m_trimButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QPointer<QProcess> m_trim;
};

} // namespace forkmesh::cleanup
