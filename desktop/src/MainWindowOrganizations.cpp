// Settings -> Organizations: the desktop twin of the website's organization
// panel (dashboard/js/04-account.js). One account owns many organizations;
// each has a member roster with roles, permission teams those members are
// grouped into, and the repositories the org name fronts at /<org>/<repo>.
//
// Everything here talks to the same /api/orgs endpoints the website uses. The
// one difference is the credential: a desktop that launched normally
// authenticated silently and holds its account key, not a session token, so
// each request is signed with the narrow per-operation proofs verified by
// _org_admin_signed_session in the worker's entry.py. Those canonical strings
// must stay byte-identical on both sides — see organizationAdminProof below.

#include "MainWindow.h"
#include "MainWindowInternal.h"

#include "KebabHeaderView.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QMessageBox>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTableWidget>

#include <utility>

using namespace forkmesh::ui;

namespace {

const QStringList &organizationRoles()
{
    static const QStringList roles{QStringLiteral("owner"),
                                   QStringLiteral("admin"),
                                   QStringLiteral("member")};
    return roles;
}

const QStringList &organizationTeamPermissions()
{
    static const QStringList permissions{
        QStringLiteral("read"), QStringLiteral("write"),
        QStringLiteral("maintain"), QStringLiteral("admin")};
    return permissions;
}

// Worker org-endpoint error codes -> human text, mirroring ORG_ERROR_TEXT in
// the website's dashboard so both surfaces explain a refusal the same way.
QString organizationErrorText(const QJsonObject &payload,
                              const QString &fallback)
{
    static const QHash<QString, QString> text{
        {QStringLiteral("invalid_org_name"),
         QStringLiteral("Invalid name. Use lowercase letters, numbers and "
                        "dashes.")},
        {QStringLiteral("invalid_team_name"),
         QStringLiteral("Invalid team name. Use lowercase letters, numbers "
                        "and dashes.")},
        {QStringLiteral("org_name_taken"),
         QStringLiteral("That name is already taken.")},
        {QStringLiteral("too_many_orgs"),
         QStringLiteral("You have reached the organization limit for this "
                        "account.")},
        {QStringLiteral("too_many_members"),
         QStringLiteral("This organization has reached its member limit.")},
        {QStringLiteral("too_many_teams"),
         QStringLiteral("This organization has reached its team limit.")},
        {QStringLiteral("too_many_repos"),
         QStringLiteral("This organization has reached its linked-repo "
                        "limit.")},
        {QStringLiteral("invalid_session"),
         QStringLiteral("Sign in to manage organizations.")},
        {QStringLiteral("forbidden"),
         QStringLiteral("Your role in this organization cannot make that "
                        "change.")},
        {QStringLiteral("last_owner"),
         QStringLiteral("An organization must keep at least one owner.")},
        {QStringLiteral("not_a_member"),
         QStringLiteral("That account is not a member of this organization.")},
        {QStringLiteral("unknown_account"),
         QStringLiteral("No ForkMesh account with that name.")},
        {QStringLiteral("unknown_repo"),
         QStringLiteral("That node has not published this repository yet.")},
        {QStringLiteral("not_your_node"),
         QStringLiteral("You can only link repositories published by your own "
                        "nodes.")},
        {QStringLiteral("bad_role"), QStringLiteral("Unknown member role.")},
        {QStringLiteral("bad_permission"),
         QStringLiteral("Unknown team permission.")},
        {QStringLiteral("email_not_verified"),
         QStringLiteral("Confirm your account email address before changing "
                        "an organization.")},
    };
    const QString code =
        payload.value(QStringLiteral("error")).toString().trimmed();
    if (code.isEmpty())
        return fallback;
    const auto known = text.constFind(code);
    if (known != text.constEnd())
        return known.value();
    QString readable = code;
    readable.replace(QLatin1Char('_'), QLatin1Char(' '));
    return readable;
}

QString organizationText(const QJsonObject &record, const QString &field)
{
    return record.value(field).toString().trimmed();
}

QString organizationTimestamp(qint64 milliseconds)
{
    if (milliseconds <= 0)
        return QString::fromUtf8("\xE2\x80\x94");
    return QDateTime::fromMSecsSinceEpoch(milliseconds)
        .toLocalTime()
        .toString(QStringLiteral("yyyy-MM-dd"));
}

QTableWidget *organizationTable(const QString &objectName,
                                const QStringList &headers, int stretchColumn)
{
    auto *table = new QTableWidget(0, headers.size());
    table->setObjectName(objectName);
    installColumnHeaderMenu(table);
    table->setHorizontalHeaderLabels(headers);
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideRight);
    QHeaderView *header = table->horizontalHeader();
    header->setStretchLastSection(false);
    for (int column = 0; column < headers.size(); ++column)
        header->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    if (stretchColumn >= 0 && stretchColumn < headers.size())
        header->setSectionResizeMode(stretchColumn, QHeaderView::Stretch);
    // A management table is a short list, not a scroll surface: sizing it to
    // its rows keeps every card on one page instead of stacking three
    // independently scrolling panes.
    table->setMinimumHeight(120);
    return table;
}

QComboBox *organizationChoice(const QString &objectName,
                              const QStringList &options,
                              const QString &selected)
{
    auto *combo = new QComboBox;
    combo->setObjectName(objectName);
    combo->addItems(options);
    const int index = combo->findText(selected);
    combo->setCurrentIndex(index < 0 ? options.size() - 1 : index);
    return combo;
}

QLineEdit *organizationInput(const QString &objectName,
                             const QString &placeholder, int maxLength)
{
    auto *edit = new QLineEdit;
    edit->setObjectName(objectName);
    edit->setMaxLength(maxLength);
    edit->setPlaceholderText(placeholder);
    return edit;
}

QHBoxLayout *organizationHeading(QLabel *label, const QString &octicon)
{
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    auto *glyph = new QLabel;
    setLabelOcticon(glyph, octicon, 12);
    row->addWidget(glyph, 0, Qt::AlignBottom);
    row->addWidget(label, 0, Qt::AlignBottom);
    row->addStretch();
    return row;
}

QPushButton *organizationButton(const QString &label, const QString &octicon,
                                bool danger = false)
{
    auto *button = new QPushButton(label);
    button->setObjectName(danger ? QStringLiteral("dangerButton")
                                 : QStringLiteral("ghostButton"));
    button->setCursor(Qt::PointingHandCursor);
    setOcticon(button, octicon, 14);
    return button;
}

QString selectedTableValue(QTableWidget *table, int column)
{
    if (!table)
        return QString();
    const int row = table->currentRow();
    if (row < 0 || row >= table->rowCount())
        return QString();
    QTableWidgetItem *item = table->item(row, column);
    return item ? item->data(Qt::UserRole).toString().trimmed() : QString();
}

void fillRow(QTableWidget *table, int row, const QStringList &columns)
{
    for (int column = 0; column < columns.size(); ++column) {
        auto *item = new QTableWidgetItem(columns.at(column));
        item->setData(Qt::UserRole, columns.at(column));
        table->setItem(row, column, item);
    }
}

