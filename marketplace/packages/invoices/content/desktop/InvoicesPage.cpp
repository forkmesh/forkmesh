#include "InvoicesPage.h"
#include "InvoicesModel.h"

#include "DesignTokens.h"
#include "LayoutMode.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QSvgRenderer>
#include <QTableWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

namespace forkmesh::invoices {

namespace {

constexpr char kApiPrefix[] = "/api/invoices";
constexpr char kWebPath[] = "/dashboard/invoices";
constexpr int kRowHeight = 26;
constexpr int kIdRole = Qt::UserRole + 1;

const QString kDot = QString::fromUtf8(" \xC2\xB7 ");

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QIcon tintedIcon(const QString &name, const QColor &color, int side = 14)
{
    QString path = QStringLiteral(":/icons/octicons/%1.svg").arg(name);
    if (!QFile::exists(path))
        path = QStringLiteral(":/icons/codicons/%1.svg").arg(name);
    QSvgRenderer renderer(path);
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(side, side) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        if (renderer.isValid()) {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            renderer.render(&painter, QRectF(0, 0, side, side));
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(QRectF(0, 0, side, side), color);
        }
        icon.addPixmap(pixmap);
    }
    return icon;
}

QLabel *mutedLabel(const QString &value = QString())
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QLabel *headingLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("invoicesHeading"));
    QFont font = label->font();
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

QPushButton *ghostButton(const QString &label, const QString &icon = QString())
{
    auto *button = new QPushButton(label);
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setAccessibleName(label);
    button->setCursor(Qt::PointingHandCursor);
    if (!icon.isEmpty())
        button->setIcon(tintedIcon(icon, Theme::color(Theme::Role::FgMuted)));
    return button;
}

QPushButton *primaryButton(const QString &label)
{
    auto *button = new QPushButton(label);
    button->setObjectName(QStringLiteral("primaryButton"));
    button->setAccessibleName(label);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

Theme::Role statusInk(const QString &key)
{
    if (key == QLatin1String("sent"))
        return Theme::Role::AccentFg;
    if (key == QLatin1String("partial"))
        return Theme::Role::Warning;
    if (key == QLatin1String("overdue"))
        return Theme::Role::Danger;
    if (key == QLatin1String("paid"))
        return Theme::Role::Success;
    return Theme::Role::FgMuted;
}

Theme::Role statusFill(const QString &key)
{
    if (key == QLatin1String("sent"))
        return Theme::Role::AccentSubtle;
    if (key == QLatin1String("partial"))
        return Theme::Role::WarningSubtle;
    if (key == QLatin1String("overdue"))
        return Theme::Role::DangerSubtle;
    if (key == QLatin1String("paid"))
        return Theme::Role::SuccessSubtle;
    return Theme::Role::SurfaceActive;
}

QLabel *statusChip(const QString &key)
{
    auto *chip = new QLabel(statusLabel(key));
    chip->setObjectName(QStringLiteral("invoicesStatusChip"));
    chip->setStyleSheet(QStringLiteral("QLabel#invoicesStatusChip { color: %1; background: %2; border-radius: 4px;"
                                       " padding: 1px 6px; font-size: 11px; font-weight: 600; }")
                            .arg(Theme::hex(statusInk(key)), Theme::hex(statusFill(key))));
    chip->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return chip;
}

QTableWidgetItem *cell(const QString &value, Qt::Alignment align = Qt::AlignLeft | Qt::AlignVCenter)
{
    auto *item = new QTableWidgetItem(value);
    item->setTextAlignment(align);
    item->setToolTip(value);
    return item;
}

QFrame *metricBox(const QString &name, const QString &value, const QString &hint)
{
    auto *box = new QFrame;
    box->setObjectName(QStringLiteral("invoicesMetric"));
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(1);
    QLabel *label = mutedLabel(name);
    layout->addWidget(label);
    auto *number = new QLabel(value);
    number->setTextFormat(Qt::PlainText);
    number->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600;"));
    layout->addWidget(number);
    if (!hint.isEmpty())
        layout->addWidget(mutedLabel(hint));
    layout->addStretch(1);
    return box;
}

QDateEdit *dateEdit(const QDate &date)
{
    auto *edit = new QDateEdit(date.isValid() ? date : QDate::currentDate());
    edit->setCalendarPopup(true);
    edit->setDisplayFormat(QStringLiteral("d MMM yyyy"));
    return edit;
}

QPlainTextEdit *textBox(const QString &value, int rows, int limit = 0)
{
    auto *edit = new QPlainTextEdit(value);
    edit->setTabChangesFocus(true);
    edit->setFixedHeight(edit->fontMetrics().lineSpacing() * rows + 14);
    if (limit > 0)
        edit->setProperty("invoicesLimit", limit);
    return edit;
}

QLabel *dialogError()
{
    auto *label = new QLabel;
    label->setObjectName(QStringLiteral("errorLabel"));
    label->setWordWrap(true);
    label->hide();
    return label;
}

void showDialogError(QLabel *label, const QString &value)
{
    label->setText(value);
    label->setVisible(!value.isEmpty());
}

}

