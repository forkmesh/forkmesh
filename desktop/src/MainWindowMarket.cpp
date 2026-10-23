#include "MainWindow.h"
#include "MessageBoxClicks.h"
#include "ProcessReaper.h"
#include "AppPaths.h"
#include "BackoffNetworkAccessManager.h"
#include "ClaudeStreamSession.h"
#include "CodexAppServerSession.h"

#include "CaptureFeature.h"
#include "ForkMeshPackageRegistry.h"
#include "LedgerFeature.h"
#include "MainWindowInternal.h"
#include "OfficeChannelMirror.h"
#include "NativePackageExtension.h"
#include "PackageFeedback.h"
#include "ServerNode.h"
#include "SettingsSectionsView.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QFont>
#include <functional>
#include <memory>
#include <QAction>
#include <QSaveFile>
#include <QMessageBox>
#include <QMenu>
#include <QFileInfo>
#include <QFileDialog>
#include <QFile>
#include <QDir>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayoutItem>
#include <QLineEdit>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QImageReader>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

using forkmesh::market::PackageManifest;

namespace {

void clearPackageCards(QVBoxLayout *layout)
{
    if (!layout)
        return;
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

QString trustLine(const PackageManifest &manifest)
{
    QStringList trust;
    trust << (manifest.creatorVerified
                  ? QString::fromUtf8("\xE2\x9C\x93 verified creator")
                  : QString::fromUtf8("\xE2\x9A\xA0 unverified creator"));
    if (manifest.creatorRetainsOwnership)
        trust << QString::fromUtf8("\xE2\x9C\x93 creator retains ownership");
    trust << (manifest.sourceAvailable
                  ? QString::fromUtf8("\xE2\x9C\x93 source available")
                  : QString::fromUtf8("\xE2\x9A\xA0 source unavailable"));
    trust << (manifest.reproducibleBuild
                  ? QString::fromUtf8("\xE2\x9C\x93 reproducible build")
                  : QString::fromUtf8("\xE2\x9A\xA0 build not reproducible"));
    trust << (manifest.signatureVerified
                  ? QString::fromUtf8("\xE2\x9C\x93 bundled signature verified")
                  : QString::fromUtf8("\xE2\x9A\xA0 signature not verified"));
    return trust.join(QString::fromUtf8("  \xC2\xB7  "));
}

QString modelSizeLabel(qint64 bytes)
{
    const double gigabytes = double(bytes) / 1000000000.0;
    return QStringLiteral("%1 GB download").arg(gigabytes, 0, 'f', 1);
}

QString nativePackageErrorText(const QJsonObject &payload,
                               const QString &fallback)
{
    QString error = payload.value(QStringLiteral("error")).toString().trimmed();
    if (error.isEmpty())
        return fallback;
    error.replace(QLatin1Char('_'), QLatin1Char(' '));
    return error;
}

bool allowedOllamaEndpoint(const QString &text)
{
    const QUrl url(text.trimmed());
    const QString host = url.host().toLower();
    return url.isValid() && url.scheme() == QLatin1String("http") &&
           (host == QLatin1String("127.0.0.1") ||
            host == QLatin1String("localhost") ||
            host == QLatin1String("::1")) &&
           url.path() == QLatin1String("/v1/chat/completions") &&
           url.query().isEmpty() && url.fragment().isEmpty();
}

QString processProgressText(QString output)
{
    static const QRegularExpression ansi(
        QStringLiteral("\\x1b\\[[0-?]*[ -/]*[@-~]"));
    output.remove(ansi);
    output.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = output.split(QLatin1Char('\n'),
                                           Qt::SkipEmptyParts);
    return lines.isEmpty() ? QString() : lines.constLast().trimmed();
}

bool ollamaUpdateRequired(const QString &output)
{
    return output.contains(
        QStringLiteral("requires a newer version of Ollama"),
        Qt::CaseInsensitive);
}

QStringList marketSearchTerms(const QString &query)
{
    return query.toCaseFolded().split(QRegularExpression(QStringLiteral("\\s+")),
                                      Qt::SkipEmptyParts);
}

bool marketPackageMatches(const PackageManifest &manifest,
                          const QStringList &terms)
{
    if (terms.isEmpty())
        return true;
    QStringList fields = {manifest.id,          manifest.name,
                          manifest.summary,     manifest.creatorHandle,
                          manifest.creatorName, manifest.category,
                          manifest.type,        manifest.licenseSpdx,
                          manifest.priceLabel()};
    for (const forkmesh::market::PackagePermission &permission :
         manifest.permissions)
        fields << permission.label;
    const QString text = fields.join(QLatin1Char('\n')).toCaseFolded();
    for (const QString &term : terms) {
        if (!text.contains(term))
            return false;
    }
    return true;
}

bool marketPackageMatchesCategory(const PackageManifest &manifest,
                                  const QString &category)
{
    if (category.isEmpty())
        return true;
    if (category == QLatin1String("virtual-goods"))
        return manifest.type.startsWith(QLatin1String("virtual."));
    if (category == QLatin1String("agents"))
        return manifest.type == QLatin1String("agent");
    if (category == QLatin1String("services"))
        return manifest.type.startsWith(QLatin1String("service."));
    return category == QLatin1String("themes") &&
           manifest.type == QLatin1String("theme");
}

void setMarketCardShown(QWidget *card, bool shown)
{
    const bool explicitlyHidden =
        card->isHidden() &&
        card->testAttribute(Qt::WA_WState_ExplicitShowHide);
    if (shown == explicitlyHidden)
        card->setVisible(shown);
}

const QString kStatusPagePackageId = QStringLiteral("status-page");
const QString kStatusMonitorsRequestProof =
    QStringLiteral("forkmesh-status-monitors-request-v1");
const QString kSignInForPackages = QStringLiteral(
    "Sign in to your ForkMesh account on this device to use this package.");
constexpr qint64 kAccountPackagesReadGapMs = 60 * 1000;
constexpr qint64 kAccountPackagesForcedGapMs = 5 * 1000;

QString packageNameList(const forkmesh::market::PackageRegistry &registry,
                        const QStringList &ids)
{
    QStringList names;
    for (const QString &id : ids) {
        const PackageManifest *manifest = registry.package(id);
        names << (manifest ? manifest->name : id);
    }
    return names.join(QStringLiteral(", "));
}

QString packagePlaceLabel(const QString &place)
{
    if (place == QLatin1String("mobile"))
        return QStringLiteral("Phone");
    if (place == QLatin1String("desktop"))
        return QStringLiteral("Desktop");
    return QStringLiteral("Web");
}

void appendPackagePlaces(
    QBoxLayout *layout, const PackageManifest &manifest,
    const QStringList &chosen, bool placedHere,
    const std::function<void(const QStringList &)> &changed)
{
    auto *row = new QWidget;
    row->setObjectName(QStringLiteral("marketPackagePlaces"));
    row->setProperty("packageId", manifest.id);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(12);
    auto *label = new QLabel(QStringLiteral("Show on"));
    label->setObjectName(QStringLiteral("mutedLabel"));
    rowLayout->addWidget(label);
    QList<QCheckBox *> boxes;
    for (const QString &place : manifest.places) {
        auto *box = new QCheckBox(packagePlaceLabel(place));
        box->setObjectName(QStringLiteral("marketPackagePlace-%1").arg(place));
        box->setProperty("packageId", manifest.id);
        box->setProperty("place", place);
        box->setChecked(chosen.contains(place));
        box->setAccessibleName(QStringLiteral("Show %1 on %2")
                                   .arg(manifest.name, packagePlaceLabel(place)));
        boxes.append(box);
        rowLayout->addWidget(box);
    }
    int checked = 0;
    for (QCheckBox *box : std::as_const(boxes))
        checked += box->isChecked() ? 1 : 0;
    for (QCheckBox *box : std::as_const(boxes)) {
        if (checked == 1 && box->isChecked()) {
            box->setEnabled(false);
            box->setToolTip(QStringLiteral(
                "%1 has to show somewhere. Remove it to take it off "
                "everywhere.")
                                .arg(manifest.name));
        }
        QObject::connect(box, &QCheckBox::clicked, row, [boxes, changed] {
            QStringList places;
            for (QCheckBox *other : boxes) {
                if (other->isChecked())
                    places.append(other->property("place").toString());
            }
            if (!places.isEmpty())
                changed(places);
        });
    }
    rowLayout->addStretch();
    layout->addWidget(row);
    if (placedHere)
        return;
    auto *note = new QLabel(
        manifest.places.contains(
            forkmesh::market::PackageRegistry::desktopPlace())
            ? QStringLiteral("Not shown on this desktop. Turn on Desktop to "
                             "show it here.")
            : QStringLiteral("%1 has nothing to show on the desktop.")
                  .arg(manifest.name));
    note->setObjectName(QStringLiteral("marketPackageNotShownHere"));
    note->setProperty("packageId", manifest.id);
    note->setTextFormat(Qt::PlainText);
    note->setWordWrap(true);
    layout->addWidget(note);
}

bool validStatusMonitorId(const QString &id)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return pattern.match(id).hasMatch();
}

bool validStatusMonitorUrl(const QString &text)
{
    const QUrl url(text, QUrl::StrictMode);
    return !text.isEmpty() && text.size() <= 300 && url.isValid() &&
           (url.scheme() == QLatin1String("http") ||
            url.scheme() == QLatin1String("https")) &&
           !url.host().isEmpty() && url.userInfo().isEmpty();
}

QString statusMonitorErrorText(const QJsonObject &payload,
                               const QString &fallback)
{
    const QString code = payload.value(QStringLiteral("error")).toString();
    if (code == QLatin1String("invalid_name"))
        return QStringLiteral("Give the monitor a name of 1 to 40 characters.");
    if (code == QLatin1String("invalid_url"))
        return QStringLiteral(
            "Enter an http or https address with a host and no user name or "
            "password, up to 300 characters.");
    if (code == QLatin1String("too_many_monitors"))
        return QStringLiteral("You already have the most monitors allowed.");
    if (code == QLatin1String("monitor_not_found"))
        return QStringLiteral("That monitor was already removed.");
    if (code == QLatin1String("status_page_not_installed"))
        return QStringLiteral(
            "Status Page is not installed on your account.");
    if (code == QLatin1String("invalid_session") ||
        fallback == kSignInForPackages)
        return QStringLiteral(
            "Sign in to your ForkMesh account to manage your monitors.");
    return fallback.isEmpty()
               ? QStringLiteral("The monitor could not be saved.")
               : fallback;
}

} // namespace

QWidget *MainWindow::buildMarketSection()
{
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("marketSection"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(24, 20, 24, 20);
    outer->setSpacing(12);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("ForkMesh Market"));
    title->setObjectName(QStringLiteral("sectionTitle"));
    title->setAccessibleName(QStringLiteral("ForkMesh Market"));
    header->addWidget(title);
    header->addStretch();
    m_marketSearch = new QLineEdit;
    m_marketSearch->setObjectName(QStringLiteral("marketSearch"));
    m_marketSearch->setPlaceholderText(QStringLiteral("Search the Market…"));
    m_marketSearch->setAccessibleName(QStringLiteral("Search the Market"));
    m_marketSearch->setClearButtonEnabled(true);
    m_marketSearch->setFixedWidth(260);
    m_marketSearch->setToolTip(QStringLiteral(
        "Show only the packages whose name, description, creator, category, "
        "type, licence, price or permissions contain every word typed"));
    connect(m_marketSearch, &QLineEdit::textChanged, this,
            [this] { applyMarketSearch(); });
    header->addWidget(m_marketSearch);
    m_marketSort = new QComboBox;
    m_marketSort->setObjectName(QStringLiteral("marketSort"));
    m_marketSort->setAccessibleName(QStringLiteral("Sort the Market"));
    m_marketSort->setToolTip(QStringLiteral("Choose the order of the package lists"));
    m_marketSort->addItem(QStringLiteral("Featured"), QStringLiteral("featured"));
    m_marketSort->addItem(QStringLiteral("Name A\u2013Z"), QStringLiteral("name"));
    m_marketSort->addItem(QStringLiteral("Top rated"), QStringLiteral("rating"));
    m_marketSort->addItem(QStringLiteral("Most installed"), QStringLiteral("installs"));
    m_marketSort->addItem(QStringLiteral("Most downloaded"), QStringLiteral("downloads"));
    m_marketSort->addItem(QStringLiteral("Most used"), QStringLiteral("used"));
    const int savedSort = m_marketSort->findData(
        QSettings().value(QStringLiteral("market/sort")).toString());
    m_marketSort->setCurrentIndex(qMax(0, savedSort));
    connect(m_marketSort, &QComboBox::currentIndexChanged, this, [this] {
        QSettings().setValue(QStringLiteral("market/sort"),
                             m_marketSort->currentData().toString());
        refreshMarketPage();
    });
    header->addWidget(m_marketSort);
    m_marketInstallAllButton = new QPushButton(QStringLiteral("Install all"));
    m_marketInstallAllButton->setObjectName(QStringLiteral("marketInstallAllButton"));
    m_marketInstallAllButton->setAccessibleName(
        QStringLiteral("Install every Market package"));
    connect(m_marketInstallAllButton, &QPushButton::clicked, this, [this] {
        if (m_packageRegistry && m_packageRegistry->isReady())
            installPackageSet(QStringLiteral("Install all"),
                              m_packageRegistry->installAllPackages());
    });
    header->addWidget(m_marketInstallAllButton);
    auto *publish = new QPushButton(QStringLiteral("Publish an item"));
    publish->setObjectName(QStringLiteral("marketPublishButton"));
    publish->setAccessibleName(QStringLiteral("Publish a Market item"));
    publish->setToolTip(
        QStringLiteral("Open the Market publishing instructions"));
    connect(publish, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(
            QUrl(QStringLiteral("https://forkmesh.com/market")));
    });
    header->addWidget(publish);
    outer->addLayout(header);

    auto *categories = new QWidget;
    categories->setObjectName(QStringLiteral("marketCategoryButtons"));
    auto *categoryLayout = new QHBoxLayout(categories);
    categoryLayout->setContentsMargins(0, 0, 0, 0);
    categoryLayout->setSpacing(8);
    const auto addCategoryButton =
        [this, categories, categoryLayout](const QString &text,
                                           const QString &category) {
            auto *button = new QPushButton(text, categories);
            button->setObjectName(
                QStringLiteral("marketCategory-%1Button").arg(category));
            button->setAccessibleName(QStringLiteral("Show %1").arg(text));
            button->setCheckable(true);
            connect(button, &QPushButton::clicked, this,
                    [this, categories, button, category] {
                        if (button->isChecked()) {
                            for (QPushButton *other :
                                 categories->findChildren<QPushButton *>()) {
                                if (other != button)
                                    other->setChecked(false);
                            }
                        }
                        m_marketCategoryFilter = button->isChecked()
                                                     ? category
                                                     : QString();
                        if (m_marketTabs)
                            m_marketTabs->setCurrentIndex(m_marketAllTabIndex);
                        applyMarketSearch();
                    });
            categoryLayout->addWidget(button);
        };
    addCategoryButton(QStringLiteral("Virtual goods"),
                      QStringLiteral("virtual-goods"));
    addCategoryButton(QStringLiteral("Agents"), QStringLiteral("agents"));
    addCategoryButton(QStringLiteral("Services"), QStringLiteral("services"));
    addCategoryButton(QStringLiteral("Themes"), QStringLiteral("themes"));
    categoryLayout->addStretch();
    outer->addWidget(categories);

    m_marketStatus = new QLabel;
    m_marketStatus->setObjectName(QStringLiteral("marketStatus"));
    m_marketStatus->setTextFormat(Qt::PlainText);
    m_marketStatus->setWordWrap(true);
    m_marketStatus->hide();
    outer->addWidget(m_marketStatus);

    m_marketSearchEmpty = new QLabel;
    m_marketSearchEmpty->setObjectName(QStringLiteral("marketSearchEmpty"));
    m_marketSearchEmpty->setTextFormat(Qt::PlainText);
    m_marketSearchEmpty->setWordWrap(true);
    m_marketSearchEmpty->hide();
    outer->addWidget(m_marketSearchEmpty);

    auto *tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("marketTabs"));
    tabs->setDocumentMode(true);
    tabs->setAccessibleName(QStringLiteral("Market package lists"));
    m_marketTabs = tabs;

    const auto addListTab = [](const QString &objectName,
                               QVBoxLayout **layout) {
        auto *scroll = new QScrollArea;
        scroll->setObjectName(objectName);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *host = new QWidget;
        *layout = new QVBoxLayout(host);
        (*layout)->setContentsMargins(4, 12, 4, 12);
        (*layout)->setSpacing(10);
        scroll->setWidget(host);
        return scroll;
    };
    m_marketAllTabIndex = tabs->addTab(
        addListTab(QStringLiteral("marketList"), &m_marketListLayout),
        QStringLiteral("All"));
    m_marketInstalledTabIndex = tabs->addTab(
        addListTab(QStringLiteral("marketInstalledList"),
                   &m_marketInstalledListLayout),
        QStringLiteral("Installed"));
    m_marketNotInstalledTabIndex = tabs->addTab(
        addListTab(QStringLiteral("marketNotInstalledList"),
                   &m_marketNotInstalledListLayout),
        QStringLiteral("Not installed"));
    m_marketPacksTabIndex = tabs->addTab(
        addListTab(QStringLiteral("marketPacksList"), &m_marketPacksLayout),
        QStringLiteral("Packs"));
    m_marketGuidesTabIndex = tabs->addTab(
        addListTab(QStringLiteral("marketGuidesList"), &m_marketGuidesLayout),
        QStringLiteral("Guides"));
    outer->addWidget(tabs, 1);

    return page;
}

void MainWindow::showPackageSettings(const QString &id)
{
    if (!id.isEmpty() &&
        (!m_packageRegistry || !m_packageRegistry->package(id))) {
        return;
    }
    m_marketSettingsPackageId = id;
    refreshMarketPage();
    if (id.isEmpty())
        return;
    refreshAccountPackages(true);
    if (id == kStatusPagePackageId) {
        m_statusMonitorsError.clear();
        refreshStatusMonitors(true);
    }
}