QStringList manageableOrganizationNames(const QJsonArray &organizations)
{
    QStringList names;
    for (const QJsonValue &value : organizations) {
        const QJsonObject organization = value.toObject();
        const QString role =
            organizationText(organization, QStringLiteral("role")).toLower();
        if (role != QLatin1String("owner") && role != QLatin1String("admin"))
            continue;
        const QString name =
            organizationText(organization, QStringLiteral("name")).toLower();
        if (!name.isEmpty() &&
            !names.contains(name, Qt::CaseInsensitive)) {
            names.append(name);
        }
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

void fillNewRepositoryOrganizationChoices(QComboBox *choices,
                                          const QJsonArray &organizations,
                                          const QString &account,
                                          const QString &selected)
{
    if (!choices)
        return;
    const QSignalBlocker blocker(choices);
    choices->clear();
    choices->addItem(
        account.trimmed().isEmpty()
            ? QStringLiteral("Personal account")
            : QStringLiteral("%1 (personal)").arg(account.trimmed()),
        QString());
    for (const QString &organization :
         manageableOrganizationNames(organizations)) {
        choices->addItem(
            QStringLiteral("%1 (organization)").arg(organization),
            organization);
    }
    const int selectedIndex = choices->findData(selected);
    choices->setCurrentIndex(selectedIndex < 0 ? 0 : selectedIndex);
}

void setNewRepositoryOrganizationHint(QComboBox *choices, QLabel *hint,
                                      const QString &text)
{
    if (!choices || !hint)
        return;
    choices->setProperty("forkmeshOrganizationHint", text);
    if (choices->isEnabled())
        hint->setText(text);
}

} // namespace

// The organization requests this desktop can sign for, and what each
// signature covers. One proof per operation; the canonical additionally binds
// the request path and, for a write, every body field listed here in this
// order. Must stay in lockstep with _org_admin_operation in the worker's
// entry.py — a renamed proof or a reordered field list simply stops
// verifying, and the panel then tells a signed-in owner to sign in.
namespace {

// Path families, named after the relay regexes they mirror. All are anchored,
// so no two can match the same path and the order here does not matter.
const char kOrgsCollectionPattern[] = "^/api/orgs/?$";
const char kOrgItemPattern[] = "^/api/orgs/([^/]+)$";
const char kOrgMembersPattern[] = "^/api/orgs/([^/]+)/members$";
const char kOrgTeamsPattern[] = "^/api/orgs/([^/]+)/teams$";
const char kOrgTeamMembersPattern[] =
    "^/api/orgs/([^/]+)/teams/([^/]+)/members$";
const char kOrgReposPattern[] = "^/api/orgs/([^/]+)/repos$";

struct OrganizationAdminOperation
{
    const char *pattern;
    const char *method;
    const QString &proof;
    QStringList fields;
};

const QList<OrganizationAdminOperation> &organizationAdminOperations()
{
    static const QList<OrganizationAdminOperation> table{
        {kOrgsCollectionPattern, "GET", kOrgAdminListProof, {}},
        {kOrgsCollectionPattern, "POST", kOrgAdminCreateProof,
         {"name", "displayName", "description"}},
        {kOrgItemPattern, "GET", kOrgAdminReadProof, {}},
        {kOrgItemPattern, "PATCH", kOrgAdminProfileProof,
         {"displayName", "description"}},
        {kOrgItemPattern, "DELETE", kOrgAdminDeleteProof, {}},
        {kOrgMembersPattern, "GET", kOrgAdminReadProof, {}},
        {kOrgMembersPattern, "POST", kOrgAdminMemberSetProof,
         {"member", "role"}},
        {kOrgMembersPattern, "DELETE", kOrgAdminMemberRemoveProof, {"member"}},
        {kOrgTeamsPattern, "GET", kOrgAdminReadProof, {}},
        {kOrgTeamsPattern, "POST", kOrgAdminTeamSetProof,
         {"team", "permission"}},
        {kOrgTeamsPattern, "DELETE", kOrgAdminTeamRemoveProof, {"team"}},
        {kOrgTeamMembersPattern, "GET", kOrgAdminReadProof, {}},
        {kOrgTeamMembersPattern, "POST", kOrgAdminTeamMemberAddProof,
         {"member", "external"}},
        {kOrgTeamMembersPattern, "DELETE", kOrgAdminTeamMemberRemoveProof,
         {"member"}},
        {kOrgReposPattern, "GET", kOrgAdminReadProof, {}},
        {kOrgReposPattern, "POST", kOrgAdminRepoLinkProof, {"repo", "node"}},
        {kOrgReposPattern, "DELETE", kOrgAdminRepoUnlinkProof, {"repo"}},
    };
    return table;
}

} // namespace

QString MainWindow::organizationAdminProof(const QByteArray &method,
                                           const QString &path,
                                           QStringList *signedFields)
{
    if (signedFields)
        signedFields->clear();
    for (const OrganizationAdminOperation &operation :
         organizationAdminOperations()) {
        if (method != QByteArray(operation.method))
            continue;
        if (!QRegularExpression(QString::fromLatin1(operation.pattern))
                 .match(path)
                 .hasMatch())
            continue;
        if (signedFields)
            *signedFields = operation.fields;
        return operation.proof;
    }
    return QString();
}

// One body field as the signature covers it: the same bounded, control-char
// free form _org_admin_canonical_field builds on the relay. Every value the
// panel sends comes from a QLineEdit or a fixed list, so nothing here is ever
// rejected in practice.
QString MainWindow::organizationCanonicalField(const QJsonValue &value)
{
    if (value.isBool())
        return value.toBool() ? QStringLiteral("true") : QString();
    if (!value.isString())
        return QString();
    return value.toString().left(kOrgAdminFieldMax);
}

// Issue one organization request. Identical in shape to
// requestOrganizationTasks: an account session token when a password login
// produced one, otherwise the per-operation account-key signature, and a
// single reply handler that turns the worker's error codes into the same
// sentences the website shows.
void MainWindow::requestOrganizationAdmin(
    const QByteArray &method, const QString &path, const QJsonObject &body,
    OrganizationAdminReplyHandler handler)
{
    if (!m_networkAccess) {
        handler(false, {},
                QStringLiteral("Sign in to manage organizations."));
        return;
    }
    QUrl url = catalogApiUrl();
    url.setPath(path);
    url.setQuery(QString());
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setTransferTimeout(15000);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    QStringList signedFields;
    const QString proof = organizationAdminProof(method, path, &signedFields);
    // The relay hashes the path and every field that decides what the write
    // does, in this order, so a captured proof can never be replayed as a
    // different edit.
    QString resource = path;
    for (const QString &field : std::as_const(signedFields))
        resource += QLatin1Char('\n') +
                    organizationCanonicalField(body.value(field));
    const bool keySigned = accountSessionTokenForUrl(url).isEmpty();
    if (proof.isEmpty() ||
        !authenticateOrgTaskRequest(url, request, proof, resource)) {
        handler(false, {},
                m_accountAuthenticated
                    ? QString::fromUtf8(
                          "This change needs a password sign-in on this "
                          "device, not just its saved account key \xE2\x80\x94 "
                          "use Settings \xE2\x86\x92 \"Log in to a user "
                          "account\", then try again.")
                    : QStringLiteral("Sign in to manage organizations."));
        return;
    }
    const QByteArray payload =
        body.isEmpty()
            ? QByteArray()
            : QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (!payload.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
    logSystem(QStringLiteral("Organizations: %1 %2 requested.")
                  .arg(QString::fromLatin1(method), path));
    QNetworkReply *reply = nullptr;
    if (method == QByteArrayLiteral("GET"))
        reply = m_networkAccess->get(request);
    else if (method == QByteArrayLiteral("POST"))
        reply = m_networkAccess->post(request, payload);
    else
        reply = m_networkAccess->sendCustomRequest(request, method, payload);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, method, path, keySigned,
             handler = std::move(handler)]() mutable {
                const QByteArray raw = reply->readAll();
                const QJsonObject data = QJsonDocument::fromJson(raw).object();
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const bool ok = reply->error() == QNetworkReply::NoError &&
                                status >= 200 && status < 300 &&
                                data.value(QStringLiteral("ok")).toBool(true);
                QString error =
                    ok ? QString()
                       : organizationErrorText(
                             data, status > 0
                                       ? QStringLiteral("HTTP %1").arg(status)
                                       : reply->errorString());
                if (keySigned && status == 401)
                    error += QString::fromUtf8(
                        " \xE2\x80\x94 this machine's key is not authorized "
                        "for the account. Use Settings \xE2\x86\x92 \"Log in "
                        "to a user account\" to register it.");
                logSystem(QStringLiteral("Organizations: %1 %2 %3.")
                              .arg(QString::fromLatin1(method), path,
                                   ok ? QStringLiteral("completed")
                                      : QStringLiteral("failed: ") + error));
                reply->deleteLater();
                handler(ok, data, error);
            });
}