InvoicesPage::InvoicesPage(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_host(host), m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("invoicesSection"));
    const bool next = Theme::isNext();
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(next ? 24 : 16, 12, next ? 24 : 16, 12);
    outer->setSpacing(8);

    auto *heading = new QHBoxLayout;
    heading->setSpacing(6);
    auto *titleBlock = new QVBoxLayout;
    titleBlock->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("Invoices"));
    title->setObjectName(QStringLiteral("pageTitle"));
    titleBlock->addWidget(title);
    m_status = mutedLabel(QStringLiteral("Loading Invoicing…"));
    m_status->setObjectName(QStringLiteral("mutedLabel"));
    titleBlock->addWidget(m_status);
    heading->addLayout(titleBlock, 1);
    m_organization = new QComboBox;
    m_organization->setObjectName(QStringLiteral("invoicesOrganization"));
    m_organization->setAccessibleName(QStringLiteral("Organization"));
    m_organization->setMinimumWidth(170);
    m_organization->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    heading->addWidget(m_organization, 0, Qt::AlignTop);
    m_newButton = new QPushButton(QStringLiteral("New invoice"));
    m_newButton->setObjectName(QStringLiteral("primaryButton"));
    m_newButton->setAccessibleName(QStringLiteral("New invoice"));
    m_newButton->setCursor(Qt::PointingHandCursor);
    m_settingsButton = ghostButton(QStringLiteral("Settings"), QStringLiteral("gear"));
    m_refreshButton = ghostButton(QStringLiteral("Refresh"), QStringLiteral("sync"));
    m_webButton = ghostButton(QStringLiteral("Open on the web"), QStringLiteral("link-external"));
    m_webButton->setToolTip(QStringLiteral("Open the Invoices page for this organization in your browser"));
    for (QPushButton *button : {m_newButton, m_settingsButton, m_refreshButton, m_webButton})
        heading->addWidget(button, 0, Qt::AlignTop);
    outer->addLayout(heading);

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    m_installNote = new QLabel;
    m_installNote->setWordWrap(true);
    m_installNote->setAlignment(Qt::AlignHCenter);
    installLayout->addWidget(m_installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch(1);
    m_installButton = primaryButton(QStringLiteral("Install Invoicing"));
    installRow->addWidget(m_installButton);
    installRow->addStretch(1);
    installLayout->addLayout(installRow);
    installLayout->addStretch(1);
    outer->addWidget(m_installPanel, 1);

    m_content = new QWidget;
    auto *content = new QVBoxLayout(m_content);
    content->setContentsMargins(0, 0, 0, 0);
    content->setSpacing(10);
    auto *tiles = new QGridLayout;
    tiles->setHorizontalSpacing(10);
    const QStringList tileNames{QStringLiteral("Outstanding"), QStringLiteral("Overdue"),
                                QStringLiteral("Paid, last 30 days"), QStringLiteral("Open rate"),
                                QStringLiteral("Average days to pay")};
    for (int column = 0; column < tileNames.size(); ++column) {
        auto *tile = new QFrame;
        tile->setObjectName(QStringLiteral("invoicesTile"));
        auto *tileLayout = new QVBoxLayout(tile);
        tileLayout->setSpacing(2);
        QLabel *name = mutedLabel(tileNames.at(column));
        name->setWordWrap(false);
        auto *value = new QLabel(QString::fromUtf8("\xE2\x80\x94"));
        value->setObjectName(QStringLiteral("invoicesTileValue"));
        value->setAccessibleName(tileNames.at(column));
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QLabel *detail = mutedLabel();
        tileLayout->addWidget(name);
        tileLayout->addWidget(value);
        tileLayout->addWidget(detail);
        tileLayout->addStretch(1);
        tiles->addWidget(tile, 0, column);
        tiles->setColumnStretch(column, 1);
        m_tiles.append(tile);
        m_tileNames.append(name);
        m_tileValues.append(value);
        m_tileDetails.append(detail);
    }
    content->addLayout(tiles);

    m_listPanel = new QWidget;
    m_listPanel->setObjectName(QStringLiteral("invoicesListPanel"));
    auto *listLayout = new QVBoxLayout(m_listPanel);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(6);
    auto *filters = new QHBoxLayout;
    filters->setSpacing(6);
    m_search = new QLineEdit;
    m_search->setObjectName(QStringLiteral("invoicesSearch"));
    m_search->setPlaceholderText(QStringLiteral("Search number or client"));
    m_search->setAccessibleName(QStringLiteral("Search invoices"));
    m_search->setClearButtonEnabled(true);
    m_search->addAction(tintedIcon(QStringLiteral("search"), Theme::color(Theme::Role::FgMuted)),
                        QLineEdit::LeadingPosition);
    filters->addWidget(m_search, 1);
    m_filter = new QComboBox;
    m_filter->setObjectName(QStringLiteral("invoicesStatusFilter"));
    m_filter->setAccessibleName(QStringLiteral("Status"));
    for (const QString &key : filterKeys())
        m_filter->addItem(filterLabel(key), key);
    filters->addWidget(m_filter);
    listLayout->addLayout(filters);
    m_table = new QTableWidget(0, 6);
    m_table->setObjectName(QStringLiteral("invoicesTable"));
    m_table->setAccessibleName(QStringLiteral("Invoices"));
    m_table->setHorizontalHeaderLabels({QStringLiteral("Invoice"), QStringLiteral("Client"), QStringLiteral("Status"),
                                        QStringLiteral("Total"), QStringLiteral("Due"),
                                        QStringLiteral("Opens · views · clicks")});
    m_table->verticalHeader()->setVisible(false);
    m_table->verticalHeader()->setDefaultSectionSize(kRowHeight);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(false);
    m_table->setShowGrid(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setHighlightSections(false);
    listLayout->addWidget(m_table, 1);
    m_emptyNote = mutedLabel(QStringLiteral("No invoices match."));
    m_emptyNote->setAlignment(Qt::AlignHCenter);
    listLayout->addWidget(m_emptyNote);

    m_activityPanel = new QWidget;
    m_activityPanel->setObjectName(QStringLiteral("invoicesActivityPanel"));
    auto *activityLayout = new QVBoxLayout(m_activityPanel);
    activityLayout->setContentsMargins(0, 0, 0, 0);
    activityLayout->setSpacing(4);
    activityLayout->addWidget(headingLabel(QStringLiteral("Recent activity")));
    m_activity = new QListWidget;
    m_activity->setObjectName(QStringLiteral("invoicesActivity"));
    m_activity->setAccessibleName(QStringLiteral("Recent activity"));
    m_activity->setWordWrap(true);
    m_activity->setUniformItemSizes(false);
    activityLayout->addWidget(m_activity, 1);

    m_detailPanel = new QWidget;
    m_detailPanel->setObjectName(QStringLiteral("invoicesDetailPanel"));
    auto *detailLayout = new QVBoxLayout(m_detailPanel);
    detailLayout->setContentsMargins(0, 0, 0, 0);
    detailLayout->setSpacing(6);
    m_backButton = ghostButton(QStringLiteral("All invoices"), QStringLiteral("arrow-left"));
    m_backButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    detailLayout->addWidget(m_backButton);
    m_detailPlaceholder = mutedLabel(QStringLiteral("Select an invoice to see its totals, engagement and history."));
    m_detailPlaceholder->setAlignment(Qt::AlignCenter);
    detailLayout->addWidget(m_detailPlaceholder, 1);
    m_detailScroll = new QScrollArea;
    m_detailScroll->setWidgetResizable(true);
    m_detailScroll->setFrameShape(QFrame::NoFrame);
    detailLayout->addWidget(m_detailScroll, 1);
    m_detailActions = new QWidget;
    m_detailActions->setObjectName(QStringLiteral("invoicesDetailActions"));
    new QVBoxLayout(m_detailActions);
    m_detailActions->layout()->setContentsMargins(0, 0, 0, 0);
    m_detailActions->layout()->setSpacing(4);
    detailLayout->addWidget(m_detailActions);

    m_modeStack = new QStackedWidget;
    m_towerSplit = new QSplitter(Qt::Horizontal);
    m_towerSplit->setObjectName(QStringLiteral("invoicesTowerSplit"));
    m_towerSplit->setChildrenCollapsible(false);
    m_towerLeft = new QSplitter(Qt::Vertical);
    m_towerLeft->setChildrenCollapsible(false);
    m_towerSplit->addWidget(m_towerLeft);
    m_modeStack->addWidget(m_towerSplit);
    m_focusStack = new QStackedWidget;
    m_focusStack->setObjectName(QStringLiteral("invoicesFocusStack"));
    m_focusList = new QSplitter(Qt::Horizontal);
    m_focusList->setChildrenCollapsible(false);
    m_focusStack->addWidget(m_focusList);
    m_focusDetail = new QWidget;
    new QVBoxLayout(m_focusDetail);
    m_focusDetail->layout()->setContentsMargins(0, 0, 0, 0);
    m_focusStack->addWidget(m_focusDetail);
    m_modeStack->addWidget(m_focusStack);
    content->addWidget(m_modeStack, 1);
    outer->addWidget(m_content, 1);

    connect(m_organization, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0 || m_loading)
            return;
        if (m_host.setSetting)
            m_host.setSetting(QStringLiteral("organization"), organization());
        m_overview = QJsonObject();
        m_invoices = QJsonArray();
        m_contacts = QJsonArray();
        m_contactsLoaded = false;
        closeDetail();
        renderSummary();
        renderList();
        renderActivity();
        loadOverview();
    });
    connect(m_search, &QLineEdit::textChanged, this, [this] { renderList(); });
    connect(m_filter, &QComboBox::currentIndexChanged, this, [this] { renderList(); });
    connect(m_table, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (QTableWidgetItem *item = m_table->item(row, 0))
            openInvoice(item->data(kIdRole).toString());
    });
    connect(m_table, &QTableWidget::cellActivated, this, [this](int row, int) {
        if (QTableWidgetItem *item = m_table->item(row, 0))
            openInvoice(item->data(kIdRole).toString());
    });
    connect(m_activity, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item && !item->data(kIdRole).toString().isEmpty())
            openInvoice(item->data(kIdRole).toString());
    });
    connect(m_backButton, &QPushButton::clicked, this, [this] { closeDetail(); });
    connect(m_newButton, &QPushButton::clicked, this, [this] { openEditor({}, {}); });
    connect(m_settingsButton, &QPushButton::clicked, this, [this] { openSettings(); });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_webButton, &QPushButton::clicked, this, [this] { openWebPage(); });
    connect(m_installButton, &QPushButton::clicked, this, [this] { installPlugin(); });

    arrange(forkmesh::ui::layoutControlTower());
    applyStyle();
    applyState();
}

void InvoicesPage::request(const QByteArray &method, const QString &path, const QJsonObject &body,
                           ReplyHandler handler)
{
    if (!m_host.request) {
        handler(false, {}, QStringLiteral("Invoicing network access is unavailable."));
        return;
    }
    const QPointer<InvoicesPage> guard(this);
    m_host.request(method, path, body,
                   [guard, handler = std::move(handler)](bool ok, const QJsonObject &payload,
                                                         const QString &error) mutable {
        if (guard)
            handler(ok, payload, error);
    });
}

QString InvoicesPage::organization() const
{
    return m_organization->currentData().toString();
}

QString InvoicesPage::organizationPath(const QString &suffix) const
{
    return QString::fromLatin1(kApiPrefix) + QLatin1Char('/') + organization() + suffix;
}

QJsonObject InvoicesPage::organizationRecord() const
{
    const QString name = organization();
    for (const QJsonValue &value : m_organizations) {
        const QJsonObject item = value.toObject();
        if (text(item, "name") == name)
            return item;
    }
    return {};
}

bool InvoicesPage::installed() const
{
    return organizationRecord().value(QStringLiteral("installed")).toBool();
}

bool InvoicesPage::canManage() const
{
    const QString role = text(organizationRecord(), "role");
    return role == QLatin1String("owner") || role == QLatin1String("admin");
}

QString InvoicesPage::defaultCurrency() const
{
    const QString currency = text(m_overview.value(QStringLiteral("settings")).toObject(), "currency");
    return currency.isEmpty() ? QStringLiteral("USD") : currency;
}

QJsonObject InvoicesPage::invoiceById(const QString &id) const
{
    for (const QJsonValue &value : m_invoices) {
        const QJsonObject invoice = value.toObject();
        if (text(invoice, "id") == id)
            return invoice;
    }
    return {};
}

void InvoicesPage::refresh()
{
    arrange(forkmesh::ui::layoutControlTower());
    refreshOrganizations();
}

void InvoicesPage::prepareNavigationBadge()
{
    if (!m_loaded && !m_loading)
        refreshOrganizations();
}

int InvoicesPage::navigationBadge() const
{
    return installed() ? overdueCount(m_overview) : 0;
}

void InvoicesPage::themeChanged()
{
    applyStyle();
    renderList();
    renderDetail();
}

void InvoicesPage::showEvent(QShowEvent *event)
{
    forkmesh::market::NativePackageWidget::showEvent(event);
    arrange(forkmesh::ui::layoutControlTower());
}

void InvoicesPage::setHostSearchQuery(const QString &query)
{
    const QString trimmed = query.trimmed();
    if (trimmed == m_hostQuery)
        return;
    m_hostQuery = trimmed;
    m_search->setText(trimmed);
}

QList<forkmesh::market::NativePackageSearchHit> InvoicesPage::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty() || limit <= 0 || !installed())
        return hits;
    for (const QJsonValue &value : m_invoices) {
        const QJsonObject invoice = value.toObject();
        if (!matchesQuery(invoice, needle))
            continue;
        QStringList detail{statusLabel(statusKey(invoice)),
                           moneyText(integer(invoice, "total"), text(invoice, "currency"))};
        if (integer(invoice, "dueAt") > 0)
            detail << QStringLiteral("due ") + dateText(integer(invoice, "dueAt"));
        detail << QStringLiteral("Invoices in ") + organization();
        hits.append({text(invoice, "id"), text(invoice, "number") + kDot + clientLabel(invoice), detail.join(kDot)});
        if (hits.size() >= limit)
            break;
    }
    return hits;
}

void InvoicesPage::activateSearchHit(const QString &id)
{
    if (m_host.showPage)
        m_host.showPage();
    openInvoice(id);
}

bool InvoicesPage::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    const QString path =
        forkmesh::market::nativePackageLifecyclePath(organization(), m_host.packageId, QStringLiteral("export"));
    if (path.isEmpty() || !installed())
        return false;
    request(QByteArrayLiteral("GET"), path, {}, std::move(done));
    return true;
}