QWidget *MainWindow::buildPackageSettingsPage(
    const PackageManifest &manifest)
{
    auto *page = new QFrame;
    page->setObjectName(QStringLiteral("marketPackageSettingsPage"));
    page->setProperty("packageId", manifest.id);
    page->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(14);

    auto *heading = new QHBoxLayout;
    auto *back = new QPushButton(QString::fromUtf8("\xE2\x86\x90 Market"));
    back->setObjectName(QStringLiteral("marketPackageSettingsBack"));
    connect(back, &QPushButton::clicked, this,
            [this] { showPackageSettings(QString()); });
    heading->addWidget(back);
    auto *title = new QLabel(
        QStringLiteral("<b>%1 settings</b> <span style='color:#8b949e'>v%2</span>")
            .arg(manifest.name.toHtmlEscaped(),
                 manifest.version.toHtmlEscaped()));
    title->setObjectName(QStringLiteral("sectionTitle"));
    title->setTextFormat(Qt::RichText);
    heading->addWidget(title);
    heading->addStretch();
    layout->addLayout(heading);

    auto *summary = new QLabel(manifest.summary);
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *metadata = new QLabel(
        QStringLiteral("Creator: %1 &nbsp;·&nbsp; Type: %2 &nbsp;·&nbsp; License: %3 "
                       "&nbsp;·&nbsp; Settings: %4")
            .arg(forkmesh::ui::userProfileLinkHtml(
                     manifest.creatorHandle, QStringLiteral("@") + manifest.creatorHandle),
                 manifest.type.toHtmlEscaped(), manifest.licenseSpdx.toHtmlEscaped(),
                 manifest.settingsEntrypoint.toHtmlEscaped()));
    metadata->setTextFormat(Qt::RichText);
    metadata->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    metadata->setOpenExternalLinks(false);
    connect(metadata, &QLabel::linkActivated, this,
            [this](const QString &href) { openBodyReference(href); });
    metadata->setObjectName(QStringLiteral("mutedLabel"));
    metadata->setWordWrap(true);
    layout->addWidget(metadata);

    const bool packageInstalled = m_packageRegistry->isInstalled(manifest.id);
    if (!manifest.dependencies.isEmpty()) {
        auto *needs = new QLabel(
            QStringLiteral("Needs: %1")
                .arg(packageNameList(*m_packageRegistry, manifest.dependencies)));
        needs->setObjectName(QStringLiteral("marketPackageDependencies"));
        needs->setTextFormat(Qt::PlainText);
        needs->setWordWrap(true);
        layout->addWidget(needs);
    }
    const bool statusPage = manifest.id == kStatusPagePackageId;
    if (!manifest.agent.isValid() &&
        (!manifest.settings.isEmpty() || statusPage)) {
        if (!manifest.settings.isEmpty())
            appendDeclaredPackageSettings(layout, manifest, packageInstalled);
        if (statusPage && m_packageRegistry->isEffectivelyInstalled(manifest.id))
            appendStatusMonitorSettings(layout);
    } else if (!manifest.agent.isValid()) {
        auto *notice = new QLabel(QStringLiteral(
            "This package has no configurable runtime options. Its lifecycle, "
            "permissions and package-owned data are still managed here."));
        notice->setObjectName(QStringLiteral("marketPackageGenericSettings"));
        notice->setWordWrap(true);
        layout->addWidget(notice);
    } else {
        auto *agentRow = new QHBoxLayout;
        auto *avatar = new QLabel;
        avatar->setObjectName(QStringLiteral("marketAgentAvatar"));
        avatar->setFixedSize(96, 96);
        avatar->setAlignment(Qt::AlignCenter);
        const QPixmap art(manifest.agent.avatarPath);
        avatar->setPixmap(art.scaled(avatar->size(), Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation));
        avatar->setAccessibleName(
            QStringLiteral("%1 agent avatar").arg(manifest.name));
        agentRow->addWidget(avatar, 0, Qt::AlignTop);

        auto *agentCopy = new QVBoxLayout;
        auto *runtime = new QLabel(QStringLiteral(
            "Runs locally through Ollama. Forkmesh activates the model only "
            "after Ollama confirms that it is available."));
        runtime->setWordWrap(true);
        agentCopy->addWidget(runtime);
        const QString executable = QStandardPaths::findExecutable(
            QStringLiteral("ollama"));
        auto *runtimeStatus = new QLabel(
            executable.isEmpty()
                ? QStringLiteral("Ollama is not installed or is not on PATH.")
                : QStringLiteral("Ollama found: %1").arg(executable));
        runtimeStatus->setObjectName(QStringLiteral("marketAgentRuntimeStatus"));
        runtimeStatus->setWordWrap(true);
        agentCopy->addWidget(runtimeStatus);
        agentRow->addLayout(agentCopy, 1);
        layout->addLayout(agentRow);

        auto *form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        auto *variant = new QComboBox;
        variant->setObjectName(QStringLiteral("marketAgentModelVariant"));
        QString desired = m_packageRegistry
                              ->packageSetting(manifest.id,
                                               QStringLiteral("desiredModel"),
                                               manifest.agent.defaultModel)
                              .toString();
        if (!manifest.agent.model(desired))
            desired = manifest.agent.defaultModel;
        for (const forkmesh::market::PackageAgentModel &model :
             manifest.agent.models) {
            variant->addItem(
                QStringLiteral("%1 · %2 · %3K context")
                    .arg(model.label, modelSizeLabel(model.downloadBytes))
                    .arg(model.contextTokens / 1024),
                model.id);
        }
        const int desiredRow = variant->findData(desired);
        variant->setCurrentIndex(qMax(0, desiredRow));
        form->addRow(QStringLiteral("Model variant"), variant);

        auto *endpoint = new QLineEdit(
            m_packageRegistry
                ->packageSetting(manifest.id, QStringLiteral("endpoint"),
                                 manifest.agent.endpoint)
                .toString());
        endpoint->setObjectName(QStringLiteral("marketAgentEndpoint"));
        endpoint->setToolTip(QStringLiteral(
            "Only the Ollama-compatible loopback chat-completions endpoint is allowed"));
        form->addRow(QStringLiteral("Local endpoint"), endpoint);
        layout->addLayout(form);

        const QString active =
            m_packageRegistry
                ->packageSetting(manifest.id, QStringLiteral("activeModel"))
                .toString();
        auto *activeLabel = new QLabel(
            active.isEmpty()
                ? QStringLiteral("No %1 model is active yet.")
                      .arg(manifest.name)
                : QStringLiteral("Active model: %1").arg(active));
        activeLabel->setObjectName(QStringLiteral("marketAgentActiveModel"));
        layout->addWidget(activeLabel);

        connect(variant, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, id = manifest.id, variant](int) {
                    m_packageRegistry->setPackageSetting(
                        id, QStringLiteral("desiredModel"),
                        variant->currentData().toString());
                });
        connect(endpoint, &QLineEdit::editingFinished, this,
                [this, id = manifest.id, endpoint,
                 fallback = manifest.agent.endpoint] {
                    const QString value = endpoint->text().trimmed();
                    if (!allowedOllamaEndpoint(value)) {
                        endpoint->setText(fallback);
                        flashMessage(QStringLiteral(
                            "The model endpoint must be the Ollama-compatible "
                            "chat-completions path on localhost."), true);
                        return;
                    }
                    m_packageRegistry->setPackageSetting(
                        id, QStringLiteral("endpoint"), value);
                    syncPackageRuntime();
                });

        m_marketModelStatus = new QLabel;
        m_marketModelStatus->setObjectName(
            QStringLiteral("marketAgentDownloadStatus"));
        m_marketModelStatus->setWordWrap(true);
        if (m_marketModelPackageId == manifest.id &&
            !m_marketModelStage.isEmpty()) {
            const QString lead = m_marketModelStage == QLatin1String("check")
                                     ? QStringLiteral("Checking the local model")
                                     : QStringLiteral("Downloading the model");
            const QString detail = processProgressText(m_marketModelOutput);
            m_marketModelStatus->setText(
                detail.isEmpty() ? lead + QStringLiteral("…")
                                 : QStringLiteral("%1… %2").arg(lead, detail));
        } else if (!m_marketModelOutput.isEmpty()) {
            m_marketModelStatus->setText(m_marketModelOutput);
        } else {
            m_marketModelStatus->setText(QStringLiteral(
                "Downloads stay in Ollama's model store. A model downloaded "
                "by this package is removed when the package is removed; a "
                "model that was already present is left untouched."));
        }
        layout->addWidget(m_marketModelStatus);

        auto *buttons = new QHBoxLayout;
        const bool needsRuntimeUpdate =
            m_marketModelPackageId == manifest.id &&
            ollamaUpdateRequired(m_marketModelOutput);
        auto *runtimeDownload = new QPushButton(
            needsRuntimeUpdate ? QStringLiteral("Update Ollama")
                               : QStringLiteral("Get Ollama"));
        runtimeDownload->setObjectName(
            QStringLiteral("marketAgentRuntimeDownload"));
        connect(runtimeDownload, &QPushButton::clicked, this,
                [url = manifest.agent.runtimeDownloadUrl] {
                    QDesktopServices::openUrl(QUrl(url));
                });
        buttons->addWidget(runtimeDownload);
        auto *docs = new QPushButton(
            QStringLiteral("%1 documentation").arg(manifest.name));
        docs->setObjectName(QStringLiteral("marketAgentDocumentation"));
        connect(docs, &QPushButton::clicked, this,
                [url = manifest.agent.documentationUrl] {
                    QDesktopServices::openUrl(QUrl(url));
                });
        buttons->addWidget(docs);
        buttons->addStretch();
        const bool operationActive =
            m_marketModelPackageId == manifest.id && m_marketModelProcess &&
            m_marketModelProcess->state() != QProcess::NotRunning;
        auto *agentAction = new QPushButton(
            operationActive
                ? QStringLiteral("Cancel download")
                : active.isEmpty()
                      ? QStringLiteral("Download & activate")
                      : desired == active
                            ? QStringLiteral("Verify / update model")
                            : QStringLiteral("Download & switch"));
        agentAction->setObjectName(QStringLiteral("marketAgentInstall"));
        if (operationActive) {
            connect(agentAction, &QPushButton::clicked, this,
                    &MainWindow::cancelPackageAgentInstall);
        } else {
            connect(agentAction, &QPushButton::clicked, this,
                    [this, id = manifest.id] {
                        startPackageAgentInstall(id);
                    });
        }
        buttons->addWidget(agentAction);
        layout->addLayout(buttons);
    }

    if (!manifest.permissions.isEmpty()) {
        QStringList permissions;
        for (const forkmesh::market::PackagePermission &permission :
             manifest.permissions) {
            permissions << QStringLiteral("%1 (%2)")
                               .arg(permission.label, permission.risk);
        }
        auto *permissionLabel = new QLabel(
            QStringLiteral("Permissions\n• %1")
                .arg(permissions.join(QStringLiteral("\n• "))));
        permissionLabel->setObjectName(QStringLiteral("marketSettingsPermissions"));
        permissionLabel->setWordWrap(true);
        layout->addWidget(permissionLabel);
    }

    const bool installed = packageInstalled;
    const bool desktopManaged =
        manifest.accountSync || manifest.type == QLatin1String("theme") ||
        manifest.entrypoints.contains(QStringLiteral("desktop"));
    const bool worldManaged =
        manifest.entrypoints.contains(QStringLiteral("world"));
    const bool projectOwned = m_packageRegistry->projectOwnsInstalls();
    if (manifest.accountSync) {
        auto *state = new QLabel(
            projectOwned
                ? installed ? QStringLiteral("Installed in this project")
                            : QStringLiteral("Not installed in this project. "
                                             "Your other projects install it "
                                             "on their own.")
            : installed ? QStringLiteral("Installed on all your devices")
                        : QStringLiteral("Not installed. Installing it here "
                                         "installs it on all your devices."));
        state->setObjectName(QStringLiteral("marketPackageAccountState"));
        state->setWordWrap(true);
        layout->addWidget(state);
        if (installed) {
            appendPackagePlaces(
                layout, manifest, m_packageRegistry->places(manifest.id),
                m_packageRegistry->isPlacedHere(manifest.id),
                [this, id = manifest.id](const QStringList &places) {
                    if (!m_packageRegistry->setPlaces(id, places))
                        return;
                    syncPackageRuntime();
                    refreshMarketPage();
                    pushAccountPackageChange(
                        id, QStringLiteral("places"),
                        QJsonObject{{QStringLiteral("places"),
                                     QJsonArray::fromStringList(places)}});
                });
        }
    }
    if (!manifest.agent.isValid()) {
        auto *action = new QPushButton(
            manifest.accountSync
                ? projectOwned
                      ? installed ? QStringLiteral("Remove from this project")
                                  : QStringLiteral("Install in this project")
                  : installed ? QStringLiteral("Remove from all your devices")
                              : QStringLiteral("Install on all your devices")
            : desktopManaged
                ? installed ? QStringLiteral("Remove package")
                            : QStringLiteral("Install package")
                : worldManaged ? QStringLiteral("Managed by ForkMesh World")
                               : QStringLiteral("Catalog only"));
        action->setObjectName(QStringLiteral("marketPackageSettingsAction"));
        action->setEnabled(desktopManaged);
        if (desktopManaged) {
            connect(action, &QPushButton::clicked, this,
                    [this, id = manifest.id, installed] {
                        if (installed)
                            requestPackageRemoval(id);
                        else
                            requestPackageInstall(id);
                    });
        }
        layout->addWidget(action, 0, Qt::AlignRight);
    } else if (installed) {
        auto *remove = new QPushButton(QStringLiteral("Remove package"));
        remove->setObjectName(QStringLiteral("marketPackageSettingsAction"));
        connect(remove, &QPushButton::clicked, this,
                [this, id = manifest.id] { setPackageInstalled(id, false); });
        layout->addWidget(remove, 0, Qt::AlignRight);
    }

    layout->addStretch();
    return page;
}

QPixmap MainWindow::marketPreviewPixmap(const QString &path, const QSize &box)
{
    const QString key = QStringLiteral("%1|%2x%3|%4")
                            .arg(path)
                            .arg(box.width())
                            .arg(box.height())
                            .arg(QFileInfo(path).lastModified().toMSecsSinceEpoch());
    const auto cached = m_marketPreviewCache.constFind(key);
    if (cached != m_marketPreviewCache.constEnd())
        return *cached;
    ++m_marketPreviewDecodes;
    QImageReader reader(path);
    const QSize natural = reader.size();
    if (natural.isValid() && !natural.isEmpty())
        reader.setScaledSize(natural.scaled(box, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (!image.isNull() &&
        (image.width() > box.width() || image.height() > box.height()))
        image = image.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QPixmap pixmap = image.isNull() ? QPixmap() : QPixmap::fromImage(image);
    m_marketPreviewCache.insert(key, pixmap);
    return pixmap;
}

void MainWindow::refreshMarketPage()
{
    if (!m_marketListLayout || !m_marketInstalledListLayout ||
        !m_marketNotInstalledListLayout)
        return;
    clearPackageCards(m_marketListLayout);
    clearPackageCards(m_marketInstalledListLayout);
    clearPackageCards(m_marketNotInstalledListLayout);

    if (!m_packageRegistry || !m_packageRegistry->isReady()) {
        if (m_marketTabs) {
            m_marketTabs->setTabText(m_marketAllTabIndex,
                                     QStringLiteral("All"));
            m_marketTabs->setTabText(m_marketInstalledTabIndex,
                                     QStringLiteral("Installed"));
            m_marketTabs->setTabText(m_marketNotInstalledTabIndex,
                                     QStringLiteral("Not installed"));
        }
        if (m_marketStatus) {
            m_marketStatus->setText(
                QStringLiteral("The bundled package catalog could not be loaded: %1")
                    .arg(m_packageRegistry
                             ? m_packageRegistry->errorString()
                             : QStringLiteral("package registry unavailable")));
            m_marketStatus->show();
        }
        return;
    }
    if (m_marketStatus) {
        const QString warning = m_packageRegistry->warningString().trimmed();
        if (warning.isEmpty()) {
            m_marketStatus->hide();
        } else {
            m_marketStatus->setText(
                QStringLiteral("Some packages were skipped because their "
                               "manifests were invalid:\n%1")
                    .arg(warning));
            m_marketStatus->show();
        }
    }
    refreshPackageInstallCounts();
    refreshPackageMarketScores();
    reportPackageInstallCounts(/*throttled=*/true);
    appendMarketPackCards();
    appendMarketGuideCards();

    const PackageManifest *settingsManifest = nullptr;
    if (!m_marketSettingsPackageId.isEmpty()) {
        settingsManifest = m_packageRegistry->package(m_marketSettingsPackageId);
        if (!settingsManifest)
            m_marketSettingsPackageId.clear();
    }
    const int settingsTabIndex =
        settingsManifest && m_marketTabs ? m_marketTabs->currentIndex() : -1;

    auto appendCard = [this](QVBoxLayout *list,
                             const PackageManifest &manifest) {
        const bool installed = m_packageRegistry->isInstalled(manifest.id);
        const bool projectOwned = m_packageRegistry->projectOwnsInstalls();
        const bool themeManaged =
            manifest.type == QLatin1String("theme") &&
            manifest.entrypoints.contains(QStringLiteral("theme"));
        const bool desktopManaged = themeManaged || manifest.accountSync ||
            manifest.entrypoints.contains(QStringLiteral("desktop"));
        const bool worldManaged =
            manifest.entrypoints.contains(QStringLiteral("world"));
        const bool activeTheme =
            themeManaged && installed &&
            QSettings().value(forkmesh::ui::kThemeSetting).toString() ==
                forkmesh::ui::kMarketplaceThemePrefix + manifest.id;
        const bool modelOperation =
            manifest.id == m_marketModelPackageId && m_marketModelProcess &&
            m_marketModelProcess->state() != QProcess::NotRunning;
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("marketPackageCard"));
        card->setFrameShape(QFrame::StyledPanel);
        card->setProperty("packageId", manifest.id);

        auto *cardLayout = new QHBoxLayout(card);
        cardLayout->setContentsMargins(16, 14, 16, 14);
        cardLayout->setSpacing(16);

        auto *screenshot = new QLabel;
        screenshot->setObjectName(QStringLiteral("marketPackageScreenshot"));
        screenshot->setProperty("packageId", manifest.id);
        screenshot->setFixedSize(240, 135);
        screenshot->setAlignment(Qt::AlignCenter);
        screenshot->setFrameShape(QFrame::StyledPanel);
        const auto &preview = manifest.screenshots.first();
        const QPixmap previewPixmap = marketPreviewPixmap(preview.path,
                                                          screenshot->size());
        if (previewPixmap.isNull()) {
            screenshot->setText(QStringLiteral("Preview unavailable"));
            screenshot->setProperty("screenshotLoaded", false);
        } else {
            screenshot->setPixmap(previewPixmap);
            screenshot->setProperty("screenshotLoaded", true);
        }
        screenshot->setAccessibleName(
            QStringLiteral("Screenshot of %1: %2")
                .arg(manifest.name, preview.alt));
        screenshot->setToolTip(preview.alt);
        cardLayout->addWidget(screenshot, 0, Qt::AlignTop);

        auto *details = new QVBoxLayout;
        details->setSpacing(7);

        auto *top = new QHBoxLayout;
        auto *name = new QLabel(
            QStringLiteral("<b>%1</b> <span style='color:#8b949e'>v%2</span>")
                .arg(manifest.name.toHtmlEscaped(),
                     manifest.version.toHtmlEscaped()));
        name->setTextFormat(Qt::RichText);
        const QString actionText =
            modelOperation
                ? QStringLiteral("Downloading…")
                : desktopManaged
                ? (installed ? QStringLiteral("Remove")
                             : QStringLiteral("Install"))
                // A World package that ships switched off (install.default
                // false) is installed from inside ForkMesh World, not here.
                : worldManaged ? (installed
                                      ? QStringLiteral("Included in World")
                                      : QStringLiteral("Install in World"))
                               : QStringLiteral("Catalog only");
        auto *action = new QPushButton(actionText);
        action->setObjectName(
            QStringLiteral("marketPackageAction-%1").arg(manifest.id));
        action->setProperty("installed", installed);
        action->setAccessibleName(
            QStringLiteral("%1 %2").arg(actionText, manifest.name));
        action->setEnabled(desktopManaged && !modelOperation);
        action->setToolTip(
            manifest.accountSync
                ? projectOwned
                      ? installed
                            ? QStringLiteral(
                                  "Remove the package from this project, "
                                  "with the packages that need it")
                            : QStringLiteral(
                                  "Install the package in this project")
                  : installed
                      ? QStringLiteral(
                            "Remove the package from all your devices, with "
                            "the packages that need it")
                      : QStringLiteral(
                            "Install the package on all your devices")
            : desktopManaged
                ? installed
                      ? QStringLiteral(
                            "Remove the package, its UI contributions and "
                            "package-owned local data")
                      : themeManaged
                            ? QStringLiteral(
                                  "Install and apply this skin across the desktop app")
                            : QStringLiteral("Install and activate this package")
                : worldManaged
                      ? installed
                            ? QStringLiteral(
                                  "This bundled package is activated by ForkMesh World")
                            : QStringLiteral(
                                  "Off by default. Open ForkMesh World → Settings → "
                                  "Marketplace to install it there")
                      : QStringLiteral(
                            "No compatible runtime is available in this client"));
        if (desktopManaged && !modelOperation) {
            connect(action, &QPushButton::clicked, this,
                    [this, id = manifest.id, installed] {
                        if (installed)
                            requestPackageRemoval(id);
                        else
                            requestPackageInstall(id);
                    });
        }
        top->addWidget(name);
        top->addStretch();
        auto *price = new QLabel(manifest.priceLabel());
        price->setObjectName(QStringLiteral("marketPrice"));
        top->addWidget(price);
        auto *settings = new QPushButton(QStringLiteral("Settings"));
        settings->setObjectName(
            QStringLiteral("marketPackageSettings-%1").arg(manifest.id));
        settings->setAccessibleName(
            QStringLiteral("Open %1 settings").arg(manifest.name));
        connect(settings, &QPushButton::clicked, this,
                [this, id = manifest.id] { showPackageSettings(id); });
        top->addWidget(settings);
        if (desktopManaged && !modelOperation && !installed &&
            manifest.localData) {
            auto *importCopy = new QPushButton(
                QStringLiteral("Install from a saved copy…"));
            importCopy->setObjectName(
                QStringLiteral("marketPackageImport-%1").arg(manifest.id));
            importCopy->setAccessibleName(
                QStringLiteral("Install %1 from a saved copy").arg(manifest.name));
            importCopy->setToolTip(QStringLiteral(
                "Choose a copy of your data saved when the package was removed, "
                "then install the package with it"));
            connect(importCopy, &QPushButton::clicked, this,
                    [this, id = manifest.id] { installPackageFromSavedCopy(id); });
            top->addWidget(importCopy);
        }
        top->addWidget(action);
        details->addLayout(top);

        const QString packageStatus =
            manifest.accountSync
                ? projectOwned
                      ? installed ? QStringLiteral("Installed in this project")
                                  : QStringLiteral(
                                        "Not installed in this project")
                  : installed ? QStringLiteral("Installed on all your devices")
                              : QStringLiteral("Not installed")
            : desktopManaged
                ? activeTheme ? QStringLiteral("Installed · Active")
                  : installed ? QStringLiteral("Installed")
                            : QStringLiteral("Not installed")
                : worldManaged ? (installed
                                      ? QStringLiteral("World package")
                                      : QStringLiteral("World package · Not installed"))
                               : QStringLiteral("Catalog only");
        auto *creator = new QLabel(
            QStringLiteral("by @%1  ·  %2  ·  %3  ·  %4  ·  %5")
                .arg(manifest.creatorHandle,
                     manifest.category.isEmpty() ? manifest.type
                                                 : manifest.category,
                     manifest.type, manifest.licenseSpdx, packageStatus));
        creator->setTextFormat(Qt::PlainText);
        creator->setObjectName(QStringLiteral("mutedLabel"));
        details->addWidget(creator);
        if (manifest.accountSync && installed) {
            appendPackagePlaces(
                details, manifest, m_packageRegistry->places(manifest.id),
                m_packageRegistry->isPlacedHere(manifest.id),
                [this, id = manifest.id](const QStringList &places) {
                    if (!m_packageRegistry->setPlaces(id, places))
                        return;
                    syncPackageRuntime();
                    refreshMarketPage();
                    pushAccountPackageChange(
                        id, QStringLiteral("places"),
                        QJsonObject{{QStringLiteral("places"),
                                     QJsonArray::fromStringList(places)}});
                });
        }

        // Popularity, once the relay has reported it. A package with no
        // reported count gets no line at all rather than a "0" the relay never
        // actually observed.
        QStringList statParts;
        const MarketPackageScore score =
            m_packageMarketScores.value(manifest.id);
        if (score.ratingCount > 0) {
            statParts << QString::fromUtf8("\xE2\x98\x85 %1 from %2 %3")
                             .arg(QLocale().toString(score.ratingAverage, 'f', 1),
                                  QLocale().toString(score.ratingCount),
                                  score.ratingCount == 1
                                      ? QStringLiteral("rating")
                                      : QStringLiteral("ratings"));
        }
        const auto counted = m_packageInstallCounts.constFind(manifest.id);
        if (counted != m_packageInstallCounts.constEnd() &&
            (counted->first > 0 || counted->second > 0)) {
            statParts << QStringLiteral("%1 downloads")
                             .arg(QLocale().toString(counted->first))
                      << QStringLiteral("%1 installs")
                             .arg(QLocale().toString(counted->second));
        }
        if (score.people > 0) {
            statParts << QStringLiteral("%1 %2")
                             .arg(QLocale().toString(score.people),
                                  score.people == 1 ? QStringLiteral("person uses it")
                                                    : QStringLiteral("people use it"));
        }
        if (score.opens > 0)
            statParts << QStringLiteral("%1 opens")
                             .arg(QLocale().toString(score.opens));
        if (!statParts.isEmpty()) {
            auto *stats = new QLabel(statParts.join(QStringLiteral("  ·  ")));
            stats->setTextFormat(Qt::PlainText);
            stats->setWordWrap(true);
            stats->setObjectName(QStringLiteral("marketPackageStats"));
            stats->setProperty("packageId", manifest.id);
            stats->setToolTip(QStringLiteral(
                "Star ratings from signed-in ForkMesh accounts, anonymous "
                "download and install totals reported by ForkMesh clients, and "
                "how many signed-in people have opened %1 and how often.")
                                  .arg(manifest.name));
            details->addWidget(stats);
        }

        auto *summary = new QLabel(manifest.summary);
        summary->setTextFormat(Qt::PlainText);
        summary->setWordWrap(true);
        details->addWidget(summary);

        auto *trust = new QLabel(trustLine(manifest));
        trust->setWordWrap(true);
        trust->setObjectName(QStringLiteral("marketTrust"));
        details->addWidget(trust);

        if (!manifest.permissions.isEmpty()) {
            QStringList lines;
            for (const forkmesh::market::PackagePermission &permission :
                 manifest.permissions) {
                const QString marker =
                    permission.risk == QLatin1String("low")
                        ? QString::fromUtf8("\xE2\x9C\x93")
                        : QString::fromUtf8("\xE2\x9A\xA0");
                lines << QStringLiteral("%1 %2")
                             .arg(marker, permission.label.toHtmlEscaped());
            }
            auto *permissions = new QLabel(
                QStringLiteral("<b>Permissions</b><br>%1")
                    .arg(lines.join(QStringLiteral("<br>"))));
            permissions->setTextFormat(Qt::RichText);
            permissions->setWordWrap(true);
            details->addWidget(permissions);
        }
        if (manifest.type == QLatin1String("extension.native")) {
            auto *nativeNotice = new QLabel(QString::fromUtf8(
                "\xE2\x9A\xA0 Native code runs inside the ForkMesh process. "
                "Review the creator, source and permissions before installing."));
            nativeNotice->setWordWrap(true);
            nativeNotice->setObjectName(QStringLiteral("marketNativeNotice"));
            details->addWidget(nativeNotice);
        }

        const QStringList nested = m_packageRegistry->nestedPackages(manifest.id);
        if (!nested.isEmpty()) {
            auto *nestedFrame = new QFrame;
            nestedFrame->setObjectName(QStringLiteral("marketNestedPackages"));
            nestedFrame->setProperty("packageId", manifest.id);
            auto *nestedLayout = new QVBoxLayout(nestedFrame);
            nestedLayout->setContentsMargins(12, 6, 0, 0);
            nestedLayout->setSpacing(4);
            auto *nestedTitle = new QLabel(
                installed
                    ? QStringLiteral("<b>Includes %1 packages</b>, added and "
                                     "removed with %2. Untick one to hide it.")
                          .arg(nested.size())
                          .arg(manifest.name.toHtmlEscaped())
                    : QStringLiteral("<b>Includes %1 packages</b>, added and "
                                     "removed with %2.")
                          .arg(nested.size())
                          .arg(manifest.name.toHtmlEscaped()));
            nestedTitle->setTextFormat(Qt::RichText);
            nestedTitle->setWordWrap(true);
            nestedLayout->addWidget(nestedTitle);
            auto *grid = new QGridLayout;
            grid->setContentsMargins(0, 0, 0, 0);
            grid->setHorizontalSpacing(16);
            grid->setVerticalSpacing(2);
            for (int i = 0; i < nested.size(); ++i) {
                const PackageManifest *child =
                    m_packageRegistry->package(nested.at(i));
                auto *box = new QCheckBox(child->name);
                box->setObjectName(
                    QStringLiteral("marketNestedPackage-%1").arg(child->id));
                box->setProperty("packageId", child->id);
                box->setChecked(installed &&
                                m_packageRegistry->isInstalled(child->id));
                box->setEnabled(installed && desktopManaged && !modelOperation);
                box->setToolTip(child->summary);
                connect(box, &QCheckBox::clicked, this,
                        [this, id = child->id](bool checked) {
                            if (checked)
                                requestPackageInstall(id);
                            else
                                requestPackageRemoval(id);
                        });
                grid->addWidget(box, i / 2, i % 2);
            }
            nestedLayout->addLayout(grid);
            details->addWidget(nestedFrame);
        }

        details->addStretch();
        cardLayout->addLayout(details, 1);
        list->addWidget(card);
    };

    for (const PackageManifest *manifest : sortedMarketPackages()) {
        if (!m_packageRegistry->marketParent(manifest->id).isEmpty())
            continue;
        const bool installed = m_packageRegistry->isInstalled(manifest->id);
        if (settingsTabIndex != m_marketAllTabIndex)
            appendCard(m_marketListLayout, *manifest);
        if (installed && settingsTabIndex != m_marketInstalledTabIndex)
            appendCard(m_marketInstalledListLayout, *manifest);
        if (!installed &&
            settingsTabIndex != m_marketNotInstalledTabIndex)
            appendCard(m_marketNotInstalledListLayout, *manifest);
    }

    if (settingsManifest) {
        QVBoxLayout *settingsLayout = m_marketListLayout;
        if (settingsTabIndex == m_marketInstalledTabIndex)
            settingsLayout = m_marketInstalledListLayout;
        else if (settingsTabIndex == m_marketNotInstalledTabIndex)
            settingsLayout = m_marketNotInstalledListLayout;
        clearPackageCards(settingsLayout);
        settingsLayout->addWidget(buildPackageSettingsPage(*settingsManifest));
        settingsLayout->addStretch();
        applyMarketSearch();
        return;
    }

    m_marketListLayout->addStretch();
    m_marketInstalledListLayout->addStretch();
    m_marketNotInstalledListLayout->addStretch();
    applyMarketSearch();
}

void MainWindow::applyMarketSearch()
{
    if (!m_marketTabs || !m_packageRegistry || !m_packageRegistry->isReady())
        return;
    const QString query =
        m_marketSearch ? m_marketSearch->text().trimmed() : QString();
    const QStringList terms = marketSearchTerms(query);
    const bool hasFilters = !terms.isEmpty() || !m_marketCategoryFilter.isEmpty();
    QSet<QString> matches;
    int total = 0;
    int installedTotal = 0;
    int matchedInstalled = 0;
    for (const PackageManifest &manifest : m_packageRegistry->packages()) {
        if (!m_packageRegistry->marketParent(manifest.id).isEmpty())
            continue;
        const bool installed = m_packageRegistry->isInstalled(manifest.id);
        ++total;
        if (installed)
            ++installedTotal;
        bool matched = marketPackageMatches(manifest, terms) &&
                       marketPackageMatchesCategory(manifest,
                                                    m_marketCategoryFilter);
        for (const QString &nested :
             m_packageRegistry->nestedPackages(manifest.id)) {
            if (matched)
                break;
            const PackageManifest *nestedManifest =
                m_packageRegistry->package(nested);
            matched = nestedManifest &&
                marketPackageMatches(*nestedManifest, terms) &&
                marketPackageMatchesCategory(*nestedManifest,
                                             m_marketCategoryFilter);
        }
        if (!matched)
            continue;
        matches.insert(manifest.id);
        if (installed)
            ++matchedInstalled;
    }
    const auto tabText = [hasFilters](const QString &label, int shown, int all) {
        return !hasFilters
                   ? QStringLiteral("%1 (%2)").arg(label).arg(all)
                   : QStringLiteral("%1 (%2 of %3)").arg(label).arg(shown).arg(all);
    };
    m_marketTabs->setTabText(
        m_marketAllTabIndex,
        tabText(QStringLiteral("All"), matches.size(), total));
    m_marketTabs->setTabText(
        m_marketInstalledTabIndex,
        tabText(QStringLiteral("Installed"), matchedInstalled, installedTotal));
    m_marketTabs->setTabText(
        m_marketNotInstalledTabIndex,
        tabText(QStringLiteral("Not installed"),
                matches.size() - matchedInstalled, total - installedTotal));

    for (QVBoxLayout *list : {m_marketListLayout, m_marketInstalledListLayout,
                              m_marketNotInstalledListLayout}) {
        if (!list)
            continue;
        for (int i = 0; i < list->count(); ++i) {
            QWidget *card = list->itemAt(i)->widget();
            if (card &&
                card->objectName() == QLatin1String("marketPackageCard"))
                setMarketCardShown(
                    card,
                    matches.contains(card->property("packageId").toString()));
        }
    }

    const auto filterCards = [&terms](QVBoxLayout *list,
                                      const QString &objectName) {
        int shown = 0;
        int all = 0;
        for (int i = 0; list && i < list->count(); ++i) {
            QWidget *card = list->itemAt(i)->widget();
            if (!card || card->objectName() != objectName)
                continue;
            ++all;
            const QString text =
                card->property("searchText").toString().toCaseFolded();
            bool matched = true;
            for (const QString &term : terms)
                matched = matched && text.contains(term);
            setMarketCardShown(card, matched);
            shown += matched ? 1 : 0;
        }
        return qMakePair(shown, all);
    };
    const auto packCounts =
        filterCards(m_marketPacksLayout, QStringLiteral("marketPackCard"));
    const auto guideCounts =
        filterCards(m_marketGuidesLayout, QStringLiteral("marketGuideCard"));
    if (m_marketPacksTabIndex >= 0)
        m_marketTabs->setTabText(
            m_marketPacksTabIndex,
            tabText(QStringLiteral("Packs"), packCounts.first, packCounts.second));
    if (m_marketGuidesTabIndex >= 0)
        m_marketTabs->setTabText(
            m_marketGuidesTabIndex,
            tabText(QStringLiteral("Guides"), guideCounts.first,
                    guideCounts.second));

    if (m_marketSearchEmpty) {
        if (hasFilters && matches.isEmpty() && packCounts.first == 0 &&
            guideCounts.first == 0) {
            m_marketSearchEmpty->setText(
                query.isEmpty()
                    ? QStringLiteral("No Market packages match this category.")
                    : QStringLiteral("No Market packages match \u201C%1\u201D.")
                          .arg(query));
            m_marketSearchEmpty->show();
        } else {
            m_marketSearchEmpty->hide();
        }
    }
}

QList<const PackageManifest *> MainWindow::sortedMarketPackages() const
{
    QList<const PackageManifest *> list;
    for (const PackageManifest &manifest : m_packageRegistry->packages())
        list.append(&manifest);
    const QString order =
        m_marketSort ? m_marketSort->currentData().toString() : QString();
    const auto downloads = [this](const PackageManifest *manifest) {
        return m_packageInstallCounts.value(manifest->id).first;
    };
    const auto installs = [this](const PackageManifest *manifest) {
        return m_packageInstallCounts.value(manifest->id).second;
    };
    const auto score = [this](const PackageManifest *manifest) {
        return m_packageMarketScores.value(manifest->id);
    };
    if (order == QLatin1String("name")) {
        std::stable_sort(list.begin(), list.end(),
                         [](const PackageManifest *a, const PackageManifest *b) {
                             return QString::localeAwareCompare(a->name, b->name) < 0;
                         });
    } else if (order == QLatin1String("rating")) {
        std::stable_sort(list.begin(), list.end(),
                         [&score](const PackageManifest *a, const PackageManifest *b) {
                             const MarketPackageScore left = score(a);
                             const MarketPackageScore right = score(b);
                             if (left.ratingAverage != right.ratingAverage)
                                 return left.ratingAverage > right.ratingAverage;
                             return left.ratingCount > right.ratingCount;
                         });
    } else if (order == QLatin1String("installs")) {
        std::stable_sort(list.begin(), list.end(),
                         [&installs](const PackageManifest *a, const PackageManifest *b) {
                             return installs(a) > installs(b);
                         });
    } else if (order == QLatin1String("downloads")) {
        std::stable_sort(list.begin(), list.end(),
                         [&downloads](const PackageManifest *a, const PackageManifest *b) {
                             return downloads(a) > downloads(b);
                         });
    } else if (order == QLatin1String("used")) {
        std::stable_sort(list.begin(), list.end(),
                         [&score](const PackageManifest *a, const PackageManifest *b) {
                             const MarketPackageScore left = score(a);
                             const MarketPackageScore right = score(b);
                             if (left.people != right.people)
                                 return left.people > right.people;
                             return left.opens > right.opens;
                         });
    }
    return list;
}

void MainWindow::updateMarketInstallAllButton()
{
    if (!m_marketInstallAllButton)
        return;
    int remaining = 0;
    if (m_packageRegistry && m_packageRegistry->isReady()) {
        for (const QString &id : m_packageRegistry->installAllPackages())
            remaining += m_packageRegistry->isInstalled(id) ? 0 : 1;
    }
    m_marketInstallAllButton->setText(remaining > 0
                                          ? QStringLiteral("Install all (%1)").arg(remaining)
                                          : QStringLiteral("All installed"));
    m_marketInstallAllButton->setEnabled(remaining > 0);
    m_marketInstallAllButton->setToolTip(
        QStringLiteral("Install every package this device can run, except "
                       "agents and themes (local AI models are large "
                       "downloads; install those on their own)"));
}

void MainWindow::appendMarketPackCards()
{
    updateMarketInstallAllButton();
    if (!m_marketPacksLayout)
        return;
    clearPackageCards(m_marketPacksLayout);
    for (const forkmesh::market::PackagePack &pack : m_packageRegistry->packs()) {
        const QStringList ids = m_packageRegistry->packPackages(pack.id);
        QStringList names;
        int installedCount = 0;
        for (const QString &id : ids) {
            names << m_packageRegistry->package(id)->name;
            installedCount += m_packageRegistry->isInstalled(id) ? 1 : 0;
        }
        const int remaining = ids.size() - installedCount;

        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("marketPackCard"));
        card->setFrameShape(QFrame::StyledPanel);
        card->setProperty("packId", pack.id);
        card->setProperty("searchText",
                          QStringList{pack.name, pack.tagline, pack.summary,
                                      names.join(QLatin1Char(' '))}
                              .join(QLatin1Char('\n')));
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(7);

        auto *top = new QHBoxLayout;
        auto *name = new QLabel(QStringLiteral("<b>%1</b>").arg(pack.name.toHtmlEscaped()));
        name->setTextFormat(Qt::RichText);
        top->addWidget(name);
        top->addStretch();
        auto *progress = new QLabel(QStringLiteral("%1 of %2 installed")
                                        .arg(installedCount)
                                        .arg(ids.size()));
        progress->setObjectName(QStringLiteral("mutedLabel"));
        progress->setProperty("packId", pack.id);
        top->addWidget(progress);
        const QString actionText =
            remaining == 0       ? QStringLiteral("All installed")
            : installedCount == 0 ? QStringLiteral("Install pack")
                                  : QStringLiteral("Install the other %1").arg(remaining);
        auto *install = new QPushButton(actionText);
        install->setObjectName(QStringLiteral("marketPackInstall-%1").arg(pack.id));
        install->setAccessibleName(QStringLiteral("%1 %2").arg(actionText, pack.name));
        install->setEnabled(remaining > 0);
        install->setToolTip(QStringLiteral("Install every package in %1 in one go")
                                .arg(pack.name));
        connect(install, &QPushButton::clicked, this,
                [this, label = pack.name, ids] { installPackageSet(label, ids); });
        top->addWidget(install);
        layout->addLayout(top);

        auto *tagline = new QLabel(pack.tagline);
        tagline->setTextFormat(Qt::PlainText);
        tagline->setWordWrap(true);
        layout->addWidget(tagline);
        auto *summary = new QLabel(pack.summary);
        summary->setTextFormat(Qt::PlainText);
        summary->setWordWrap(true);
        summary->setObjectName(QStringLiteral("mutedLabel"));
        layout->addWidget(summary);
        auto *includes = new QLabel(
            (pack.everything
                 ? QStringLiteral("Includes all %1 packages: %2")
                 : QStringLiteral("Includes %1 packages: %2"))
                .arg(ids.size())
                .arg(names.join(QStringLiteral(", "))));
        includes->setObjectName(QStringLiteral("marketPackIncludes"));
        includes->setTextFormat(Qt::PlainText);
        includes->setWordWrap(true);
        layout->addWidget(includes);
        m_marketPacksLayout->addWidget(card);
    }
    m_marketPacksLayout->addStretch();
}