QString MainWindow::selectedOrganization() const
{
    return selectedTableValue(m_organizationsTable, 0);
}

void MainWindow::setOrganizationStatus(const QString &text, bool error)
{
    if (!m_organizationsStatus)
        return;
    m_organizationsStatus->setText(text);
    m_organizationsStatus->setStyleSheet(Theme::remapSheet(
        error ? QStringLiteral("color:#f85149;") : QString()));
}

void MainWindow::populateNewRepositoryOrganizationChoices(QComboBox *choices,
                                                           QLabel *hint)
{
    if (!choices || !hint)
        return;
    fillNewRepositoryOrganizationChoices(
        choices, m_organizations, accountOwner(),
        choices->currentData().toString());
    const QStringList cached = manageableOrganizationNames(m_organizations);
    setNewRepositoryOrganizationHint(
        choices, hint,
        cached.isEmpty()
            ? QString::fromUtf8(
                  "Loading organizations\xE2\x80\xA6 Choose one you own or "
                  "administer to also publish this repository at "
                  "/<org>/<repo>.")
            : QStringLiteral(
                  "Choose an organization you own or administer to also "
                  "publish this repository at /<org>/<repo>."));

    const QPointer<QComboBox> guardedChoices(choices);
    const QPointer<QLabel> guardedHint(hint);
    requestOrganizationAdmin(
        QByteArrayLiteral("GET"), QStringLiteral("/api/orgs"), {},
        [this, guardedChoices, guardedHint](bool ok,
                                            const QJsonObject &payload,
                                            const QString &error) {
            if (!guardedChoices || !guardedHint)
                return;
            const QString selected =
                guardedChoices->currentData().toString();
            if (ok) {
                m_organizations =
                    payload.value(QStringLiteral("orgs")).toArray();
                m_organizationsKnown = true;
            }
            fillNewRepositoryOrganizationChoices(
                guardedChoices, m_organizations, accountOwner(), selected);
            const QStringList manageable =
                manageableOrganizationNames(m_organizations);
            if (!ok) {
                setNewRepositoryOrganizationHint(
                    guardedChoices, guardedHint,
                    manageable.isEmpty()
                        ? QStringLiteral(
                              "Organizations are unavailable: %1. The "
                              "repository can still be created under your "
                              "personal account.")
                              .arg(error)
                        : QStringLiteral(
                              "Using saved organizations; refresh failed: %1")
                              .arg(error));
                return;
            }
            setNewRepositoryOrganizationHint(
                guardedChoices, guardedHint,
                manageable.isEmpty()
                    ? QString::fromUtf8(
                          "No organizations you can administer. Create one in "
                          "Settings \xE2\x86\x92 Organizations, or publish "
                          "under your personal account.")
                    : QStringLiteral(
                          "Choose an organization you own or administer to "
                          "also publish this repository at /<org>/<repo>."));
        });
}

void MainWindow::linkNewRepositoryToOrganization(
    const QString &organization, const QString &repository,
    const QString &node, int retry)
{
    const QString org = organization.trimmed().toLower();
    const QString repo = repository.trimmed().toLower();
    const QString servingNode = node.trimmed().toLower();
    if (org.isEmpty() || repo.isEmpty() || servingNode.isEmpty())
        return;

    requestOrganizationAdmin(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/orgs/%1/repos").arg(org),
        {{QStringLiteral("repo"), repo},
         {QStringLiteral("node"), servingNode}},
        [this, org, repo, servingNode, retry](
            bool ok, const QJsonObject &payload, const QString &error) {
            if (ok) {
                m_organizationCounts.remove(org);
                if (selectedOrganization() == org)
                    refreshOrganizationDetail();
                flashMessage(
                    QStringLiteral("Created %1/%2 and added it to %3 at "
                                   "/%3/%2.")
                        .arg(servingNode, repo, org));
                return;
            }

            // The local mirror and its catalog row are published
            // asynchronously. A just-created repository can therefore be
            // perfectly valid while the org endpoint still reports
            // unknown_repo for a few seconds. Retry only that expected race;
            // authorization and validation failures are final.
            static constexpr int retryDelaysMs[] = {
                1000, 2000, 4000, 8000, 15000, 30000};
            const QString code =
                payload.value(QStringLiteral("error")).toString();
            if (code == QLatin1String("unknown_repo") &&
                retry >= 0 &&
                retry < int(sizeof(retryDelaysMs) /
                            sizeof(retryDelaysMs[0]))) {
                if (retry == 0) {
                    flashMessage(
                        QString::fromUtf8(
                            "Created %1/%2; adding it to %3 as soon as the "
                            "network mirror is published\xE2\x80\xA6")
                            .arg(servingNode, repo, org));
                }
                QTimer::singleShot(
                    retryDelaysMs[retry], this,
                    [this, org, repo, servingNode, retry] {
                        linkNewRepositoryToOrganization(
                            org, repo, servingNode, retry + 1);
                    });
                return;
            }

            flashMessage(
                QString::fromUtf8(
                    "Created %1/%2, but could not add it to %3: %4. Link it "
                    "from Settings \xE2\x86\x92 Organizations.")
                    .arg(servingNode, repo, org, error),
                true);
        });
}