void InvoicesPage::refreshOrganizations()
{
    if (m_loading)
        return;
    m_loading = true;
    QString previous = organization();
    if (previous.isEmpty() && m_host.setting)
        previous = m_host.setting(QStringLiteral("organization"), QString()).toString();
    setStatus(QStringLiteral("Loading your organizations…"));
    request(QByteArrayLiteral("GET"), QString::fromLatin1(kApiPrefix), {},
            [this, previous](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok) {
            applyState();
            return setStatus(QStringLiteral("Invoicing could not load: %1").arg(error), true);
        }
        m_loaded = true;
        m_organizations = payload.value(QStringLiteral("organizations")).toArray();
        int preferred = -1;
        m_organization->blockSignals(true);
        m_organization->clear();
        for (const QJsonValue &value : std::as_const(m_organizations)) {
            const QJsonObject item = value.toObject();
            QString label = text(item, "name") + kDot + text(item, "role");
            if (!item.value(QStringLiteral("installed")).toBool())
                label += kDot + QStringLiteral("not installed");
            else if (preferred < 0)
                preferred = m_organization->count();
            m_organization->addItem(label, text(item, "name"));
        }
        const int index = m_organization->findData(previous);
        m_organization->setCurrentIndex(index >= 0 ? index : std::max(0, preferred));
        m_organization->blockSignals(false);
        if (!m_organization->count()) {
            m_overview = QJsonObject();
            m_invoices = QJsonArray();
            applyState();
            return setStatus(QStringLiteral("Join or create an organization before using Invoicing."), true);
        }
        loadOverview();
    });
}

void InvoicesPage::loadOverview()
{
    if (organization().isEmpty())
        return;
    if (!installed()) {
        m_overview = QJsonObject();
        m_invoices = QJsonArray();
        closeDetail();
        applyState();
        if (m_host.navigationBadgeChanged)
            m_host.navigationBadgeChanged();
        return setStatus(QStringLiteral("Invoicing is not installed for %1.").arg(organization()));
    }
    const int serial = ++m_serial;
    const QString name = organization();
    setStatus(QStringLiteral("Loading invoices for %1…").arg(name));
    request(QByteArrayLiteral("GET"), organizationPath(), {},
            [this, serial, name](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_serial || name != organization())
            return;
        if (!ok) {
            if (!notInstalledError(payload).isEmpty()) {
                for (int index = 0; index < m_organizations.size(); ++index) {
                    QJsonObject item = m_organizations.at(index).toObject();
                    if (text(item, "name") == name) {
                        item.insert(QStringLiteral("installed"), false);
                        m_organizations.replace(index, item);
                    }
                }
                m_overview = QJsonObject();
                m_invoices = QJsonArray();
                applyState();
                return setStatus(QStringLiteral("Invoicing is not installed for %1.").arg(name));
            }
            applyState();
            return setStatus(QStringLiteral("Invoicing is unavailable: %1").arg(error), true);
        }
        applyOverview(payload);
    });
}

void InvoicesPage::applyOverview(const QJsonObject &payload)
{
    if (m_overview.value(QStringLiteral("crmInstalled")).toBool() !=
        payload.value(QStringLiteral("crmInstalled")).toBool()) {
        m_contacts = QJsonArray();
        m_contactsLoaded = false;
    }
    m_overview = payload;
    m_invoices = payload.value(QStringLiteral("invoices")).toArray();
    const QJsonObject counts =
        payload.value(QStringLiteral("summary")).toObject().value(QStringLiteral("counts")).toObject();
    QStringList parts{countPhrase(m_invoices.size(), QStringLiteral("invoice"))};
    if (integer(counts, "overdue") > 0)
        parts << QStringLiteral("%1 overdue").arg(integer(counts, "overdue"));
    if (integer(counts, "draft") > 0)
        parts << countPhrase(integer(counts, "draft"), QStringLiteral("draft"));
    parts << QStringLiteral("shared with every member of %1").arg(organization());
    setStatus(parts.join(kDot));
    applyState();
    renderSummary();
    renderList();
    renderActivity();
    if (!m_detailId.isEmpty()) {
        if (invoiceById(m_detailId).isEmpty())
            closeDetail();
        else
            openInvoice(m_detailId);
    }
    if (m_searchIndexChanged)
        m_searchIndexChanged();
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
}

void InvoicesPage::applyState()
{
    const QJsonObject record = organizationRecord();
    const bool listed = !record.isEmpty();
    const bool ready = installed() && m_overview.contains(QStringLiteral("invoices"));
    m_installPanel->setVisible(listed && !installed());
    m_installButton->setVisible(canManage());
    m_installNote->setText(canManage()
        ? QStringLiteral("Invoicing lets every member of %1 create invoices for CRM contacts, email them with a "
                         "private view-and-pay link, and see when each one is opened, viewed and paid. Install it "
                         "for %1 to start.").arg(organization())
        : QStringLiteral("Ask an owner or admin of %1 to install Invoicing.").arg(organization()));
    m_content->setVisible(ready);
    m_newButton->setVisible(ready);
    m_settingsButton->setVisible(ready);
    m_organization->setVisible(m_organization->count() > 0);
}

void InvoicesPage::arrange(bool controlTower)
{
    const int mode = controlTower ? 1 : 0;
    if (mode == m_arranged)
        return;
    m_arranged = mode;
    if (controlTower) {
        m_towerLeft->addWidget(m_listPanel);
        m_towerLeft->addWidget(m_activityPanel);
        m_towerSplit->addWidget(m_detailPanel);
        m_towerLeft->setStretchFactor(0, 3);
        m_towerLeft->setStretchFactor(1, 1);
        m_towerSplit->setStretchFactor(0, 3);
        m_towerSplit->setStretchFactor(1, 2);
        m_towerSplit->setSizes({600, 420});
        m_towerLeft->setSizes({420, 140});
        m_modeStack->setCurrentWidget(m_towerSplit);
    } else {
        m_focusList->addWidget(m_listPanel);
        m_focusList->addWidget(m_activityPanel);
        m_focusDetail->layout()->addWidget(m_detailPanel);
        m_focusList->setStretchFactor(0, 3);
        m_focusList->setStretchFactor(1, 1);
        m_focusList->setSizes({760, 280});
        m_modeStack->setCurrentWidget(m_focusStack);
        m_focusStack->setCurrentWidget(m_detailId.isEmpty() ? static_cast<QWidget *>(m_focusList)
                                                            : m_focusDetail);
    }
    for (QWidget *panel : {m_listPanel, m_activityPanel, m_detailPanel})
        panel->show();
    m_backButton->setVisible(!controlTower);
    for (int index = 0; index < m_tiles.size(); ++index) {
        m_tiles.at(index)->layout()->setContentsMargins(controlTower ? 10 : 16, controlTower ? 6 : 12,
                                                        controlTower ? 10 : 16, controlTower ? 6 : 12);
    }
    applyStyle();
    renderDetail();
}

void InvoicesPage::applyStyle()
{
    const bool tower = m_arranged == 1;
    const QString surface = Theme::hex(Theme::Role::Surface);
    const QString border = Theme::hex(Theme::Role::Border);
    m_content->setStyleSheet(
        QStringLiteral("QFrame#invoicesTile, QFrame#invoicesMetric { background: %1; border: 1px solid %2;"
                       " border-radius: 8px; }"
                       "QFrame#invoicesTile QLabel, QFrame#invoicesMetric QLabel { background: transparent;"
                       " border: none; }"
                       "QLabel#invoicesTileValue { font-size: %3px; font-weight: 600; color: %4; }")
            .arg(surface, border, QString::number(tower ? 15 : 20), Theme::hex(Theme::Role::Fg)));
    const QColor ink = Theme::color(Theme::Role::FgMuted);
    m_settingsButton->setIcon(tintedIcon(QStringLiteral("gear"), ink));
    m_refreshButton->setIcon(tintedIcon(QStringLiteral("sync"), ink));
    m_webButton->setIcon(tintedIcon(QStringLiteral("link-external"), ink));
    m_backButton->setIcon(tintedIcon(QStringLiteral("arrow-left"), ink));
}

void InvoicesPage::renderSummary()
{
    const QJsonObject summary = m_overview.value(QStringLiteral("summary")).toObject();
    const QJsonObject counts = summary.value(QStringLiteral("counts")).toObject();
    const QString currency = defaultCurrency();
    const QStringList values{
        moneyMapText(summary.value(QStringLiteral("outstanding")).toObject(), currency),
        moneyMapText(summary.value(QStringLiteral("overdueAmount")).toObject(), currency),
        moneyMapText(summary.value(QStringLiteral("paidLast30Days")).toObject(), currency),
        openRateValue(summary),
        daysToPayValue(summary),
    };
    const QStringList details{
        countPhrase(integer(counts, "sent"), QStringLiteral("unpaid invoice")),
        countPhrase(integer(counts, "overdue"), QStringLiteral("overdue invoice")),
        countPhrase(integer(counts, "paid"), QStringLiteral("paid invoice")) + QStringLiteral(" in all"),
        openRateDetail(summary),
        daysToPayDetail(summary),
    };
    for (int index = 0; index < m_tileValues.size(); ++index) {
        m_tileValues.at(index)->setText(values.at(index));
        m_tileValues.at(index)->setToolTip(values.at(index));
        m_tileDetails.at(index)->setText(details.at(index));
    }
    const bool overdue = integer(counts, "overdue") > 0;
    m_tileValues.at(1)->setStyleSheet(overdue ? QStringLiteral("color: %1;").arg(Theme::hex(Theme::Role::Danger))
                                              : QString());
}

void InvoicesPage::renderList()
{
    const QString query = m_search->text().trimmed();
    const QString filter = m_filter->currentData().toString();
    const QString selected = m_detailId;
    m_table->setRowCount(0);
    for (const QJsonValue &value : std::as_const(m_invoices)) {
        const QJsonObject invoice = value.toObject();
        if (!matchesFilter(invoice, filter) || !matchesQuery(invoice, query))
            continue;
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *number = cell(text(invoice, "number"));
        number->setData(kIdRole, text(invoice, "id"));
        QFont bold = number->font();
        bold.setWeight(QFont::DemiBold);
        number->setFont(bold);
        m_table->setItem(row, 0, number);
        QString client = clientLabel(invoice);
        if (!text(invoice, "contactId").isEmpty())
            client += kDot + QStringLiteral("CRM");
        m_table->setItem(row, 1, cell(client));
        const QString key = statusKey(invoice);
        auto *status = cell(statusLabel(key));
        status->setForeground(Theme::color(statusInk(key)));
        m_table->setItem(row, 2, status);
        const qint64 total = integer(invoice, "total");
        const qint64 balance = integer(invoice, "balance");
        auto *amount = cell(moneyText(total, text(invoice, "currency")), Qt::AlignRight | Qt::AlignVCenter);
        if (balance && balance != total)
            amount->setToolTip(moneyText(balance, text(invoice, "currency")) + QStringLiteral(" due"));
        m_table->setItem(row, 3, amount);
        m_table->setItem(row, 4, cell(dateText(integer(invoice, "dueAt"))));
        auto *engagement = cell(engagementText(invoice));
        if (integer(invoice, "lastActivityAt") > 0)
            engagement->setToolTip(QStringLiteral("Last activity %1").arg(
                relativeText(nowMs(), integer(invoice, "lastActivityAt"))));
        m_table->setItem(row, 5, engagement);
        if (text(invoice, "id") == selected)
            m_table->selectRow(row);
    }
    const bool none = m_table->rowCount() == 0;
    m_emptyNote->setText(m_invoices.isEmpty()
        ? QStringLiteral("No invoices yet. Press New invoice to bill a client.")
        : QStringLiteral("No invoices match."));
    m_emptyNote->setVisible(none);
}

