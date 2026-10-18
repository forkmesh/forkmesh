#pragma once

#include "NativePackageExtension.h"

#include <QByteArray>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>

class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QNetworkAccessManager;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTabWidget;
class QTableWidget;
class QTableWidgetItem;
class QTemporaryDir;
class QToolButton;
class QThreadPool;

namespace forkmesh::crm {

class CrmStageStrip;
class CrmTeamChart;

// One column of the contacts table, in logical (creation) order. The
// header stays draggable, so the visual order can differ; hidden columns keep
// their slot and only their section is collapsed.
struct ContactColumnSpec
{
    enum Kind {
        Field,          // a contact field the worker stores by name
        CustomField,    // a key inside the contact's `fields` map
        MastodonLatest, // newest public post of the Mastodon handle
        LastContacted,  // worker-computed stamp
        LinkedEmails,   // emails linked to the contact, newest first
        LinkedToots,    // Mastodon conversations linked to the contact
        Actions,        // Email / Edit / Delete buttons
        AddColumn,      // the trailing "+" whose header adds a column
    };
    Kind kind = Field;
    QString id;    // stable identity used for hiding and sorting
    QString key;   // field name (Field) or custom key (CustomField)
    QString label;
    int width = 150;
};

// The newest public post of one Mastodon account, cached per user@host so a
// handle shared by several contacts is looked up once per refresh.
struct MastodonLatest
{
    qint64 fetchedAt = 0;
    qint64 postedAt = 0;
    QString text;
    // Plain-text excerpts, cut once when the post arrives: parsing the HTML
    // for every row of every render was a measurable share of a repaint.
    QString excerpt;
    QString detail;
    QString url;
    QString error;
};

// One recent post, or one public event, read from an account a contact
// names, for the detail card that opens off its row.
struct AccountUpdate
{
    qint64 at = 0;
    QString text;
    QUrl url;
};

// One contact as the table shows it: every cell's text, worked out off the UI
// thread so a large organization never stalls the window while it re-renders.
struct ContactRow
{
    QString id;
    QString flow;
    QString stage;
    QString mastodonKey;
    // Where the contact's picture can be read, best first ("mastodon:user@host",
    // "bluesky:handle", "github:user"), and the initials shown until one loads
    // or when none does, on one of six tones picked from the email or name.
    QStringList avatarSources;
    QString name;
    qint64 createdAt = 0;
    QString initials;
    int avatarTone = 0;
    qint64 lastContactedAt = 0;
    QStringList cells; // one per column; empty for computed and button columns
    QString search;    // the cells joined, for the top search box
    bool hasEmail = false;
};

// Everything the contact tabs need from one copy of the organization, built on
// a pool thread: the rows in table order and which rows each tab holds.
struct ContactSnapshot
{
    quint64 generation = 0;
    QStringList columnIds;
    QList<ContactRow> rows;
    QHash<QString, QVector<int>> sheetRows;
};

// One CSV import in flight. The file is split into records here and uploaded
// a part at a time, so the progress bar tracks rows the worker has applied and
// every part stays a small request.
struct AccountRead
{
    QString error;
    QUrl picture;
    QList<AccountUpdate> updates;
};

struct CsvImportJob
{
    bool active = false;
    QString id;
    QString path; // the organization's import path, fixed when it starts
    QString fileName;
    // The tab the import opens, named on the mapping step.
    QString sheetName;
    QString header;
    QStringList rows;
    QString viewId;
    QString delimiter;
    // What each CSV column is imported as, one target per header in file
    // order, confirmed on the mapping step and sent with every part.
    QJsonArray mapping;
    int part = 0;
    int parts = 1;
    int offset = 0;
    int created = 0;
    int updated = 0;
    int skipped = 0;
    QJsonArray errors; // every skipped-row error returned so far
};

// A Mastodon account's public follower list being read, page by page, before
// it is handed to the CSV import as one file.
struct MastodonFollowersJob
{
    bool active = false;
    QString path;   // the organization's import path, fixed when it starts
    QString host;   // the instance the list is read from
    QString handle; // "@user@host", for the status line and the tab name
    int total = 0;  // followers_count, which a hidden list still reports
    QStringList rows;
    QSet<QString> seen;
};

struct GithubContributorsJob
{
    bool active = false;
    QString path;
    QString repository;
    std::shared_ptr<QTemporaryDir> checkout;
};

enum class NextMode
{
    ReachOut,
    FollowUp,
};

struct LeadMarks
{
    double rank = 0;
    QString rankField;
    double score = 0;
    QString scoreField;
    qint64 value = 0;
};

struct NextContact
{
    QString id;
    QString reason;
    int row = -1;
    int due = 0;
    QStringList upcoming;
    QStringList ranked;
};

struct NoteDraft
{
    QString contactId;
    QString name;
    bool mastodon = false;
    QString to;
    QString subject;
    QString body;
    QString thread;
};

class CrmFeature final : public forkmesh::market::NativePackageWidget
{
public:
    using ReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;
    using Request = std::function<void(const QByteArray &, const QString &,
                                       const QJsonObject &, ReplyHandler)>;