QWidget *MainWindow::buildOrganizationsTab()
{
    auto *page = new QWidget;
    auto *col = new QVBoxLayout(page);
    col->setContentsMargins(2, 14, 2, 14);
    col->setSpacing(10);

    auto *orgsLabel = new QLabel("ORGANIZATIONS");
    orgsLabel->setObjectName("sectionLabel");
    auto *orgsHint = new QLabel(
        "Organizations serve your repositories at /<org>/<repo> instead of a "
        "node name. Create one, invite accounts and give them a role, group "
        "them into permission teams, and link the repositories the "
        "organization fronts \xE2\x80\x94 the same controls as the website's "
        "Settings \xE2\x86\x92 Organizations page, signed with this machine's "
        "account key.");
    orgsHint->setObjectName("statusLine");
    orgsHint->setWordWrap(true);

    m_organizationsStatus = new QLabel;
    m_organizationsStatus->setObjectName("statusLine");
    m_organizationsStatus->setWordWrap(true);

    m_organizationsTable = organizationTable(
        QStringLiteral("organizationsTable"),
        {"Organization", "Your role", "Members", "Teams", "Repos"}, 0);
    connect(m_organizationsTable, &QTableWidget::itemSelectionChanged, this,
            [this] { refreshOrganizationDetail(); });

    auto *refreshButton = organizationButton("Refresh", "sync");
    connect(refreshButton, &QPushButton::clicked, this,
            [this] { refreshOrganizations(); });

    m_organizationNewName = organizationInput(
        QStringLiteral("organizationNewName"), "name (lowercase, dashes)", 63);
    m_organizationNewDisplay = organizationInput(
        QStringLiteral("organizationNewDisplay"), "Display name (optional)",
        80);
    m_organizationNewDescription = organizationInput(
        QStringLiteral("organizationNewDescription"),
        "Description (optional)", 500);
    auto *createButton = organizationButton("Create organization", "plus");
    connect(createButton, &QPushButton::clicked, this,
            [this] { createOrganization(); });

    auto *createForm = new QFormLayout;
    createForm->setLabelAlignment(Qt::AlignLeft);
    createForm->setSpacing(8);
    createForm->addRow("Name", m_organizationNewName);
    createForm->addRow("Display name", m_organizationNewDisplay);
    createForm->addRow("Description", m_organizationNewDescription);

    auto *createRow = new QHBoxLayout;
    createRow->setContentsMargins(0, 0, 0, 0);
    createRow->addWidget(refreshButton);
    createRow->addWidget(createButton);
    createRow->addStretch();

    col->addLayout(organizationHeading(orgsLabel, QStringLiteral("people")));
    col->addWidget(orgsHint);
    col->addWidget(m_organizationsStatus);
    col->addWidget(m_organizationsTable);
    col->addLayout(createForm);
    col->addLayout(createRow);

    m_organizationDetail = new QWidget;
    m_organizationDetail->setObjectName(
        QStringLiteral("organizationDetailPanel"));
    auto *detail = new QVBoxLayout(m_organizationDetail);
    detail->setContentsMargins(0, 8, 0, 0);
    detail->setSpacing(10);

    auto *profileLabel = new QLabel("SELECTED ORGANIZATION");
    profileLabel->setObjectName("sectionLabel");
    m_organizationHeading = new QLabel;
    m_organizationHeading->setObjectName("statusLine");
    m_organizationHeading->setWordWrap(true);
    m_organizationDisplayEdit = organizationInput(
        QStringLiteral("organizationDisplayEdit"), "Display name", 80);
    m_organizationDescriptionEdit = organizationInput(
        QStringLiteral("organizationDescriptionEdit"), "Description", 500);
    auto *saveProfileButton = organizationButton("Save profile", "pencil");
    connect(saveProfileButton, &QPushButton::clicked, this,
            [this] { saveOrganizationProfile(); });
    m_organizationDeleteButton =
        organizationButton("Delete organization", "trash", true);
    connect(m_organizationDeleteButton, &QPushButton::clicked, this,
            [this] { deleteOrganization(); });

    auto *profileForm = new QFormLayout;
    profileForm->setLabelAlignment(Qt::AlignLeft);
    profileForm->setSpacing(8);
    profileForm->addRow("Display name", m_organizationDisplayEdit);
    profileForm->addRow("Description", m_organizationDescriptionEdit);
    auto *profileRow = new QHBoxLayout;
    profileRow->setContentsMargins(0, 0, 0, 0);
    profileRow->addWidget(saveProfileButton);
    profileRow->addWidget(m_organizationDeleteButton);
    profileRow->addStretch();

    detail->addLayout(
        organizationHeading(profileLabel, QStringLiteral("shield-check")));
    detail->addWidget(m_organizationHeading);
    detail->addLayout(profileForm);
    detail->addLayout(profileRow);

    auto *membersLabel = new QLabel("MEMBERS");
    membersLabel->setObjectName("sectionLabel");
    auto *membersHint = new QLabel(
        "Members see the organization, its linked repositories and its teams. "
        "Admins additionally manage members, teams and repository links; only "
        "owners grant or remove owners and admins, and an organization always "
        "keeps one owner.");
    membersHint->setObjectName("statusLine");
    membersHint->setWordWrap(true);
    m_organizationMembersTable = organizationTable(
        QStringLiteral("organizationMembersTable"),
        {"Account", "Role", "Teams", "Member since"}, 2);
    m_organizationMemberName = organizationInput(
        QStringLiteral("organizationMemberName"), "Account name", 63);
    m_organizationMemberRole = organizationChoice(
        QStringLiteral("organizationMemberRole"), organizationRoles(),
        QStringLiteral("member"));
    auto *memberAddButton = organizationButton("Add / set role", "person");
    connect(memberAddButton, &QPushButton::clicked, this,
            [this] { saveOrganizationMember(); });
    auto *memberRemoveButton = organizationButton("Remove member", "trash", true);
    connect(memberRemoveButton, &QPushButton::clicked, this,
            [this] { removeOrganizationMember(); });
    connect(m_organizationMembersTable, &QTableWidget::itemSelectionChanged,
            this, [this] {
                const QString member =
                    selectedTableValue(m_organizationMembersTable, 0);
                if (member.isEmpty() || !m_organizationMemberName)
                    return;
                m_organizationMemberName->setText(member);
                const QString role =
                    selectedTableValue(m_organizationMembersTable, 1);
                const int index = m_organizationMemberRole->findText(role);
                if (index >= 0)
                    m_organizationMemberRole->setCurrentIndex(index);
            });

    auto *memberRow = new QHBoxLayout;
    memberRow->setContentsMargins(0, 0, 0, 0);
    memberRow->addWidget(m_organizationMemberName, 1);
    memberRow->addWidget(m_organizationMemberRole);
    memberRow->addWidget(memberAddButton);
    memberRow->addWidget(memberRemoveButton);

    detail->addLayout(organizationHeading(membersLabel, QStringLiteral("people")));
    detail->addWidget(membersHint);
    detail->addWidget(m_organizationMembersTable);
    detail->addLayout(memberRow);

    auto *teamsLabel = new QLabel("TEAMS");
    teamsLabel->setObjectName("sectionLabel");
    auto *teamsHint = new QLabel(
        "A team is a named subset of the roster holding one repository "
        "permission over every linked repo: read views, write changes "
        "content, maintain manages repository work, admin manages repository "
        "access. Team permission never changes a member's organization role.");
    teamsHint->setObjectName("statusLine");
    teamsHint->setWordWrap(true);
    m_organizationTeamsTable = organizationTable(
        QStringLiteral("organizationTeamsTable"),
        {"Team", "Permission", "Members"}, 0);
    m_organizationTeamName = organizationInput(
        QStringLiteral("organizationTeamName"), "Team name", 63);
    m_organizationTeamPermission = organizationChoice(
        QStringLiteral("organizationTeamPermission"),
        organizationTeamPermissions(), QStringLiteral("read"));
    auto *teamSaveButton = organizationButton("Create / set permission", "plus");
    connect(teamSaveButton, &QPushButton::clicked, this,
            [this] { saveOrganizationTeam(); });
    auto *teamDeleteButton = organizationButton("Delete team", "trash", true);
    connect(teamDeleteButton, &QPushButton::clicked, this,
            [this] { deleteOrganizationTeam(); });
    connect(m_organizationTeamsTable, &QTableWidget::itemSelectionChanged, this,
            [this] { onOrganizationTeamSelected(); });

    auto *teamRow = new QHBoxLayout;
    teamRow->setContentsMargins(0, 0, 0, 0);
    teamRow->addWidget(m_organizationTeamName, 1);
    teamRow->addWidget(m_organizationTeamPermission);
    teamRow->addWidget(teamSaveButton);
    teamRow->addWidget(teamDeleteButton);

    m_organizationTeamMembersLabel = new QLabel;
    m_organizationTeamMembersLabel->setObjectName("statusLine");
    m_organizationTeamMembersLabel->setWordWrap(true);
    m_organizationTeamMembersTable = organizationTable(
        QStringLiteral("organizationTeamMembersTable"),
        {"Account", "Kind", "On team since"}, 0);
    m_organizationTeamMemberName = organizationInput(
        QStringLiteral("organizationTeamMemberName"), "Account name", 63);
    auto *teamMemberAddButton = organizationButton("Add to team", "person");
    connect(teamMemberAddButton, &QPushButton::clicked, this,
            [this] { addOrganizationTeamMember(); });
    auto *teamMemberRemoveButton =
        organizationButton("Remove from team", "trash", true);
    connect(teamMemberRemoveButton, &QPushButton::clicked, this,
            [this] { removeOrganizationTeamMember(); });

    auto *teamMemberRow = new QHBoxLayout;
    teamMemberRow->setContentsMargins(0, 0, 0, 0);
    teamMemberRow->addWidget(m_organizationTeamMemberName, 1);
    teamMemberRow->addWidget(teamMemberAddButton);
    teamMemberRow->addWidget(teamMemberRemoveButton);

    detail->addLayout(organizationHeading(teamsLabel, QStringLiteral("key")));
    detail->addWidget(teamsHint);
    detail->addWidget(m_organizationTeamsTable);
    detail->addLayout(teamRow);
    detail->addWidget(m_organizationTeamMembersLabel);
    detail->addWidget(m_organizationTeamMembersTable);
    detail->addLayout(teamMemberRow);

    auto *reposLabel = new QLabel("LINKED REPOSITORIES");
    reposLabel->setObjectName("sectionLabel");
    auto *reposHint = new QLabel(
        "Linking publishes a repository at /<org>/<repo>. You can only link "
        "repositories one of your own nodes has already published, so an "
        "organization can never take over another node's URL.");
    reposHint->setObjectName("statusLine");
    reposHint->setWordWrap(true);
    m_organizationReposTable = organizationTable(
        QStringLiteral("organizationReposTable"),
        {"Repository", "Served by node"}, 0);
    m_organizationRepoName = organizationInput(
        QStringLiteral("organizationRepoName"), "Repository name", 80);
    m_organizationRepoNode = organizationInput(
        QStringLiteral("organizationRepoNode"),
        "Node namespace (blank = your account)", 63);
    auto *repoLinkButton = organizationButton("Link repository", "link");
    connect(repoLinkButton, &QPushButton::clicked, this,
            [this] { linkOrganizationRepository(); });
    auto *repoUnlinkButton = organizationButton("Unlink", "trash", true);
    connect(repoUnlinkButton, &QPushButton::clicked, this,
            [this] { unlinkOrganizationRepository(); });

    auto *repoRow = new QHBoxLayout;
    repoRow->setContentsMargins(0, 0, 0, 0);
    repoRow->addWidget(m_organizationRepoName, 1);
    repoRow->addWidget(m_organizationRepoNode, 1);
    repoRow->addWidget(repoLinkButton);
    repoRow->addWidget(repoUnlinkButton);

    detail->addLayout(organizationHeading(reposLabel, QStringLiteral("repo")));
    detail->addWidget(reposHint);
    detail->addWidget(m_organizationReposTable);
    detail->addLayout(repoRow);

    m_organizationDetail->hide();
    col->addWidget(m_organizationDetail);
    col->addStretch();
    return page;
}