void MainWindow::appendMarketGuideCards()
{
    if (!m_marketGuidesLayout)
        return;
    clearPackageCards(m_marketGuidesLayout);
    for (const forkmesh::market::PackageGuide &guide :
         m_packageRegistry->guides()) {
        const QStringList ids = m_packageRegistry->guidePackages(guide.id);
        QStringList names;
        int installedCount = 0;
        for (const QString &id : ids) {
            names << m_packageRegistry->package(id)->name;
            installedCount += m_packageRegistry->isInstalled(id) ? 1 : 0;
        }
        const int remaining = ids.size() - installedCount;
        const forkmesh::market::PackagePack *pack =
            m_packageRegistry->pack(guide.pack);

        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("marketGuideCard"));
        card->setFrameShape(QFrame::StyledPanel);
        card->setProperty("guideId", guide.id);
        card->setProperty("searchText",
                          QStringList{guide.title, guide.from, guide.summary,
                                      guide.steps.join(QLatin1Char(' ')),
                                      names.join(QLatin1Char(' '))}
                              .join(QLatin1Char('\n')));
        auto *layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 14, 16, 14);
        layout->setSpacing(7);

        auto *top = new QHBoxLayout;
        auto *title = new QLabel(
            QStringLiteral("<b>%1</b> <span style='color:#8b949e'>from %2</span>")
                .arg(guide.title.toHtmlEscaped(), guide.from.toHtmlEscaped()));
        title->setTextFormat(Qt::RichText);
        title->setWordWrap(true);
        top->addWidget(title, 1);
        const QString actionText = remaining == 0
                                       ? QStringLiteral("Ready")
                                       : QStringLiteral("Install what you need");
        auto *install = new QPushButton(actionText);
        install->setObjectName(QStringLiteral("marketGuideInstall-%1").arg(guide.id));
        install->setAccessibleName(QStringLiteral("%1 for %2").arg(actionText, guide.title));
        install->setEnabled(remaining > 0);
        install->setToolTip(QStringLiteral("Install %1").arg(names.join(QStringLiteral(", "))));
        connect(install, &QPushButton::clicked, this,
                [this, label = pack ? pack->name : guide.title, ids] {
                    installPackageSet(label, ids);
                });
        top->addWidget(install, 0, Qt::AlignTop);
        layout->addLayout(top);

        auto *summary = new QLabel(guide.summary);
        summary->setTextFormat(Qt::PlainText);
        summary->setWordWrap(true);
        layout->addWidget(summary);

        QStringList steps;
        for (const QString &step : guide.steps)
            steps << QStringLiteral("<li>%1</li>").arg(step.toHtmlEscaped());
        auto *stepList = new QLabel(QStringLiteral("<ol style='margin-left:0'>%1</ol>")
                                        .arg(steps.join(QString())));
        stepList->setObjectName(QStringLiteral("marketGuideSteps"));
        stepList->setTextFormat(Qt::RichText);
        stepList->setWordWrap(true);
        stepList->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(stepList);

        auto *uses = new QLabel(
            pack ? QStringLiteral("Uses the %1 pack: %2 (%3 of %4 installed)")
                       .arg(pack->name, names.join(QStringLiteral(", ")))
                       .arg(installedCount)
                       .arg(ids.size())
                 : QStringLiteral("Uses %1 (%2 of %3 installed)")
                       .arg(names.join(QStringLiteral(", ")))
                       .arg(installedCount)
                       .arg(ids.size()));
        uses->setObjectName(QStringLiteral("mutedLabel"));
        uses->setTextFormat(Qt::PlainText);
        uses->setWordWrap(true);
        layout->addWidget(uses);

        if (!guide.links.isEmpty()) {
            QStringList links;
            for (const forkmesh::market::PackageGuideLink &link : guide.links)
                links << QStringLiteral("<a href=\"%1\">%2</a>")
                             .arg(link.url.toHtmlEscaped(), link.label.toHtmlEscaped());
            auto *linkLabel = new QLabel(links.join(QStringLiteral("  ·  ")));
            linkLabel->setObjectName(QStringLiteral("marketGuideLinks"));
            linkLabel->setTextFormat(Qt::RichText);
            linkLabel->setWordWrap(true);
            linkLabel->setOpenExternalLinks(true);
            layout->addWidget(linkLabel);
        }
        m_marketGuidesLayout->addWidget(card);
    }
    m_marketGuidesLayout->addStretch();
}

void MainWindow::installPackageSet(const QString &label, const QStringList &ids)
{
    if (!m_packageRegistry || !m_packageRegistry->isReady())
        return;
    QStringList pending;
    for (const QString &id : ids) {
        if (!m_packageRegistry->isInstalled(id))
            pending << id;
    }
    if (pending.isEmpty()) {
        flashMessage(QStringLiteral("Everything in %1 is already installed.").arg(label));
        return;
    }
    m_marketBatchInstall = true;
    int added = 0;
    QStringList failed;
    for (const QString &id : std::as_const(pending)) {
        if (m_packageRegistry->isInstalled(id))
            continue;
        if (setPackageInstalled(id, true, false))
            ++added;
        else
            failed << id;
    }
    m_marketBatchInstall = false;
    syncPackageRuntime();
    reportPackageInstallCounts();
    refreshMarketPage();
    if (failed.isEmpty()) {
        flashMessage(QStringLiteral("%1: installed %2 %3.")
                         .arg(label)
                         .arg(added)
                         .arg(added == 1 ? QStringLiteral("package")
                                         : QStringLiteral("packages")));
    } else {
        flashMessage(QStringLiteral("%1: installed %2, but %3 could not be installed.")
                         .arg(label)
                         .arg(added)
                         .arg(packageNameList(*m_packageRegistry, failed)),
                     true);
    }
}

void MainWindow::startPackageAgentInstall(const QString &id)
{
    if (!m_packageRegistry || !m_packageRegistry->isReady())
        return;
    const PackageManifest *manifest = m_packageRegistry->package(id);
    if (!manifest || !manifest->agent.isValid()) {
        flashMessage(QStringLiteral("This package has no supported local model."),
                     true);
        return;
    }
    if (m_marketModelProcess &&
        m_marketModelProcess->state() != QProcess::NotRunning) {
        flashMessage(QStringLiteral("Another model operation is already running."),
                     true);
        return;
    }

    QString model =
        m_packageRegistry
            ->packageSetting(id, QStringLiteral("desiredModel"),
                             manifest->agent.defaultModel)
            .toString();
    if (!manifest->agent.model(model))
        model = manifest->agent.defaultModel;
    m_packageRegistry->setPackageSetting(id, QStringLiteral("desiredModel"),
                                         model);
    QString endpoint =
        m_packageRegistry
            ->packageSetting(id, QStringLiteral("endpoint"),
                             manifest->agent.endpoint)
            .toString()
            .trimmed();
    if (!allowedOllamaEndpoint(endpoint))
        endpoint = manifest->agent.endpoint;
    m_packageRegistry->setPackageSetting(id, QStringLiteral("endpoint"),
                                         endpoint);

    const QString executable =
        QStandardPaths::findExecutable(QStringLiteral("ollama"));
    if (executable.isEmpty()) {
        m_marketModelPackageId = id;
        m_marketModelName = model;
        m_marketModelStage.clear();
        m_marketModelOutput = QStringLiteral(
            "Ollama is required. Install it, start the Ollama service, then "
            "return here and download the model.");
        m_marketSettingsPackageId = id;
        refreshMarketPage();
        flashMessage(QStringLiteral(
            "Ollama was not found. Open %1 settings for the install link.")
                         .arg(manifest->name),
                     true);
        return;
    }

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    m_marketModelProcess = process;
    m_marketModelPackageId = id;
    m_marketModelName = model;
    m_marketModelStage = QStringLiteral("check");
    m_marketModelOutput.clear();
    m_marketModelCancelled = false;
    m_marketSettingsPackageId = id;

    connect(process, &QProcess::readyRead, this, [this, process] {
        if (process != m_marketModelProcess)
            return;
        m_marketModelOutput += QString::fromUtf8(process->readAll());
        constexpr int kMaxProgressChars = 16 * 1024;
        if (m_marketModelOutput.size() > kMaxProgressChars)
            m_marketModelOutput = m_marketModelOutput.right(kMaxProgressChars);
        if (m_marketModelStatus) {
            const QString lead =
                m_marketModelStage == QLatin1String("check")
                    ? QStringLiteral("Checking the local model")
                    : QStringLiteral("Downloading the model");
            const QString detail = processProgressText(m_marketModelOutput);
            m_marketModelStatus->setText(
                detail.isEmpty() ? lead + QStringLiteral("…")
                                 : QStringLiteral("%1… %2").arg(lead, detail));
        }
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError processError) {
                if (process != m_marketModelProcess ||
                    processError != QProcess::FailedToStart) {
                    return;
                }
                const QString message = QStringLiteral(
                    "Ollama could not be started. Check the installation and PATH.");
                m_marketModelOutput = message;
                m_marketModelStage.clear();
                process->deleteLater();
                m_marketModelProcess.clear();
                refreshMarketPage();
                flashMessage(message, true);
            });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this, process, executable](int exitCode,
                                        QProcess::ExitStatus exitStatus) {
                if (process != m_marketModelProcess)
                    return;
                m_marketModelOutput += QString::fromUtf8(process->readAll());
                if (m_marketModelCancelled) {
                    m_marketModelOutput = QStringLiteral("Model download cancelled.");
                    m_marketModelStage.clear();
                    process->deleteLater();
                    m_marketModelProcess.clear();
                    refreshMarketPage();
                    return;
                }
                const bool success =
                    exitStatus == QProcess::NormalExit && exitCode == 0;
                if (m_marketModelStage == QLatin1String("check")) {
                    if (success) {
                        completePackageAgentInstall(false);
                        return;
                    }
                    m_marketModelStage = QStringLiteral("pull");
                    m_marketModelOutput.clear();
                    if (m_marketModelStatus)
                        m_marketModelStatus->setText(
                            QStringLiteral("Downloading %1…")
                                .arg(m_marketModelName));
                    process->start(executable,
                                   {QStringLiteral("pull"),
                                    m_marketModelName});
                    return;
                }
                if (success) {
                    completePackageAgentInstall(true);
                    return;
                }
                QString message;
                if (ollamaUpdateRequired(m_marketModelOutput)) {
                    message = QStringLiteral(
                        "This model requires a newer version of Ollama. "
                        "Update Ollama, restart its service, then try again.");
                } else {
                    QString detail = processProgressText(m_marketModelOutput);
                    if (detail.isEmpty())
                        detail = QStringLiteral("Ollama returned exit code %1.")
                                     .arg(exitCode);
                    message = QStringLiteral("The model could not be downloaded: %1")
                                  .arg(detail);
                }
                m_marketModelOutput = message;
                m_marketModelStage.clear();
                process->deleteLater();
                m_marketModelProcess.clear();
                refreshMarketPage();
                flashMessage(message, true);
            });

    process->start(executable, {QStringLiteral("show"), model});
    refreshMarketPage();
}