void InvoicesPage::renderActivity()
{
    m_activity->clear();
    const QJsonArray events = m_overview.value(QStringLiteral("activity")).toArray();
    const qint64 now = nowMs();
    for (const QJsonValue &value : events) {
        const QJsonObject event = value.toObject();
        auto *item = new QListWidgetItem(activityLine(event) + QLatin1Char('\n') +
                                         relativeText(now, integer(event, "at")));
        item->setData(kIdRole, text(event, "invoiceId"));
        item->setToolTip(activityLine(event));
        m_activity->addItem(item);
    }
    if (events.isEmpty()) {
        auto *item = new QListWidgetItem(QStringLiteral("Sends, opens, views and payments show up here."));
        item->setFlags(Qt::NoItemFlags);
        m_activity->addItem(item);
    }
}

void InvoicesPage::openInvoice(const QString &id)
{
    const QJsonObject cached = invoiceById(id);
    if (id.isEmpty() || cached.isEmpty())
        return;
    if (m_detailId != id || m_detail.isEmpty()) {
        m_detail = cached;
        m_detail.remove(QStringLiteral("events"));
    }
    m_detailId = id;
    if (m_arranged == 0)
        m_focusStack->setCurrentWidget(m_focusDetail);
    renderDetail();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0) && m_table->item(row, 0)->data(kIdRole).toString() == id) {
            m_table->selectRow(row);
            m_table->scrollToItem(m_table->item(row, 0));
        }
    }
    const int serial = ++m_detailSerial;
    request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/items/") + id), {},
            [this, serial, id](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_detailSerial || id != m_detailId)
            return;
        if (!ok)
            return setStatus(QStringLiteral("Could not open the invoice: %1").arg(error), true);
        m_detail = payload.value(QStringLiteral("invoice")).toObject();
        renderDetail();
    });
}

void InvoicesPage::closeDetail()
{
    m_detailId.clear();
    m_detail = QJsonObject();
    ++m_detailSerial;
    if (m_table)
        m_table->clearSelection();
    if (m_arranged == 0)
        m_focusStack->setCurrentWidget(m_focusList);
    renderDetail();
}

void InvoicesPage::renderDetail()
{
    if (!m_detailScroll)
        return;
    const bool tower = m_arranged == 1;
    const bool open = !m_detailId.isEmpty() && !m_detail.isEmpty();
    m_detailPlaceholder->setVisible(!open);
    m_detailScroll->setVisible(open);
    m_detailActions->setVisible(open);
    QLayout *actionsLayout = m_detailActions->layout();
    while (QLayoutItem *item = actionsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        if (QLayout *child = item->layout()) {
            while (QLayoutItem *inner = child->takeAt(0)) {
                if (inner->widget()) {
                    inner->widget()->hide();
                    inner->widget()->deleteLater();
                }
                delete inner;
            }
        }
        delete item;
    }
    if (!open) {
        if (QWidget *old = m_detailScroll->takeWidget())
            old->deleteLater();
        return;
    }
    const QJsonObject &invoice = m_detail;
    const QString currency = text(invoice, "currency");
    const qint64 now = nowMs();
    auto *body = new QWidget;
    body->setObjectName(QStringLiteral("invoicesDetailBody"));
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, tower ? 4 : 0, 0);
    layout->setSpacing(10);

    auto *titleRow = new QHBoxLayout;
    titleRow->setSpacing(8);
    auto *number = new QLabel(text(invoice, "number"));
    number->setTextFormat(Qt::PlainText);
    number->setTextInteractionFlags(Qt::TextSelectableByMouse);
    number->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 600;"));
    titleRow->addWidget(number);
    titleRow->addWidget(statusChip(statusKey(invoice)), 0, Qt::AlignVCenter);
    titleRow->addStretch(1);
    layout->addLayout(titleRow);
    const QJsonObject client = invoice.value(QStringLiteral("client")).toObject();
    QStringList who{clientLabel(invoice)};
    if (!text(client, "company").isEmpty() && !text(client, "name").isEmpty())
        who << text(client, "company");
    if (!text(client, "email").isEmpty() && who.first() != text(client, "email"))
        who << text(client, "email");
    if (!text(invoice, "contactId").isEmpty())
        who << (invoice.contains(QStringLiteral("contact")) && invoice.value(QStringLiteral("contact")).isNull()
                    ? QStringLiteral("CRM contact removed")
                    : QStringLiteral("CRM contact"));
    QLabel *whoLabel = mutedLabel(who.join(kDot));
    whoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(whoLabel);

    const qint64 total = integer(invoice, "total");
    const qint64 paid = integer(invoice, "paidAmount");
    const qint64 balance = integer(invoice, "balance");
    const bool sent = integer(invoice, "sentAt") > 0;
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    const QString openHint = integer(invoice, "firstOpenedAt") > 0
        ? QStringLiteral("first ") + relativeText(now, integer(invoice, "firstOpenedAt"))
        : (sent ? QStringLiteral("not yet") : QString());
    QString viewHint = integer(invoice, "firstViewedAt") > 0
        ? QStringLiteral("first ") + relativeText(now, integer(invoice, "firstViewedAt"))
        : (sent ? QStringLiteral("not yet") : QString());
    if (integer(invoice, "clicks") > 0)
        viewHint += kDot + countPhrase(integer(invoice, "clicks"), QStringLiteral("pay click"));
    auto *metrics = new QGridLayout;
    metrics->setSpacing(8);
    const QList<QFrame *> boxes{
        metricBox(QStringLiteral("Total"), moneyText(total, currency),
                  paid ? moneyText(paid, currency) + QStringLiteral(" paid") : QString()),
        metricBox(QStringLiteral("Due"), dateText(integer(invoice, "dueAt")),
                  balance ? moneyText(balance, currency) + QStringLiteral(" to pay")
                          : (text(invoice, "status") == QLatin1String("paid")
                                 ? QStringLiteral("paid ") + dateText(integer(invoice, "paidAt"))
                                 : QString())),
        metricBox(QStringLiteral("Email opens"), sent ? QString::number(integer(invoice, "opens")) : dash, openHint),
        metricBox(QStringLiteral("Page views"), sent ? QString::number(integer(invoice, "views")) : dash, viewHint),
    };
    const int perRow = tower ? 2 : 4;
    for (int index = 0; index < boxes.size(); ++index)
        metrics->addWidget(boxes.at(index), index / perRow, index % perRow);
    layout->addLayout(metrics);
    if (sent) {
        QString line = QStringLiteral("Emailed to %1 %2")
                           .arg(text(invoice, "sentTo").isEmpty()
                                    ? (text(client, "email").isEmpty() ? QStringLiteral("the client")
                                                                       : text(client, "email"))
                                    : text(invoice, "sentTo"),
                                relativeText(now, integer(invoice, "lastSentAt") > 0 ? integer(invoice, "lastSentAt")
                                                                                     : integer(invoice, "sentAt")));
        if (!text(invoice, "sentBy").isEmpty())
            line += QStringLiteral(" by ") + text(invoice, "sentBy");
        line += QStringLiteral(". Opens count when the reader's mail app loads images; some apps load them for "
                               "privacy without the reader opening it.");
        layout->addWidget(mutedLabel(line));
    }

    const QJsonArray items = invoice.value(QStringLiteral("items")).toArray();
    auto *lines = new QTableWidget(0, 3);
    lines->setObjectName(QStringLiteral("invoicesDetailLines"));
    lines->setAccessibleName(QStringLiteral("Line items"));
    lines->setHorizontalHeaderLabels({QStringLiteral("Item"), QStringLiteral("Quantity × price"),
                                      QStringLiteral("Amount")});
    lines->verticalHeader()->setVisible(false);
    lines->verticalHeader()->setDefaultSectionSize(kRowHeight - 2);
    lines->setEditTriggers(QAbstractItemView::NoEditTriggers);
    lines->setSelectionMode(QAbstractItemView::NoSelection);
    lines->setFocusPolicy(Qt::NoFocus);
    lines->setShowGrid(false);
    lines->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    lines->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    const auto addLine = [lines](const QString &first, const QString &second, const QString &third, bool strong) {
        const int row = lines->rowCount();
        lines->insertRow(row);
        lines->setItem(row, 0, cell(first));
        lines->setItem(row, 1, cell(second, Qt::AlignRight | Qt::AlignVCenter));
        auto *amount = cell(third, Qt::AlignRight | Qt::AlignVCenter);
        if (strong) {
            QFont font = amount->font();
            font.setWeight(QFont::DemiBold);
            amount->setFont(font);
            lines->item(row, 1)->setFont(font);
        }
        lines->setItem(row, 2, amount);
    };
    for (const InvoiceLine &line : invoiceLines(items)) {
        addLine(line.description.isEmpty() ? QStringLiteral("Item") : line.description,
                QStringLiteral("%1 × %2").arg(QString::number(line.quantity, 'g', 6), moneyText(line.unitAmount, currency)),
                moneyText(lineAmount(line), currency), false);
    }
    addLine(QString(), QStringLiteral("Subtotal"), moneyText(integer(invoice, "subtotal"), currency), false);
    if (integer(invoice, "tax") > 0)
        addLine(QString(), QStringLiteral("Tax (%1)").arg(percentText(int(integer(invoice, "taxRate")))),
                moneyText(integer(invoice, "tax"), currency), false);
    addLine(QString(), QStringLiteral("Total"), moneyText(total, currency), true);
    if (paid > 0)
        addLine(QString(), QStringLiteral("Paid"), moneyText(paid, currency), false);
    if (balance > 0 && balance != total)
        addLine(QString(), QStringLiteral("Balance due"), moneyText(balance, currency), true);
    lines->setFixedHeight(lines->horizontalHeader()->sizeHint().height() + lines->rowCount() * (kRowHeight - 2) + 4);
    layout->addWidget(lines);

    const QJsonArray payments = invoice.value(QStringLiteral("payments")).toArray();
    if (!payments.isEmpty()) {
        layout->addWidget(headingLabel(QStringLiteral("Payments")));
        QStringList rows;
        for (const QJsonValue &value : payments) {
            const QJsonObject payment = value.toObject();
            QStringList parts{moneyText(integer(payment, "amount"), currency), dateText(integer(payment, "at"))};
            for (const char *key : {"method", "note"}) {
                if (!text(payment, key).isEmpty())
                    parts << text(payment, key);
            }
            if (!text(payment, "by").isEmpty())
                parts << QStringLiteral("by ") + text(payment, "by");
            rows << parts.join(kDot);
        }
        QLabel *paymentList = mutedLabel(rows.join(QLatin1Char('\n')));
        paymentList->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(paymentList);
    }

    for (const auto &[key, label] : {std::pair{"notes", "Notes"}, std::pair{"terms", "Terms"}}) {
        if (text(invoice, key).trimmed().isEmpty())
            continue;
        layout->addWidget(headingLabel(QString::fromLatin1(label)));
        auto *value = new QLabel(text(invoice, key));
        value->setTextFormat(Qt::PlainText);
        value->setWordWrap(true);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(value);
    }
    if (!text(invoice, "paymentUrl").isEmpty()) {
        QLabel *pay = mutedLabel(QStringLiteral("Pay link: ") + text(invoice, "paymentUrl"));
        pay->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(pay);
    }

    if (!text(invoice, "publicUrl").isEmpty()) {
        layout->addWidget(headingLabel(QStringLiteral("Client link")));
        auto *linkRow = new QHBoxLayout;
        linkRow->setSpacing(6);
        auto *link = new QLineEdit(text(invoice, "publicUrl"));
        link->setObjectName(QStringLiteral("invoicesPublicLink"));
        link->setAccessibleName(QStringLiteral("Invoice link"));
        link->setReadOnly(true);
        link->setCursorPosition(0);
        linkRow->addWidget(link, 1);
        QPushButton *copy = ghostButton(QStringLiteral("Copy link"), QStringLiteral("copy"));
        QPushButton *preview = ghostButton(QStringLiteral("Preview"), QStringLiteral("link-external"));
        linkRow->addWidget(copy);
        linkRow->addWidget(preview);
        layout->addLayout(linkRow);
        layout->addWidget(mutedLabel(QStringLiteral(
            "Anyone with this link can read the invoice. Opening it yourself counts as a view.")));
        connect(copy, &QPushButton::clicked, this, [this] { copyPublicLink(); });
        connect(preview, &QPushButton::clicked, this, [this] { openPublicLink(); });
    }

    layout->addWidget(headingLabel(QStringLiteral("History")));
    if (!invoice.contains(QStringLiteral("events"))) {
        layout->addWidget(mutedLabel(QStringLiteral("Loading history…")));
    } else {
        const QJsonArray events = invoice.value(QStringLiteral("events")).toArray();
        if (events.isEmpty())
            layout->addWidget(mutedLabel(QStringLiteral("Nothing has happened to this invoice yet.")));
        auto *history = new QVBoxLayout;
        history->setSpacing(4);
        for (const QJsonValue &value : events) {
            const QJsonObject event = value.toObject();
            auto *row = new QVBoxLayout;
            row->setSpacing(0);
            auto *what = new QLabel(eventLabel(text(event, "kind")) + kDot + relativeText(now, integer(event, "at")));
            what->setTextFormat(Qt::PlainText);
            what->setToolTip(QDateTime::fromMSecsSinceEpoch(integer(event, "at")).toString(Qt::TextDate));
            row->addWidget(what);
            const QString detail = eventDetail(event, currency);
            if (!detail.isEmpty())
                row->addWidget(mutedLabel(detail));
            history->addLayout(row);
        }
        layout->addLayout(history);
    }
    layout->addStretch(1);
    if (QWidget *old = m_detailScroll->takeWidget())
        old->deleteLater();
    m_detailScroll->setWidget(body);

    auto *primary = new QHBoxLayout;
    primary->setSpacing(6);
    auto *secondary = new QHBoxLayout;
    secondary->setSpacing(6);
    addDetailActions(primary, secondary);
    static_cast<QVBoxLayout *>(actionsLayout)->addLayout(primary);
    if (secondary->count())
        static_cast<QVBoxLayout *>(actionsLayout)->addLayout(secondary);
    else
        delete secondary;
}