void MainWindow::refreshOrganizations()
{
    if (m_organizationsLoading)
        return;
    m_organizationsLoading = true;
    setOrganizationStatus(
        QString::fromUtf8("Loading organizations\xE2\x80\xA6"), false);
    const QString keep = m_organizationPendingSelection.isEmpty()
                             ? selectedOrganization()
                             : m_organizationPendingSelection;
    m_organizationPendingSelection.clear();
    requestOrganizationAdmin(
        QByteArrayLiteral("GET"), QStringLiteral("/api/orgs"), {},
        [this, keep](bool ok, const QJsonObject &payload,
                     const QString &error) {
            m_organizationsLoading = false;
            const QList<std::function<void()>> waiters =
                std::exchange(m_organizationsWaiters, {});
            if (!ok) {
                setOrganizationStatus(
                    QStringLiteral("Organizations unavailable: %1").arg(error),
                    true);
                for (const auto &waiter : waiters)
                    waiter();
                return;
            }
            m_organizations = payload.value(QStringLiteral("orgs")).toArray();
            m_organizationsKnown = true;
            for (const auto &waiter : waiters)
                waiter();
            renderOrganizations(keep);
            // Pull details can request this roster before the Settings tab is
            // opened. Re-evaluate the peer gate now that an organization owner
            // can be recognized for an organization-fronted repository.
            updatePullActionState();
            setOrganizationStatus(
                m_organizations.isEmpty()
                    ? QStringLiteral("No organizations yet. Create one below.")
                    : QStringLiteral("%1 organization%2 for %3.")
                          .arg(m_organizations.size())
                          .arg(m_organizations.size() == 1 ? QString()
                                                           : QStringLiteral("s"),
                               accountOwner()),
                false);
        });
}

