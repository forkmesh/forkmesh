#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include <functional>

class QNetworkAccessManager;

namespace forkmesh::market {

using NativePackageReplyHandler =
    std::function<void(bool, const QJsonObject &, const QString &)>;
using NativePackageUserRecordsHandler =
    std::function<void(const QJsonArray &, const QString &)>;
using NativePackageAgentReplyHandler =
    std::function<void(const QString &, const QString &)>;
using NativePackageSpecialAgentActionHandler =
    std::function<void(bool, const QString &)>;
using NativePackageRequest =
    std::function<void(const QByteArray &, const QString &, const QJsonObject &,
                       NativePackageReplyHandler)>;

struct NativePackageRepository {
    QString key;
    QString title;
    QString path;
    QString ref;
};

struct NativePackageChatTarget {
    QString id;
    QString label;
    bool person = false;
};

struct NativePackageLogin {
    QString title;
    QString url;
    QString username;
    QString password;
};

struct NativePackageLoginReply {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QList<NativePackageLogin> logins;
};

using NativePackageLoginHandler =
    std::function<void(const NativePackageLoginReply &)>;

struct NativePackageHost {
    // Manifest metadata is supplied by the generic host. A package can adapt
    // its own UI without reaching into MainWindow or assuming its catalog id.
    QString packageId;
    QString packageName;
    QString packageType;
    QString packageVersion;
    QStringList permissionIds;
    NativePackageRequest request;
    // The window's shared manager. Requests a package makes itself go through
    // it so the desktop firewall decides every external destination; a
    // private QNetworkAccessManager would reach the network unasked.
    QNetworkAccessManager *network = nullptr;
    // A package calls this when the records behind searchHits change, so a
    // query already typed in the top search box picks the new rows up.
    std::function<void()> searchIndexChanged;
    std::function<QStringList()> userDirectory;
    std::function<void()> promptTargetChanged;
    std::function<void()> navigationBadgeChanged;
    std::function<bool()> userRecordsAvailable;
    std::function<void(NativePackageUserRecordsHandler)> userRecords;
    std::function<QString()> agentName;
    std::function<void(const QString &, NativePackageAgentReplyHandler)> askAgent;
    std::function<void()> railChanged;
    std::function<QJsonArray()> specialAgents;
    std::function<void(const QString &, const QString &, const QString &,
                       NativePackageSpecialAgentActionHandler)>
        specialAgentAction;
    std::function<QString()> accountName;
    std::function<void(const QString &)> openDirectMessage;
    std::function<void(const QString &)> openUserProfile;
    std::function<QList<NativePackageRepository>()> repositories;
    std::function<NativePackageRepository()> currentRepository;
    std::function<void(const NativePackageRepository &, const QString &, int)>
        openRepositoryFile;
    std::function<QList<NativePackageChatTarget>()> chatTargets;
    std::function<bool(const QString &, const QString &)> sendChatMessage;
    std::function<void(const QString &, bool)> openNetworkShare;
    std::function<QWidget *(QWidget *)> agentUsagePage;
    std::function<QString()> loginStoreName;
    std::function<void(const NativePackageLogin &, QWidget *,
                       NativePackageLoginHandler)>
        saveLogin;
    std::function<void(const QString &, QWidget *, NativePackageLoginHandler)>
        findLogins;
    std::function<void(const QString &)> openWebPage;
    std::function<QJsonObject()> repositoryWorkflows;
    std::function<void(const QString &, const QString &, const QString &,
                       const QJsonObject &, NativePackageReplyHandler)>
        repositoryWorkflowAction;
};

struct NativePackagePromptTarget {
    QString placeholder;
    QString action;
};

// One row a package contributes to the window's top search box. `id` is
// package-private: the host hands it straight back through
// NativePackageWidget::activateSearchHit when the row is chosen.
struct NativePackageSearchHit {
    QString id;
    QString title;
    QString detail;
};

enum class NativePackageRailTone { Neutral, Good, Attention, Warning, Paused };

struct NativePackageRailStep {
    QString id;
    QString text;
};

struct NativePackageRailTile {
    QString id;
    QString label;
    QString count;
    QString countLabel;
    NativePackageRailTone tone = NativePackageRailTone::Neutral;
    QString stateLabel;
    QStringList details;
    QList<NativePackageRailStep> steps;
    int pending = 0;
    bool opensPage = false;
};

class NativePackageWidget : public QWidget
{
public:
    using QWidget::QWidget;
    ~NativePackageWidget() override = default;
    virtual void refresh() = 0;
    // The window's top search box is the page's own filter while the page is
    // the visible section; the host clears it again when the user leaves.
    virtual void setHostSearchQuery(const QString &) {}
    // Rows for the top search popup, so the one box searches every installed
    // package. Runs on the GUI thread per keystroke: answer from memory.
    virtual QList<NativePackageSearchHit> searchHits(const QString &,
                                                     int) const
    {
        return {};
    }
    virtual void activateSearchHit(const QString &) {}
    // Called once, the first time the top search box is used, on a page the
    // user has not opened yet: load whatever searchHits answers from. The
    // default is the page's own refresh, which is what opening it would do.
    virtual void prepareSearch() { refresh(); }
    // A copy of the package's server-side records, so the host can offer to
    // save them when the package is removed from the rail. Calls done with
    // the package's lifecycle export payload ({hasData, filename, export}).
    // Returns false, without calling done, when the page has nothing it can
    // export right now (no organization picked, not installed for the
    // account, or no server-side data at all).
    virtual bool exportData(NativePackageReplyHandler) { return false; }
    virtual NativePackagePromptTarget hostPromptTarget() const { return {}; }
    virtual bool submitHostPrompt(const QString &, const QStringList &)
    {
        return false;
    }
    virtual void userDirectoryChanged() {}
    virtual int navigationBadge() const { return 0; }
    virtual void prepareNavigationBadge() {}
    virtual QString railTitle() const { return {}; }
    virtual QList<NativePackageRailTile> railTiles() const { return {}; }
    virtual void prepareRail() {}
    virtual void activateRailStep(const QString &, const QString &) {}
    virtual void openRailTile(const QString &) {}
    virtual void packageChanged() {}
    virtual void repositoryChanged() {}
    virtual void repositoryWorkflowsChanged() {}
    virtual bool storesLogins() const { return false; }
    virtual void saveLogin(const NativePackageLogin &, const QString &,
                           QWidget *, NativePackageLoginHandler done)
    {
        done({});
    }
    virtual void findLogins(const QString &, const QString &, QWidget *,
                            NativePackageLoginHandler done)
    {
        done({});
    }
};

using NativePackageFactory =
    std::function<NativePackageWidget *(const NativePackageHost &, QWidget *)>;

struct NativePackageContribution {
    QString entrypoint;
    // Empty means the contribution accepts any manifest type. Packages should
    // normally declare their supported types explicitly so a catalog typo
    // fails closed rather than activating the wrong native runtime.
    QStringList supportedTypes;
    QString navigationLabel;
    QString navigationIcon;
    QString apiPrefix;
    QString requestProof;
    NativePackageFactory factory;
    QString browserPath;
};

// Package translation units register their contribution while they are linked.
// The desktop core only knows this neutral socket; removing a package from the
// catalog removes its source from the generated CMake inputs and therefore
// removes its registration without leaving package identifiers in the host.
class NativePackageRegistration
{
public:
    explicit NativePackageRegistration(NativePackageContribution contribution);

    bool accepted() const { return m_accepted; }
    QString errorString() const { return m_error; }

private:
    bool m_accepted = false;
    QString m_error;
};

const NativePackageContribution *nativePackageContribution(
    const QString &entrypoint, const QString &packageType = QString());
QList<QString> nativePackageEntrypoints();
void setNativePackageLoader(std::function<bool(const QString &entrypoint)> loader);
void unregisterNativePackageEntrypoint(const QString &entrypoint);

// Lifecycle paths are host-owned because the host enforces the matching
// package boundary. Keeping construction and validation beside that boundary
// prevents native packages from drifting to subtly different URL shapes.
QString nativePackageLifecyclePath(const QString &organization,
                                   const QString &packageId,
                                   const QString &operation);
bool nativePackageRequestPathAllowed(const QString &packageId,
                                     const QString &apiPrefix,
                                     const QString &path);
QUrl nativePackageWebPageUrl(const QUrl &base, const QString &path);

} // namespace forkmesh::market