    explicit CrmFeature(const forkmesh::market::NativePackageHost &host,
                        QWidget *parent = nullptr);

    void refresh() override;
    // The window's top search box filters the open tab's contacts and, from
    // anywhere in the app, lists matching contacts under a CRM heading.
    void setHostSearchQuery(const QString &query) override;
    QList<forkmesh::market::NativePackageSearchHit>
    searchHits(const QString &query, int limit) const override;
    void activateSearchHit(const QString &id) override;
    bool exportData(forkmesh::market::NativePackageReplyHandler done) override;
    void userDirectoryChanged() override;
    int navigationBadge() const override;
    void prepareNavigationBadge() override;

    // Where a click on a contact's address opens a compose window: the
    // organization's Zoho Mail data center unless settings pick Gmail.
    QUrl mailComposeUrl(const QString &email) const;
    // user@host for "@user@host", "user@host", or a profile URL; empty when
    // the handle names no instance to ask.
    static QString mastodonHandleKey(const QString &value);
    static QUrl mastodonProfileUrl(const QString &value);
    // CSV text split into records without parsing fields: a newline inside a
    // quoted value stays in its record, and empty lines are dropped the way
    // the worker's reader drops them.
    static QStringList csvRecords(const QString &text);
    static QString githubRepositoryPath(const QString &value);
    static bool maskedGitEmail(const QString &email);
    static QString githubContributorsCsv(const QByteArray &log, int limit,
                                         int *found = nullptr);
    static QString forkMeshUsersCsv(const QJsonArray &users, int limit,
                                    int *found = nullptr,
                                    int *withoutEmail = nullptr);
    static QString forkMeshUsersSheetId(const QJsonArray &views);
    // The places a contact's picture can be read from, best first; empty when
    // the record names no Mastodon, Bluesky or GitHub account.
    static QStringList avatarSources(const QJsonObject &contact);
    static QStringList pictureSources(const QJsonObject &contact);
    static bool parseEmailDraft(const QString &reply, const QString &deckUrl,
                                QString *subject, QString *body);

protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void request(const QByteArray &method, const QString &path,
                 const QJsonObject &body, ReplyHandler handler);
    QString organizationPath(const QString &suffix = QString()) const;
    QString pluginPath(const QString &suffix = QString()) const;
    QJsonObject organizationRecord() const;
    void applyInstallState();
    void installPlugin();
    void uninstallPlugin();
    void exportAllData();
    bool saveExportBundle(const QJsonObject &payload);
    void refreshOrganizations();
    void fillOrganizations(const QString &previous);
    // The organizations and records last read, from the app's cache folder:
    // drawn the moment the page first opens, then replaced only when the
    // worker says the organization changed since that copy's revision.
    void drawCachedOrganizations();
    bool drawCachedSummary(const QString &organization);
    void cacheSummary(const QJsonObject &payload);
    void forgetCachedSummary(const QString &organization);
    void loadOrganization();
    void applyOrganization(const QJsonObject &payload);
    void updateZohoAction();
    void updateZohoBar();
    void runZohoStep(const QString &action);
    void markZohoReadOnly();
    void rebuildContactColumns();
    int columnIndex(ContactColumnSpec::Kind kind,
                    const QString &key = QString()) const;
    int columnIndexById(const QString &id) const;
    // Sorting, cell text and tab membership for every contact go to a pool
    // thread; the reply lands through applyContactSnapshot, dropped when a
    // newer build has been asked for since.
    void refreshContactSnapshot();
    void applyContactSnapshot(ContactSnapshot snapshot);
    // Fills the open tab's rows from the snapshot a chunk per event-loop turn,
    // so a tab of thousands of contacts keeps the window answering meanwhile.
    void renderContacts();
    void fillContactRows();
    bool contactRowShown(const ContactRow &row) const;
    // The chevron header over the open tab: its pipeline's stages in order,
    // each with how many of the tab's contacts sit there.
    void renderStageStrip();
    NextContact nextContact(NextMode mode) const;
    void renderNextContact();
    QWidget *nextContactCard(NextMode mode);
    QLabel *nextPostLabel(const QJsonObject &contact, bool compact) const;
    void queueReachMastodon(const QStringList &ranked);
    void scheduleNextRender();
    void selectFirstStage();
    QStringList stripStages(QHash<QString, int> *counts) const;
    void focusContactRow();
    void rebuildContactIndex();
    void renderActivity();
    void renderTeam();
    void renderMail();
    void renderSheets();
    void renderSheetPicker();
    void renderSheetList();
    void syncSheetList();
    void tintToolbar();
    void showPeople();
    void renderPeople();
    QIcon contactPicture(const ContactRow &row, int side);
    QStringList contactListNames(const QString &contactId) const;
    void newList();
    void mergeSheetInto(const QString &id, const QString &targetId);
    // The list (a sheet on the web) or pipeline the contacts table shows.
    QJsonObject activeSheet() const;
    QJsonObject sheetById(const QString &id) const;
    // Every contact tab in strip order: the organization's lists, then a tab
    // for each pipeline no list is bound to, then "No pipeline" while some
    // contact's flow is not a live pipeline. There is no every-contact tab.
    QList<QJsonObject> tabSheets() const;
    void rebuildVirtualSheets();
    static bool contactInSheet(const QJsonObject &contact, const QJsonObject &sheet,
                               const QHash<QString, QStringList> &pipelineLists);
    static QHash<QString, QStringList> pipelineLists(const QJsonArray &views);
    bool contactInSheet(const QJsonObject &contact, const QJsonObject &sheet) const;
    int sheetContactCount(const QJsonObject &sheet) const;
    QStringList sheetShareNames(const QJsonObject &sheet) const;
    QString sheetShareSummary(const QJsonObject &sheet) const;
    QString sheetField(const ContactColumnSpec &column) const;
    QString viewField(const ContactColumnSpec &column) const;
    bool columnHiddenInView(const ContactColumnSpec &column) const;
    void applyColumnVisibility();
    void applyColumnOrder();
    void setColumnsHidden(const QStringList &ids, bool hidden);
    void showColumn(const QString &id);
    void addColumnToSheet(const QString &id);
    bool sheetCarriesField(const QString &field) const;
    QJsonArray tableViews() const;
    QStringList tableViewFields() const;
    QStringList shownViewFields() const;
    bool builtinTableView() const;
    bool allColumnsView() const;
    QStringList builtinViewFields() const;
    void renderTableViews();
    void selectTableView(const QString &id);
    void showTableViewMenu();
    void changeViewFields(const QStringList &fields);
    void createTableView(const QStringList &fields);
    void addTableView(const QString &name, const QStringList &fields, const QString &done);
    void saveTableViewDraft();
    void renameTableView();
    void deleteTableView();
    void saveTableViews(const QJsonArray &views, const QString &done);
    void showSheetMenu(const QPoint &position);
    // Every list and pipeline is a tab ahead of the fixed ones, carrying a
    // share badge with the count of people it reaches. The contacts table
    // moves into whichever of those tabs is open; `select` is one to land on.
    void renderSheetTabs(const QString &select = QString());
    void syncSheetTab();
    int sheetTabIndex(const QString &id) const;
    QString sheetTabId(int index) const;
    void showSheetTabMenu(const QString &id, const QPoint &globalPosition);
    // Shares the tab's list or pipeline with more organization members or
    // ForkMesh users; a pipeline tab with no list gets one bound to its flow.
    void addSheetMembers(const QString &id);
    void openBrowserExtension();
    void openSheet(const QString &id);
    void renameSheet(const QString &id);
    void moveSheet(const QString &id, int offset);
    // Takes the pipeline's flow out of the shared settings and deletes every
    // list bound to it; its contacts stay, under "No pipeline".
    void deletePipeline(const QString &id);
    void deleteList(const QString &id);
    void newPipeline();
    // Add, rename, restage, reorder and remove the organization's pipelines
    // in one place, instead of the raw "name | id | stages" lines the settings
    // dialog kept them in.
    // `focusFlow` opens the dialog on that pipeline, for editing its stages.
    void managePipelines(const QString &focusFlow = QString());
    void saveFlows(const QJsonArray &flows, const QString &note);
    QStringList flowStages(const QString &flowId) const;
    QString flowLabel(const QString &flowId) const;
    // The pipeline's type (the worker's FLOW_ICONS) and its glyph.
    QString flowIconId(const QString &flowId) const;
    QIcon flowIcon(const QString &flowId) const;
    void addEditStagesAction(QMenu *menu, const QString &flowId);
    int flowContactCount(const QString &flowId) const;
    // Which pipeline a list reads off. Empty makes it a plain list again, one
    // holding only the contacts put on it.
    void setSheetFlow(const QString &sheetId, const QString &flowId);
    void loadRankedGrants(const QString &sheetId);
    // Right-click on contact rows: move the selected contacts into a
    // pipeline (their flow), move them along its stages, or add them to,
    // move them to, or take them off a list.
    void showContactMenu(const QPoint &position);
    void moveContactsToSheet(const QStringList &ids, const QString &sheetId);
    // "Move to list", as opposed to "Add to list": the target list and no
    // other. Only plain lists can be moved to, because belonging to a
    // pipeline list is having its flow, not a membership that can be dropped.
    void moveContactsToListOnly(const QStringList &ids, const QString &sheetId);
    void removeContactsFromSheet(const QStringList &ids, const QString &sheetId);
    void setContactsStage(const QStringList &ids, const QString &stage);
    // Applies the edits to the table's own rows first, then sends them all as
    // one POST to /contacts/bulk and takes the worker's copies from the reply.
    // `gerund`/`past`/`target` build the messages ("Moving", "moved", "to X").
    void patchContacts(const QList<QPair<QString, QJsonObject>> &edits,
                       const QString &gerund, const QString &past,
                       const QString &target);
    QString zohoMailHost() const;
    void openMailbox(bool gmail);
    void filterContacts();
    void showColumnMenu(const QPoint &headerPosition);
    void addCustomColumn();
    void deleteCustomColumn(const QString &key);
    void deleteBuiltinColumn(const QString &field);
    void restoreBuiltinColumn(const QString &field);
    QStringList removedColumns() const;
    // Starts the next few reads the row refresh buttons queued, and rests
    // each button while its account is queued or in flight.
    void refreshMastodonUpdates();
    // The refresh button on a row's "Mastodon last update" cell: that
    // account's cached post goes stale and is read again. This is the only
    // thing that reads a contact's instance.
    void refreshMastodonNow(const QString &key);
    void fetchMastodonLatest(const QString &key);
    void applyMastodonLatest(const QString &key);
    void fillMastodonItem(QTableWidgetItem *item, const QString &key) const;
    // Contact pictures beside the name: queued once per load (opening the page
    // or pressing Refresh), read a few at a time, kept in memory for the
    // session and on disk for a week. A source that fails gives way to the
    // contact's next one, and the initials stay when none answers.
    void keepForkMeshAvatars(const QJsonArray &users);
    void readForkMeshAvatars();
    QImage forkMeshAvatar(const QString &email) const;
    void queueAvatars();
    void pumpAvatars();
    void fetchAvatar(const QString &source);
    void fetchAvatarImage(const QString &source, const QUrl &url);
    void finishAvatar(const QString &source, const QImage &image);
    void applyAvatars();
    QString avatarSource(const ContactRow &row) const;
    QIcon contactAvatar(const ContactRow &row);
    void openMailCompose(const QString &email);
    // Everything the organization knows about one contact, in the panel beside
    // the table: the picture at a size worth looking at, every field, the
    // messages tied to the record, and the newest posts on the accounts it
    // names. A click on a row or a picture in the grid opens it.
    void showContactDetail(const QString &contactId, bool reread = false);
    void closeContactDetail();
    // What one connected account answered with: why it could not be read,
    // the address of its picture, and its newest posts, newest first.
    using UpdatesHandler = std::function<void(const QString &, const QUrl &,
                                             const QList<AccountUpdate> &)>;
    // Reads one picture source ("mastodon:user@host", "bluesky:handle",
    // "github:user") for what that account has posted. `context` guards the
    // handlers, so a card closed mid-read drops what comes back.
    void readAccountUpdates(const QString &source, QObject *context,
                            UpdatesHandler done);
    void readMastodonUpdates(const QString &key, QObject *context,
                             UpdatesHandler done);
    void readBlueskyUpdates(const QString &actor, QObject *context,
                            UpdatesHandler done);
    void readGithubUpdates(const QString &user, QObject *context,
                           UpdatesHandler done);
    // The card's picture, read once per source per session.
    void readContactPhoto(const QString &source, const QUrl &picture,
                          QObject *context, std::function<void()> done);
    QJsonObject selectedContact() const;
    // Every selected, visible row of the contacts table (Ctrl/Shift+click).
    QList<QJsonObject> selectedContacts() const;
    QJsonObject selectedEmail() const;
    QJsonObject contactById(const QString &id) const;
    int rowOfContact(const QString &id) const;
    void updateContactField(const QString &id, const ContactColumnSpec &column,
                            const QString &value);
    // The options a table cell with a fixed set of values offers (Flow, Stage),
    // as (label, stored value); false for a cell edited as free text.
    bool contactFieldChoices(int row, int column,
                             QList<QPair<QString, QString>> *choices) const;
    void showLinkedEmails(const QString &contactId, bool toots,
                          const QPoint &globalPosition);
    void markContacted(const QStringList &ids, qint64 stamp);
    void editContact(const QJsonObject &contact = QJsonObject());
    void deleteContact(const QJsonObject &contact = QJsonObject());
    // A box made twice for one person: the name and email a row is known by,
    // a picker for the record it duplicates, and the merge itself, which folds
    // each duplicate into `keepId` on the worker and deletes it.
    QString contactLabel(const QJsonObject &contact) const;
    void pickMergeTarget(const QString &duplicateId);
    void mergeContacts(const QStringList &duplicateIds, const QString &keepId);
    void composeEmail(const QJsonObject &contact = QJsonObject(),
                      const QString &subject = QString(),
                      const QString &draft = QString(),
                      std::function<void()> sent = {});
    QJsonObject deckLinkFor(const QString &contactId) const;
    QString deckActivityLine(const QJsonObject &link) const;
    QString investorEmailPrompt(const QJsonObject &contact,
                                const QString &deckUrl) const;
    void draftInvestorEmail(const QJsonObject &contact);
    void askAgentForEmail(const QString &contactId, const QString &deckUrl);
    void syncDraftButton();
    void sendAutoEmail(const QString &contactId, const QString &mode = QString(),
                       const QString &reason = QString(),
                       const QJsonObject &draft = QJsonObject());
    void autoEmailWithAgent(const QString &contactId, const QString &mode,
                            const QString &reason, const QJsonObject &payload);
    QString outreachPrompt(const QJsonObject &contact) const;
    void copyOutreachPrompt(const QJsonObject &contact);
    struct AiDraft
    {
        QString contactId;
        QString format;
        QString mode;
        QString reason;
        QString brief;
        QList<QJsonObject> messages;
        QList<QPair<QString, AccountRead>> updates;
        bool updatesRead = false;
    };
    QString aiBrief() const;
    QString aiPrompt(const AiDraft &draft) const;
    void showAiWriter(const QString &contactId, const QString &mode = QString(),
                      const QString &reason = QString());
    void copyContactLink(const QJsonObject &contact);
    void copySheetLink(const QString &id);
    void noteSheetViewed();
    void showSheetHistory(const QString &id);
    void linkExternalEmail();
    void editSettings();
    void syncZohoMail();
    void connectZohoMail();
    void exchangeZohoCode(const QJsonObject &body);
    void logToot(const QString &contactId);
    void syncToots(bool automatic,
                   std::function<void(const QJsonObject &)> done = {});
    void showZohoGuide();
    QList<NoteDraft> noteDrafts() const;
    void syncNoteDrafts();
    void showNoteDrafts();
    void fillNoteDrafts();
    void sendNoteDraft(const NoteDraft &draft);
    void markNoteDraftSent(const QString &contactId, bool stampContacted);
    void importCsv();
    // Reads a Mastodon account's public followers straight from its instance
    // (the worker cannot reach arbitrary instances) and imports them as a
    // Name/Mastodon/Notes file, so the mapping step, tab naming, split upload
    // and pings are the CSV import's own.
    void importMastodonFollowers();
    void fetchMastodonFollowersPage(const QUrl &url);
    void finishMastodonFollowers(const QString &error = QString());
    void importGithubContributors();
    void runContributorsGit(const QStringList &arguments,
                            std::function<void(const QByteArray &)> done);
    void finishGithubContributors(const QString &error,
                                  const QByteArray &log = QByteArray());
    void importForkMeshUsers();
    void finishForkMeshUsers(const QString &path, const QJsonArray &users,
                             const QString &error);
    void updateUsersImportAction();
    bool importBusy() const;
    void setImportButtonsEnabled(bool enabled);
    void startCsvImport(const QString &path, const QString &fileName,
                        const QString &csv, const QString &sheetName,
                        const QString &viewId = QString(),
                        const QJsonArray &mapping = QJsonArray());
    bool mapImportColumns(const QJsonArray &columns);
    void beginImportUpload();
    QJsonObject importEnvelope() const;
    void sendImportPart();
    void failImport(const QString &error, const QJsonObject &payload);
    void finishImport();
    void showImportErrors();
    void exportCsv();
    void openProviderSend();
    void setStatus(const QString &text, bool error = false);
    void updateActions();

