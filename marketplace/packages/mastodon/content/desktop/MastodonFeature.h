#pragma once

#include "MastodonClient.h"
#include "MastodonCore.h"
#include "MastodonDialogs.h"
#include "MastodonWidgets.h"
#include "NativePackageExtension.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTcpServer;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace forkmesh::mastodon {

struct Page {
    QString kind;
    QString arg;
    QString tab;

    QString key() const { return arg.isEmpty() ? kind : kind + QLatin1Char(':') + arg; }
    bool operator==(const Page &other) const { return kind == other.kind && arg == other.arg && tab == other.tab; }
};

struct PendingSignIn {
    QString instance;
    QString clientId;
    QString clientSecret;
    QString redirect;
    QString loopback;
    QString verifier;
    QString state;
};

class MastodonFeature final : public forkmesh::market::NativePackageWidget
{
public:
    explicit MastodonFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent = nullptr);
    ~MastodonFeature() override;

    void refresh() override;
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    void prepareSearch() override {}
    int navigationBadge() const override { return m_unread; }
    void prepareNavigationBadge() override;
    void themeChanged() override;

    bool signedIn() const;
    QList<SavedAccount> accounts() const { return m_accounts; }
    SavedAccount activeAccount() const;
    QJsonObject self() const { return m_self; }
    Page currentPage() const { return m_page; }
    QString notice() const;
    QList<PostCard *> postCards() const;
    QList<AccountRow *> accountRows() const;
    ComposeLimits limits() const { return m_limits; }
    void setExternalOpener(std::function<void(const QUrl &)> opener) { m_openExternal = std::move(opener); }
    void setConfirmer(std::function<bool(const QString &, const QString &)> confirm) { m_confirm = std::move(confirm); }

    void beginBrowserSignIn(const QString &instance);
    void useCodeInstead();
    void finishSignIn(const QString &codeOrUrl);
    void signInWithToken(const QString &instance, const QString &token);
    void switchAccount(const QString &key);
    void signOut(const QString &key);
    void showSignIn(const QString &instance = QString(), const QString &message = QString());
    PendingSignIn pendingSignIn() const { return m_pending; }
    quint16 callbackPort() const;

    void openPage(const Page &page, bool remember = true);
    void goBack();
    void reload();
    void loadMore();
    void act(const QJsonObject &status, const QString &action);
    void vote(const QJsonObject &poll, const QList<int> &choices);
    void deleteStatus(const QJsonObject &status, bool redraft);
    ComposeDialog *compose(const QJsonObject &replyTo = {}, const QString &text = QString(),
                           const QString &visibility = QString());
    void editStatus(const QJsonObject &status);
    void follow(const QString &accountId, bool on, const QJsonObject &options = {});
    void relationshipAction(const QString &accountId, const QString &action, const QJsonObject &body = {});
    void blockDomain(const QString &domain, bool on);
    void saveProfile(const QJsonObject &fields, const QString &avatarPath, const QString &headerPath);
    void savePostingDefaults(const QString &visibility, bool sensitive, const QString &language);
    void createList(const QString &title);
    void updateList(const QString &id, const QJsonObject &body);
    void deleteList(const QString &id);
    void listMember(const QString &listId, const QString &accountId, bool add);
    void followTag(const QString &name, bool on);
    void featureTag(const QString &name, const QString &featuredId);
    void saveFilter(const QString &id, const QJsonObject &body);
    void deleteFilter(const QString &id);
    void cancelScheduled(const QString &id);
    void reschedule(const QString &id, const QDateTime &when);
    void answerFollowRequest(const QString &accountId, bool accept);
    void dismissNotification(const QString &id);
    void clearNotifications();
    void report(const QJsonObject &body);
    void savePersonalNote(const QString &accountId, const QString &note);

protected:
    void changeEvent(QEvent *event) override;