void InvoicesPage::addDetailActions(QHBoxLayout *primary, QHBoxLayout *secondary)
{
    const QJsonObject &invoice = m_detail;
    const bool tower = m_arranged == 1;
    struct Action {
        QString label;
        QString icon;
        std::function<void()> run;
        bool danger = false;
    };
    QList<Action> main;
    QList<Action> more;
    if (canSend(invoice))
        main.append({sendActionLabel(invoice), QStringLiteral("paper-airplane"), [this] { openSendDialog(); }});
    if (canRecordPayment(invoice))
        main.append({QStringLiteral("Record payment"), QStringLiteral("credit-card"), [this] { openPaymentDialog(); }});
    if (canMarkPaid(invoice))
        main.append({QStringLiteral("Mark paid"), QStringLiteral("check"), [this] { openPaymentDialog(); }});
    if (canEdit(invoice))
        main.append({QStringLiteral("Edit"), QStringLiteral("pencil"), [this] { openEditor(m_detail, {}); }});
    if (canReopen(invoice))
        main.append({QStringLiteral("Reopen"), QStringLiteral("issue-reopened"), [this] { runStatusAction(QStringLiteral("reopen")); }});
    more.append({QStringLiteral("Duplicate"), QStringLiteral("copy"), [this] { duplicateInvoice(); }});
    if (!text(invoice, "publicUrl").isEmpty()) {
        more.append({QStringLiteral("Copy client link"), QStringLiteral("link"), [this] { copyPublicLink(); }});
        more.append({QStringLiteral("Open client link"), QStringLiteral("link-external"), [this] { openPublicLink(); }});
    }
    if (canDelete(invoice))
        more.append({QStringLiteral("Delete"), QStringLiteral("trash"), [this] { deleteInvoice(); }, true});
    else if (canVoid(invoice) && text(invoice, "status") != QLatin1String("void"))
        more.append({QStringLiteral("Void"), QStringLiteral("circle-slash"), [this] { runStatusAction(QStringLiteral("void")); }, true});

    const QColor ink = Theme::color(Theme::Role::FgMuted);
    bool first = true;
    for (const Action &action : std::as_const(main)) {
        QPushButton *button = first ? primaryButton(action.label) : ghostButton(action.label);
        if (!first)
            button->setIcon(tintedIcon(action.icon, ink));
        first = false;
        connect(button, &QPushButton::clicked, this, action.run);
        primary->addWidget(button);
    }
    if (tower) {
        primary->addStretch(1);
        for (const Action &action : std::as_const(more)) {
            QPushButton *button = ghostButton(action.label);
            button->setIcon(tintedIcon(action.icon, action.danger ? Theme::color(Theme::Role::Danger) : ink));
            if (action.danger)
                button->setObjectName(QStringLiteral("dangerButton"));
            connect(button, &QPushButton::clicked, this, action.run);
            secondary->addWidget(button);
        }
        secondary->addStretch(1);
        return;
    }
    auto *menuButton = new QToolButton;
    menuButton->setObjectName(QStringLiteral("invoicesMoreButton"));
    menuButton->setAccessibleName(QStringLiteral("More actions"));
    menuButton->setToolTip(QStringLiteral("More actions"));
    menuButton->setIcon(tintedIcon(QStringLiteral("kebab-horizontal"), ink));
    menuButton->setPopupMode(QToolButton::InstantPopup);
    menuButton->setAutoRaise(true);
    auto *menu = new QMenu(menuButton);
    for (const Action &action : std::as_const(more)) {
        QAction *item = menu->addAction(
            tintedIcon(action.icon, action.danger ? Theme::color(Theme::Role::Danger) : ink), action.label);
        connect(item, &QAction::triggered, this, action.run);
    }
    menuButton->setMenu(menu);
    primary->addWidget(menuButton);
    primary->addStretch(1);
}

void InvoicesPage::invoiceChanged(const QJsonObject &invoice, const QString &notice)
{
    if (!invoice.isEmpty()) {
        m_detailId = text(invoice, "id");
        m_detail = invoice;
        bool replaced = false;
        for (int index = 0; index < m_invoices.size(); ++index) {
            if (text(m_invoices.at(index).toObject(), "id") == m_detailId) {
                m_invoices.replace(index, invoice);
                replaced = true;
            }
        }
        if (!replaced)
            m_invoices.prepend(invoice);
        if (m_arranged == 0)
            m_focusStack->setCurrentWidget(m_focusDetail);
        renderList();
        renderDetail();
    }
    if (!notice.isEmpty() && m_host.flashMessage)
        m_host.flashMessage(notice, false);
    const int serial = ++m_serial;
    const QString name = organization();
    request(QByteArrayLiteral("GET"), organizationPath(), {},
            [this, serial, name, notice](bool ok, const QJsonObject &payload, const QString &error) {
        if (serial != m_serial || name != organization())
            return;
        if (!ok)
            return setStatus(QStringLiteral("Invoicing could not reload: %1").arg(error), true);
        applyOverview(payload);
        if (!notice.isEmpty())
            setStatus(notice);
    });
}