void MainWindow::renderOrganizations(const QString &keepSelected)
{
    if (!m_organizationsTable)
        return;
    const QSignalBlocker blocker(m_organizationsTable);
    m_organizationsTable->setRowCount(0);
    m_organizationsTable->setRowCount(m_organizations.size());
    int selectRow = -1;
    for (int row = 0; row < m_organizations.size(); ++row) {
        const QJsonObject org = m_organizations.at(row).toObject();
        const QString name = organizationText(org, QStringLiteral("name"));
        // Counts arrive with the per-org profile, so a list built before any
        // detail load shows a dash rather than a misleading zero.
        const QJsonObject counts = m_organizationCounts.value(name).toObject();
        const auto count = [&counts](const QString &field) {
            return counts.contains(field)
                       ? QString::number(counts.value(field).toInt())
                       : QString::fromUtf8("\xE2\x80\x94");
        };
        fillRow(m_organizationsTable, row,
                {name, organizationText(org, QStringLiteral("role")),
                 count(QStringLiteral("members")),
                 count(QStringLiteral("teams")),
                 count(QStringLiteral("repos"))});
        if (name == keepSelected)
            selectRow = row;
    }
    if (selectRow >= 0)
        m_organizationsTable->selectRow(selectRow);
    else if (m_organizationsTable->rowCount() > 0)
        m_organizationsTable->selectRow(0);
    // The blocker above swallowed the selection change, so ask for the detail
    // explicitly — including the empty case, which hides the detail half.
    refreshOrganizationDetail();
}

void MainWindow::createOrganization()
{
    if (!m_organizationNewName)
        return;
    const QString name = m_organizationNewName->text().trimmed().toLower();
    if (name.isEmpty()) {
        setOrganizationStatus(QStringLiteral("Enter an organization name."),
                              true);
        return;
    }
    const QJsonObject body{
        {QStringLiteral("name"), name},
        {QStringLiteral("displayName"),
         m_organizationNewDisplay->text().trimmed()},
        {QStringLiteral("description"),
         m_organizationNewDescription->text().trimmed()}};
    setOrganizationStatus(QString::fromUtf8("Creating\xE2\x80\xA6"), false);
    requestOrganizationAdmin(
        QByteArrayLiteral("POST"), QStringLiteral("/api/orgs"), body,
        [this, name](bool ok, const QJsonObject &, const QString &error) {
            if (!ok) {
                setOrganizationStatus(
                    QStringLiteral("Could not create %1: %2").arg(name, error),
                    true);
                return;
            }
            m_organizationNewName->clear();
            m_organizationNewDisplay->clear();
            m_organizationNewDescription->clear();
            m_organizationPendingSelection = name;
            refreshOrganizations();
        });
}

void MainWindow::deleteOrganization()
{
    const QString org = selectedOrganization();
    if (org.isEmpty())
        return;
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Delete organization"),
        QStringLiteral(
            "Delete %1? This dissolves the organization, its members, teams "
            "and repository links. The linked repositories themselves are not "
            "deleted.")
            .arg(org),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    requestOrganizationAdmin(
        QByteArrayLiteral("DELETE"),
        QStringLiteral("/api/orgs/%1").arg(org), {},
        [this, org](bool ok, const QJsonObject &, const QString &error) {
            if (!ok) {
                setOrganizationStatus(
                    QStringLiteral("Could not delete %1: %2").arg(org, error),
                    true);
                return;
            }
            m_organizationPendingSelection.clear();
            m_organizationCounts.remove(org);
            refreshOrganizations();
        });
}

void MainWindow::refreshOrganizationDetail()
{
    const QString org = selectedOrganization();
    if (!m_organizationDetail)
        return;
    if (org.isEmpty()) {
        m_organizationDetail->hide();
        return;
    }
    m_organizationDetail->show();
    m_organizationHeading->setText(
        QString::fromUtf8("Loading %1\xE2\x80\xA6").arg(org));
    // Each pane is loaded independently so one refused subresource (a plain
    // member may not read the team layout) never blanks the whole page.
    requestOrganizationAdmin(
        QByteArrayLiteral("GET"), QStringLiteral("/api/orgs/%1").arg(org), {},
        [this, org](bool ok, const QJsonObject &payload, const QString &error) {
            if (org != selectedOrganization())
                return;
            if (!ok) {
                m_organizationHeading->setText(
                    QStringLiteral("%1: %2").arg(org, error));
                return;
            }
            applyOrganizationProfile(org, payload);
        });
    loadOrganizationMembers(org);
    loadOrganizationTeams(org);
    loadOrganizationRepositories(org);
}

void MainWindow::applyOrganizationProfile(const QString &org,
                                          const QJsonObject &payload)
{
    const QString role =
        organizationText(payload, QStringLiteral("viewerRole"));
    const QString display =
        organizationText(payload, QStringLiteral("displayName"));
    m_organizationHeading->setText(
        QString::fromUtf8("%1%2 \xC2\xB7 you are %3 \xC2\xB7 serving /%1/<repo>")
            .arg(org,
                 display.isEmpty() ? QString()
                                   : QStringLiteral(" (%1)").arg(display),
                 role.isEmpty() ? QStringLiteral("not a member") : role));
    m_organizationDisplayEdit->setText(display);
    m_organizationDescriptionEdit->setText(
        organizationText(payload, QStringLiteral("description")));
    const bool canManage =
        role == QLatin1String("owner") || role == QLatin1String("admin");
    m_organizationDisplayEdit->setEnabled(canManage);
    m_organizationDescriptionEdit->setEnabled(canManage);
    m_organizationDeleteButton->setEnabled(role == QLatin1String("owner"));
    QJsonObject counts;
    counts.insert(QStringLiteral("members"),
                  payload.value(QStringLiteral("members")).toInt());
    counts.insert(QStringLiteral("teams"),
                  payload.value(QStringLiteral("teams")).toInt());
    counts.insert(QStringLiteral("repos"),
                  payload.value(QStringLiteral("repos")).toArray().size());
    m_organizationCounts.insert(org, counts);
    static const QStringList countFields{QStringLiteral("members"),
                                         QStringLiteral("teams"),
                                         QStringLiteral("repos")};
    const int row = m_organizationsTable->currentRow();
    if (row >= 0) {
        const QSignalBlocker blocker(m_organizationsTable);
        for (int column = 2; column < 5; ++column) {
            const QString value = QString::number(
                counts.value(countFields.at(column - 2)).toInt());
            auto *item = new QTableWidgetItem(value);
            item->setData(Qt::UserRole, value);
            m_organizationsTable->setItem(row, column, item);
        }
    }
}

void MainWindow::saveOrganizationProfile()
{
    const QString org = selectedOrganization();
    if (org.isEmpty())
        return;
    const QJsonObject body{
        {QStringLiteral("displayName"),
         m_organizationDisplayEdit->text().trimmed()},
        {QStringLiteral("description"),
         m_organizationDescriptionEdit->text().trimmed()}};
    requestOrganizationAdmin(
        QByteArrayLiteral("PATCH"), QStringLiteral("/api/orgs/%1").arg(org),
        body, [this, org](bool ok, const QJsonObject &, const QString &error) {
            setOrganizationStatus(
                ok ? QStringLiteral("Saved the %1 profile.").arg(org)
                   : QStringLiteral("Could not save %1: %2").arg(org, error),
                !ok);
            if (ok)
                refreshOrganizationDetail();
        });
}