void MainWindow::cancelPackageAgentInstall()
{
    if (!m_marketModelProcess ||
        m_marketModelProcess->state() == QProcess::NotRunning) {
        return;
    }
    m_marketModelCancelled = true;
    if (m_marketModelStatus)
        m_marketModelStatus->setText(QStringLiteral("Cancelling model download…"));
    QPointer<QProcess> process = m_marketModelProcess;
    process->terminate();
    QTimer::singleShot(1500, process, [process] {
        if (process && process->state() != QProcess::NotRunning)
            process->kill();
    });
}

void MainWindow::completePackageAgentInstall(bool downloaded)
{
    const QString id = m_marketModelPackageId;
    const QString model = m_marketModelName;
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!manifest || !manifest->agent.model(model)) {
        m_marketModelOutput = QStringLiteral(
            "The package changed while the model was being prepared.");
        m_marketModelStage.clear();
        if (m_marketModelProcess)
            m_marketModelProcess->deleteLater();
        m_marketModelProcess.clear();
        refreshMarketPage();
        return;
    }

    if (downloaded) {
        QStringList managed =
            m_packageRegistry
                ->packageSetting(id, QStringLiteral("managedModels"))
                .toStringList();
        if (!managed.contains(model))
            managed.append(model);
        m_packageRegistry->setPackageSetting(
            id, QStringLiteral("managedModels"), managed);
    }
    m_packageRegistry->setPackageSetting(id, QStringLiteral("activeModel"),
                                         model);
    QString error;
    const bool installed = m_packageRegistry->install(id, &error);
    const QString packageName = manifest->name;
    if (m_marketModelProcess)
        m_marketModelProcess->deleteLater();
    m_marketModelProcess.clear();
    m_marketModelStage.clear();
    m_marketModelCancelled = false;
    if (!installed) {
        m_marketModelOutput = error.isEmpty()
                                  ? QStringLiteral("Package activation failed.")
                                  : error;
        refreshMarketPage();
        flashMessage(m_marketModelOutput, true);
        return;
    }
    m_marketModelOutput = QStringLiteral("%1 is installed and active.").arg(model);
    syncPackageRuntime();
    refreshMarketPage();
    flashMessage(QStringLiteral("%1 installed with %2.")
                     .arg(packageName, model));
}

bool MainWindow::removePackageAgentModels(const QString &id,
                                          const QStringList &models)
{
    if (models.isEmpty())
        return true;
    if (!m_packageRegistry)
        return false;
    const PackageManifest *manifest = m_packageRegistry->package(id);
    const QString executable =
        QStandardPaths::findExecutable(QStringLiteral("ollama"));
    if (!manifest || !manifest->agent.isValid() || executable.isEmpty())
        return false;
    bool started = true;
    for (const QString &model : models) {
        if (!manifest->agent.model(model))
            continue;
        started = forkmesh::processreaper::startReleased(
                      executable, {QStringLiteral("rm"), model}) &&
                  started;
    }
    return started;
}

bool MainWindow::setPackageInstalled(const QString &id, bool installed,
                                     bool announce)
{
    if (!m_packageRegistry || !m_packageRegistry->isReady())
        return false;
    const PackageManifest *package = m_packageRegistry->package(id);
    const bool themePackage =
        package && package->type == QLatin1String("theme") &&
        package->entrypoints.contains(QStringLiteral("theme"));
    const bool agentPackage = package && package->agent.isValid();
    if (!package || (!themePackage && !package->accountSync &&
                     !package->entrypoints.contains(QStringLiteral("desktop")))) {
        if (announce) {
            flashMessage(QStringLiteral(
                "This package is managed by its compatible runtime."), true);
        }
        return false;
    }
    if (agentPackage && installed && !m_packageRegistry->isInstalled(id)) {
        startPackageAgentInstall(id);
        return (m_marketModelProcess &&
                m_marketModelProcess->state() != QProcess::NotRunning) ||
               m_packageRegistry->isInstalled(id);
    }
    const bool alreadyThere =
        installed ? m_packageRegistry->isInstalled(id) &&
                        m_packageRegistry->missingDependencies(id).isEmpty()
                  : !m_packageRegistry->isInstalled(id);
    if (alreadyThere) {
        if (themePackage && installed) {
            QSettings().setValue(forkmesh::ui::kThemeSetting,
                                 forkmesh::ui::kMarketplaceThemePrefix +
                                     package->id);
            applyTheme(m_packageRegistry.get());
        }
        if (!m_marketBatchInstall) {
            syncPackageRuntime();
            refreshMarketPage();
        }
        return true;
    }

    QStringList managedModels;
    if (agentPackage && !installed) {
        managedModels =
            m_packageRegistry
                ->packageSetting(id, QStringLiteral("managedModels"))
                .toStringList();
        if (m_marketModelPackageId == id)
            cancelPackageAgentInstall();
    }
    QString error;
    QStringList changedIds;
    const bool changed =
        installed ? m_packageRegistry->install(id, &error, &changedIds)
                  : m_packageRegistry->uninstall(id, &error, &changedIds);
    if (!changed) {
        if (announce)
            flashMessage(error, true);
        return false;
    }
    const bool projectOwned = m_packageRegistry->projectOwnsInstalls();
    if (package->accountSync && (installed || !projectOwned))
        pushAccountPackageChange(
            id, installed ? QStringLiteral("install")
                          : QStringLiteral("uninstall"));
    const bool modelCleanupStarted =
        !agentPackage || installed ||
        removePackageAgentModels(id, managedModels);

    if (!installed) {
        QStringList kept;
        const QStringList removedIds = changedIds.isEmpty() ? QStringList{id} : changedIds;
        for (const QString &removedId : removedIds) {
            const QString folder = forkmesh::paths::pluginDataDir(removedId);
            const QDir dir(folder);
            if (!dir.exists() || dir.isEmpty())
                continue;
            const PackageManifest *removed = m_packageRegistry->package(removedId);
            const QString name = removed && !removed->name.isEmpty() ? removed->name : removedId;
            kept << QStringLiteral("%1: %2").arg(name, QDir::toNativeSeparators(folder));
        }
        if (!kept.isEmpty()) {
            QMessageBox::information(
                this, QStringLiteral("Plugin data kept"),
                QStringLiteral("The plugin is removed, but its data stays on disk:\n\n%1\n\n"
                               "Install the plugin again to use it, or delete the folder "
                               "yourself if you no longer need it.")
                    .arg(kept.join(QLatin1Char('\n'))));
        }
    }

    if (themePackage) {
        QSettings settings;
        const QString preference =
            forkmesh::ui::kMarketplaceThemePrefix + package->id;
        if (installed) {
            settings.setValue(forkmesh::ui::kThemeSetting, preference);
        } else if (settings.value(forkmesh::ui::kThemeSetting).toString() ==
                   preference) {
            settings.setValue(forkmesh::ui::kThemeSetting,
                              QStringLiteral("system"));
        }
        applyTheme(m_packageRegistry.get());
    } else {
        refreshAppThemeChoice();
    }
    if (!m_marketBatchInstall) {
        syncPackageRuntime();
        reportPackageInstallCounts();
        refreshMarketPage();
    }
    if (announce) {
        const QString name = package->name;
        if (!error.isEmpty()) {
            flashMessage(error, true);
        } else if (!modelCleanupStarted) {
            flashMessage(
                QStringLiteral(
                    "%1 was removed, but Ollama was unavailable. Its model "
                    "weights remain in Ollama's store and can be removed there.")
                    .arg(name),
                true);
        } else if (package->accountSync) {
            changedIds.removeAll(id);
            const QString others =
                changedIds.isEmpty()
                    ? QString()
                    : installed
                          ? QStringLiteral(" %1 came with it.")
                                .arg(packageNameList(*m_packageRegistry,
                                                     changedIds))
                          : QStringLiteral(" %1 went with it.")
                                .arg(packageNameList(*m_packageRegistry,
                                                     changedIds));
            flashMessage(
                projectOwned
                    ? installed
                          ? QStringLiteral("%1 installed in this project.%2")
                                .arg(name, others)
                          : QStringLiteral("%1 removed from this project.%2")
                                .arg(name, others)
                : installed
                    ? QStringLiteral("%1 installed on all your devices.%2")
                          .arg(name, others)
                    : QStringLiteral("%1 removed from all your devices.%2")
                          .arg(name, others));
        } else {
            flashMessage(installed
                             ? QStringLiteral("%1 installed and active.").arg(name)
                             : QStringLiteral("%1 removed cleanly. Its Library "
                                              "entitlement was preserved.")
                                   .arg(name));
        }
    }
    return true;
}

QString MainWindow::packageDataBackupPath(const QString &id) const
{
    return forkmesh::paths::dataRoot() +
           QStringLiteral("/package-data/forkmesh-%1-data.json").arg(id);
}

void MainWindow::savePackageDataCopy(
    const QString &id,
    std::function<void(bool proceed, const QString &path)> done)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!manifest) {
        done(false, QString());
        return;
    }
    const QString name = manifest->name;
    const QString scope = manifest->dataScope;
    auto write = [this, id, name, done](const QJsonObject &server,
                                        const QString &serverError) {
        if (!serverError.isEmpty()) {
            flashMessage(QStringLiteral("Couldn't fetch a copy of your %1 "
                                        "records: %2")
                             .arg(name, serverError),
                         true);
            done(false, QString());
            return;
        }
        if (!m_packageRegistry || !m_packageRegistry->package(id)) {
            done(false, QString());
            return;
        }
        QJsonObject bundle = m_packageRegistry->exportPackageData(id);
        const bool local =
            !bundle.value(QStringLiteral("data")).toObject().isEmpty();
        if (!server.isEmpty())
            bundle.insert(QStringLiteral("server"), server);
        if (!local && server.isEmpty()) {
            done(true, QString());
            return;
        }
        QString path = m_packageDataTestFilePath;
        if (path.isEmpty()) {
            const QString suggested = packageDataBackupPath(id);
            QDir().mkpath(QFileInfo(suggested).absolutePath());
            path = QFileDialog::getSaveFileName(
                this, QStringLiteral("Save a copy of your %1 data").arg(name),
                suggested, QStringLiteral("JSON files (*.json)"));
            if (path.isEmpty()) {
                done(false, QString());
                return;
            }
        }
        QSaveFile file(path);
        const QByteArray bytes =
            QJsonDocument(bundle).toJson(QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(bytes) != bytes.size() || !file.commit()) {
            flashMessage(QStringLiteral("The %1 data file could not be "
                                        "written to %2.")
                             .arg(name, QDir::toNativeSeparators(path)),
                         true);
            done(false, QString());
            return;
        }
        done(true, path);
    };

    // Server-side records only come along when the package's page is live on
    // the rail: it knows the organization or account they belong to.
    forkmesh::market::NativePackageWidget *page = nullptr;
    for (const NativePackagePage &candidate :
         std::as_const(m_nativePackagePages)) {
        if (candidate.id == id)
            page = candidate.widget;
    }
    if (page && scope != QLatin1String("none")) {
        const bool started = page->exportData(
            [write, scope](bool ok, const QJsonObject &payload,
                           const QString &error) {
                if (!ok) {
                    write({}, error.isEmpty() ? QStringLiteral("the export "
                                                               "failed")
                                              : error);
                    return;
                }
                if (!payload.value(QStringLiteral("hasData")).toBool()) {
                    write({}, QString());
                    return;
                }
                write(QJsonObject{
                          {QStringLiteral("scope"), scope},
                          {QStringLiteral("filename"),
                           payload.value(QStringLiteral("filename"))},
                          {QStringLiteral("export"),
                           payload.value(QStringLiteral("export"))},
                      },
                      QString());
            });
        if (started) {
            flashMessage(
                QStringLiteral("Preparing a copy of your %1 data…").arg(name));
            return;
        }
    }
    write({}, QString());
}

bool MainWindow::importPackageDataFile(const QString &id, const QString &path,
                                       QString *error)
{
    constexpr qint64 kMaxPackageDataFile = 16 * 1024 * 1024;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("%1 could not be opened.")
                     .arg(QDir::toNativeSeparators(path));
        return false;
    }
    if (file.size() > kMaxPackageDataFile) {
        *error = QStringLiteral("%1 is too large to be a package data export.")
                     .arg(QDir::toNativeSeparators(path));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        *error = QStringLiteral(
            "This file is not a ForkMesh package data export.");
        return false;
    }
    return m_packageRegistry &&
           m_packageRegistry->importPackageData(id, document.object(), error);
}

void MainWindow::requestPackageRemoval(const QString &id, bool fromRail)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!manifest || !m_packageRegistry->isInstalled(id))
        return;
    if (manifest->accountSync &&
        !(manifest->localData && m_packageRegistry->hasPackageData(id))) {
        const QStringList dependents =
            m_packageRegistry->installedDependents(id);
        const bool projectOwned = m_packageRegistry->projectOwnsInstalls();
        const bool accountData =
            !projectOwned && manifest->dataScope == QLatin1String("account");
        if (dependents.isEmpty() && !accountData && !fromRail) {
            setPackageInstalled(id, false);
            return;
        }
        if (auto *existing = findChild<QMessageBox *>(
                QStringLiteral("packageRemoveConfirm"));
            existing && existing->isVisible()) {
            existing->raise();
            existing->activateWindow();
            return;
        }
        QStringList details;
        if (!dependents.isEmpty()) {
            details << QStringLiteral("These packages need %1 and are removed "
                                      "with it: %2.")
                           .arg(manifest->name,
                                packageNameList(*m_packageRegistry,
                                                dependents));
        }
        if (accountData) {
            details << QStringLiteral("Its settings and the records it keeps "
                                      "on your ForkMesh account are deleted.");
        }
        details << (projectOwned
                        ? QStringLiteral("Your other projects keep it. You "
                                         "can install it again from the "
                                         "Market.")
                        : QStringLiteral("It leaves every device on your "
                                         "account. You can install it again "
                                         "from the Market."));
        auto *box = new QMessageBox(this);
        box->setObjectName(QStringLiteral("packageRemoveConfirm"));
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setModal(false);
        box->setIcon(QMessageBox::Question);
        box->setWindowTitle(QStringLiteral("Remove package"));
        box->setText((projectOwned
                          ? QStringLiteral("Remove %1 from this project?")
                          : QStringLiteral("Remove %1 from all your devices?"))
                         .arg(manifest->name));
        box->setInformativeText(details.join(QStringLiteral("\n\n")));
        QPushButton *remove = box->addButton(QStringLiteral("Remove"),
                                             QMessageBox::DestructiveRole);
        remove->setObjectName(QStringLiteral("packageRemoveConfirmButton"));
        QPushButton *cancel = box->addButton(QMessageBox::Cancel);
        box->setDefaultButton(cancel);
        box->setEscapeButton(cancel);
        forkmesh::ui::onMessageBoxClicked(box, this,
                [this, id, remove](QAbstractButton *clicked) {
                    if (clicked == remove)
                        setPackageInstalled(id, false);
                });
        box->show();
        return;
    }
    const bool localAtRisk =
        manifest->localData && m_packageRegistry->hasPackageData(id);
    bool serverCopy = false;
    if (manifest->dataScope != QLatin1String("none")) {
        for (const NativePackagePage &page :
             std::as_const(m_nativePackagePages))
            serverCopy = serverCopy || (page.id == id && page.widget);
    }
    // Nothing on this device would be lost: the Market's Remove button just
    // removes, as it always has.
    if (!localAtRisk && !fromRail) {
        setPackageInstalled(id, false);
        return;
    }
    if (auto *existing =
            findChild<QMessageBox *>(QStringLiteral("packageRemoveConfirm"));
        existing && existing->isVisible()) {
        existing->raise();
        existing->activateWindow();
        return;
    }

    const QString name = manifest->name;
    QStringList details;
    if (localAtRisk) {
        details << QStringLiteral(
                       "Its data on this device is deleted with it. Save a "
                       "copy first and you can import it when you install "
                       "%1 again.")
                       .arg(name);
    }
    if (manifest->dataScope == QLatin1String("organization") ||
        manifest->dataScope == QLatin1String("account")) {
        details << QStringLiteral(
                       "The records %1 keeps on ForkMesh for your %2 stay "
                       "there: removing it from this desktop does not delete "
                       "them.%3")
                       .arg(name,
                            manifest->dataScope ==
                                    QLatin1String("organization")
                                ? QStringLiteral("organization")
                                : QStringLiteral("account"),
                            serverCopy && !localAtRisk
                                ? QStringLiteral(" You can still save a copy.")
                                : QString());
    }
    if (details.isEmpty()) {
        details << QStringLiteral("Its pages and buttons leave the app. You "
                                  "can install it again from the Market.");
    }

    auto *box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("packageRemoveConfirm"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false); // never park the window behind the question
    box->setIcon(localAtRisk ? QMessageBox::Warning : QMessageBox::Question);
    box->setWindowTitle(QStringLiteral("Remove package"));
    box->setText(QStringLiteral("Remove %1 from this desktop?").arg(name));
    box->setInformativeText(details.join(QStringLiteral("\n\n")));
    QPushButton *save = nullptr;
    if (localAtRisk || serverCopy) {
        save = box->addButton(QStringLiteral("Save a copy and remove"),
                              QMessageBox::AcceptRole);
        save->setObjectName(QStringLiteral("packageRemoveSaveButton"));
    }
    QPushButton *remove = box->addButton(
        localAtRisk ? QStringLiteral("Remove without saving")
                    : QStringLiteral("Remove"),
        QMessageBox::DestructiveRole);
    remove->setObjectName(QStringLiteral("packageRemoveConfirmButton"));
    QPushButton *cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(save ? save : cancel);
    box->setEscapeButton(cancel);
    forkmesh::ui::onMessageBoxClicked(box, this,
            [this, id, name, save, remove](QAbstractButton *clicked) {
                if (clicked == remove) {
                    setPackageInstalled(id, false);
                    return;
                }
                if (!save || clicked != save)
                    return;
                savePackageDataCopy(
                    id, [this, id, name](bool proceed, const QString &path) {
                        if (!proceed)
                            return; // cancelled or reported: keep it installed
                        if (setPackageInstalled(id, false) && !path.isEmpty()) {
                            flashMessage(
                                QStringLiteral("%1 removed. A copy of its data "
                                               "was saved to %2.")
                                    .arg(name,
                                         QDir::toNativeSeparators(path)));
                        }
                    });
            });
    box->show();
}

void MainWindow::requestPackageInstall(const QString &id)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!manifest || !manifest->localData ||
        m_packageRegistry->isInstalled(id) ||
        !m_packageRegistry->missingDependencies(id).isEmpty()) {
        setPackageInstalled(id, true);
        return;
    }
    const QString saved = m_packageDataTestFilePath.isEmpty()
                              ? packageDataBackupPath(id)
                              : m_packageDataTestFilePath;
    const QFileInfo savedInfo(saved);
    if (!savedInfo.isFile()) {
        setPackageInstalled(id, true);
        return;
    }
    if (auto *existing =
            findChild<QMessageBox *>(QStringLiteral("packageInstallImport"));
        existing && existing->isVisible()) {
        existing->raise();
        existing->activateWindow();
        return;
    }

    const QString name = manifest->name;
    auto *box = new QMessageBox(this);
    box->setObjectName(QStringLiteral("packageInstallImport"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setModal(false);
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(QStringLiteral("Install package"));
    box->setText(QStringLiteral("Import your saved %1 data?").arg(name));
    box->setInformativeText(
        QStringLiteral("%1 keeps your data on this device. A copy saved %2 is "
                       "in %3; import it to pick up where you left off.")
            .arg(name,
                 QLocale().toString(savedInfo.lastModified(),
                                    QLocale::ShortFormat),
                 QDir::toNativeSeparators(saved)));
    QPushButton *import = box->addButton(
        QStringLiteral("Import a saved copy…"), QMessageBox::AcceptRole);
    import->setObjectName(QStringLiteral("packageInstallImportButton"));
    QPushButton *fresh =
        box->addButton(QStringLiteral("Start fresh"), QMessageBox::AcceptRole);
    fresh->setObjectName(QStringLiteral("packageInstallFreshButton"));
    QPushButton *cancel = box->addButton(QMessageBox::Cancel);
    box->setDefaultButton(import);
    box->setEscapeButton(cancel);
    forkmesh::ui::onMessageBoxClicked(box, this,
            [this, id, import, fresh](QAbstractButton *clicked) {
                if (clicked == fresh)
                    setPackageInstalled(id, true);
                else if (clicked == import)
                    installPackageFromSavedCopy(id);
            });
    box->show();
}

void MainWindow::installPackageFromSavedCopy(const QString &id)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!manifest)
        return;
    const QString name = manifest->name;
    const QString backup = packageDataBackupPath(id);
    QString path = m_packageDataTestFilePath;
    if (path.isEmpty()) {
        path = QFileDialog::getOpenFileName(
            this, QStringLiteral("Import your %1 data").arg(name),
            QFileInfo(backup).isFile() ? backup
                                       : QFileInfo(backup).absolutePath(),
            QStringLiteral("JSON files (*.json)"));
    }
    if (path.isEmpty())
        return;
    QString error;
    if (!importPackageDataFile(id, path, &error)) {
        flashMessage(QStringLiteral("%1 was not installed: %2").arg(name, error),
                     true);
        return;
    }
    if (!setPackageInstalled(id, true)) {
        m_packageRegistry->clearPackageData(id);
        return;
    }
    flashMessage(QStringLiteral("%1 installed with your saved data.").arg(name));
}

QMenu *MainWindow::buildPackageRailMenu(const QString &entrypoint,
                                        QWidget *parent)
{
    const PackageManifest *manifest = installedPackageForEntrypoint(entrypoint);
    if (!manifest)
        return nullptr;
    const QString id = manifest->id;
    const QString name = manifest->name;

    auto *menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("packageRailMenu"));
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.id != id || !page.widget)
            continue;
        menu->addAction(QStringLiteral("Open in new window"), this,
                        [this, id] { openNativePackageWindow(id); });
        if (page.window)
            menu->addAction(QStringLiteral("Return to main window"), this,
                            [this, id] { dockNativePackageWindow(id); });
        menu->addSeparator();
        break;
    }
    menu->addAction(QStringLiteral("%1 settings").arg(name), this, [this, id] {
        showSection(kMarketSectionIndex);
        showPackageSettings(id);
    });
    bool exportable =
        manifest->localData && m_packageRegistry->hasPackageData(id);
    if (manifest->dataScope != QLatin1String("none")) {
        for (const NativePackagePage &page :
             std::as_const(m_nativePackagePages))
            exportable = exportable || (page.id == id && page.widget);
    }
    if (exportable) {
        menu->addAction(QStringLiteral("Export data…"), this, [this, id, name] {
            savePackageDataCopy(id, [this, name](bool proceed,
                                                 const QString &path) {
                if (!proceed)
                    return;
                flashMessage(
                    path.isEmpty()
                        ? QStringLiteral("%1 has no data to export yet.")
                              .arg(name)
                        : QStringLiteral("Your %1 data was saved to %2.")
                              .arg(name, QDir::toNativeSeparators(path)));
            });
        });
    }
    addPackageFeedbackActions(menu, id);
    menu->addSeparator();
    addPackageUninstallAction(menu, id);
    addButtonPlacementActions(menu, parent);
    return menu;
}

