#pragma once

#include "AppArmorSystem.h"
#include "NativePackageExtension.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTemporaryFile;
class QThread;
class QTreeWidget;
class QTreeWidgetItem;
class QWidget;

namespace forkmesh::apparmor {

class ModeBar;
class TimelineChart;

struct ProfileRow {
    QString name;
    QString attachment;
    int fileIndex = -1;
    Mode declared = Mode::Unknown;
    Mode live = Mode::Unknown;
    bool loaded = false;
    int processes = 0;
    int denied = 0;
    int allowed = 0;
};

class AppArmorFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit AppArmorFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~AppArmorFeature() override;

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;

private:
    QWidget *buildOverview();
    QWidget *buildProfiles();
    QWidget *buildProcesses();
    QWidget *buildEvents();

    void rebuildRows();
    void render();
    void renderOverview();
    void renderProfiles();
    void renderProcesses();
    void renderEvents();
    void showProfile(const QString &name);
    void showEventDetails();
    void selectProfile(const QString &name);
    void showEventsFor(const QString &profile);
    void navigate(const QString &target);

    const QList<Event> &events() const;
    const ProfileRow *rowByName(const QString &name) const;
    const ProfileFile *fileFor(const ProfileRow *row) const;
    QString displayMode(const ProfileRow &row) const;
    QString displayState(const ProfileRow &row) const;
    QString profileFor(const ProcessInfo &process) const;
    bool matchesQuery(const QStringList &values) const;

    void readLive();
    void setMode(Mode mode);
    void toggleDisabled();
    void reloadProfile();
    void unloadProfile();
    void editProfile();
    void newProfile();
    void reloadAll();
    void allowSelected();
    void writePolicy(const QString &title, const QString &description, const QString &dest, const QString &text,
                     const QString &load);
    void runAdmin(const QString &title, const QString &description, const QString &details, const QString &script,
                  const QStringList &arguments, bool confirm, std::shared_ptr<QTemporaryFile> keep = {});
    void setStatus(const QString &text);
    void updateActions();

    Snapshot m_snapshot;
    std::optional<QList<LoadedProfile>> m_liveProfiles;
    std::optional<QList<Event>> m_liveEvents;
    QDateTime m_liveReadAt;
    QList<ProfileRow> m_rows;
    QHash<qint64, QString> m_missed;
    QString m_query;
    QString m_selectedProfile;
    QString m_eventProfile;
    bool m_scanned = false;
    bool m_busy = false;
    std::function<void()> m_searchIndexChanged;
    QThread *m_scanThread = nullptr;

    QLabel *m_summary = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_refresh = nullptr;
    QPushButton *m_live = nullptr;
    QPushButton *m_newProfile = nullptr;
    QPushButton *m_reloadAll = nullptr;
    QTabWidget *m_tabs = nullptr;

    QTreeWidget *m_checks = nullptr;
    ModeBar *m_profileBar = nullptr;
    ModeBar *m_processBar = nullptr;
    TimelineChart *m_timeline = nullptr;
    QLabel *m_timelineCaption = nullptr;
    QTreeWidget *m_attention = nullptr;

    QComboBox *m_profileFilter = nullptr;
    QTreeWidget *m_profiles = nullptr;
    QWidget *m_profileDetails = nullptr;
    QLabel *m_profileTitle = nullptr;
    QLabel *m_profileInfo = nullptr;
    QLabel *m_profileHint = nullptr;
    QPushButton *m_enforce = nullptr;
    QPushButton *m_complain = nullptr;
    QPushButton *m_toggleDisabled = nullptr;
    QPushButton *m_reload = nullptr;
    QPushButton *m_unload = nullptr;
    QPushButton *m_edit = nullptr;
    QPushButton *m_profileEvents = nullptr;
    QPlainTextEdit *m_profileText = nullptr;

    QComboBox *m_processFilter = nullptr;
    QTreeWidget *m_processes = nullptr;
    QLabel *m_processNote = nullptr;

    QComboBox *m_eventFilter = nullptr;
    QLabel *m_eventScope = nullptr;
    QPushButton *m_eventScopeClear = nullptr;
    QLabel *m_eventEmpty = nullptr;
    QTreeWidget *m_events = nullptr;
    QLabel *m_eventDetail = nullptr;
    QLabel *m_eventRule = nullptr;
    QPushButton *m_allow = nullptr;
};

}