void MainWindow::loadOrganizationMembers(const QString &org)
{
    requestOrganizationAdmin(
        QByteArrayLiteral("GET"),
        QStringLiteral("/api/orgs/%1/members").arg(org), {},
        [this, org](bool ok, const QJsonObject &payload, const QString &error) {
            if (org != selectedOrganization() || !m_organizationMembersTable)
                return;
            const QSignalBlocker blocker(m_organizationMembersTable);
            m_organizationMembersTable->setRowCount(0);
            if (!ok) {
                setOrganizationStatus(
                    QStringLiteral("Members unavailable: %1").arg(error), true);
                return;
            }
            const QJsonArray members =
                payload.value(QStringLiteral("members")).toArray();
            m_organizationMembersTable->setRowCount(members.size());
            for (int row = 0; row < members.size(); ++row) {
                const QJsonObject member = members.at(row).toObject();
                QStringList teams;
                for (const QJsonValue &team :
                     member.value(QStringLiteral("teams")).toArray())
                    teams << team.toString();
                fillRow(m_organizationMembersTable, row,
                        {organizationText(member, QStringLiteral("name")),
                         organizationText(member, QStringLiteral("role")),
                         teams.join(QStringLiteral(", ")),
                         organizationTimestamp(static_cast<qint64>(
                             member.value(QStringLiteral("since")).toDouble()))});
            }
        });
}

void MainWindow::saveOrganizationMember()
{
    const QString org = selectedOrganization();
    const QString member =
        m_organizationMemberName->text().trimmed().toLower();
    if (org.isEmpty() || member.isEmpty()) {
        setOrganizationStatus(QStringLiteral("Enter an account name."), true);
        return;
    }
    const QString role = m_organizationMemberRole->currentText();
    const QJsonObject body{{QStringLiteral("member"), member},
                           {QStringLiteral("role"), role}};
    requestOrganizationAdmin(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/orgs/%1/members").arg(org), body,
        [this, org, member, role](bool ok, const QJsonObject &,
                                  const QString &error) {
            setOrganizationStatus(
                ok ? QStringLiteral("%1 is now %2 in %3.")
                         .arg(member, role, org)
                   : QStringLiteral("Could not update %1: %2")
                         .arg(member, error),
                !ok);
            if (ok) {
                m_organizationMemberName->clear();
                refreshOrganizationDetail();
            }
        });
}

void MainWindow::removeOrganizationMember()
{
    const QString org = selectedOrganization();
    const QString member = selectedTableValue(m_organizationMembersTable, 0);
    if (org.isEmpty() || member.isEmpty()) {
        setOrganizationStatus(
            QStringLiteral("Select the member to remove."), true);
        return;
    }
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Remove member"),
        QStringLiteral("Remove %1 from %2?").arg(member, org),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    requestOrganizationAdmin(
        QByteArrayLiteral("DELETE"),
        QStringLiteral("/api/orgs/%1/members").arg(org),
        {{QStringLiteral("member"), member}},
        [this, org, member](bool ok, const QJsonObject &,
                            const QString &error) {
            setOrganizationStatus(
                ok ? QStringLiteral("Removed %1 from %2.").arg(member, org)
                   : QStringLiteral("Could not remove %1: %2")
                         .arg(member, error),
                !ok);
            if (ok)
                refreshOrganizationDetail();
        });
}

void MainWindow::loadOrganizationTeams(const QString &org)
{
    requestOrganizationAdmin(
        QByteArrayLiteral("GET"),
        QStringLiteral("/api/orgs/%1/teams").arg(org), {},
        [this, org](bool ok, const QJsonObject &payload, const QString &error) {
            if (org != selectedOrganization() || !m_organizationTeamsTable)
                return;
            const QSignalBlocker blocker(m_organizationTeamsTable);
            m_organizationTeamsTable->setRowCount(0);
            if (!ok) {
                m_organizationTeamMembersLabel->setText(
                    QStringLiteral("Teams unavailable: %1").arg(error));
                return;
            }
            const QJsonArray teams =
                payload.value(QStringLiteral("teams")).toArray();
            m_organizationTeamsTable->setRowCount(teams.size());
            for (int row = 0; row < teams.size(); ++row) {
                const QJsonObject team = teams.at(row).toObject();
                fillRow(m_organizationTeamsTable, row,
                        {organizationText(team, QStringLiteral("team")),
                         organizationText(team, QStringLiteral("permission")),
                         QString::number(
                             team.value(QStringLiteral("members")).toInt())});
            }
            m_organizationTeamMembersLabel->setText(
                teams.isEmpty()
                    ? QStringLiteral("No teams yet. Create one above.")
                    : QStringLiteral("Select a team to see who is on it."));
            m_organizationTeamMembersTable->setRowCount(0);
        });
}

void MainWindow::onOrganizationTeamSelected()
{
    const QString org = selectedOrganization();
    const QString team = selectedTableValue(m_organizationTeamsTable, 0);
    if (team.isEmpty() || org.isEmpty())
        return;
    m_organizationTeamName->setText(team);
    const QString permission = selectedTableValue(m_organizationTeamsTable, 1);
    const int index = m_organizationTeamPermission->findText(permission);
    if (index >= 0)
        m_organizationTeamPermission->setCurrentIndex(index);
    m_organizationTeamMembersLabel->setText(
        QString::fromUtf8("Loading the %1 team\xE2\x80\xA6").arg(team));
    requestOrganizationAdmin(
        QByteArrayLiteral("GET"),
        QStringLiteral("/api/orgs/%1/teams/%2/members").arg(org, team), {},
        [this, org, team](bool ok, const QJsonObject &payload,
                          const QString &error) {
            if (org != selectedOrganization() ||
                team != selectedTableValue(m_organizationTeamsTable, 0))
                return;
            const QSignalBlocker blocker(m_organizationTeamMembersTable);
            m_organizationTeamMembersTable->setRowCount(0);
            if (!ok) {
                m_organizationTeamMembersLabel->setText(
                    QStringLiteral("%1 team unavailable: %2")
                        .arg(team, error));
                return;
            }
            const QJsonArray members =
                payload.value(QStringLiteral("members")).toArray();
            m_organizationTeamMembersTable->setRowCount(members.size());
            for (int row = 0; row < members.size(); ++row) {
                const QJsonObject member = members.at(row).toObject();
                fillRow(m_organizationTeamMembersTable, row,
                        {organizationText(member, QStringLiteral("name")),
                         member.value(QStringLiteral("external")).toBool()
                             ? QStringLiteral("collaborator")
                             : QStringLiteral("member"),
                         organizationTimestamp(static_cast<qint64>(
                             member.value(QStringLiteral("since")).toDouble()))});
            }
            m_organizationTeamMembersLabel->setText(
                QString::fromUtf8("%1 \xC2\xB7 %2 permission \xC2\xB7 %3 on the "
                               "team")
                    .arg(team,
                         organizationText(payload, QStringLiteral("permission")),
                         QString::number(members.size())));
        });
}

void MainWindow::saveOrganizationTeam()
{
    const QString org = selectedOrganization();
    const QString team = m_organizationTeamName->text().trimmed().toLower();
    if (org.isEmpty() || team.isEmpty()) {
        setOrganizationStatus(QStringLiteral("Enter a team name."), true);
        return;
    }
    const QJsonObject body{
        {QStringLiteral("team"), team},
        {QStringLiteral("permission"),
         m_organizationTeamPermission->currentText()}};
    requestOrganizationAdmin(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/orgs/%1/teams").arg(org), body,
        [this, org, team](bool ok, const QJsonObject &, const QString &error) {
            setOrganizationStatus(
                ok ? QStringLiteral("Team %1 saved in %2.").arg(team, org)
                   : QStringLiteral("Could not save %1: %2").arg(team, error),
                !ok);
            if (ok)
                refreshOrganizationDetail();
        });
}