const PackageManifest *
MainWindow::installedPackageForEntrypoint(const QString &entrypoint) const
{
    if (entrypoint.isEmpty() || !m_packageRegistry ||
        !m_packageRegistry->isReady())
        return nullptr;
    for (const PackageManifest &candidate : m_packageRegistry->packages()) {
        if (candidate.desktopEntrypoint == entrypoint &&
            m_packageRegistry->isInstalled(candidate.id))
            return &candidate;
    }
    return nullptr;
}

void MainWindow::addHomeTilePackageActions(QMenu *menu, const QString &key)
{
    QWidget *widget = railWidgetForKey(key);
    const PackageManifest *manifest =
        widget ? installedPackageForEntrypoint(
                     widget->property("forkmeshPackageEntrypoint").toString())
               : nullptr;
    if (!menu || !manifest)
        return;
    const QString id = manifest->id;
    menu->addAction(QStringLiteral("%1 settings").arg(manifest->name), this,
                    [this, id] {
                        showSection(kMarketSectionIndex);
                        showPackageSettings(id);
                    });
    addPackageUninstallAction(menu, id);
}

void MainWindow::addPackageUninstallAction(QMenu *menu, const QString &id)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!menu || !manifest)
        return;
    const QString name = manifest->name.isEmpty() ? id : manifest->name;
    QAction *uninstall = menu->addAction(
        QString::fromUtf8("Uninstall %1\xE2\x80\xA6").arg(name), this,
        [this, id] { requestPackageRemoval(id, true); });
    uninstall->setObjectName(QStringLiteral("packageUninstallAction"));
    if (manifest->managedExternally) {
        uninstall->setEnabled(false);
        uninstall->setToolTip(
            QStringLiteral("External packages are removed with marketctl"));
    } else if (!m_packageRegistry->isInstalled(id)) {
        uninstall->setEnabled(false);
    }
}

void MainWindow::addPackageFeedbackActions(QMenu *menu, const QString &id)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!menu || !manifest)
        return;
    const QString name = manifest->name.isEmpty() ? id : manifest->name;
    QAction *rate = menu->addAction(
        QString::fromUtf8("Rate %1\xE2\x80\xA6").arg(name), this,
        [this, id] { openPackageRating(id); });
    rate->setObjectName(QStringLiteral("packageRateAction"));
    rate->setToolTip(
        QStringLiteral("Rate and review %1 on ForkMesh in your browser")
            .arg(name));
    QAction *feedback = menu->addAction(
        QString::fromUtf8("Send feedback or report a bug\xE2\x80\xA6"), this,
        [this, id] { showPackageFeedbackDialog(id); });
    feedback->setObjectName(QStringLiteral("packageFeedbackAction"));
    const QUrl link =
        forkmesh::market::packageShareUrl(manifest->marketplaceId, id);
    if (!link.isValid())
        return;
    QAction *copyLink = menu->addAction(
        QStringLiteral("Copy link to %1").arg(name), this, [this, link, name] {
            QApplication::clipboard()->setText(link.toString());
            flashMessage(QStringLiteral("Copied the link to %1").arg(name));
        });
    copyLink->setObjectName(QStringLiteral("packageCopyLinkAction"));
    copyLink->setToolTip(
        QStringLiteral("Copy %1 to share this plugin").arg(link.toString()));
}

QMenu *MainWindow::buildPackageActionsMenu(const QString &id, QWidget *parent)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!manifest)
        return nullptr;
    auto *menu = new QMenu(parent);
    menu->setObjectName(QStringLiteral("packageActionsMenu"));
    menu->setToolTipsVisible(true);
    addPackageFeedbackActions(menu, id);
    menu->addSeparator();
    addPackageUninstallAction(menu, id);
    return menu;
}

QPushButton *MainWindow::createPackageActionsButton(
    std::function<QString()> packageId, QWidget *parent)
{
    auto *button = new QPushButton(parent);
    button->setObjectName(QStringLiteral("packageActionsButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(32, 30);
    forkmesh::ui::setOcticon(button, QStringLiteral("kebab-horizontal"), 16);
    button->setAccessibleName(QStringLiteral("Plugin options"));
    button->setToolTip(QStringLiteral("Plugin options"));
    connect(button, &QPushButton::clicked, this,
            [this, button, packageId = std::move(packageId)] {
                showPackageActionsMenu(button, packageId());
            });
    return button;
}

void MainWindow::showPackageActionsMenu(QPushButton *button, const QString &id)
{
    if (!button || id.isEmpty())
        return;
    for (QMenu *stale : button->findChildren<QMenu *>(
             QStringLiteral("packageActionsMenu"), Qt::FindDirectChildrenOnly))
        stale->deleteLater();
    QMenu *menu = buildPackageActionsMenu(id, button);
    if (!menu)
        return;
    menu->setAttribute(Qt::WA_DeleteOnClose);
    const QSize hint = menu->sizeHint();
    menu->popup(button->mapToGlobal(
        QPoint(button->width() - hint.width(), button->height())));
}

void MainWindow::openPackageRating(const QString &id)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!manifest)
        return;
    const QUrl url = forkmesh::market::packageRatingUrl(
        activeRelayOrigin(), manifest->marketplaceId, id);
    if (!url.isValid()) {
        flashMessage(QStringLiteral(
            "No ForkMesh host is set, so there is no page to open."));
        return;
    }
    QDesktopServices::openUrl(url);
}

void MainWindow::showPackageFeedbackDialog(const QString &id)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!manifest)
        return;
    QWidget *anchor = this;
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.id == id && page.window && page.window->isActiveWindow())
            anchor = page.window;
    }
    QList<QWidget *> hosts{this};
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.window)
            hosts.append(page.window);
    }
    for (QWidget *host : std::as_const(hosts)) {
        for (auto *existing : host->findChildren<QDialog *>(
                 QStringLiteral("packageFeedbackDialog"))) {
            if (existing->property("forkmeshPackageId").toString() == id &&
                existing->isVisible()) {
                existing->raise();
                existing->activateWindow();
                return;
            }
        }
    }
    QString appVersion;
    if (const auto *network =
            qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess))
        appVersion = network->clientVersion();
    const forkmesh::market::PackageFeedbackReport base =
        forkmesh::market::packageFeedbackReport(
            id, manifest->name, manifest->version, appVersion);
    auto sender = [this](const QJsonObject &payload,
                         forkmesh::market::PackageFeedbackReplyHandler done) {
        const QString origin = activeRelayOrigin();
        const QUrl url(origin + QStringLiteral("/api/bug-reports"));
        if (origin.isEmpty() || !url.isValid() || !m_networkAccess) {
            done(0, {}, QStringLiteral("no ForkMesh host is set"));
            return;
        }
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
        request.setRawHeader("accept", "application/json");
        request.setTransferTimeout(20000);
        const QString sessionToken = accountSessionTokenForUrl(url);
        if (!sessionToken.isEmpty())
            request.setRawHeader("Authorization",
                                 QByteArrayLiteral("Bearer ") +
                                     sessionToken.toUtf8());
        QNetworkReply *reply = m_networkAccess->post(
            request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this,
                [reply, done = std::move(done)] {
                    reply->deleteLater();
                    const int status =
                        reply->attribute(
                                 QNetworkRequest::HttpStatusCodeAttribute)
                            .toInt();
                    done(status, reply->read(64 * 1024),
                         status > 0 ? QString() : reply->errorString());
                });
    };
    auto *dialog = new forkmesh::market::PackageFeedbackDialog(
        base, std::move(sender), anchor);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

QString MainWindow::currentNativePackageId() const
{
    if (!m_sectionStack)
        return {};
    const QWidget *current = m_sectionStack->currentWidget();
    if (!current)
        return {};
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if ((page.widget && current == page.widget) ||
            (page.placeholder && current == page.placeholder))
            return page.id;
    }
    return {};
}

void MainWindow::updatePackageActionsButton()
{
    if (!m_packageActionsButton)
        return;
    const QString id = currentNativePackageId();
    const PackageManifest *manifest =
        m_packageRegistry && !id.isEmpty() ? m_packageRegistry->package(id)
                                           : nullptr;
    if (!manifest) {
        m_packageActionsButton->hide();
        return;
    }
    const QString name = manifest->name.isEmpty() ? id : manifest->name;
    m_packageActionsButton->setToolTip(
        QStringLiteral("%1 options: rate it, send feedback or report a bug, "
                       "or uninstall it")
            .arg(name));
    m_packageActionsButton->setAccessibleName(
        QStringLiteral("%1 options").arg(name));
    m_packageActionsButton->show();
}

void MainWindow::attachPackageRailMenu(QPushButton *button,
                                       const QString &entrypoint)
{
    if (!button)
        return;
    button->setProperty("forkmeshPackageEntrypoint", entrypoint);
    button->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(button, &QWidget::customContextMenuRequested, this,
            [this, button, entrypoint](const QPoint &position) {
                QMenu *menu = buildPackageRailMenu(entrypoint, button);
                if (!menu) {
                    menu = new QMenu(button);
                    addButtonPlacementActions(menu, button);
                }
                if (menu->isEmpty()) {
                    menu->deleteLater();
                    return;
                }
                menu->setAttribute(Qt::WA_DeleteOnClose);
                menu->popup(button->mapToGlobal(position));
            });
}

void MainWindow::syncPackageRuntime()
{
    bool captureInstalled = false;
    bool ledgerInstalled = false;
    bool discordInstalled = false;
    bool timersInstalled = false;
    bool tourInstalled = false;
    bool statusPageInstalled = false;
    QJsonArray localModels;
    forkmesh::ui::clearMarketplaceAgentIcons();
    if (m_packageRegistry && m_packageRegistry->isReady()) {
        for (const PackageManifest &manifest : m_packageRegistry->packages()) {
            if (manifest.agent.isValid()) {
                // The package artwork also identifies historical sessions and
                // manually installed matching models while the package itself
                // is inactive. Installation controls picker visibility below.
                for (const forkmesh::market::PackageAgentModel &candidate :
                     manifest.agent.models) {
                    forkmesh::ui::registerMarketplaceAgentIcon(
                        manifest.agent.provider, candidate.id,
                        manifest.agent.avatarPath);
                }
            }
            if (!m_packageRegistry->isInstalledHere(manifest.id))
                continue;
            if (manifest.desktopEntrypoint == QLatin1String("builtin:capture"))
                captureInstalled = true;
            else if (manifest.desktopEntrypoint == QLatin1String("builtin:ledger"))
                ledgerInstalled = true;
            else if (manifest.desktopEntrypoint == QLatin1String("builtin:discord"))
                discordInstalled = true;
            else if (manifest.desktopEntrypoint == QLatin1String("builtin:timers"))
                timersInstalled = true;
            else if (manifest.desktopEntrypoint == QLatin1String("builtin:tour"))
                tourInstalled = true;
            else if (manifest.desktopEntrypoint ==
                     QLatin1String("builtin:status-page"))
                statusPageInstalled = true;
            if (manifest.agent.isValid()) {
                QString active =
                    m_packageRegistry
                        ->packageSetting(manifest.id,
                                         QStringLiteral("activeModel"),
                                         manifest.agent.defaultModel)
                        .toString();
                const forkmesh::market::PackageAgentModel *model =
                    manifest.agent.model(active);
                if (!model) {
                    active = manifest.agent.defaultModel;
                    model = manifest.agent.model(active);
                }
                if (model) {
                    const QString endpoint =
                        m_packageRegistry
                            ->packageSetting(manifest.id,
                                             QStringLiteral("endpoint"),
                                             manifest.agent.endpoint)
                            .toString();
                    localModels.append(QJsonObject{
                        {QStringLiteral("packageId"), manifest.id},
                        {QStringLiteral("provider"), manifest.agent.provider},
                        {QStringLiteral("id"), active},
                        {QStringLiteral("label"), model->label},
                        {QStringLiteral("agentName"), manifest.name},
                        {QStringLiteral("endpoint"), endpoint},
                        {QStringLiteral("contextTokens"),
                         model->contextTokens},
                        {QStringLiteral("avatar"),
                         manifest.agent.avatarPath},
                    });
                }
            }
        }
    }

    QSettings settings;
    const QByteArray localModelsJson =
        QJsonDocument(localModels).toJson(QJsonDocument::Compact);
    const QByteArray priorLocalModels =
        settings
            .value(forkmesh::ui::kMarketplaceLocalModelsCacheSetting)
            .toByteArray();
    if (priorLocalModels != localModelsJson)
        settings.setValue(forkmesh::ui::kMarketplaceLocalModelsCacheSetting,
                          localModelsJson);

    if (captureInstalled && !m_captureFeature) {
        m_captureFeature = std::make_unique<forkmesh::capture::CaptureFeature>(
            this,
            [this](const QImage &annotated) {
                const QString path = saveNewAgentPromptImage(annotated);
                if (path.isEmpty()) {
                    logSystem("Couldn't save the screenshot.");
                    return;
                }
                queueQuickAddImage(path);
                if (m_issueQuickAdd)
                    m_issueQuickAdd->setFocus();
            },
            [this](const QString &status) { logSystem(status); }, this);
    } else if (!captureInstalled && m_captureFeature) {
        m_captureFeature->shutdown();
        m_captureFeature.reset();
    }

    if (m_navScreenshotButton) {
        m_navScreenshotButton->setVisible(captureInstalled);
        m_navScreenshotButton->setEnabled(captureInstalled);
    }
    if (ledgerInstalled && !m_ledgerFeature) {
        m_ledgerFeature = std::make_unique<forkmesh::ledger::LedgerFeature>(
            [](const QUrl &url) { return QDesktopServices::openUrl(url); });
    } else if (!ledgerInstalled) {
        m_ledgerFeature.reset();
    }
    if (m_ledgerNavButton) {
        m_ledgerNavButton->setVisible(ledgerInstalled);
        m_ledgerNavButton->setEnabled(ledgerInstalled);
    }
    // Removing the package while its page is open would otherwise leave the
    // reader on a section whose rail entry has just disappeared. Market is
    // where they would put it back.
    if (!ledgerInstalled && m_sectionStack &&
        m_sectionStack->currentIndex() == kLedgerSectionIndex)
        showSection(kMarketSectionIndex);

    // Timers contributes the clock in the window's top bar; its lifecycle
    // lives with the rest of the strip in MainWindowTimers.cpp. Detaching takes
    // the clock and its chips off the top bar, and the registry's uninstall
    // clears the package's own settings, so the entries go with it.
    if (timersInstalled)
        attachTimersPackage();
    else
        detachTimersPackage();

    syncTourPackage(tourInstalled);
    syncStatusPagePackage(statusPageInstalled);

    // Discord contributes to the Chat sidebar; its lifecycle lives next to
    // the rest of the Discord code in MainWindowSetup.cpp.
    if (discordInstalled && !m_discordPackageActive)
        attachDiscordPackage();
    else if (!discordInstalled && m_discordPackageActive)
        detachDiscordPackage();

    syncDesktopPackageExtensions();

    if (priorLocalModels != localModelsJson) {
        m_composerModelCatalogInputs.clear();
        m_composerModelCatalog.clear();
        refreshAgentModelSelectors();
        refreshComposerModelVisibilityList();
    }
    syncNativePackagePages();
    syncPackageSurfaces();
}

bool MainWindow::packageSurfaceShown(const QString &packageId) const
{
    if (!m_packageRegistry)
        return true;
    if (!m_packageRegistry->isReady() || !m_packageRegistry->package(packageId))
        return m_packageRegistry->isLegacyProject();
    return m_packageRegistry->isShownHere(packageId);
}

void MainWindow::syncPackageSurfaces()
{
    QHash<QString, bool> shown;
    const auto on = [this, &shown](const QString &id) {
        auto it = shown.constFind(id);
        if (it == shown.constEnd())
            it = shown.insert(id, packageSurfaceShown(id));
        return it.value();
    };
    QWidget *shortcutsTab =
        m_repoDetailTabs && m_shortcutsTabIndex >= 0
            ? m_repoDetailTabs->button(m_shortcutsTabIndex)
            : nullptr;
    QWidget *sizeMapTab =
        m_repoDetailTabs && m_sizeMapTabIndex >= 0
            ? m_repoDetailTabs->button(m_sizeMapTabIndex)
            : nullptr;
    const QList<std::pair<QWidget *, bool>> gated = {
        {m_agentsNavButton, on(QStringLiteral("agents"))},
        {m_sessionsNavButton, on(QStringLiteral("agents"))},
        {m_railCodeButton, on(QStringLiteral("code"))},
        {m_railGitButton, on(QStringLiteral("commits"))},
        {m_chatButton, on(QStringLiteral("chat"))},
        {m_notesNavButton, on(QStringLiteral("notes"))},
        {m_calendarNavButton, on(QStringLiteral("calendar"))},
        {m_tasksNavButton, on(QStringLiteral("tasks"))},
        {m_notificationButton, on(QStringLiteral("inbox"))},
        {m_networkNavButton,
         on(QStringLiteral("network")) || on(QStringLiteral("hosts"))},
        {m_teamNavButton, on(QStringLiteral("special-agents"))},
        {m_statsNavButton, on(QStringLiteral("stats"))},
        {m_repoIssuesTab, on(QStringLiteral("issues"))},
        {m_repoPullsTab, on(QStringLiteral("pull-requests"))},
        {m_repoDiscussionsTab, on(QStringLiteral("discussions"))},
        {m_repoActionsTab, on(QStringLiteral("actions"))},
        {m_repoReleasesTab, on(QStringLiteral("releases"))},
        {m_branchesButton, on(QStringLiteral("branches"))},
        {shortcutsTab, on(QStringLiteral("shortcuts"))},
        {sizeMapTab, on(QStringLiteral("size-map"))},
        {m_codeMapButton, on(QStringLiteral("code-map"))},
    };
    bool changed = false;
    for (const auto &[widget, visible] : gated) {
        if (!widget)
            continue;
        const bool explicitlyHidden =
            widget->isHidden() &&
            widget->testAttribute(Qt::WA_WState_ExplicitShowHide);
        if (visible != explicitlyHidden ||
            (visible && !widget->parentWidget()))
            continue;
        widget->setVisible(visible);
        changed = true;
    }

    if (m_networkTabs) {
        const bool nodes = on(QStringLiteral("network"));
        const bool hosts = on(QStringLiteral("hosts"));
        m_networkTabs->setTabVisible(kNetworkNodesTab, nodes);
        m_networkTabs->setTabVisible(kNetworkHostsTab, hosts);
        const int current = m_networkTabs->currentIndex();
        if (!m_networkTabs->isTabVisible(current)) {
            for (int i = 0; i < m_networkTabs->count(); ++i) {
                if (m_networkTabs->isTabVisible(i)) {
                    m_networkTabs->setCurrentIndex(i);
                    break;
                }
            }
        }
    }
    if (m_settingsSections && m_organizationsSettingsTabIndex >= 0)
        m_settingsSections->setSectionHidden(
            m_organizationsSettingsTabIndex,
            !on(QStringLiteral("organizations")));

    QSet<QString> hidden;
    for (auto it = shown.constBegin(); it != shown.constEnd(); ++it) {
        if (!it.value())
            hidden.insert(it.key());
    }
    const QSet<QString> newlyHidden = m_packageSurfacesSynced
                                          ? hidden - m_hiddenPackageSurfaces
                                          : QSet<QString>();
    const bool chatNewlyShown =
        !hidden.contains(QStringLiteral("chat")) &&
        (!m_packageSurfacesSynced ||
         m_hiddenPackageSurfaces.contains(QStringLiteral("chat")));
    m_hiddenPackageSurfaces = hidden;
    m_packageSurfacesSynced = true;
    if (chatNewlyShown || newlyHidden.contains(QStringLiteral("chat")))
        checkLocalMeshFirewall();

    if (!newlyHidden.isEmpty() && m_sectionStack) {
        const int section = m_sectionStack->currentIndex();
        const QHash<int, QString> sectionPackages = {
            {2, QStringLiteral("chat")},
            {3, QStringLiteral("inbox")},
            {kNotesSectionIndex, QStringLiteral("notes")},
            {kCalendarSectionIndex, QStringLiteral("calendar")},
            {kOrganizationTasksSectionIndex, QStringLiteral("tasks")},
            {kTeamSectionIndex, QStringLiteral("special-agents")},
            {kStatsSectionIndex, QStringLiteral("stats")},
        };
        const bool networkGone =
            section == kNetworkDiagnosticsSectionIndex &&
            !on(QStringLiteral("network")) && !on(QStringLiteral("hosts")) &&
            (newlyHidden.contains(QStringLiteral("network")) ||
             newlyHidden.contains(QStringLiteral("hosts")));
        if (networkGone ||
            newlyHidden.contains(sectionPackages.value(section))) {
            showSection(kMarketSectionIndex);
        } else if (section == 0 && m_repoDetailStack && m_repoDetailTabs) {
            const int tab = m_repoDetailStack->currentIndex();
            const QHash<int, QString> tabPackages = {
                {1, QStringLiteral("commits")},
                {2, QStringLiteral("issues")},
                {forkmesh::ui::kRepoAgentsTab, QStringLiteral("agents")},
                {4, QStringLiteral("pull-requests")},
                {5, QStringLiteral("discussions")},
                {6, QStringLiteral("actions")},
                {m_branchesTabIndex, QStringLiteral("branches")},
                {m_releasesTabIndex, QStringLiteral("releases")},
                {m_shortcutsTabIndex, QStringLiteral("shortcuts")},
                {m_sizeMapTabIndex, QStringLiteral("size-map")},
            };
            if (newlyHidden.contains(tabPackages.value(tab)) &&
                m_repoDetailTabs->button(0))
                m_repoDetailTabs->button(0)->click();
        }
    }

    applyTeammateRailVisibility();
    if (changed) {
        updateRepoActivityRail();
        refreshHomeGridPage();
    }
}

#ifdef FORKMESH_WINDOW_TESTS
bool MainWindow::testRailButtonShown(const QString &key) const
{
    for (const auto &[railKey, widget] : railHomeButtons()) {
        if (railKey == key)
            return widget &&
                   !(widget->isHidden() &&
                     widget->testAttribute(Qt::WA_WState_ExplicitShowHide));
    }
    return false;
}

bool MainWindow::testRepoTabShown(const QString &key) const
{
    for (const auto &[tabKey, widget] :
         m_repoTabRowTabs + m_repoCodeBarButtons) {
        if (tabKey == key)
            return widget &&
                   !(widget->isHidden() &&
                     widget->testAttribute(Qt::WA_WState_ExplicitShowHide));
    }
    return false;
}
#endif