void InvoicesPage::loadContacts(std::function<void()> done)
{
    if (m_contactsLoaded || !m_overview.value(QStringLiteral("crmInstalled")).toBool()) {
        done();
        return;
    }
    setStatus(QStringLiteral("Loading CRM contacts…"));
    request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/contacts")), {},
            [this, done = std::move(done)](bool ok, const QJsonObject &payload, const QString &error) {
        m_contactsLoaded = ok;
        m_contacts = ok ? payload.value(QStringLiteral("contacts")).toArray() : QJsonArray();
        setStatus(ok ? QString() : QStringLiteral("CRM contacts could not load: %1").arg(error), !ok);
        done();
    });
}

void InvoicesPage::installPlugin()
{
    const QString path =
        forkmesh::market::nativePackageLifecyclePath(organization(), m_host.packageId, QStringLiteral("install"));
    if (path.isEmpty())
        return setStatus(QStringLiteral("Select a valid organization before installing Invoicing."), true);
    m_installButton->setEnabled(false);
    setStatus(QStringLiteral("Installing Invoicing…"));
    request(QByteArrayLiteral("POST"), path, {}, [this](bool ok, const QJsonObject &, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Invoicing was not installed: %1").arg(error), true);
        setStatus(QStringLiteral("Invoicing installed for this organization."));
        refreshOrganizations();
    });
}

void InvoicesPage::openEditor(const QJsonObject &invoice, const QJsonObject &seed)
{
    if (!installed() || organization().isEmpty())
        return;
    loadContacts([this, invoice, seed] {
        const bool editing = !invoice.isEmpty();
        const QJsonObject source = editing ? invoice : seed;
        const QJsonObject settings = m_overview.value(QStringLiteral("settings")).toObject();
        const bool crm = m_overview.value(QStringLiteral("crmInstalled")).toBool();
        auto *dialog = new QDialog(this);
        dialog->setObjectName(QStringLiteral("invoicesEditor"));
        dialog->setWindowTitle(editing ? QStringLiteral("Edit %1").arg(text(invoice, "number"))
                                       : QStringLiteral("New invoice"));
        dialog->resize(720, 760);
        auto *outer = new QVBoxLayout(dialog);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 6, 0);
        auto *form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        const QJsonObject client = source.value(QStringLiteral("client")).toObject();
        auto *contact = new QComboBox;
        contact->setAccessibleName(QStringLiteral("CRM contact"));
        contact->addItem(QStringLiteral("Not linked to a CRM contact"), QString());
        for (const QJsonValue &value : std::as_const(m_contacts)) {
            const QJsonObject item = value.toObject();
            QStringList label{text(item, "name").isEmpty() ? text(item, "email") : text(item, "name")};
            if (!text(item, "company").isEmpty())
                label << text(item, "company");
            contact->addItem(label.join(kDot), text(item, "id"));
        }
        const int contactIndex = contact->findData(text(source, "contactId"));
        if (contactIndex >= 0)
            contact->setCurrentIndex(contactIndex);
        if (crm)
            form->addRow(QStringLiteral("CRM contact"), contact);
        else
            contact->hide();
        auto *name = new QLineEdit(text(client, "name"));
        name->setMaxLength(160);
        auto *email = new QLineEdit(text(client, "email"));
        email->setMaxLength(254);
        auto *company = new QLineEdit(text(client, "company"));
        company->setMaxLength(160);
        QPlainTextEdit *address = textBox(text(client, "address"), 2);
        form->addRow(QStringLiteral("Client name"), name);
        form->addRow(QStringLiteral("Client email"), email);
        form->addRow(QStringLiteral("Company"), company);
        form->addRow(QStringLiteral("Address"), address);
        const auto fillFromContact = [this, name, email, company](const QString &id) {
            for (const QJsonValue &value : std::as_const(m_contacts)) {
                const QJsonObject item = value.toObject();
                if (text(item, "id") != id)
                    continue;
                if (!text(item, "name").isEmpty())
                    name->setText(text(item, "name"));
                if (!text(item, "email").isEmpty())
                    email->setText(text(item, "email"));
                if (!text(item, "company").isEmpty())
                    company->setText(text(item, "company"));
            }
        };
        if (!editing && !text(source, "contactId").isEmpty() && client.isEmpty())
            fillFromContact(text(source, "contactId"));
        connect(contact, &QComboBox::currentIndexChanged, dialog,
                [contact, fillFromContact] { fillFromContact(contact->currentData().toString()); });

        auto *numberField = new QLineEdit(editing ? text(invoice, "number") : QString());
        numberField->setMaxLength(40);
        numberField->setPlaceholderText(QStringLiteral("Next number from settings"));
        form->addRow(QStringLiteral("Number"), numberField);
        const qint64 today = todayUtc(nowMs());
        const qint64 issuedAt = integer(source, "issuedAt") > 0 ? integer(source, "issuedAt") : today;
        const qint64 dueAt = editing && integer(invoice, "dueAt") > 0
            ? integer(invoice, "dueAt")
            : issuedAt + qint64(settings.value(QStringLiteral("dueDays")).toInt(30)) * kDayMs;
        QDateEdit *issued = dateEdit(utcDate(issuedAt));
        QDateEdit *due = dateEdit(utcDate(dueAt));
        auto *dates = new QHBoxLayout;
        dates->addWidget(issued, 1);
        dates->addWidget(new QLabel(QStringLiteral("Due")));
        dates->addWidget(due, 1);
        form->addRow(QStringLiteral("Issued"), dates);
        auto *currency = new QLineEdit(text(source, "currency").isEmpty() ? defaultCurrency()
                                                                          : text(source, "currency"));
        currency->setMaxLength(3);
        currency->setInputMask(QStringLiteral(">AAA"));
        currency->setFixedWidth(70);
        auto *tax = new QDoubleSpinBox;
        tax->setRange(0, 1000);
        tax->setDecimals(2);
        tax->setSuffix(QStringLiteral(" %"));
        tax->setValue(double(source.contains(QStringLiteral("taxRate")) ? integer(source, "taxRate")
                                                                        : integer(settings, "taxRate")) / 100.0);
        auto *money = new QHBoxLayout;
        money->addWidget(currency);
        money->addWidget(new QLabel(QStringLiteral("Tax")));
        money->addWidget(tax);
        money->addStretch(1);
        form->addRow(QStringLiteral("Currency"), money);
        layout->addLayout(form);
        form->addRow(headingLabel(QStringLiteral("Line items")));
        auto *lines = new QTableWidget(0, 5);
        lines->setObjectName(QStringLiteral("invoicesEditorLines"));
        lines->setAccessibleName(QStringLiteral("Line items"));
        lines->setHorizontalHeaderLabels({QStringLiteral("Description"), QStringLiteral("Quantity"),
                                          QStringLiteral("Unit price"), QStringLiteral("Amount"), QString()});
        lines->verticalHeader()->setVisible(false);
        lines->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        lines->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        lines->setMinimumHeight(150);
        lines->setEditTriggers(QAbstractItemView::AllEditTriggers);
        form->addRow(lines);
        auto *lineButtons = new QHBoxLayout;
        QPushButton *addLineButton = ghostButton(QStringLiteral("Add line"), QStringLiteral("plus"));
        lineButtons->addWidget(addLineButton);
        lineButtons->addStretch(1);
        auto *totalsLabel = new QLabel;
        totalsLabel->setObjectName(QStringLiteral("invoicesEditorTotals"));
        totalsLabel->setTextFormat(Qt::PlainText);
        totalsLabel->setAlignment(Qt::AlignRight);
        lineButtons->addWidget(totalsLabel);
        form->addRow(lineButtons);

        auto readLines = std::make_shared<std::function<QList<InvoiceLine>()>>();
        auto updateTotals = std::make_shared<std::function<void()>>();
        auto addRow = std::make_shared<std::function<void(const InvoiceLine &)>>();
        *readLines = [lines] {
            QList<InvoiceLine> result;
            for (int row = 0; row < lines->rowCount(); ++row) {
                const auto value = [lines, row](int column) {
                    return lines->item(row, column) ? lines->item(row, column)->text() : QString();
                };
                bool ok = false;
                const double quantity = value(1).trimmed().toDouble(&ok);
                result.append({value(0), ok ? std::max(0.0, quantity) : 0.0, parseCents(value(2))});
            }
            return result;
        };
        *updateTotals = [lines, readLines, currency, tax, totalsLabel] {
            const QList<InvoiceLine> current = (*readLines)();
            const QString code = currency->text().trimmed().toUpper();
            lines->blockSignals(true);
            for (int row = 0; row < current.size(); ++row) {
                auto *amount = lines->item(row, 3);
                if (!amount) {
                    amount = new QTableWidgetItem;
                    amount->setFlags(Qt::ItemIsEnabled);
                    amount->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                    lines->setItem(row, 3, amount);
                }
                amount->setText(moneyText(lineAmount(current.at(row)), code));
            }
            lines->blockSignals(false);
            const InvoiceTotals sum = totals(current, basisPointsFromPercent(tax->value()));
            totalsLabel->setText(QStringLiteral("Subtotal %1%4Tax %2%4Total %3")
                                     .arg(moneyText(sum.subtotal, code), moneyText(sum.tax, code),
                                          moneyText(sum.total, code), kDot));
        };
        *addRow = [lines, updateTotals, dialog](const InvoiceLine &line) {
            const int row = lines->rowCount();
            lines->blockSignals(true);
            lines->insertRow(row);
            lines->setItem(row, 0, new QTableWidgetItem(line.description));
            auto *quantity = new QTableWidgetItem(QString::number(line.quantity, 'g', 6));
            quantity->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            lines->setItem(row, 1, quantity);
            auto *unit = new QTableWidgetItem(line.unitAmount ? centsInput(line.unitAmount) : QString());
            unit->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            lines->setItem(row, 2, unit);
            auto *remove = new QToolButton;
            remove->setAutoRaise(true);
            remove->setToolTip(QStringLiteral("Remove line"));
            remove->setAccessibleName(QStringLiteral("Remove line"));
            remove->setIcon(tintedIcon(QStringLiteral("trash"), Theme::color(Theme::Role::FgMuted)));
            lines->setCellWidget(row, 4, remove);
            lines->blockSignals(false);
            QObject::connect(remove, &QToolButton::clicked, dialog, [lines, remove, updateTotals] {
                for (int index = 0; index < lines->rowCount(); ++index) {
                    if (lines->cellWidget(index, 4) == remove) {
                        lines->removeRow(index);
                        break;
                    }
                }
                (*updateTotals)();
            });
            (*updateTotals)();
        };
        QList<InvoiceLine> initial = invoiceLines(source.value(QStringLiteral("items")).toArray());
        if (initial.isEmpty())
            initial.append(InvoiceLine{});
        for (const InvoiceLine &line : std::as_const(initial))
            (*addRow)(line);
        connect(addLineButton, &QPushButton::clicked, dialog, [addRow, lines] {
            (*addRow)(InvoiceLine{});
            lines->setCurrentCell(lines->rowCount() - 1, 0);
            lines->editItem(lines->item(lines->rowCount() - 1, 0));
        });
        connect(lines, &QTableWidget::itemChanged, dialog, [updateTotals](QTableWidgetItem *item) {
            if (item && item->column() != 3)
                (*updateTotals)();
        });
        connect(tax, &QDoubleSpinBox::valueChanged, dialog, [updateTotals] { (*updateTotals)(); });
        connect(currency, &QLineEdit::textChanged, dialog, [updateTotals] { (*updateTotals)(); });

        QFormLayout *details = form;
        auto *payment = new QLineEdit(source.contains(QStringLiteral("paymentUrl")) ? text(source, "paymentUrl")
                                                                                     : text(settings, "paymentUrl"));
        payment->setPlaceholderText(QStringLiteral("https://… where the client pays"));
        QPlainTextEdit *notes = textBox(text(source, "notes"), 3);
        QPlainTextEdit *terms = textBox(text(source, "terms"), 2);
        details->addRow(QStringLiteral("Pay link"), payment);
        details->addRow(QStringLiteral("Notes"), notes);
        details->addRow(QStringLiteral("Terms"), terms);
        layout->addStretch(1);
        scroll->setWidget(page);
        outer->addWidget(scroll, 1);
        QLabel *error = dialogError();
        outer->addWidget(error);
        auto *buttons = new QDialogButtonBox;
        QPushButton *save = buttons->addButton(editing ? QStringLiteral("Save") : QStringLiteral("Create draft"),
                                               QDialogButtonBox::AcceptRole);
        save->setObjectName(QStringLiteral("primaryButton"));
        save->setDefault(true);
        buttons->addButton(QDialogButtonBox::Cancel);
        QPushButton *remove = nullptr;
        if (editing && canDelete(invoice)) {
            remove = buttons->addButton(QStringLiteral("Delete"), QDialogButtonBox::DestructiveRole);
            remove->setObjectName(QStringLiteral("dangerButton"));
        }
        outer->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
        if (remove)
            connect(remove, &QPushButton::clicked, dialog, [this, dialog] {
                dialog->reject();
                deleteInvoice();
            });
        const QString invoiceId = text(invoice, "id");
        connect(save, &QPushButton::clicked, dialog, [=] {
            QJsonObject body{
                {QStringLiteral("client"), QJsonObject{{QStringLiteral("name"), name->text()},
                                                       {QStringLiteral("email"), email->text()},
                                                       {QStringLiteral("company"), company->text()},
                                                       {QStringLiteral("address"), address->toPlainText()}}},
                {QStringLiteral("currency"), currency->text().trimmed().toUpper()},
                {QStringLiteral("taxRate"), basisPointsFromPercent(tax->value())},
                {QStringLiteral("issuedAt"), double(utcMidnight(issued->date()))},
                {QStringLiteral("dueAt"), double(utcMidnight(due->date()))},
                {QStringLiteral("paymentUrl"), payment->text().trimmed()},
                {QStringLiteral("notes"), notes->toPlainText()},
                {QStringLiteral("terms"), terms->toPlainText()},
                {QStringLiteral("items"), linesJson((*readLines)())},
            };
            if (!numberField->text().trimmed().isEmpty())
                body.insert(QStringLiteral("number"), numberField->text().trimmed());
            if (crm)
                body.insert(QStringLiteral("contactId"), contact->currentData().toString());
            if (due->date() < issued->date())
                return showDialogError(error, QStringLiteral("The due date is before the issue date."));
            save->setEnabled(false);
            showDialogError(error, QString());
            const QPointer<QDialog> alive(dialog);
            request(invoiceId.isEmpty() ? QByteArrayLiteral("POST") : QByteArrayLiteral("PATCH"),
                    organizationPath(invoiceId.isEmpty() ? QStringLiteral("/items")
                                                         : QStringLiteral("/items/") + invoiceId),
                    body, [this, alive, save, error](bool ok, const QJsonObject &payload, const QString &message) {
                if (!alive)
                    return;
                save->setEnabled(true);
                if (!ok)
                    return showDialogError(error, QStringLiteral("The invoice was not saved: %1.")
                                                      .arg(readableError(message)));
                const QJsonObject saved = payload.value(QStringLiteral("invoice")).toObject();
                alive->accept();
                invoiceChanged(saved, QStringLiteral("Invoice %1 saved.").arg(text(saved, "number")));
            });
        });
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    });
}