void MainWindow::deleteOrganizationTeam()
{
    const QString org = selectedOrganization();
    const QString team = selectedTableValue(m_organizationTeamsTable, 0);
    if (org.isEmpty() || team.isEmpty()) {
        setOrganizationStatus(QStringLiteral("Select the team to delete."),
                              true);
        return;
    }
    const auto answer = QMessageBox::warning(
        this, QStringLiteral("Delete team"),
        QStringLiteral("Delete the %1 team in %2? Its members keep their "
                       "organization role and lose the team's repository "
                       "permission.")
            .arg(team, org),
        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes)
        return;
    requestOrganizationAdmin(
        QByteArrayLiteral("DELETE"),
        QStringLiteral("/api/orgs/%1/teams").arg(org),
        {{QStringLiteral("team"), team}},
        [this, org, team](bool ok, const QJsonObject &, const QString &error) {
            setOrganizationStatus(
                ok ? QStringLiteral("Deleted the %1 team.").arg(team)
                   : QStringLiteral("Could not delete %1: %2").arg(team, error),
                !ok);
            if (ok)
                refreshOrganizationDetail();
        });
}

void MainWindow::addOrganizationTeamMember()
{
    const QString org = selectedOrganization();
    const QString team = selectedTableValue(m_organizationTeamsTable, 0);
    const QString member =
        m_organizationTeamMemberName->text().trimmed().toLower();
    if (org.isEmpty() || team.isEmpty() || member.isEmpty()) {
        setOrganizationStatus(
            QStringLiteral("Select a team and enter an account name."), true);
        return;
    }
    // Team membership can only ever raise an existing member's permission.
    // Someone outside the roster joins as an explicit external collaborator,
    // which the relay refuses unless it is asked for by name.
    const bool onRoster = organizationHasMember(member);
    const QJsonObject body{{QStringLiteral("member"), member},
                           {QStringLiteral("external"), !onRoster}};
    requestOrganizationAdmin(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/orgs/%1/teams/%2/members").arg(org, team), body,
        [this, team, member, onRoster](bool ok, const QJsonObject &,
                                       const QString &error) {
            setOrganizationStatus(
                ok ? QStringLiteral("Added %1 to %2%3.")
                         .arg(member, team,
                              onRoster ? QString()
                                       : QStringLiteral(
                                             " as an external collaborator"))
                   : QStringLiteral("Could not add %1: %2").arg(member, error),
                !ok);
            if (ok) {
                m_organizationTeamMemberName->clear();
                onOrganizationTeamSelected();
            }
        });
}

void MainWindow::removeOrganizationTeamMember()
{
    const QString org = selectedOrganization();
    const QString team = selectedTableValue(m_organizationTeamsTable, 0);
    const QString member =
        selectedTableValue(m_organizationTeamMembersTable, 0);
    if (org.isEmpty() || team.isEmpty() || member.isEmpty()) {
        setOrganizationStatus(
            QStringLiteral("Select the team member to remove."), true);
        return;
    }
    requestOrganizationAdmin(
        QByteArrayLiteral("DELETE"),
        QStringLiteral("/api/orgs/%1/teams/%2/members").arg(org, team),
        {{QStringLiteral("member"), member}},
        [this, team, member](bool ok, const QJsonObject &,
                             const QString &error) {
            setOrganizationStatus(
                ok ? QStringLiteral("Removed %1 from %2.").arg(member, team)
                   : QStringLiteral("Could not remove %1: %2")
                         .arg(member, error),
                !ok);
            if (ok)
                onOrganizationTeamSelected();
        });
}

bool MainWindow::organizationHasMember(const QString &member) const
{
    if (!m_organizationMembersTable)
        return false;
    for (int row = 0; row < m_organizationMembersTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_organizationMembersTable->item(row, 0);
        if (item && item->text().compare(member, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

void MainWindow::loadOrganizationRepositories(const QString &org)
{
    requestOrganizationAdmin(
        QByteArrayLiteral("GET"),
        QStringLiteral("/api/orgs/%1/repos").arg(org), {},
        [this, org](bool ok, const QJsonObject &payload, const QString &error) {
            if (org != selectedOrganization() || !m_organizationReposTable)
                return;
            const QSignalBlocker blocker(m_organizationReposTable);
            m_organizationReposTable->setRowCount(0);
            if (!ok) {
                setOrganizationStatus(
                    QStringLiteral("Linked repositories unavailable: %1")
                        .arg(error),
                    true);
                return;
            }
            const QJsonArray repos =
                payload.value(QStringLiteral("repos")).toArray();
            m_organizationReposTable->setRowCount(repos.size());
            for (int row = 0; row < repos.size(); ++row) {
                const QJsonObject repo = repos.at(row).toObject();
                fillRow(m_organizationReposTable, row,
                        {organizationText(repo, QStringLiteral("repo")),
                         organizationText(repo, QStringLiteral("node"))});
            }
        });
}

void MainWindow::linkOrganizationRepository()
{
    const QString org = selectedOrganization();
    const QString repo = m_organizationRepoName->text().trimmed().toLower();
    if (org.isEmpty() || repo.isEmpty()) {
        setOrganizationStatus(QStringLiteral("Enter a repository name."), true);
        return;
    }
    const QString node = m_organizationRepoNode->text().trimmed().toLower();
    const QJsonObject body{
        {QStringLiteral("repo"), repo},
        {QStringLiteral("node"), node.isEmpty() ? accountOwner().toLower()
                                                : node}};
    requestOrganizationAdmin(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/orgs/%1/repos").arg(org), body,
        [this, org, repo](bool ok, const QJsonObject &, const QString &error) {
            setOrganizationStatus(
                ok ? QStringLiteral("Linked %1 at /%2/%1.").arg(repo, org)
                   : QStringLiteral("Could not link %1: %2").arg(repo, error),
                !ok);
            if (ok) {
                m_organizationRepoName->clear();
                refreshOrganizationDetail();
            }
        });
}

void MainWindow::unlinkOrganizationRepository()
{
    const QString org = selectedOrganization();
    const QString repo = selectedTableValue(m_organizationReposTable, 0);
    if (org.isEmpty() || repo.isEmpty()) {
        setOrganizationStatus(
            QStringLiteral("Select the repository to unlink."), true);
        return;
    }
    requestOrganizationAdmin(
        QByteArrayLiteral("DELETE"),
        QStringLiteral("/api/orgs/%1/repos").arg(org),
        {{QStringLiteral("repo"), repo}},
        [this, org, repo](bool ok, const QJsonObject &, const QString &error) {
            setOrganizationStatus(
                ok ? QStringLiteral("Unlinked %1 from %2.").arg(repo, org)
                   : QStringLiteral("Could not unlink %1: %2")
                         .arg(repo, error),
                !ok);
            if (ok)
                refreshOrganizationDetail();
        });
}