void MainWindow::syncNativePackagePages()
{
    if (!m_packageRegistry || !m_packageRegistry->isReady() ||
        !m_sectionStack || !m_appNavigationRailLayout || !m_navGroup)
        return;

    QList<QPair<QString, QString>> desired;
    for (const PackageManifest &manifest : m_packageRegistry->packages()) {
        if (!m_packageRegistry->isInstalledHere(manifest.id) ||
            !forkmesh::market::nativePackageContribution(
                manifest.desktopEntrypoint, manifest.type))
            continue;
        desired.append({manifest.id, manifest.desktopEntrypoint});
    }
    bool unchanged = desired.size() == m_nativePackagePages.size();
    for (int index = 0; unchanged && index < desired.size(); ++index) {
        unchanged = m_nativePackagePages.at(index).id == desired.at(index).first &&
                    m_nativePackagePages.at(index).entrypoint ==
                        desired.at(index).second;
    }
    if (unchanged)
        return;

    bool removingCurrent = false;
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        removingCurrent = removingCurrent ||
            (page.widget && m_sectionStack->currentWidget() == page.widget) ||
            (page.placeholder &&
             m_sectionStack->currentWidget() == page.placeholder);
    }
    if (removingCurrent)
        showSection(kMarketSectionIndex);
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.window) {
            page.window->removeEventFilter(this);
            page.window->hide();
            page.window->deleteLater();
        }
        if (page.placeholder) {
            m_sectionStack->removeWidget(page.placeholder);
            page.placeholder->deleteLater();
        }
        if (page.navigation) {
            m_navGroup->removeButton(page.navigation);
            m_appNavigationRailLayout->removeWidget(page.navigation);
            page.navigation->deleteLater();
        }
        if (page.widget) {
            m_sectionStack->removeWidget(page.widget);
            page.widget->deleteLater();
        }
    }
    m_nativePackagePages.clear();

    for (const auto &[id, entrypoint] : desired) {
        const PackageManifest *manifest = m_packageRegistry->package(id);
        if (!manifest)
            continue;
        const forkmesh::market::NativePackageContribution *contribution =
            forkmesh::market::nativePackageContribution(
                entrypoint, manifest->type);
        if (!contribution)
            continue;
        const forkmesh::market::NativePackageContribution contributionCopy =
            *contribution;
        if (!contribution->browserPath.isEmpty()) {
            auto *link = new ActivityRailButton(contribution->navigationIcon,
                                                contribution->navigationLabel);
            link->setCheckable(false);
            link->setCursor(Qt::PointingHandCursor);
            link->setObjectName(QStringLiteral("nativePackageNav-%1").arg(id));
            link->setToolTip(QStringLiteral("Open %1 in your browser")
                                 .arg(contribution->navigationLabel));
            const int beforeChat = m_appNavigationRailLayout->indexOf(m_chatButton);
            m_appNavigationRailLayout->insertWidget(
                beforeChat < 0 ? 0 : beforeChat, link, 0, Qt::AlignLeft);
            connect(link, &QPushButton::clicked, this,
                    [this, id] { openNativePackageLink(id); });
            attachPackageRailMenu(link, entrypoint);
            m_nativePackagePages.append({id, entrypoint, link, nullptr});
            continue;
        }
        QStringList permissionIds;
        for (const forkmesh::market::PackagePermission &permission :
             manifest->permissions)
            permissionIds.append(permission.id);
        forkmesh::market::NativePackageHost host{
            manifest->id,
            manifest->name,
            manifest->type,
            manifest->version,
            permissionIds,
            [this, id, contributionCopy](
                const QByteArray &method, const QString &path,
                const QJsonObject &body,
                forkmesh::market::NativePackageReplyHandler handler) {
                requestNativePackage(id, contributionCopy, method, path, body,
                                     std::move(handler));
            },
            m_networkAccess,
            [this] { nativePackageSearchIndexChanged(); },
            [this] {
                refreshChatUserDirectory();
                return ledgerRecipientCandidates();
            },
            [this] { updateQuickAddEnterTarget(); },
            [this, id] { syncNativePackageBadge(id); },
            [this] { return m_isAdmin; },
            [this](forkmesh::market::NativePackageUserRecordsHandler done) {
                fetchNativePackageUserRecords(std::move(done));
            },
            [this] { return nativePackageAgentName(); },
            [this](const QString &prompt,
                   forkmesh::market::NativePackageAgentReplyHandler done) {
                askNativePackageAgent(prompt, std::move(done));
            },
            [this] { schedulePackageRailRefresh(); },
            [this] { return nativePackageSpecialAgents(); },
            [this](const QString &agentId, const QString &action,
                   const QString &text,
                   forkmesh::market::NativePackageSpecialAgentActionHandler done) {
                runNativePackageSpecialAgentAction(agentId, action, text,
                                                   std::move(done));
            },
        };
        if (!permissionIds.contains(QStringLiteral("desktop.agents.run"))) {
            host.specialAgents = nullptr;
            host.specialAgentAction = nullptr;
        }
        host.accountName = [this] {
            return AccountCapability::normalizedAccount(m_accountName);
        };
        host.openDirectMessage = [this](const QString &account) {
            const QString name = account.trimmed().toLower();
            if (forkmesh::ui::isValidNodeName(name))
                startDirectMessageWith(name, QString(), QString());
        };
        host.openUserProfile = [this](const QString &account) {
            const QString name = account.trimmed().toLower();
            if (!forkmesh::ui::isValidNodeName(name))
                return;
            MemberInfo member;
            member.id = QStringLiteral("user:") + name;
            member.name = name;
            member.nodeName = name;
            member.ownerUser = name;
            member.accountKind = QStringLiteral("user");
            showChatUserProfile(member, {});
        };
        if (permissionIds.contains(QStringLiteral("chat.messages.share"))) {
            host.chatTargets = [this] {
                QList<forkmesh::market::NativePackageChatTarget> targets;
                if (!m_officeChannelMirror || !m_officeChannelMirror->isActive())
                    return targets;
                QStringList seen;
                const QStringList channels = m_officeChannelMirror->conversations();
                const QStringList direct = m_officeChannelMirror->directConversations();
                for (const QStringList &group : {channels, direct}) {
                    for (const QString &conversation : group) {
                        if (seen.contains(conversation) ||
                            !m_officeChannelMirror->canSend(conversation))
                            continue;
                        seen.append(conversation);
                        targets.append({conversation, chatConversationLabel(conversation),
                                        forkmesh::office::isDirectOfficeConversation(conversation)});
                    }
                }
                return targets;
            };
            host.sendChatMessage = [this](const QString &conversation, const QString &text) {
                return m_officeChannelMirror && m_officeChannelMirror->isActive() &&
                       m_officeChannelMirror->sendMessage(conversation, text);
            };
        }
        host.openNetworkShare = [this](const QString &url, bool guest) {
            openNetworkShareLocation(url, guest);
        };
        host.openWebPage = [this](const QString &path) {
            const QUrl url = forkmesh::market::nativePackageWebPageUrl(
                catalogApiUrl(), path);
            if (!url.isValid()) {
                flashMessage(QStringLiteral(
                    "That page cannot be opened from here."));
                return;
            }
            QDesktopServices::openUrl(url);
        };
        if (permissionIds.contains(QStringLiteral("desktop.models.usage.read")))
            host.agentUsagePage = [this](QWidget *parent) {
                return buildAgentUsagePage(parent);
            };
        if (permissionIds.contains(QStringLiteral("desktop.logins.use"))) {
            const QString requester = manifest->name;
            host.loginStoreName = [this] {
                QString name;
                return nativeLoginStore(&name) ? name : QString();
            };
            host.saveLogin = [this, requester](
                                 const forkmesh::market::NativePackageLogin &login,
                                 QWidget *asker,
                                 forkmesh::market::NativePackageLoginHandler done) {
                QString name;
                if (forkmesh::market::NativePackageWidget *store = nativeLoginStore(&name))
                    store->saveLogin(login, requester, asker, std::move(done));
                else
                    done({false, false, QStringLiteral("No password manager is installed."), {}});
            };
            host.findLogins = [this, requester](
                                  const QString &url, QWidget *asker,
                                  forkmesh::market::NativePackageLoginHandler done) {
                QString name;
                if (forkmesh::market::NativePackageWidget *store = nativeLoginStore(&name))
                    store->findLogins(url, requester, asker, std::move(done));
                else
                    done({false, false, QStringLiteral("No password manager is installed."), {}});
            };
        }
        if (permissionIds.contains(QStringLiteral("repository.read"))) {
            host.repositories = [this] { return nativePackageRepositories(); };
            host.currentRepository = [this] { return currentNativePackageRepository(); };
            host.openRepositoryFile =
                [this](const forkmesh::market::NativePackageRepository &repository,
                       const QString &path, int line) {
                    openNativePackageRepositoryFile(repository, path, line);
                };
        }
        forkmesh::market::NativePackageWidget *widget =
            contribution->factory(host, m_sectionStack);
        if (!widget)
            continue;
        connect(this, &MainWindow::chatUserDirectoryChanged, widget,
                [widget] { widget->userDirectoryChanged(); });
        widget->setProperty("forkmeshPackageId", id);
        const int sectionIndex = m_sectionStack->addWidget(widget);
        auto *navigation = new ActivityRailButton(
            contribution->navigationIcon, contribution->navigationLabel);
        navigation->setCheckable(true);
        navigation->setCursor(Qt::PointingHandCursor);
        navigation->setObjectName(QStringLiteral("nativePackageNav-%1").arg(id));
        navigation->setToolTip(
            QStringLiteral("Open %1").arg(contribution->navigationLabel));
        m_navGroup->addButton(navigation, sectionIndex);
        const int beforeChat = m_appNavigationRailLayout->indexOf(m_chatButton);
        m_appNavigationRailLayout->insertWidget(
            beforeChat < 0 ? 0 : beforeChat, navigation, 0, Qt::AlignLeft);
        connect(navigation, &QPushButton::clicked, this,
                [this, id] { showNativePackage(id); });
        attachPackageRailMenu(navigation, entrypoint);
        m_nativePackagePages.append({id, entrypoint, navigation, widget});
        syncNativePackageBadge(id);
        navigation->setDesktopOffline(
            m_desktopOfflineDots &&
            forkmesh::market::PackageRegistry::reliesOnDesktop(id));
        if (hasActiveAccountSession() || m_startupAuthResolved)
            QTimer::singleShot(0, widget, [this, widget] {
                preparePackagePage(widget);
            });
    }
    applyRailCaptionPreference(); // the new package pages joined the rail
    applyRailOrder();
    schedulePackageRailRefresh();
    updatePackageActionsButton();
}

QUrl MainWindow::nativePackageLinkUrl(const QString &id) const
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    const forkmesh::market::NativePackageContribution *contribution =
        manifest ? forkmesh::market::nativePackageContribution(
                       manifest->desktopEntrypoint, manifest->type)
                 : nullptr;
    if (!contribution || contribution->browserPath.isEmpty())
        return {};
    QUrl url = catalogApiUrl();
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != QLatin1String("http") &&
         url.scheme() != QLatin1String("https")))
        return {};
    url.setUserName(QString());
    url.setPassword(QString());
    url.setPath(contribution->browserPath);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

forkmesh::market::NativePackageWidget *MainWindow::nativeLoginStore(QString *name) const
{
    if (!m_packageRegistry)
        return nullptr;
    for (const NativePackagePage &page : m_nativePackagePages) {
        if (!page.widget || !page.widget->storesLogins())
            continue;
        const PackageManifest *manifest = m_packageRegistry->package(page.id);
        if (!manifest || !m_packageRegistry->isInstalledHere(page.id))
            continue;
        const bool allowed = std::any_of(
            manifest->permissions.cbegin(), manifest->permissions.cend(),
            [](const forkmesh::market::PackagePermission &permission) {
                return permission.id == QLatin1String("desktop.logins.store");
            });
        if (!allowed)
            continue;
        if (name)
            *name = manifest->name;
        return page.widget;
    }
    return nullptr;
}

void MainWindow::openNativePackageLink(const QString &id)
{
    const QUrl url = nativePackageLinkUrl(id);
    if (!url.isValid()) {
        flashMessage(QStringLiteral("No ForkMesh host is set, so there is no page to open."));
        return;
    }
    QDesktopServices::openUrl(url);
}

// The top search box answers from each package's loaded records, and a page
// loads its records when it is opened. A package the user has not visited this
// session would otherwise be silently missing from the results, so the first
// query warms every page that has not loaded yet.
void MainWindow::prepareNativePackageSearch()
{
    for (NativePackagePage &page : m_nativePackagePages) {
        if (page.searchPrepared || !page.widget)
            continue;
        page.searchPrepared = true;
        page.widget->prepareSearch();
    }
}

void MainWindow::syncDesktopOfflineDots(bool offline)
{
    m_desktopOfflineDots = offline;
    const auto mark = [offline](QPushButton *button, const QString &id) {
        if (auto *rail = dynamic_cast<ActivityRailButton *>(button))
            rail->setDesktopOffline(
                offline &&
                forkmesh::market::PackageRegistry::reliesOnDesktop(id));
    };
    mark(m_agentsNavButton, QStringLiteral("agents"));
    mark(m_sessionsNavButton, QStringLiteral("agents"));
    mark(m_teamNavButton, QStringLiteral("special-agents"));
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
        mark(page.navigation.data(), page.id);
}

void MainWindow::syncNativePackageBadge(const QString &id)
{
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.id != id || !page.widget)
            continue;
        if (auto *rail = dynamic_cast<ActivityRailButton *>(page.navigation.data()))
            rail->setBadgeCount(qMax(0, page.widget->navigationBadge()));
    }
}

forkmesh::market::NativePackageRepository
MainWindow::nativePackageRepository(int index) const
{
    if (index < 0 || index >= m_repositories.size())
        return {};
    const RepositoryRecord &record = m_repositories.at(index);
    QString path;
    if (!record.localPath.isEmpty() && QDir(record.localPath).exists())
        path = record.localPath;
    else if (!record.mirrorPath.isEmpty() && QDir(record.mirrorPath).exists())
        path = record.mirrorPath;
    if (path.isEmpty())
        return {};
    const QString title = record.owner.isEmpty()
                              ? record.name
                              : record.owner + QLatin1Char('/') + record.name;
    return {title.toLower(), title, path,
            index == m_repoDetailIndex ? currentRef() : QString()};
}

QList<forkmesh::market::NativePackageRepository>
MainWindow::nativePackageRepositories() const
{
    QList<forkmesh::market::NativePackageRepository> repositories;
    const forkmesh::market::NativePackageRepository open =
        currentNativePackageRepository();
    if (!open.path.isEmpty())
        repositories.append(open);
    for (int i = 0; i < m_repositories.size(); ++i) {
        if (i == m_repoDetailIndex || m_repositories.at(i).previewOnly)
            continue;
        const forkmesh::market::NativePackageRepository repository =
            nativePackageRepository(i);
        if (!repository.path.isEmpty())
            repositories.append(repository);
    }
    return repositories;
}

forkmesh::market::NativePackageRepository
MainWindow::currentNativePackageRepository() const
{
    return nativePackageRepository(m_repoDetailIndex);
}

void MainWindow::openNativePackageRepositoryFile(
    const forkmesh::market::NativePackageRepository &repository,
    const QString &path, int line)
{
    if (path.isEmpty() || repository.path.isEmpty())
        return;
    const forkmesh::market::NativePackageRepository open =
        currentNativePackageRepository();
    if (QDir::cleanPath(open.path) == QDir::cleanPath(repository.path)) {
        showSection(0);
        openRepoFileAtLine(path, line);
        return;
    }
    const QString absolute = QDir(repository.path).filePath(path);
    if (QFileInfo(absolute).isFile()) {
        openRepoFile(absolute);
        return;
    }
    flashMessage(QStringLiteral("Open %1 to see %2.").arg(repository.title, path), true);
}

void MainWindow::notifyNativePackagesRepositoryChanged()
{
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
        if (page.widget)
            page.widget->repositoryChanged();
}

QString MainWindow::nativePackageAgentName() const
{
    using namespace forkmesh::ui;
    const QString provider = defaultAgentProvider();
    if (agentIsClaudeProvider(provider))
        return QStringLiteral("Claude Code");
    if (agentIsCodexProvider(provider) || agentUsesOpenAiKey(provider))
        return QStringLiteral("Codex");
    return QString();
}

void MainWindow::askNativePackageAgent(
    const QString &prompt,
    forkmesh::market::NativePackageAgentReplyHandler done)
{
    using namespace forkmesh::ui;
    const QString provider = defaultAgentProvider();
    if (nativePackageAgentName().isEmpty()) {
        if (done)
            done(QString(), QStringLiteral(
                "The default agent (%1) cannot write text for a package. Pick "
                "Claude Code or Codex as the default agent in Settings.")
                                .arg(provider));
        return;
    }
    const bool claude = agentIsClaudeProvider(provider);
    AgentRuntimeSession *session = claude
        ? static_cast<AgentRuntimeSession *>(new ClaudeStreamSession(this))
        : static_cast<AgentRuntimeSession *>(new CodexAppServerSession(this));
    auto *timeout = new QTimer(session);
    timeout->setSingleShot(true);
    auto handled = std::make_shared<bool>(false);
    auto errors = std::make_shared<QString>();
    QPointer<AgentRuntimeSession> guard(session);
    auto finish = std::make_shared<std::function<void(const QString &, const QString &)>>();
    *finish = [guard, handled, done](const QString &reply, const QString &error) {
        if (*handled)
            return;
        *handled = true;
        if (done)
            done(reply, error);
        if (guard) {
            guard->stop();
            guard->deleteLater();
        }
    };
    connect(session, &AgentRuntimeSession::event, this,
            [finish](const QJsonObject &agentEvent) {
        if (agentEvent.value(QStringLiteral("type")).toString() != QLatin1String("result"))
            return;
        const QString reply = agentEvent.value(QStringLiteral("result")).toString().trimmed();
        if (agentEvent.value(QStringLiteral("is_error")).toBool() || reply.isEmpty())
            (*finish)(QString(), reply.isEmpty()
                ? QStringLiteral("The agent finished without writing anything.") : reply);
        else
            (*finish)(reply, QString());
    });
    connect(session, &AgentRuntimeSession::stderrText, this,
            [errors](const QString &text) {
        if (errors->size() < 1200)
            errors->append(text);
    });
    connect(session, &AgentRuntimeSession::finished, this, [finish, errors](int) {
        const QString failure = errors->trimmed();
        (*finish)(QString(), failure.isEmpty()
            ? QStringLiteral("The agent stopped without a reply. Make sure its CLI is "
                             "installed and signed in.")
            : failure);
    });
    connect(timeout, &QTimer::timeout, this, [finish] {
        (*finish)(QString(), QStringLiteral(
            "The agent took too long to answer. Try again in a moment."));
    });
    timeout->start(300000);
    const QString workdir = workingClientDir();
    if (claude) {
        QString model = QSettings().value(kClaudeCodeModelSetting).toString().trimmed();
        if (model == kClaudeAutoModelId ||
            !agentModelMatchesProvider(QStringLiteral("claude-code"), model))
            model.clear();
        QStringList env{QStringLiteral("ANTHROPIC_API_KEY"),
                        QStringLiteral("ANTHROPIC_AUTH_TOKEN"),
                        QStringLiteral("ANTHROPIC_ADMIN_KEY"),
                        QStringLiteral("CLAUDE_CODE_OAUTH_TOKEN")};
        env << activeAgentAccountEnv(QStringLiteral("claude-code"));
        static_cast<ClaudeStreamSession *>(session)->start(
            workdir, env, prompt, QStringLiteral("plan"), QString(), model,
            QStringLiteral("low"));
    } else {
        QString model = codexChatGptModelId(
            QSettings().value(kCodexModelSetting).toString().trimmed());
        if (!agentModelMatchesProvider(QStringLiteral("codex"), model))
            model.clear();
        QStringList env{QStringLiteral("OPENAI_API_KEY"),
                        QStringLiteral("CODEX_API_KEY"),
                        QStringLiteral("OPENAI_ACCESS_TOKEN"),
                        QStringLiteral("OPENAI_ADMIN_KEY")};
        env << activeAgentAccountEnv(QStringLiteral("codex"));
        static_cast<CodexAppServerSession *>(session)->start(
            workdir, env, prompt, QString(), model, QStringLiteral("Plan"),
            QStringLiteral("low"));
    }
}

QJsonArray MainWindow::nativePackageSpecialAgents()
{
    static QHash<QString, QString> faces;
    QJsonArray rows;
    for (const QJsonValue &value :
         localControlTeammatesPayload().value(QStringLiteral("teammates")).toArray()) {
        QJsonObject row = value.toObject();
        const QString avatar = row.value(QStringLiteral("avatar")).toString();
        const QString provider = row.value(QStringLiteral("provider")).toString();
        const QString model = row.value(QStringLiteral("model")).toString();
        const QString key = avatar + QLatin1Char('|') + provider + QLatin1Char('|') + model;
        auto face = faces.constFind(key);
        if (face == faces.constEnd()) {
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            forkmesh::ui::roundedAvatar(
                forkmesh::ui::teammateFacePixmap(avatar, provider, model, 64), 64, 0.5)
                .toImage()
                .save(&buffer, "PNG");
            face = faces.insert(key, QString::fromLatin1(png.toBase64()));
        }
        row.insert(QStringLiteral("face"), face.value());
        rows.append(row);
    }
    return rows;
}

void MainWindow::runNativePackageSpecialAgentAction(
    const QString &agentId, const QString &action, const QString &text,
    forkmesh::market::NativePackageSpecialAgentActionHandler done)
{
    QTimer::singleShot(0, this, [this, agentId, action, text, done = std::move(done)] {
        const auto reply = [&done](bool ok, const QString &message) {
            if (done)
                done(ok, message);
        };
        forkmesh::teammates::Config saved;
        if (!localControlFindTeammate(agentId, &saved)) {
            reply(false, QStringLiteral("That special agent no longer exists."));
            return;
        }
        if (action == QLatin1String("open")) {
            showTeammateHome(agentId);
            reply(true, QStringLiteral("Opened %1.").arg(saved.name));
            return;
        }
        if (action == QLatin1String("run")) {
            const int session = runTeammate(agentId);
            if (session <= 0)
                reply(false, QStringLiteral("%1 did not start. Open it to see why.")
                                 .arg(saved.name));
            else
                reply(true, QStringLiteral("%1 is running \u00b7 session #%2")
                                .arg(saved.name)
                                .arg(session));
            return;
        }
        if (action == QLatin1String("prompt")) {
            const QString message = text.trimmed();
            if (!saved.workflow.isEmpty()) {
                reply(false, QStringLiteral("%1 runs a built-in workflow, so it "
                                            "takes no message. Run it instead.")
                                 .arg(saved.name));
                return;
            }
            if (message.isEmpty()) {
                reply(false, QStringLiteral("Type a message for %1 first.").arg(saved.name));
                return;
            }
            sendTeammatePrompt(agentId, message);
            reply(true, QStringLiteral("Sent to %1.").arg(saved.name));
            return;
        }
        reply(false, QStringLiteral("Special agents cannot do \"%1\".").arg(action));
    });
}

void MainWindow::nativePackageSearchIndexChanged()
{
    // Only a query the user is still looking at is worth redrawing: a rebuild
    // after they picked a row would pop the dropdown open again.
    if (!m_globalSearch || !m_globalSearchTimer || !m_globalSearch->hasFocus() ||
        m_globalSearch->text().trimmed().isEmpty())
        return;
    m_globalSearchTimer->start();
}