void InvoicesPage::openSendDialog()
{
    const QJsonObject invoice = m_detail;
    if (invoice.isEmpty() || !canSend(invoice))
        return;
    const bool reminder = integer(invoice, "sentAt") > 0;
    const QJsonObject settings = m_overview.value(QStringLiteral("settings")).toObject();
    const QString business = text(settings, "businessName").isEmpty() ? organization() : text(settings, "businessName");
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("invoicesSendDialog"));
    dialog->setWindowTitle(reminder ? QStringLiteral("Remind about %1").arg(text(invoice, "number"))
                                    : QStringLiteral("Email %1").arg(text(invoice, "number")));
    dialog->resize(520, 420);
    auto *layout = new QVBoxLayout(dialog);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *to = new QLineEdit(text(invoice.value(QStringLiteral("client")).toObject(), "email"));
    to->setPlaceholderText(QStringLiteral("client@example.com"));
    auto *subject = new QLineEdit;
    subject->setMaxLength(300);
    subject->setPlaceholderText(QStringLiteral("%1 %2 from %3")
                                    .arg(reminder ? QStringLiteral("Reminder: invoice") : QStringLiteral("Invoice"),
                                         text(invoice, "number"), business));
    QPlainTextEdit *message = textBox(QString(), 8);
    message->setPlaceholderText(QStringLiteral("Leave empty for a short message with the amount and due date."));
    form->addRow(QStringLiteral("To"), to);
    form->addRow(QStringLiteral("Subject"), subject);
    form->addRow(QStringLiteral("Message"), message);
    layout->addLayout(form);
    layout->addWidget(mutedLabel(QStringLiteral(
        "The email carries a private link to view and pay the invoice. You will see when it is opened, viewed "
        "and when the pay link is clicked.")));
    layout->addStretch(1);
    QLabel *error = dialogError();
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox;
    QPushButton *send = buttons->addButton(reminder ? QStringLiteral("Send reminder") : QStringLiteral("Send"),
                                           QDialogButtonBox::AcceptRole);
    send->setObjectName(QStringLiteral("primaryButton"));
    send->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const QString id = text(invoice, "id");
    connect(send, &QPushButton::clicked, dialog, [=] {
        send->setEnabled(false);
        showDialogError(error, QString());
        const QPointer<QDialog> alive(dialog);
        request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/items/%1/send").arg(id)),
                QJsonObject{{QStringLiteral("to"), to->text().trimmed()},
                            {QStringLiteral("subject"), subject->text()},
                            {QStringLiteral("message"), message->toPlainText()}},
                [this, alive, send, error](bool ok, const QJsonObject &payload, const QString &failure) {
            if (!alive)
                return;
            send->setEnabled(true);
            if (!ok)
                return showDialogError(error, QStringLiteral("The invoice was not sent: %1.")
                                                  .arg(readableError(failure)));
            const QJsonObject sent = payload.value(QStringLiteral("invoice")).toObject();
            alive->accept();
            invoiceChanged(sent, QStringLiteral("Invoice %1 emailed to %2.")
                                     .arg(text(sent, "number"), text(sent, "sentTo")));
        });
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void InvoicesPage::openPaymentDialog()
{
    const QJsonObject invoice = m_detail;
    if (invoice.isEmpty() || !canEdit(invoice))
        return;
    const QString currency = text(invoice, "currency");
    const bool draft = canMarkPaid(invoice);
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("invoicesPaymentDialog"));
    dialog->setWindowTitle(draft ? QStringLiteral("Mark %1 paid").arg(text(invoice, "number"))
                                 : QStringLiteral("Record a payment on %1").arg(text(invoice, "number")));
    auto *layout = new QVBoxLayout(dialog);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const qint64 balance = integer(invoice, "balance") > 0 ? integer(invoice, "balance") : integer(invoice, "total");
    auto *amount = new QLineEdit(centsInput(balance));
    amount->setAccessibleName(QStringLiteral("Amount"));
    QDateEdit *date = dateEdit(QDate::currentDate());
    auto *method = new QLineEdit;
    method->setMaxLength(60);
    method->setPlaceholderText(QStringLiteral("Bank transfer, card, cash…"));
    auto *note = new QLineEdit;
    note->setMaxLength(500);
    form->addRow(QStringLiteral("Amount (%1)").arg(currency), amount);
    form->addRow(QStringLiteral("Paid on"), date);
    form->addRow(QStringLiteral("Method"), method);
    form->addRow(QStringLiteral("Note"), note);
    layout->addLayout(form);
    layout->addWidget(mutedLabel(QStringLiteral("%1 is still due. A smaller amount records a part payment.")
                                     .arg(moneyText(balance, currency))));
    QLabel *error = dialogError();
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox;
    QPushButton *save = buttons->addButton(draft ? QStringLiteral("Mark paid") : QStringLiteral("Record payment"),
                                           QDialogButtonBox::AcceptRole);
    save->setObjectName(QStringLiteral("primaryButton"));
    save->setDefault(true);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const QString id = text(invoice, "id");
    connect(save, &QPushButton::clicked, dialog, [=] {
        const qint64 cents = parseCents(amount->text());
        if (cents <= 0)
            return showDialogError(error, QStringLiteral("Enter the amount that was paid."));
        save->setEnabled(false);
        const QPointer<QDialog> alive(dialog);
        request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/items/%1/paid").arg(id)),
                QJsonObject{{QStringLiteral("amount"), double(cents)},
                            {QStringLiteral("paidAt"), double(utcMidnight(date->date()))},
                            {QStringLiteral("method"), method->text()},
                            {QStringLiteral("note"), note->text()}},
                [this, alive, save, error](bool ok, const QJsonObject &payload, const QString &failure) {
            if (!alive)
                return;
            save->setEnabled(true);
            if (!ok)
                return showDialogError(error, QStringLiteral("The payment was not recorded: %1.")
                                                  .arg(readableError(failure)));
            const QJsonObject updated = payload.value(QStringLiteral("invoice")).toObject();
            alive->accept();
            invoiceChanged(updated, text(updated, "status") == QLatin1String("paid")
                ? QStringLiteral("Invoice %1 is paid.").arg(text(updated, "number"))
                : QStringLiteral("Payment recorded; %1 still due.")
                      .arg(moneyText(integer(updated, "balance"), text(updated, "currency"))));
        });
    });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void InvoicesPage::runStatusAction(const QString &action)
{
    const QJsonObject invoice = m_detail;
    if (invoice.isEmpty())
        return;
    if (action == QLatin1String("void") &&
        QMessageBox::question(this, QStringLiteral("Void invoice"),
                              QStringLiteral("Void invoice %1? Its link will show it as void.")
                                  .arg(text(invoice, "number"))) != QMessageBox::Yes)
        return;
    const QString id = text(invoice, "id");
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/items/%1/%2").arg(id, action)), {},
            [this, action](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not %1 the invoice: %2").arg(action, readableError(error)), true);
        const QJsonObject updated = payload.value(QStringLiteral("invoice")).toObject();
        invoiceChanged(updated, action == QLatin1String("void")
                                    ? QStringLiteral("Invoice %1 voided.").arg(text(updated, "number"))
                                    : QStringLiteral("Invoice %1 reopened.").arg(text(updated, "number")));
    });
}