private:
    QWidget *buildWelcome();
    QWidget *buildSidebar();
    QWidget *buildMain();
    void loadAccounts();
    void saveAccounts();
    void activate(const SavedAccount &account);
    void afterSignIn(const QString &instance, const QString &token, const QString &clientId,
                     const QString &clientSecret);
    void exchangeCode(const QString &code);
    void startLoopback();
    void stopLoopback();
    void setWelcomeState(const QString &state, const QString &message = QString(), bool error = false);
    void refreshAccountHeader();
    void rebuildNav();
    void selectNav();
    void loadInstance();
    void loadLists();
    void loadSidebarCounts();
    void sessionExpired();

    void beginPage(const QString &title, const QString &subtitle = QString());
    void setTabs(const QList<QPair<QString, QString>> &tabs);
    void addHeaderAction(QWidget *widget);
    void clearList();
    void addRow(QWidget *widget);
    void addSection(const QString &title);
    void setEmpty(const QString &text);
    void setLoading(bool loading);
    void setNotice(const QString &text, bool error = false);
    void setLoadMore(std::function<void()> loader);
    bool current(quint64 generation) const { return generation == m_generation; }
    CardContextPtr cardContext();

    void loadStatuses(const QString &path, const QUrlQuery &query, const QString &context, const QString &empty);
    void showStatuses(const Result &result, quint64 generation, const QString &context, const QString &empty,
                      bool append);
    void loadAccountList(const QString &path, const QUrlQuery &query, const QString &empty,
                         const QString &mode);
    void showAccounts(const Result &result, quint64 generation, const QString &empty, const QString &mode, bool append);
    void decorateAccount(AccountRow *row, const QString &mode, const QJsonObject &relationship);
    void fetchRelationships(const QStringList &ids, std::function<void(const QHash<QString, QJsonObject> &)> done);
    QPushButton *followButton(const QJsonObject &account, const QJsonObject &relationship);

    void showHome();
    void showNotifications();
    void showNotificationsResult(const Result &result, quint64 generation, bool append);
    QWidget *notificationRow(const QJsonObject &notification);
    void showExplore();
    void showConversations();
    void showThread();
    void showAccount();
    void showConnections();
    void showFollows();
    void showModeration();
    void showLists();
    void showListMembers();
    void showHashtags();
    void showTag();
    void showProfileEditor();
    void showFilters();
    void showScheduled();
    void showPreferences();
    void showSearch();

    void updateCards(const QJsonObject &status);
    void removeCards(const QString &id);
    void openLink(const QString &link);
    void openAccountObject(const QJsonObject &account);
    void openAccountId(const QString &id);
    void openStatusMenu(const QJsonObject &status, QToolButton *anchor);
    void openAccountMenu(const QJsonObject &account, const QJsonObject &relationship, QWidget *anchor);
    void openMedia(const QJsonArray &media, int index);
    void translate(const QJsonObject &status);
    void markNotificationsRead(const QString &newestId);
    void openOnInstance(const QString &path);
    void openExternal(const QUrl &url);
    bool confirm(const QString &title, const QString &text);
    void reportAccount(const QJsonObject &account, const QJsonObject &status);
    void editListMembership(const QJsonObject &account);
    void setUnread(int count);
    void run(const QByteArray &method, const QString &path, const QJsonObject &body, const QString &done,
             std::function<void(const Result &)> after = {});
    void restyle();
    QFrame *panelRow();
    QWidget *tagRow(const QJsonObject &tag, const QString &mode);
    QPushButton *textButton(const QString &text, const QString &style = QStringLiteral("ghostButton"));

    forkmesh::market::NativePackageHost m_host;
    std::unique_ptr<Client> m_client;
    QList<SavedAccount> m_accounts;
    QString m_activeKey;
    QJsonObject m_self;
    QJsonArray m_lists;
    ComposeLimits m_limits;
    PendingSignIn m_pending;
    QPointer<QTcpServer> m_loopback;
    Page m_page;
    QList<Page> m_history;
    quint64 m_generation = 0;
    std::function<void()> m_loadMore;
    CardContextPtr m_cardContext;
    QString m_lastNotificationId;
    int m_unread = 0;
    int m_followRequests = 0;
    qint64 m_badgeCheckedMs = 0;
    QString m_hostQuery;
    std::function<void(const QUrl &)> m_openExternal;
    std::function<bool(const QString &, const QString &)> m_confirm;
    QHash<QString, QJsonObject> m_seenStatuses;
    bool m_restylePending = false;

    QStackedWidget *m_stack = nullptr;
    QWidget *m_welcome = nullptr;
    QWidget *m_app = nullptr;
    QLineEdit *m_instanceField = nullptr;
    QPushButton *m_signInButton = nullptr;
    QWidget *m_waitingBox = nullptr;
    QLabel *m_waitingText = nullptr;
    QLineEdit *m_codeField = nullptr;
    QWidget *m_tokenBox = nullptr;
    QLineEdit *m_tokenField = nullptr;
    QLabel *m_welcomeNotice = nullptr;
    QPushButton *m_welcomeBack = nullptr;

    QToolButton *m_accountButton = nullptr;
    AvatarLabel *m_accountAvatar = nullptr;
    QLabel *m_accountName = nullptr;
    QLabel *m_accountHandle = nullptr;
    QLineEdit *m_searchField = nullptr;
    QListWidget *m_nav = nullptr;
    bool m_navUpdating = false;
    bool m_navJustChanged = false;

    QToolButton *m_backButton = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_subtitle = nullptr;
    QHBoxLayout *m_headerActions = nullptr;
    QWidget *m_tabBar = nullptr;
    QHBoxLayout *m_tabLayout = nullptr;
    QLabel *m_notice = nullptr;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_column = nullptr;
    QVBoxLayout *m_list = nullptr;
    QPushButton *m_loadMoreButton = nullptr;
    QLabel *m_loading = nullptr;
    QTimer *m_searchTimer = nullptr;
};

}