void MainWindow::syncNativePackagePageSearch()
{
    const QWidget *current = m_sectionStack ? m_sectionStack->currentWidget()
                                            : nullptr;
    const QString query = m_globalSearch ? m_globalSearch->text().trimmed()
                                         : QString();
    for (NativePackagePage &page : m_nativePackagePages) {
        if (!page.widget)
            continue;
        const bool shown = current == page.widget;
        const QString wanted = shown ? query : QString();
        const bool sent = page.hostSearchTarget.data() ==
                          static_cast<QWidget *>(page.widget.data());
        if (sent ? page.hostSearchQuery == wanted : !shown && wanted.isEmpty())
            continue;
        page.hostSearchTarget = page.widget.data();
        page.hostSearchQuery = wanted;
        page.widget->setHostSearchQuery(wanted);
    }
}

forkmesh::market::NativePackageWidget *MainWindow::quickAddPackagePromptTarget() const
{
    const QWidget *current = m_sectionStack ? m_sectionStack->currentWidget()
                                            : nullptr;
    if (!current)
        return nullptr;
    for (const NativePackagePage &page : m_nativePackagePages) {
        if (page.widget && page.widget == current &&
            !page.widget->hostPromptTarget().placeholder.isEmpty())
            return page.widget;
    }
    return nullptr;
}

bool MainWindow::submitQuickAddToPackage()
{
    forkmesh::market::NativePackageWidget *target = quickAddPackagePromptTarget();
    if (!target || !m_issueQuickAdd)
        return false;
    const QString text = m_issueQuickAdd->toPlainText().trimmed();
    if (text.isEmpty() && m_quickAddImages.isEmpty())
        return true;
    if (!target->submitHostPrompt(text, m_quickAddImages))
        return true;
    if (!text.isEmpty())
        recordQuickAddHistory(text);
    m_issueQuickAdd->clear();
    clearQuickAddImages();
    return true;
}

namespace {

void bringWindowForward(QWidget *window)
{
    window->setWindowState(window->windowState() & ~Qt::WindowMinimized);
    window->show();
    window->raise();
    window->activateWindow();
}

QString nativePackageWindowGeometryKey(const QString &id)
{
    return QStringLiteral("market/windows/%1/geometry").arg(id);
}

} // namespace

void MainWindow::showNativePackage(const QString &id)
{
    for (NativePackagePage &page : m_nativePackagePages) {
        if (page.id != id || !page.widget)
            continue;
        if (page.navigation)
            page.navigation->setChecked(true);
        if (page.window && page.placeholder) {
            m_sectionStack->setCurrentWidget(page.placeholder);
            bringWindowForward(page.window);
        } else {
            m_sectionStack->setCurrentWidget(page.widget);
        }
        page.searchPrepared = true;
        page.widget->refresh();
        scheduleNavRecord();
        updateBreadcrumb();
        return;
    }
}

void MainWindow::openNativePackageWindow(const QString &id)
{
    if (!m_sectionStack)
        return;
    for (NativePackagePage &page : m_nativePackagePages) {
        if (page.id != id || !page.widget)
            continue;
        if (page.window) {
            bringWindowForward(page.window);
            return;
        }
        const int index = m_sectionStack->indexOf(page.widget);
        if (index < 0)
            return;
        const forkmesh::market::PackageManifest *manifest =
            m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
        const QString name =
            manifest && !manifest->name.isEmpty() ? manifest->name : id;
        const bool wasCurrent = m_sectionStack->currentWidget() == page.widget;
        const QSize size = page.widget->size();

        auto *placeholder = new QWidget;
        placeholder->setObjectName(
            QStringLiteral("nativePackagePlaceholder-%1").arg(id));
        placeholder->setProperty("forkmeshPackageId", id);
        auto *placeholderLayout = new QVBoxLayout(placeholder);
        placeholderLayout->addStretch(1);
        auto *message = new QLabel(
            QStringLiteral("%1 is open in its own window.").arg(name),
            placeholder);
        message->setAlignment(Qt::AlignCenter);
        placeholderLayout->addWidget(message);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch(1);
        auto *showWindow =
            new QPushButton(QStringLiteral("Show window"), placeholder);
        showWindow->setObjectName(QStringLiteral("nativePackageShowWindow"));
        showWindow->setCursor(Qt::PointingHandCursor);
        connect(showWindow, &QPushButton::clicked, this,
                [this, id] { openNativePackageWindow(id); });
        buttons->addWidget(showWindow);
        auto *returnHere = new QPushButton(
            QStringLiteral("Return to main window"), placeholder);
        returnHere->setObjectName(QStringLiteral("nativePackageReturnWindow"));
        returnHere->setCursor(Qt::PointingHandCursor);
        connect(returnHere, &QPushButton::clicked, this,
                [this, id] { dockNativePackageWindow(id); });
        buttons->addWidget(returnHere);
        buttons->addStretch(1);
        placeholderLayout->addLayout(buttons);
        placeholderLayout->addStretch(1);

        m_sectionStack->insertWidget(index, placeholder);
        if (wasCurrent)
            m_sectionStack->setCurrentWidget(placeholder);
        m_sectionStack->removeWidget(page.widget);

        auto *window = new QWidget(this, Qt::Window);
        window->setObjectName(QStringLiteral("nativePackageWindow-%1").arg(id));
        window->setAttribute(Qt::WA_StyledBackground, true);
        window->setWindowTitle(QStringLiteral("%1 - ForkMesh").arg(name));
        auto *windowLayout = new QVBoxLayout(window);
        windowLayout->setContentsMargins(0, 0, 0, 0);
        windowLayout->setSpacing(0);
        auto *header = new QWidget(window);
        header->setObjectName(QStringLiteral("nativePackageWindowHeader"));
        auto *headerRow = new QHBoxLayout(header);
        headerRow->setContentsMargins(8, 4, 8, 0);
        headerRow->addStretch(1);
        QPushButton *actions =
            createPackageActionsButton([id] { return id; }, header);
        actions->setToolTip(
            QStringLiteral("%1 options: rate it, send feedback or report a "
                           "bug, or uninstall it")
                .arg(name));
        actions->setAccessibleName(QStringLiteral("%1 options").arg(name));
        headerRow->addWidget(actions);
        windowLayout->addWidget(header);
        windowLayout->addWidget(page.widget, 1);
        page.widget->show();
        window->installEventFilter(this);
        if (!window->restoreGeometry(
                QSettings().value(nativePackageWindowGeometryKey(id))
                    .toByteArray()))
            window->resize(size.isValid() && !size.isEmpty() ? size
                                                             : QSize(1100, 760));
        page.window = window;
        page.placeholder = placeholder;
        page.searchPrepared = true;
        bringWindowForward(window);
        page.widget->refresh();
        return;
    }
}

void MainWindow::dockNativePackageWindow(const QString &id)
{
    for (NativePackagePage &page : m_nativePackagePages) {
        if (page.id != id || !page.window)
            continue;
        QWidget *window = page.window;
        QWidget *placeholder = page.placeholder;
        page.window.clear();
        page.placeholder.clear();
        QSettings().setValue(nativePackageWindowGeometryKey(id),
                             window->saveGeometry());
        window->removeEventFilter(this);
        const int index =
            placeholder && m_sectionStack ? m_sectionStack->indexOf(placeholder)
                                          : -1;
        if (page.widget && index >= 0) {
            const bool wasCurrent =
                m_sectionStack->currentWidget() == placeholder;
            window->layout()->removeWidget(page.widget);
            m_sectionStack->insertWidget(index, page.widget);
            if (wasCurrent)
                m_sectionStack->setCurrentWidget(page.widget);
        }
        if (placeholder) {
            if (m_sectionStack)
                m_sectionStack->removeWidget(placeholder);
            placeholder->deleteLater();
        }
        window->hide();
        window->deleteLater();
        return;
    }
}

void MainWindow::requestNativePackage(
    const QString &id,
    const forkmesh::market::NativePackageContribution &contribution,
    const QByteArray &requestedMethod, const QString &path,
    const QJsonObject &body,
    forkmesh::market::NativePackageReplyHandler handler,
    int attempt)
{
    if (!m_networkAccess) {
        handler(false, {}, QStringLiteral("Package network access is unavailable."));
        return;
    }
    const QByteArray method = requestedMethod.toUpper();
    if (method != QByteArrayLiteral("GET") &&
        method != QByteArrayLiteral("POST") &&
        method != QByteArrayLiteral("PATCH") &&
        method != QByteArrayLiteral("PUT") &&
        method != QByteArrayLiteral("DELETE")) {
        handler(false, {}, QStringLiteral("Unsupported package request method."));
        return;
    }
    if (!forkmesh::market::nativePackageRequestPathAllowed(
            id, contribution.apiPrefix, path)) {
        handler(false, {}, QStringLiteral("Invalid package request path."));
        return;
    }
    const QUrl relative(path);
    const QString cleanPath = relative.path(QUrl::FullyDecoded);
    const bool lifecycleApi =
        forkmesh::market::nativePackageRequestPathAllowed(
            id, QString(), path);

    QUrl url = catalogApiUrl();
    url.setPath(cleanPath);
    url.setQuery(relative.query());
    url.setFragment(QString());
    const bool hasBody = method != QByteArrayLiteral("GET");
    const QByteArray encoded = hasBody
        ? QJsonDocument(body).toJson(QJsonDocument::Compact) : QByteArray();
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    if (hasBody)
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));

    const QString proof = lifecycleApi ? forkmesh::ui::kPluginRequestProof
                                       : contribution.requestProof;
    if (!signForkMeshRequest(request, method, cleanPath, encoded, proof)) {
        handler(false, {}, kSignInForPackages);
        return;
    }

    QNetworkReply *reply = method == QByteArrayLiteral("GET")
        ? m_networkAccess->get(request)
        : method == QByteArrayLiteral("POST")
            ? m_networkAccess->post(request, encoded)
            : m_networkAccess->sendCustomRequest(request, method, encoded);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, id, contribution, method, path, body, attempt,
             host = url.host(), handler = std::move(handler)]() mutable {
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError &&
                        status >= 200 && status < 300 &&
                        payload.value(QStringLiteral("ok")).toBool(true);
        const bool suppressed =
            BackoffNetworkAccessManager::isBackoffSuppressed(reply);
        const QString replyError = reply->errorString();
        reply->deleteLater();
        const bool relayBusy = !ok &&
            (suppressed || status == 429 || status == 502 || status == 503 ||
             status == 504);
        if (!relayBusy) {
            const QString error = ok ? QString() : nativePackageErrorText(
                payload, status ? QStringLiteral("HTTP %1").arg(status)
                                : replyError);
            handler(ok, payload, error);
            return;
        }
        const auto *network =
            qobject_cast<BackoffNetworkAccessManager *>(m_networkAccess);
        const qint64 waitMs =
            network ? network->hostCooldownRemainingMs(host) : 0;
        constexpr int kMaxRelayRetries = 4;
        constexpr qint64 kMaxRelayWaitMs = 90 * 1000;
        if ((suppressed || method == QByteArrayLiteral("GET")) &&
            attempt < kMaxRelayRetries && waitMs <= kMaxRelayWaitMs) {
            QTimer::singleShot(
                int(qMax<qint64>(1000 * (attempt + 1), waitMs + 500)), this,
                [this, id, contribution, method, path, body, attempt,
                 handler = std::move(handler)]() mutable {
                    requestNativePackage(id, contribution, method, path, body,
                                         std::move(handler), attempt + 1);
                });
            return;
        }
        const QString cause = status == 429 ||
                                      replyError.contains(QLatin1String("429"))
            ? QStringLiteral("rate-limited")
            : QStringLiteral("restarting or unreachable");
        handler(false, payload, nativePackageErrorText(
            payload,
            QStringLiteral("the ForkMesh relay is %1; try again in %2 s")
                .arg(cause)
                .arg(qMax<qint64>(1, (waitMs + 999) / 1000))));
    });
}

bool MainWindow::signForkMeshRequest(QNetworkRequest &request,
                                     const QByteArray &method,
                                     const QString &cleanPath,
                                     const QByteArray &encoded,
                                     const QString &proof) const
{
    QUrl url = request.url();
    const QString token = accountSessionTokenForUrl(url);
    if (!token.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("Authorization"),
                             QByteArrayLiteral("Bearer ") + token.toUtf8());
        return true;
    }
    QString resource = QString::fromLatin1(method) + QLatin1Char('\n') +
                       cleanPath;
    if (method != QByteArrayLiteral("GET")) {
        resource += QLatin1Char('\n') + QString::fromLatin1(
            QCryptographicHash::hash(encoded, QCryptographicHash::Sha256)
                .toHex());
    }
    QString node;
    QString timestamp;
    QString signature;
    if (proof.isEmpty() ||
        !signOrgTaskProof(proof, resource, &node, &timestamp, &signature))
        return false;
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("node"), node);
    query.addQueryItem(QStringLiteral("ts"), timestamp);
    query.addQueryItem(QStringLiteral("sig"), signature);
    url.setQuery(query);
    request.setUrl(url);
    return true;
}

void MainWindow::sendSignedForkMeshRequest(
    const QByteArray &method, const QString &path, const QJsonObject &body,
    const QString &proof, forkmesh::market::NativePackageReplyHandler handler)
{
    if (!m_networkAccess) {
        handler(false, {}, QStringLiteral("Network access is unavailable."));
        return;
    }
    const QUrl relative(path);
    const QString cleanPath = relative.path(QUrl::FullyDecoded);
    QUrl url = catalogApiUrl();
    if (!url.isValid() || url.host().isEmpty() ||
        !cleanPath.startsWith(QLatin1Char('/'))) {
        handler(false, {}, QStringLiteral("No ForkMesh server is set up."));
        return;
    }
    url.setPath(cleanPath);
    url.setQuery(relative.query());
    url.setFragment(QString());
    const bool hasBody = method != QByteArrayLiteral("GET");
    const QByteArray encoded = hasBody
        ? QJsonDocument(body).toJson(QJsonDocument::Compact) : QByteArray();
    QNetworkRequest request(url);
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    if (hasBody)
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
    if (!signForkMeshRequest(request, method, cleanPath, encoded, proof)) {
        handler(false, {}, kSignInForPackages);
        return;
    }
    QNetworkReply *reply = method == QByteArrayLiteral("GET")
        ? m_networkAccess->get(request)
        : method == QByteArrayLiteral("POST")
            ? m_networkAccess->post(request, encoded)
            : m_networkAccess->sendCustomRequest(request, method, encoded);
    connect(reply, &QNetworkReply::finished, this,
            [reply, handler = std::move(handler)] {
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError &&
                        status >= 200 && status < 300 &&
                        payload.value(QStringLiteral("ok")).toBool(true);
        const QString replyError = reply->errorString();
        reply->deleteLater();
        handler(ok, payload,
                ok ? QString()
                   : nativePackageErrorText(
                         payload, status ? QStringLiteral("HTTP %1").arg(status)
                                         : replyError));
    });
}

void MainWindow::refreshAccountPackages(bool force)
{
    if (!m_packageRegistry || !m_packageRegistry->isReady() ||
        m_accountPackagesInFlight)
        return;
    bool synced = false;
    for (const PackageManifest &manifest : m_packageRegistry->packages())
        synced = synced || manifest.accountSync;
    if (!synced)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 gap =
        force ? kAccountPackagesForcedGapMs : kAccountPackagesReadGapMs;
    if (m_accountPackagesReadMs > 0 && now - m_accountPackagesReadMs < gap)
        return;
    m_accountPackagesReadMs = now;
    m_accountPackagesInFlight = true;
    const int generation = m_accountPackagesGeneration;
    sendSignedForkMeshRequest(
        QByteArrayLiteral("GET"), QStringLiteral("/api/plugins/@account"), {},
        forkmesh::ui::kPluginRequestProof,
        [this, generation](bool ok, const QJsonObject &payload,
                           const QString &) {
            m_accountPackagesInFlight = false;
            if (!ok || generation != m_accountPackagesGeneration)
                return;
            applyAccountPackages(
                payload.value(QStringLiteral("packages")).toArray());
        });
}

void MainWindow::applyAccountPackages(const QJsonArray &packages)
{
    if (!m_packageRegistry || !m_packageRegistry->isReady() ||
        packages.isEmpty())
        return;
    bool installChanged = false;
    bool placesChanged = false;
    QSet<QString> settingsChanged;
    QStringList carried;
    m_applyingAccountPackages = true;
    for (const QJsonValue &value : packages) {
        const QJsonObject entry = value.toObject();
        const QString id = entry.value(QStringLiteral("id")).toString();
        const PackageManifest *manifest = m_packageRegistry->package(id);
        const QJsonValue installedValue =
            entry.value(QStringLiteral("installed"));
        if (!manifest || !manifest->accountSync || !installedValue.isBool())
            continue;
        const bool unchosen = entry.value(QStringLiteral("unchosen")).toBool();
        const bool installed = m_packageRegistry->accountAnswerInstalls(
            *manifest, installedValue.toBool(),
            entry.value(QStringLiteral("legacy")).toBool(), unchosen);
        if (unchosen && installed && !installedValue.toBool())
            carried.append(id);
        const bool before = m_packageRegistry->isInstalled(id);
        if (m_packageRegistry->setInstalledState(id, installed) &&
            before != installed)
            installChanged = true;
        QStringList places;
        for (const QJsonValue &place :
             entry.value(QStringLiteral("places")).toArray())
            places.append(place.toString());
        if (m_packageRegistry->setPlaces(id, places))
            placesChanged = true;
        if (!installed)
            continue;
        const QJsonObject settings =
            entry.value(QStringLiteral("settings")).toObject();
        for (const forkmesh::market::PackageSettingField &field :
             manifest->settings) {
            const QVariant incoming = settings.value(field.key).toVariant();
            if (!field.accepts(incoming) ||
                m_packageRegistry->declaredSetting(id, field.key) == incoming)
                continue;
            if (m_packageRegistry->setPackageSetting(id, field.key, incoming))
                settingsChanged.insert(id);
        }
    }
    m_applyingAccountPackages = false;
    for (const QString &id : carried)
        pushAccountPackageChange(id, QStringLiteral("install"));
    if (installChanged || placesChanged) {
        syncPackageRuntime();
        if (installChanged)
            reportPackageInstallCounts();
        refreshMarketPage();
    } else if (!settingsChanged.isEmpty()) {
        publishFooterWebsiteStatuses();
        if (settingsChanged.contains(m_marketSettingsPackageId))
            refreshMarketPage();
    }
    if (m_packageRegistry->isEffectivelyInstalled(kStatusPagePackageId))
        refreshStatusMonitors();
}

void MainWindow::pushAccountPackageChange(const QString &id,
                                          const QString &operation,
                                          const QJsonObject &body)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    if (!manifest || !manifest->accountSync || m_applyingAccountPackages)
        return;
    const int generation = ++m_accountPackagesGeneration;
    const QString name = manifest->name;
    sendSignedForkMeshRequest(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/plugins/@account/%1/%2").arg(id, operation), body,
        forkmesh::ui::kPluginRequestProof,
        [this, generation, name](bool ok, const QJsonObject &payload,
                                 const QString &error) {
            if (!ok) {
                if (error != kSignInForPackages) {
                    flashMessage(
                        QStringLiteral("%1 changed on this desktop, but your "
                                       "account could not be updated: %2")
                            .arg(name, error),
                        true);
                }
                return;
            }
            m_accountPackagesReadMs = QDateTime::currentMSecsSinceEpoch();
            if (generation == m_accountPackagesGeneration)
                applyAccountPackages(
                    payload.value(QStringLiteral("packages")).toArray());
        });
}

void MainWindow::setDeclaredPackageSetting(const QString &id,
                                           const QString &key,
                                           const QVariant &value)
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    const forkmesh::market::PackageSettingField *field =
        manifest ? manifest->setting(key) : nullptr;
    if (!field || !field->accepts(value) ||
        m_packageRegistry->declaredSetting(id, key) == value)
        return;
    QString error;
    if (!m_packageRegistry->setPackageSetting(id, key, value, &error)) {
        flashMessage(error, true);
        return;
    }
    publishFooterWebsiteStatuses();
    pushAccountPackageChange(
        id, QStringLiteral("settings"),
        QJsonObject{{QStringLiteral("settings"),
                     QJsonObject{{key, QJsonValue::fromVariant(value)}}}});
}

void MainWindow::appendDeclaredPackageSettings(QVBoxLayout *layout,
                                               const PackageManifest &manifest,
                                               bool installed)
{
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    for (const forkmesh::market::PackageSettingField &field :
         manifest.settings) {
        const QVariant current =
            m_packageRegistry->declaredSetting(manifest.id, field.key);
        const QString objectName =
            QStringLiteral("marketPackageSetting-%1").arg(field.key);
        if (field.type == QLatin1String("boolean")) {
            auto *box = new QCheckBox(field.label);
            box->setObjectName(objectName);
            box->setChecked(current.toBool());
            box->setEnabled(installed);
            connect(box, &QCheckBox::toggled, this,
                    [this, id = manifest.id, key = field.key](bool checked) {
                        setDeclaredPackageSetting(id, key, checked);
                    });
            form->addRow(box);
            continue;
        }
        auto *choice = new QComboBox;
        choice->setObjectName(objectName);
        choice->setAccessibleName(field.label);
        for (const forkmesh::market::PackageSettingOption &option :
             field.options)
            choice->addItem(option.label, option.value);
        choice->setCurrentIndex(qMax(0, choice->findData(current.toString())));
        choice->setEnabled(installed);
        connect(choice, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this, id = manifest.id, key = field.key, choice](int) {
                    setDeclaredPackageSetting(id, key,
                                              choice->currentData().toString());
                });
        form->addRow(field.label, choice);
    }
    layout->addLayout(form);
    if (!installed) {
        auto *hint = new QLabel(
            QStringLiteral("Install %1 to change these settings.")
                .arg(manifest.name));
        hint->setObjectName(QStringLiteral("mutedLabel"));
        hint->setWordWrap(true);
        layout->addWidget(hint);
    }
}

