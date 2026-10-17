#pragma once

#include "NativePackageExtension.h"
#include "RepoInsight.h"

#include <QFutureWatcher>
#include <QPointer>
#include <QWidget>

#include <functional>

class QComboBox;
class QHBoxLayout;
class QLabel;
class QProgressBar;
class QPushButton;

namespace forkmesh::insight {

class RepoInsightBar : public QWidget
{
public:
    RepoInsightBar(const forkmesh::market::NativePackageHost &host, const QString &title,
                   const QString &subtitle, QWidget *parent = nullptr);
    ~RepoInsightBar() override;

    void refresh();
    void repositoryChanged();
    void reanalyze();
    void setRoot(const QString &root, const QString &title = QString());

    QString root() const { return m_root; }
    QString repositoryTitle() const { return m_title; }
    SnapshotPtr snapshot() const { return m_snapshot; }
    bool busy() const;
    QHBoxLayout *actions() const { return m_actions; }
    void setSummary(const QString &text);
    void openFile(const QString &relativePath, int line = 0) const;
    void setOnSnapshot(std::function<void(SnapshotPtr)> callback);
    void setOnRootChanged(std::function<void(const QString &)> callback);

    static QString describe(const Snapshot &snapshot);

private:
    void rebuildChoices();
    void chooseFolder();
    void start(bool force);
    void finish();
    void showProgress(int value, const QString &text);

    forkmesh::market::NativePackageHost m_host;
    QComboBox *m_repositories = nullptr;
    QPushButton *m_folderButton = nullptr;
    QPushButton *m_analyzeButton = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QHBoxLayout *m_actions = nullptr;
    QFutureWatcher<SnapshotPtr> *m_watcher = nullptr;
    QString m_root;
    QString m_title;
    QString m_pickedFolder;
    QString m_summary;
    bool m_followOpen = true;
    bool m_rebuilding = false;
    SnapshotPtr m_snapshot;
    std::function<void(SnapshotPtr)> m_onSnapshot;
    std::function<void(const QString &)> m_onRootChanged;
};

} // namespace forkmesh::insight