void InvoicesPage::deleteInvoice()
{
    const QJsonObject invoice = m_detail;
    if (invoice.isEmpty() || !canDelete(invoice))
        return;
    if (QMessageBox::question(this, QStringLiteral("Delete invoice"),
                              QStringLiteral("Delete invoice %1? This cannot be undone.").arg(text(invoice, "number")))
        != QMessageBox::Yes)
        return;
    const QString id = text(invoice, "id");
    const QString number = text(invoice, "number");
    request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/items/") + id), {},
            [this, id, number](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Could not delete the invoice: %1").arg(readableError(error)), true);
        for (int index = m_invoices.size() - 1; index >= 0; --index) {
            if (text(m_invoices.at(index).toObject(), "id") == id)
                m_invoices.removeAt(index);
        }
        closeDetail();
        renderList();
        invoiceChanged({}, QStringLiteral("Invoice %1 deleted.").arg(number));
    });
}

void InvoicesPage::duplicateInvoice()
{
    if (!m_detail.isEmpty())
        openEditor({}, duplicateSeed(m_detail));
}

void InvoicesPage::openSettings()
{
    if (!installed())
        return;
    const QJsonObject settings = m_overview.value(QStringLiteral("settings")).toObject();
    const bool manage = canManage();
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("invoicesSettingsDialog"));
    dialog->setWindowTitle(QStringLiteral("Invoicing settings"));
    dialog->resize(560, 640);
    auto *outer = new QVBoxLayout(dialog);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *businessName = new QLineEdit(text(settings, "businessName"));
    businessName->setMaxLength(160);
    QPlainTextEdit *businessAddress = textBox(text(settings, "businessAddress"), 3);
    auto *fromName = new QLineEdit(text(settings, "fromName"));
    fromName->setMaxLength(120);
    auto *fromEmail = new QLineEdit(text(settings, "fromEmail"));
    fromEmail->setPlaceholderText(QStringLiteral("Uses the CRM's sender when empty"));
    auto *currency = new QLineEdit(text(settings, "currency").isEmpty() ? QStringLiteral("USD")
                                                                        : text(settings, "currency"));
    currency->setInputMask(QStringLiteral(">AAA"));
    currency->setFixedWidth(70);
    auto *prefix = new QLineEdit(text(settings, "prefix"));
    prefix->setMaxLength(20);
    auto *nextNumber = new QSpinBox;
    nextNumber->setRange(1, 1000000000);
    nextNumber->setValue(std::max(1, settings.value(QStringLiteral("nextNumber")).toInt(1)));
    auto *dueDays = new QSpinBox;
    dueDays->setRange(0, 365);
    dueDays->setSuffix(QStringLiteral(" days"));
    dueDays->setValue(settings.value(QStringLiteral("dueDays")).toInt(30));
    auto *tax = new QDoubleSpinBox;
    tax->setRange(0, 1000);
    tax->setDecimals(2);
    tax->setSuffix(QStringLiteral(" %"));
    tax->setValue(double(integer(settings, "taxRate")) / 100.0);
    auto *paymentUrl = new QLineEdit(text(settings, "paymentUrl"));
    paymentUrl->setPlaceholderText(QStringLiteral("https://…"));
    QPlainTextEdit *instructions = textBox(text(settings, "paymentInstructions"), 3);
    QPlainTextEdit *footer = textBox(text(settings, "footer"), 2);
    form->addRow(QStringLiteral("Business name"), businessName);
    form->addRow(QStringLiteral("Default currency"), currency);
    form->addRow(QStringLiteral("Business address"), businessAddress);
    form->addRow(QStringLiteral("From name"), fromName);
    form->addRow(QStringLiteral("From email"), fromEmail);
    form->addRow(QStringLiteral("Number prefix"), prefix);
    form->addRow(QStringLiteral("Next number"), nextNumber);
    form->addRow(QStringLiteral("Due after"), dueDays);
    form->addRow(QStringLiteral("Default tax"), tax);
    form->addRow(QStringLiteral("Default pay link"), paymentUrl);
    form->addRow(QStringLiteral("Payment instructions"), instructions);
    form->addRow(QStringLiteral("Footer"), footer);
    for (QWidget *field : std::initializer_list<QWidget *>{businessName, businessAddress, fromName, fromEmail, currency,
                                                           prefix, nextNumber, dueDays, tax, paymentUrl, instructions,
                                                           footer})
        field->setEnabled(manage);
    scroll->setWidget(page);
    outer->addWidget(mutedLabel(manage ? QStringLiteral("Shown on every invoice and invoice email.")
                                       : QStringLiteral("Only an organization owner or admin can change these.")));
    outer->addWidget(scroll, 1);
    QLabel *error = dialogError();
    outer->addWidget(error);
    auto *buttons = new QDialogButtonBox;
    if (manage) {
        QPushButton *save = buttons->addButton(QStringLiteral("Save"), QDialogButtonBox::AcceptRole);
        save->setObjectName(QStringLiteral("primaryButton"));
        save->setDefault(true);
        connect(save, &QPushButton::clicked, dialog, [=] {
            save->setEnabled(false);
            const QPointer<QDialog> alive(dialog);
            request(QByteArrayLiteral("PATCH"), organizationPath(QStringLiteral("/settings")),
                    QJsonObject{{QStringLiteral("businessName"), businessName->text()},
                                {QStringLiteral("businessAddress"), businessAddress->toPlainText()},
                                {QStringLiteral("fromName"), fromName->text()},
                                {QStringLiteral("fromEmail"), fromEmail->text().trimmed()},
                                {QStringLiteral("currency"), currency->text().trimmed().toUpper()},
                                {QStringLiteral("prefix"), prefix->text()},
                                {QStringLiteral("nextNumber"), nextNumber->value()},
                                {QStringLiteral("dueDays"), dueDays->value()},
                                {QStringLiteral("taxRate"), basisPointsFromPercent(tax->value())},
                                {QStringLiteral("paymentUrl"), paymentUrl->text().trimmed()},
                                {QStringLiteral("paymentInstructions"), instructions->toPlainText()},
                                {QStringLiteral("footer"), footer->toPlainText()}},
                    [this, alive, save, error](bool ok, const QJsonObject &payload, const QString &failure) {
                if (!alive)
                    return;
                save->setEnabled(true);
                if (!ok)
                    return showDialogError(error, QStringLiteral("Settings were not saved: %1.")
                                                      .arg(readableError(failure)));
                m_overview.insert(QStringLiteral("settings"), payload.value(QStringLiteral("settings")));
                alive->accept();
                setStatus(QStringLiteral("Invoicing settings saved."));
                renderSummary();
            });
        });
        buttons->addButton(QDialogButtonBox::Cancel);
    } else {
        buttons->addButton(QDialogButtonBox::Close);
    }
    outer->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->open();
}

void InvoicesPage::copyPublicLink()
{
    const QString url = text(m_detail, "publicUrl");
    if (url.isEmpty())
        return;
    QApplication::clipboard()->setText(url);
    setStatus(QStringLiteral("Invoice link copied."));
}

void InvoicesPage::openPublicLink()
{
    const QUrl url(text(m_detail, "publicUrl"));
    if (url.isValid() && url.scheme() == QLatin1String("https"))
        QDesktopServices::openUrl(url);
}

void InvoicesPage::openWebPage()
{
    QString path = QString::fromLatin1(kWebPath);
    QStringList query;
    if (!organization().isEmpty())
        query << QStringLiteral("org=") + organization();
    if (!m_detailId.isEmpty())
        query << QStringLiteral("invoice=") + m_detailId;
    if (!query.isEmpty())
        path += QLatin1Char('?') + query.join(QLatin1Char('&'));
    if (m_host.openWebPage)
        m_host.openWebPage(path);
}

void InvoicesPage::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

}

namespace {

const forkmesh::market::NativePackageRegistration invoicesRegistration({
    QStringLiteral("builtin:invoices"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Invoices"),
    QStringLiteral("credit-card"),
    QStringLiteral("/api/invoices"),
    QStringLiteral("forkmesh-invoices-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::invoices::InvoicesPage(host, parent);
    },
});

}