void MainWindow::appendStatusMonitorSettings(QVBoxLayout *layout)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("statusMonitorSettings"));
    auto *section = new QVBoxLayout(frame);
    section->setContentsMargins(0, 6, 0, 6);
    section->setSpacing(8);
    auto *title = new QLabel(QStringLiteral("<b>Your monitors</b>"));
    title->setTextFormat(Qt::RichText);
    section->addWidget(title);
    auto *intro = new QLabel(QStringLiteral(
        "Each of your devices loads these addresses itself and shows a dot for "
        "each one: green when it answers, red when it does not."));
    intro->setObjectName(QStringLiteral("mutedLabel"));
    intro->setWordWrap(true);
    section->addWidget(intro);

    if (m_statusCustomMonitors.isEmpty()) {
        auto *empty = new QLabel(
            m_statusMonitorsInFlight && m_statusMonitorsReadMs <= 0
                ? QStringLiteral("Loading your monitors…")
                : QStringLiteral("No monitors yet."));
        empty->setObjectName(QStringLiteral("statusMonitorsEmpty"));
        section->addWidget(empty);
    }
    for (const StatusCustomMonitor &monitor :
         std::as_const(m_statusCustomMonitors)) {
        auto *row = new QHBoxLayout;
        auto *name = new QLabel(monitor.name);
        name->setTextFormat(Qt::PlainText);
        name->setObjectName(QStringLiteral("statusMonitorName"));
        QFont bold = name->font();
        bold.setBold(true);
        name->setFont(bold);
        row->addWidget(name);
        auto *address = new QLabel(monitor.url);
        address->setTextFormat(Qt::PlainText);
        address->setObjectName(QStringLiteral("mutedLabel"));
        address->setTextInteractionFlags(Qt::TextSelectableByMouse);
        row->addWidget(address, 1);
        auto *remove = new QPushButton(QStringLiteral("Remove"));
        remove->setObjectName(
            QStringLiteral("statusMonitorRemove-%1").arg(monitor.id));
        remove->setAccessibleName(
            QStringLiteral("Remove the %1 monitor").arg(monitor.name));
        remove->setEnabled(!m_statusMonitorsBusy);
        connect(remove, &QPushButton::clicked, this,
                [this, id = monitor.id] { removeStatusMonitor(id); });
        row->addWidget(remove);
        section->addLayout(row);
    }

    auto *error = new QLabel(m_statusMonitorsError);
    error->setObjectName(QStringLiteral("statusMonitorsError"));
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    error->setStyleSheet(QStringLiteral("color: #f85149;"));
    error->setVisible(!m_statusMonitorsError.isEmpty());
    section->addWidget(error);

    auto *add = new QHBoxLayout;
    auto *name = new QLineEdit(m_statusMonitorDraftName);
    name->setObjectName(QStringLiteral("statusMonitorNameInput"));
    name->setPlaceholderText(QStringLiteral("Name"));
    name->setAccessibleName(QStringLiteral("New monitor name"));
    name->setMaxLength(40);
    name->setFixedWidth(160);
    connect(name, &QLineEdit::textChanged, this,
            [this](const QString &text) { m_statusMonitorDraftName = text; });
    add->addWidget(name);
    auto *address = new QLineEdit(m_statusMonitorDraftUrl);
    address->setObjectName(QStringLiteral("statusMonitorUrlInput"));
    address->setPlaceholderText(QStringLiteral("https://example.com/"));
    address->setAccessibleName(QStringLiteral("New monitor address"));
    address->setMaxLength(300);
    connect(address, &QLineEdit::textChanged, this,
            [this](const QString &text) { m_statusMonitorDraftUrl = text; });
    add->addWidget(address, 1);
    auto *addButton = new QPushButton(
        m_statusMonitorsBusy ? QStringLiteral("Saving…")
                             : QStringLiteral("Add"));
    addButton->setObjectName(QStringLiteral("statusMonitorAdd"));
    addButton->setAccessibleName(QStringLiteral("Add monitor"));
    const bool full = m_statusCustomMonitors.size() >= m_statusCustomMonitorsMax;
    addButton->setEnabled(!m_statusMonitorsBusy && !full);
    if (full) {
        addButton->setToolTip(
            QStringLiteral("You can keep up to %1 monitors.")
                .arg(m_statusCustomMonitorsMax));
    }
    const auto submit = [this, name, address] {
        if (m_statusMonitorsBusy)
            return;
        addStatusMonitor(name->text().trimmed(), address->text().trimmed());
    };
    connect(addButton, &QPushButton::clicked, this, submit);
    connect(address, &QLineEdit::returnPressed, this, submit);
    add->addWidget(addButton);
    section->addLayout(add);
    layout->addWidget(frame);
}

void MainWindow::loadCachedStatusMonitors()
{
    m_statusCustomMonitorsLoaded = true;
    m_statusCustomMonitors.clear();
    if (!m_packageRegistry)
        return;
    const QJsonArray cached =
        QJsonDocument::fromJson(
            m_packageRegistry
                ->packageSetting(kStatusPagePackageId,
                                 QStringLiteral("customMonitors"))
                .toString()
                .toUtf8())
            .array();
    for (const QJsonValue &value : cached) {
        const QJsonObject object = value.toObject();
        const StatusCustomMonitor monitor{
            object.value(QStringLiteral("id")).toString(),
            object.value(QStringLiteral("name")).toString().trimmed(),
            object.value(QStringLiteral("url")).toString().trimmed()};
        if (validStatusMonitorId(monitor.id) && !monitor.name.isEmpty() &&
            validStatusMonitorUrl(monitor.url))
            m_statusCustomMonitors.append(monitor);
    }
}

void MainWindow::refreshStatusMonitors(bool force)
{
    if (!m_packageRegistry ||
        !m_packageRegistry->isEffectivelyInstalled(kStatusPagePackageId) ||
        m_statusMonitorsInFlight)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 gap =
        force ? kAccountPackagesForcedGapMs : kAccountPackagesReadGapMs;
    if (m_statusMonitorsReadMs > 0 && now - m_statusMonitorsReadMs < gap)
        return;
    m_statusMonitorsInFlight = true;
    sendSignedForkMeshRequest(
        QByteArrayLiteral("GET"), QStringLiteral("/api/status-monitors"), {},
        kStatusMonitorsRequestProof,
        [this](bool ok, const QJsonObject &payload, const QString &) {
            m_statusMonitorsInFlight = false;
            m_statusMonitorsReadMs = QDateTime::currentMSecsSinceEpoch();
            if (ok)
                applyStatusMonitorsPayload(payload);
            else if (m_marketSettingsPackageId == kStatusPagePackageId)
                refreshMarketPage();
        });
}

void MainWindow::applyStatusMonitorsPayload(const QJsonObject &payload)
{
    if (!m_packageRegistry ||
        !m_packageRegistry->isEffectivelyInstalled(kStatusPagePackageId))
        return;
    const QJsonValue monitorsValue = payload.value(QStringLiteral("monitors"));
    if (!monitorsValue.isArray())
        return;
    const int max = payload.value(QStringLiteral("max")).toInt();
    if (max > 0)
        m_statusCustomMonitorsMax = max;
    QList<StatusCustomMonitor> monitors;
    QJsonArray cache;
    for (const QJsonValue &value : monitorsValue.toArray()) {
        const QJsonObject object = value.toObject();
        const StatusCustomMonitor monitor{
            object.value(QStringLiteral("id")).toString(),
            object.value(QStringLiteral("name")).toString().trimmed(),
            object.value(QStringLiteral("url")).toString().trimmed()};
        if (!validStatusMonitorId(monitor.id) || monitor.name.isEmpty() ||
            !validStatusMonitorUrl(monitor.url))
            continue;
        monitors.append(monitor);
        cache.append(QJsonObject{{QStringLiteral("id"), monitor.id},
                                 {QStringLiteral("name"), monitor.name},
                                 {QStringLiteral("url"), monitor.url}});
    }
    QStringList fresh;
    for (const StatusCustomMonitor &monitor : std::as_const(monitors)) {
        bool known = false;
        for (const StatusCustomMonitor &existing :
             std::as_const(m_statusCustomMonitors))
            known = known || (existing.id == monitor.id &&
                              existing.url == monitor.url);
        if (!known)
            fresh << monitor.id;
    }
    m_statusCustomMonitors = monitors;
    m_statusCustomMonitorsLoaded = true;
    m_footerCustomStatuses.removeIf([&monitors](const FooterStatusRow &row) {
        for (const StatusCustomMonitor &monitor : monitors) {
            if (row.id == QStringLiteral("custom:") + monitor.id)
                return false;
        }
        return true;
    });
    for (const QString &id : std::as_const(fresh)) {
        m_footerCustomStatuses.removeIf([&id](const FooterStatusRow &row) {
            return row.id == QStringLiteral("custom:") + id;
        });
    }
    m_packageRegistry->setPackageSetting(
        kStatusPagePackageId, QStringLiteral("customMonitors"),
        QString::fromUtf8(QJsonDocument(cache).toJson(QJsonDocument::Compact)));
    publishFooterWebsiteStatuses();
    for (const QString &id : std::as_const(fresh))
        probeCustomStatusMonitor(id);
    if (m_marketSettingsPackageId == kStatusPagePackageId)
        refreshMarketPage();
}

void MainWindow::addStatusMonitor(const QString &name, const QString &url)
{
    if (name.isEmpty() || name.size() > 40) {
        m_statusMonitorsError = statusMonitorErrorText(
            QJsonObject{{QStringLiteral("error"), QStringLiteral("invalid_name")}},
            QString());
        refreshMarketPage();
        return;
    }
    if (!validStatusMonitorUrl(url)) {
        m_statusMonitorsError = statusMonitorErrorText(
            QJsonObject{{QStringLiteral("error"), QStringLiteral("invalid_url")}},
            QString());
        refreshMarketPage();
        return;
    }
    m_statusMonitorsBusy = true;
    m_statusMonitorsError.clear();
    refreshMarketPage();
    sendSignedForkMeshRequest(
        QByteArrayLiteral("POST"),
        QStringLiteral("/api/status-monitors/monitors"),
        QJsonObject{{QStringLiteral("name"), name},
                    {QStringLiteral("url"), url}},
        kStatusMonitorsRequestProof,
        [this](bool ok, const QJsonObject &payload, const QString &error) {
            if (ok) {
                m_statusMonitorDraftName.clear();
                m_statusMonitorDraftUrl.clear();
            }
            finishStatusMonitorWrite(ok, payload, error);
        });
}

void MainWindow::removeStatusMonitor(const QString &monitorId)
{
    if (!validStatusMonitorId(monitorId) || m_statusMonitorsBusy)
        return;
    m_statusMonitorsBusy = true;
    m_statusMonitorsError.clear();
    refreshMarketPage();
    sendSignedForkMeshRequest(
        QByteArrayLiteral("DELETE"),
        QStringLiteral("/api/status-monitors/monitors/%1").arg(monitorId), {},
        kStatusMonitorsRequestProof,
        [this](bool ok, const QJsonObject &payload, const QString &error) {
            finishStatusMonitorWrite(ok, payload, error);
        });
}

void MainWindow::finishStatusMonitorWrite(bool ok, const QJsonObject &payload,
                                          const QString &error)
{
    m_statusMonitorsBusy = false;
    if (ok) {
        m_statusMonitorsError.clear();
        m_statusMonitorsReadMs = QDateTime::currentMSecsSinceEpoch();
        applyStatusMonitorsPayload(payload);
    } else {
        m_statusMonitorsError = statusMonitorErrorText(payload, error);
        if (payload.value(QStringLiteral("monitors")).isArray())
            applyStatusMonitorsPayload(payload);
    }
    if (m_marketSettingsPackageId == kStatusPagePackageId)
        refreshMarketPage();
}

void MainWindow::captureScreenRegion()
{
    if (!m_captureFeature) {
        flashMessage(QStringLiteral(
            "Capture is not installed. Install it from the Market list."),
            true);
        return;
    }
    m_captureFeature->start();
}

void MainWindow::openLedger()
{
    if (!m_ledgerFeature) {
        flashMessage(QStringLiteral(
            "Ledger is not installed. Install it from Market → Library."),
                     true);
        return;
    }
    showSection(kLedgerSectionIndex);
}

QString MainWindow::packageStatsRunId() const
{
    // A rotating random id, not an identity: it lets the relay count one
    // client once per package without ever learning who that client is, and it
    // is replaced monthly so it cannot become a long-lived tracker. Nothing
    // derived from the account, node name, or hardware goes into it.
    static constexpr qint64 kRotationMs = qint64(30) * 24 * 60 * 60 * 1000;
    QSettings settings;
    const QString stored =
        settings.value(QStringLiteral("market/statsRun")).toString();
    const qint64 mintedAt =
        settings.value(QStringLiteral("market/statsRunAt"), 0).toLongLong();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (stored.size() == 32 && mintedAt > 0 && now - mintedAt < kRotationMs)
        return stored;

    QString minted;
    minted.reserve(32);
    for (int i = 0; i < 4; ++i) {
        minted += QStringLiteral("%1").arg(
            QRandomGenerator::system()->generate(), 8, 16, QLatin1Char('0'));
    }
    settings.setValue(QStringLiteral("market/statsRun"), minted);
    settings.setValue(QStringLiteral("market/statsRunAt"), now);
    return minted;
}

void MainWindow::refreshPackageInstallCounts()
{
#ifdef FORKMESH_WINDOW_TESTS
    // Window tests drive the shelf offline: counts arrive through
    // testSetPackageInstallCounts() so a card's popularity line never depends
    // on a live relay. The request below still compiles here so it cannot rot.
    return;
#endif
    if (m_packageInstallCountsPending || !m_networkAccess)
        return;
    // refreshMarketPage() runs on every install, removal and settings close,
    // so the counts are fetched on a timer rather than once per repaint.
    static constexpr qint64 kCountsFreshMs = 10 * 60 * 1000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_packageInstallCountsFetchedMs > 0 &&
        now - m_packageInstallCountsFetchedMs < kCountsFreshMs)
        return;
    const QString origin = activeRelayOrigin();
    if (origin.isEmpty())
        return;
    QUrl url(origin + QStringLiteral("/api/market/installs"));
    if (!url.isValid())
        return;

    // Stamped on dispatch, not on success, so a relay that is down cannot turn
    // every card repaint into another request.
    m_packageInstallCountsFetchedMs = now;
    m_packageInstallCountsPending = true;
    QNetworkRequest request(url);
    request.setRawHeader("accept", "application/json");
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_packageInstallCountsPending = false;
        if (reply->error() != QNetworkReply::NoError)
            return;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject marketplaces =
            payload.value(QStringLiteral("marketplaces")).toObject();
        const QJsonObject legacyPackages =
            payload.value(QStringLiteral("packages")).toObject();
        QHash<QString, QPair<qint64, qint64>> counts;
        for (const PackageManifest &manifest : m_packageRegistry->packages()) {
            QJsonObject packages =
                marketplaces.value(manifest.marketplaceId).toObject();
            if (packages.isEmpty() &&
                manifest.marketplaceId == QLatin1String("forkmesh"))
                packages = legacyPackages;
            const QJsonObject entry = packages.value(manifest.id).toObject();
            const qint64 downloads = qMax(
                qint64(0),
                qint64(entry.value(QStringLiteral("downloads")).toDouble()));
            const qint64 installs = qMax(
                qint64(0),
                qint64(entry.value(QStringLiteral("installs")).toDouble()));
            if (downloads > 0 || installs > 0)
                counts.insert(manifest.id, qMakePair(downloads, installs));
        }
        if (counts == m_packageInstallCounts)
            return;
        m_packageInstallCounts = counts;
        // Repaint the shelf with the numbers that just arrived. The settings
        // page owns the list while it is open, so leave it alone.
        if (m_marketListLayout && m_marketSettingsPackageId.isEmpty())
            refreshMarketPage();
    });
}

void MainWindow::reportPackageInstallCounts(bool throttled)
{
#ifdef FORKMESH_WINDOW_TESTS
    // Window tests install and remove packages constantly; none of that may
    // reach a relay or land in the public counts. The body below still
    // compiles here so it cannot rot.
    return;
#endif
    // Opt-out honoured: the same switch that governs node diagnostics governs
    // this. Nothing is sent when it is off.
    if (!QSettings()
             .value(TelemetrySettings::kReportDiagnostics, true)
             .toBool())
        return;
    if (!m_networkAccess || !m_packageRegistry || !m_packageRegistry->isReady())
        return;
    // Opening the Market reports the current set at most once a day, which is
    // what gets the packages installed by default counted at all: a client
    // that never toggles anything would otherwise never report. An actual
    // install or removal reports immediately. Re-sending an unchanged set is
    // a no-op on the relay, which only counts a not-installed -> installed
    // transition.
    static constexpr qint64 kReportIntervalMs = qint64(24) * 60 * 60 * 1000;
    const qint64 reportedAt =
        QSettings().value(QStringLiteral("market/statsReportedAt"), 0)
            .toLongLong();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (throttled && reportedAt > 0 && nowMs - reportedAt < kReportIntervalMs)
        return;
    const QString origin = activeRelayOrigin();
    if (origin.isEmpty())
        return;
    QUrl url(origin + QStringLiteral("/api/market/installs"));
    if (!url.isValid())
        return;

    QJsonArray packages;
    for (const PackageManifest &manifest : m_packageRegistry->packages()) {
        QJsonObject entry;
        entry.insert(QStringLiteral("market"), manifest.marketplaceId);
        entry.insert(QStringLiteral("id"), manifest.id);
        entry.insert(QStringLiteral("installed"),
                     m_packageRegistry->isInstalled(manifest.id));
        packages.append(entry);
    }
    if (packages.isEmpty())
        return;

    QJsonObject payload;
    payload.insert(QStringLiteral("run"), packageStatsRunId());
    payload.insert(QStringLiteral("packages"), packages);

    QSettings().setValue(QStringLiteral("market/statsReportedAt"), nowMs);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_networkAccess->post(
        request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    // Fire and forget: a failed count report must never surface as an error.
    connect(reply, &QNetworkReply::finished, reply, &QNetworkReply::deleteLater);
}

void MainWindow::refreshPackageMarketScores()
{
#ifdef FORKMESH_WINDOW_TESTS
    return;
#endif
    if (m_packageMarketScoresPending || !m_networkAccess)
        return;
    static constexpr qint64 kScoresFreshMs = 10 * 60 * 1000;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_packageMarketScoresFetchedMs > 0 &&
        now - m_packageMarketScoresFetchedMs < kScoresFreshMs)
        return;
    const QString origin = activeRelayOrigin();
    if (origin.isEmpty())
        return;
    const QUrl url(origin + QStringLiteral("/api/market/reviews/summary"));
    if (!url.isValid())
        return;
    m_packageMarketScoresFetchedMs = now;
    m_packageMarketScoresPending = true;
    QNetworkRequest request(url);
    request.setRawHeader("accept", "application/json");
    QNetworkReply *reply = m_networkAccess->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_packageMarketScoresPending = false;
        if (reply->error() != QNetworkReply::NoError || !m_packageRegistry)
            return;
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject ratings =
            payload.value(QStringLiteral("marketplaces")).toObject();
        const QJsonObject usage =
            payload.value(QStringLiteral("usageByMarketplace")).toObject();
        QHash<QString, MarketPackageScore> scores;
        for (const PackageManifest &manifest : m_packageRegistry->packages()) {
            const QJsonObject rating = ratings.value(manifest.marketplaceId)
                                           .toObject()
                                           .value(manifest.id)
                                           .toObject();
            const QJsonObject used = usage.value(manifest.marketplaceId)
                                         .toObject()
                                         .value(manifest.id)
                                         .toObject();
            MarketPackageScore score;
            score.ratingCount = qMax(
                qint64(0), qint64(rating.value(QStringLiteral("count")).toDouble()));
            score.ratingAverage =
                qBound(0.0, rating.value(QStringLiteral("average")).toDouble(), 5.0);
            score.people = qMax(
                qint64(0), qint64(used.value(QStringLiteral("people")).toDouble()));
            score.opens = qMax(
                qint64(0), qint64(used.value(QStringLiteral("opens")).toDouble()));
            score.views = qMax(
                qint64(0), qint64(used.value(QStringLiteral("views")).toDouble()));
            if (score.ratingCount > 0 || score.people > 0 || score.opens > 0 ||
                score.views > 0)
                scores.insert(manifest.id, score);
        }
        if (scores == m_packageMarketScores)
            return;
        m_packageMarketScores = scores;
        if (m_marketListLayout && m_marketSettingsPackageId.isEmpty())
            refreshMarketPage();
    });
}

#ifdef FORKMESH_WINDOW_TESTS
void MainWindow::testSetPackageMarketScores(
    const QHash<QString, MarketPackageScore> &scores)
{
    m_packageMarketScores = scores;
    refreshMarketPage();
}

void MainWindow::testSetPackageInstallCounts(
    const QHash<QString, QPair<qint64, qint64>> &counts)
{
    m_packageInstallCounts = counts;
    refreshMarketPage();
}

QStringList MainWindow::testPackageRailMenuActions(const QString &entrypoint)
{
    QStringList texts;
    std::unique_ptr<QMenu> menu(buildPackageRailMenu(entrypoint, this));
    if (!menu)
        return texts;
    for (QAction *action : menu->actions()) {
        if (!action->isSeparator())
            texts << action->text();
    }
    return texts;
}

QStringList MainWindow::testPackageActionsMenu(const QString &id)
{
    QStringList texts;
    std::unique_ptr<QMenu> menu(buildPackageActionsMenu(id, this));
    if (!menu)
        return texts;
    for (QAction *action : menu->actions()) {
        if (!action->isSeparator())
            texts << action->text() +
                         (action->isEnabled() ? QString()
                                              : QStringLiteral(" (disabled)"));
    }
    return texts;
}

void MainWindow::testTriggerPackageRailAction(const QString &entrypoint,
                                              const QString &text)
{
    std::unique_ptr<QMenu> menu(buildPackageRailMenu(entrypoint, this));
    if (!menu)
        return;
    for (QAction *action : menu->actions()) {
        if (action->text() == text) {
            action->trigger();
            return;
        }
    }
}

QString MainWindow::testPackageName(const QString &id) const
{
    const PackageManifest *manifest =
        m_packageRegistry ? m_packageRegistry->package(id) : nullptr;
    return manifest ? manifest->name : id;
}

QStringList MainWindow::testInstallAllPackages() const
{
    return m_packageRegistry ? m_packageRegistry->installAllPackages()
                             : QStringList();
}

bool MainWindow::testPackageInstalled(const QString &id) const
{
    return m_packageRegistry && m_packageRegistry->isInstalled(id);
}

bool MainWindow::testCapturePackageActionVisible() const
{
    return m_navScreenshotButton && !m_navScreenshotButton->isHidden() &&
           m_navScreenshotButton->isEnabled();
}

void MainWindow::testStartCapturePackage()
{
    captureScreenRegion();
}

bool MainWindow::testCapturePackageRuntimeActive() const
{
    return m_captureFeature && m_captureFeature->isActive();
}

bool MainWindow::testLedgerPackageActionVisible() const
{
    return m_ledgerNavButton && !m_ledgerNavButton->isHidden() &&
           m_ledgerNavButton->isEnabled();
}

bool MainWindow::testLedgerPackageRuntimeActive() const
{
    return bool(m_ledgerFeature);
}

QUrl MainWindow::testLedgerPackageUrl() const
{
    return m_ledgerFeature
               ? forkmesh::ledger::LedgerFeature::urlFor(catalogApiUrl())
               : QUrl();
}

bool MainWindow::testDiscordPackageActionVisible() const
{
    return m_discordConnectButton && !m_discordConnectButton->isHidden() &&
           m_discordConnectButton->isEnabled();
}

bool MainWindow::testDiscordPackageRuntimeActive() const
{
    return m_discordPackageActive;
}

bool MainWindow::testNativePackageActionVisible(const QString &id) const
{
    for (const NativePackagePage &page : m_nativePackagePages) {
        if (page.id == id)
            return page.navigation && !page.navigation->isHidden() &&
                   page.navigation->isEnabled();
    }
    return false;
}

bool MainWindow::testNativePackageRuntimeActive(const QString &id) const
{
    for (const NativePackagePage &page : m_nativePackagePages) {
        if (page.id == id)
            return bool(page.widget);
    }
    return false;
}

QStringList MainWindow::testNativePackageIds() const
{
    QStringList ids;
    if (!m_packageRegistry)
        return ids;
    for (const PackageManifest &manifest : m_packageRegistry->packages()) {
        if (forkmesh::market::nativePackageContribution(
                manifest.desktopEntrypoint, manifest.type))
            ids.append(manifest.id);
    }
    return ids;
}

void MainWindow::testShowNativePackage(const QString &id)
{
    showNativePackage(id);
}

QWidget *MainWindow::testNativePackageWindow(const QString &id) const
{
    for (const NativePackagePage &page : m_nativePackagePages) {
        if (page.id == id)
            return page.window;
    }
    return nullptr;
}
#endif