    Request m_request;
    std::function<void()> m_searchIndexChanged;
    std::function<void()> m_navigationBadgeChanged;
    int m_uncontacted = 0;
    std::function<QStringList()> m_userDirectory;
    std::function<void()> m_userDirectoryListener;
    std::function<bool()> m_userRecordsAvailable;
    std::function<void(forkmesh::market::NativePackageUserRecordsHandler)>
        m_userRecords;
    std::function<QString()> m_agentName;
    std::function<void(const QString &,
                       forkmesh::market::NativePackageAgentReplyHandler)>
        m_askAgent;
    QJsonArray m_decks;
    QSet<QString> m_drafting;
    QSet<QString> m_autoSending;
    QPointer<QPushButton> m_draftButton;
    bool m_usersReading = false;
    bool m_grantsLoading = false;
    QComboBox *m_organization = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_importProgress = nullptr;
    QPlainTextEdit *m_importErrors = nullptr;
    CsvImportJob m_import;
    QLabel *m_summary = nullptr;
    QLabel *m_filterNote = nullptr;
    QComboBox *m_stage = nullptr;
    QComboBox *m_flow = nullptr;
    QComboBox *m_sheet = nullptr;
    CrmStageStrip *m_stageStrip = nullptr;
    QFrame *m_nextCard = nullptr;
    QHash<QString, QPointer<QLabel>> m_nextPhotos;
    QSet<QString> m_nextSkipped;
    QTableWidget *m_contactsTable = nullptr;
    QTableWidget *m_emailsTable = nullptr;
    QTableWidget *m_mailTable = nullptr;
    QTableWidget *m_sheetsTable = nullptr;
    QPushButton *m_providerOpen = nullptr;
    QToolButton *m_settingsButton = nullptr;
    QToolButton *m_zohoSyncButton = nullptr;
    QToolButton *m_zohoGuideButton = nullptr;
    QWidget *m_zohoBar = nullptr;
    QLabel *m_zohoBarLabel = nullptr;
    QList<QPushButton *> m_zohoStepButtons;
    QToolButton *m_noteDraftsButton = nullptr;
    QPointer<QTableWidget> m_noteDraftsTable;
    QPointer<QLabel> m_noteDraftsCount;
    QToolButton *m_importButton = nullptr;
    QToolButton *m_githubButton = nullptr;
    QToolButton *m_usersButton = nullptr;
    QPushButton *m_usersPageButton = nullptr;
    QToolButton *m_exportButton = nullptr;
    QToolButton *m_newButton = nullptr;
    QToolButton *m_peopleButton = nullptr;
    QToolButton *m_historyButton = nullptr;
    QHash<QString, qint64> m_sheetViewedAt;
    QToolButton *m_pipelinesButton = nullptr;
    QList<QPair<QToolButton *, QString>> m_toolIcons;
    QListWidget *m_sheetList = nullptr;
    QWidget *m_body = nullptr;
    QWidget *m_peoplePage = nullptr;
    QJsonObject m_activity;
    QString m_viewerName;
    QComboBox *m_teamPeriod = nullptr;
    QComboBox *m_teamMember = nullptr;
    QTableWidget *m_teamTable = nullptr;
    CrmTeamChart *m_teamChart = nullptr;
    QLabel *m_teamChartTitle = nullptr;
    QListWidget *m_peopleGrid = nullptr;
    QComboBox *m_peopleSort = nullptr;
    QLabel *m_peopleNote = nullptr;
    QWidget *m_detailPanel = nullptr;
    QWidget *m_detailBody = nullptr;
    QString m_detailContactId;
    QString m_stageSheet;
    QJsonObject m_detailContact;
    QHash<QString, AccountRead> m_accountReads;
    QHash<QString, QImage> m_avatarImages;
    QHash<QString, QIcon> m_pictureIcons;
    bool m_peopleQueued = false;
    QPushButton *m_pipelineButton = nullptr;
    QPushButton *m_managePipelinesButton = nullptr;
    QTabWidget *m_tabs = nullptr;
    // Where the contacts table waits, off screen, while no contact tab holds it.
    QWidget *m_contactsPage = nullptr;
    QWidget *m_contactsBody = nullptr;
    QWidget *m_sheetsPage = nullptr;
    // One thread, so snapshot builds queue instead of racing each other.
    QThreadPool *m_workers = nullptr;
    ContactSnapshot m_snapshot;
    quint64 m_snapshotGeneration = 0;
    // The snapshot rows being put into the table, and how far that has got.
    QList<ContactRow> m_fillSource;
    QVector<int> m_fillRows;
    QVector<int> m_tableRows; // table row -> m_fillSource index
    int m_fillNext = 0;
    quint64 m_fillGeneration = 0;
    bool m_filling = false;
    bool m_renderQueued = false;
    QSet<QString> m_fillSelected;
    QString m_fillCurrent;
    // A contact picked from the top search box, selected once its row is in.
    QString m_focusContact;
    QHash<QString, qsizetype> m_contactIndex;
    // Pipelines no list is bound to, and "No pipeline", as client-side tabs.
    QList<QJsonObject> m_virtualSheets;
    bool m_sheetTabsSeeded = false;
    QWidget *m_installPanel = nullptr;
    QLabel *m_installNote = nullptr;
    QPushButton *m_installButton = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QJsonArray m_organizations;
    QJsonArray m_contacts;
    QJsonArray m_emails;
    // m_emails by the contact each is linked to, newest sent first: mail in
    // m_contactEmails, Mastodon conversations in m_contactToots.
    QHash<QString, QList<QJsonObject>> m_contactEmails;
    QHash<QString, QList<QJsonObject>> m_contactToots;
    QJsonArray m_mail;
    QJsonArray m_views;
    QJsonArray m_members;
    QJsonObject m_settings;
    // A list to select once the next load brings it (a new pipeline's tab).
    QString m_pendingSheet;
    QString m_role;
    QString m_query;
    QList<ContactColumnSpec> m_columns;
    QStringList m_customColumns;
    QComboBox *m_tableView = nullptr;
    QToolButton *m_tableViewMenu = nullptr;
    QToolButton *m_tableViewSave = nullptr;
    QString m_tableViewId = QStringLiteral("default");
    QStringList m_tableViewDraft;
    bool m_orderingColumns = false;
    QString m_sortId;
    bool m_sortDescending = false;
    QHash<QString, MastodonLatest> m_mastodonLatest;
    QSet<QString> m_mastodonPending;
    QStringList m_mastodonQueue;
    QHash<QString, QString> m_mastodonIds;
    QHash<QString, qint64> m_mastodonBackoff;
    bool m_nextRenderQueued = false;
    MastodonFollowersJob m_followers;
    GithubContributorsJob m_contributors;
    // Pictures by source, and the sources queued or in flight, or that
    // answered with nothing usable since the last load.
    QHash<QString, QIcon> m_avatars;
    QHash<QString, QIcon> m_initialsAvatars;
    QStringList m_avatarQueue;
    QSet<QString> m_avatarPending;
    QSet<QString> m_avatarFailed;
    QSet<QString> m_avatarChanged;
    // Pictures read for a detail card, at that card's size. A source that
    // answered with nothing is held as a null image, so it is asked once.
    QHash<QString, QImage> m_photos;
    int m_avatarInFlight = 0;
    bool m_avatarsWanted = false;
    bool m_avatarApplyQueued = false;
    QHash<QString, QByteArray> m_forkMeshAvatars;
    qint64 m_forkMeshAvatarsReadAt = 0;
    bool m_forkMeshAvatarsReading = false;
    bool m_canManageSettings = false;
    bool m_installed = false;
    bool m_loading = false;
    bool m_tootSyncRunning = false;
    bool m_rendering = false;
    // The summary revision last applied; a re-read sends it so an unchanged
    // organization comes back as a few bytes instead of every row.
    QString m_revision;
    // The account the organizations were read as, and the organization whose
    // records are on screen; a saved copy is drawn only for that pair.
    QString m_actor;
    QString m_shownOrganization;
    bool m_cacheRead = false;
};

} // namespace forkmesh::crm
