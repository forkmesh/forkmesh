// Component health: a dense board of tiny cards, one per source file, showing
// how large each part of this codebase is and where it puts the UI thread at
// risk. Clicking a card drills into that file's functions as a second board.
//
// The scan reads 340k lines, so it never runs on the GUI thread: it goes to a
// worker through runOffThread while a progress row counts files. That row is
// the point as much as the numbers are — work you can see is work that does
// not look like a freeze.
#include "MainWindow.h"
#include "CodeHealth.h"
#include "ComponentHealthBoard.h"
#include "BackgroundActivity.h"
#include "BinarySizeMap.h"
#include "MainWindowInternal.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantMap>

using namespace forkmesh::ui;
using namespace forkmesh::codehealth;

namespace {

QString sourceRoot()
{
    // forkmesh::sourceDir() points at <checkout>/desktop; the scan wants the
    // checkout so paths read as "desktop/src/File.cpp".
    return QDir(forkmesh::sourceDir()).absolutePath();
}

QString diagnosticsRoot()
{
    return forkmesh::paths::diagnosticsDir();
}

enum HealthView {
    ViewSource = 0,
    ViewBinary,
};

QString binaryComponent(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    if (name.startsWith(QLatin1String("qrc_")))
        return QStringLiteral("Embedded resources");
    if (name.startsWith(QLatin1String("forkmesh_autogen")))
        return QStringLiteral("Qt meta-object code");
    if (path.startsWith(QLatin1String("marketplace/packages/")))
        return QStringLiteral("Marketplace · ") + path.section(QLatin1Char('/'), 2, 2);
    if (path.startsWith(QLatin1String("src/")))
        return componentFor(path);
    if (path.startsWith(QLatin1String("generated/")))
        return QStringLiteral("Generated");
    const QString dir = QFileInfo(path).path();
    return dir == QLatin1String(".") ? QStringLiteral("Core") : dir;
}

// Sort orders offered in the header. "Needs attention" is the default and is
// what the board is for; the rest are the questions people actually ask next
// ("what did I just touch?", "what is biggest?", "where does the time go?").
enum SortMode {
    SortNeedsAttention = 0,
    SortStallTime,
    SortFlags,
    SortLargest,
    SortRecentlyChanged,
    SortName,
    SortComponent,
};

void populateSortModes(QComboBox *combo)
{
    combo->addItem(QStringLiteral("Needs attention"), SortNeedsAttention);
    combo->addItem(QStringLiteral("Most stall time"), SortStallTime);
    combo->addItem(QStringLiteral("Most flags"), SortFlags);
    combo->addItem(QStringLiteral("Largest"), SortLargest);
    combo->addItem(QStringLiteral("Recently changed"), SortRecentlyChanged);
    combo->addItem(QStringLiteral("Name"), SortName);
    combo->addItem(QStringLiteral("Component"), SortComponent);
}

QVariantMap nextHealthColors()
{
    if (!Theme::isNext())
        return {};
    const struct {
        const char *key;
        Theme::Role role;
    } roles[] = {
        {"surface", Theme::Role::Surface}, {"surfaceRaised", Theme::Role::SurfaceRaised},
        {"border", Theme::Role::Border},   {"fg", Theme::Role::Fg},
        {"fgMuted", Theme::Role::FgMuted}, {"fgSubtle", Theme::Role::FgSubtle},
        {"success", Theme::Role::Success}, {"warning", Theme::Role::Warning},
        {"danger", Theme::Role::Danger},
    };
    QVariantMap map;
    for (const auto &entry : roles)
        map.insert(QLatin1String(entry.key), Theme::color(entry.role));
    return map;
}

void applyNextHealthColors(QWidget *widget)
{
    if (!widget)
        return;
    const QVariantMap map = nextHealthColors();
    if (widget->property("nextColors").toMap() != map)
        widget->setProperty("nextColors", map);
}

class NextHealthColorSync : public QObject
{
public:
    explicit NextHealthColorSync(QWidget *target) : QObject(target), m_target(target)
    {
        target->installEventFilter(this);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_target &&
            (event->type() == QEvent::StyleChange || event->type() == QEvent::PaletteChange ||
             event->type() == QEvent::Show))
            applyNextHealthColors(m_target);
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *m_target;
};

QLabel *healthLabel(const QString &role, const QString &text = QString())
{
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setProperty("healthRole", role);
    return label;
}

QFrame *healthStatTile(const QString &key, const QString &title)
{
    auto *tile = new QFrame;
    tile->setObjectName(QStringLiteral("healthStatTile"));
    tile->setProperty("tileKey", key);
    auto *layout = new QVBoxLayout(tile);
    layout->setContentsMargins(16, 12, 16, 12);
    layout->setSpacing(4);
    layout->addWidget(healthLabel(QStringLiteral("tileLabel"), title));
    auto *value = healthLabel(QStringLiteral("tileValue"));
    value->setObjectName(QStringLiteral("healthStatValue"));
    layout->addWidget(value);
    auto *hint = healthLabel(QStringLiteral("tileHint"));
    hint->setObjectName(QStringLiteral("healthStatHint"));
    layout->addWidget(hint);
    return tile;
}

void setHealthStatTile(QWidget *root, const QString &key, const QString &value,
                       const QString &hint)
{
    for (QFrame *tile : root->findChildren<QFrame *>(QStringLiteral("healthStatTile"))) {
        if (tile->property("tileKey").toString() != key)
            continue;
        if (auto *label = tile->findChild<QLabel *>(QStringLiteral("healthStatValue")))
            label->setText(value);
        if (auto *label = tile->findChild<QLabel *>(QStringLiteral("healthStatHint")))
            label->setText(hint);
    }
}

void syncBinaryTiles(QWidget *root, const forkmesh::binarysize::Report &report)
{
    QWidget *row = root ? root->findChild<QWidget *>(QStringLiteral("healthBinaryTiles")) : nullptr;
    if (!row)
        return;
    row->setVisible(!report.nodes.isEmpty());
    if (report.nodes.isEmpty())
        return;
    using forkmesh::binarysize::formatBytes;
    const forkmesh::binarysize::Node &top = report.nodes.at(0);
    setHealthStatTile(root, QStringLiteral("total"), formatBytes(report.fileSize), top.name);
    setHealthStatTile(root, QStringLiteral("symbols"), QLocale().toString(report.symbolCount),
                      report.fromSymtab ? QStringLiteral("from the symbol table")
                                        : QStringLiteral("stripped: exported symbols only"));
    setHealthStatTile(root, QStringLiteral("objects"),
                      report.objectsListed > 0 ? QLocale().toString(report.objectCount)
                                               : QString::fromUtf8("\xE2\x80\x94"),
                      report.objectsListed > 0
                          ? QStringLiteral("of %1 object files").arg(report.objectsListed)
                          : QStringLiteral("no build folder"));
    int largest = -1;
    for (int child : top.children) {
        if (child >= 0 && child < report.nodes.size() &&
            (largest < 0 || report.nodes.at(child).bytes > report.nodes.at(largest).bytes))
            largest = child;
    }
    if (largest >= 0) {
        const qint64 bytes = report.nodes.at(largest).bytes;
        setHealthStatTile(
            root, QStringLiteral("largest"), report.nodes.at(largest).name,
            QStringLiteral("%1, %2%")
                .arg(formatBytes(bytes))
                .arg(report.fileSize > 0 ? 100.0 * double(bytes) / double(report.fileSize) : 0.0,
                     0, 'f', 0));
    } else {
        setHealthStatTile(root, QStringLiteral("largest"), QString::fromUtf8("\xE2\x80\x94"),
                          QString());
    }
}

void clearHealthLayout(QLayout *layout)
{
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
}

QFrame *healthDetailRow(QWidget *left, QWidget *right, bool first)
{
    auto *row = new QFrame;
    row->setObjectName(QStringLiteral("healthDetailRow"));
    row->setProperty("first", first);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(8);
    layout->addWidget(left, 1);
    if (right)
        layout->addWidget(right, 0, Qt::AlignRight | Qt::AlignVCenter);
    return row;
}

QString healthTone(int health)
{
    if (health >= 80)
        return QStringLiteral("success");
    if (health >= 55)
        return QStringLiteral("warning");
    return QStringLiteral("danger");
}

QString healthVerdict(int health)
{
    if (health >= 80)
        return QStringLiteral("Healthy");
    if (health >= 55)
        return QStringLiteral("Needs attention");
    return QStringLiteral("At risk");
}

void repolish(QWidget *widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

void syncHealthDetailPanel(QWidget *page, const FileHealth *file)
{
    auto *panel = page ? page->findChild<QFrame *>(QStringLiteral("healthDetailPanel")) : nullptr;
    if (!panel)
        return;
    if (!file) {
        panel->setVisible(false);
        return;
    }
    if (auto *icon = panel->findChild<QLabel *>(QStringLiteral("healthDetailIcon")))
        icon->setPixmap(kit::tintedIcon(QStringLiteral("file"), Theme::color(Theme::Role::FgMuted),
                                        16, icon->devicePixelRatioF()));
    if (auto *name = panel->findChild<QLabel *>(QStringLiteral("healthDetailName")))
        name->setText(file->display);
    if (auto *path = panel->findChild<QLabel *>(QStringLiteral("healthDetailPath")))
        path->setText(file->path);
    if (auto *meta = panel->findChild<QLabel *>(QStringLiteral("healthDetailMeta")))
        meta->setText(QStringLiteral("%1 \u00b7 %2 lines")
                          .arg(file->component, QLocale().toString(file->loc)));
    if (auto *score = panel->findChild<QLabel *>(QStringLiteral("healthDetailScore"))) {
        score->setText(QString::number(file->health));
        score->setProperty("tone", healthTone(file->health));
        repolish(score);
    }
    if (auto *verdict = panel->findChild<QLabel *>(QStringLiteral("healthDetailVerdict")))
        verdict->setText(healthVerdict(file->health));

    if (auto *facts = panel->findChild<QFrame *>(QStringLiteral("healthDetailFacts"))) {
        clearHealthLayout(facts->layout());
        QList<QPair<QString, QString>> rows{
            {QStringLiteral("Code lines"), QLocale().toString(file->codeLoc)},
            {QStringLiteral("Functions"), QLocale().toString(file->functionCount)},
            {QStringLiteral("Longest function"),
             QStringLiteral("%1 lines").arg(QLocale().toString(file->longestFunction))},
            {QStringLiteral("Stall attributed"),
             file->stallMs > 0 ? QStringLiteral("%1 ms").arg(QLocale().toString(file->stallMs))
                               : QStringLiteral("None")},
            {QStringLiteral("Acknowledged in code"), QLocale().toString(file->acknowledgedCount)},
        };
        if (file->modifiedMs > 0)
            rows.append({QStringLiteral("Last changed"),
                         QDateTime::fromMSecsSinceEpoch(file->modifiedMs)
                             .toString(QStringLiteral("yyyy-MM-dd HH:mm"))});
        bool first = true;
        for (const auto &[key, value] : rows) {
            facts->layout()->addWidget(healthDetailRow(healthLabel(QStringLiteral("factKey"), key),
                                                       healthLabel(QStringLiteral("factValue"), value),
                                                       first));
            first = false;
        }
    }

    QStringList kinds;
    for (auto it = file->findingKinds.constBegin(); it != file->findingKinds.constEnd(); ++it)
        kinds.append(QStringLiteral("%1 \u00d7 %2").arg(it.value()).arg(it.key()));
    kinds.sort();
    if (auto *heading = panel->findChild<QLabel *>(QStringLiteral("healthDetailFlagsHeading")))
        heading->setText(QStringLiteral("FLAGS \u00b7 %1").arg(file->findingCount));
    if (auto *flags = panel->findChild<QFrame *>(QStringLiteral("healthDetailFlags"))) {
        clearHealthLayout(flags->layout());
        if (kinds.isEmpty()) {
            auto *empty = healthLabel(QStringLiteral("empty"),
                                      QStringLiteral("Nothing flagged in this file"));
            empty->setAlignment(Qt::AlignCenter);
            flags->layout()->addWidget(healthDetailRow(empty, nullptr, true));
        }
        bool first = true;
        for (const QString &kind : std::as_const(kinds)) {
            auto *label = healthLabel(QStringLiteral("flag"), kind);
            label->setWordWrap(true);
            flags->layout()->addWidget(healthDetailRow(label, nullptr, first));
            first = false;
        }
    }
    panel->setVisible(true);
}

QWidget *healthPage(QWidget *stack)
{
    return stack ? stack->parentWidget() : nullptr;
}

} // namespace

QWidget *MainWindow::buildComponentHealthSection()
{
    const bool next = Theme::isNext();
    auto *page = new QWidget;
    page->setObjectName(QStringLiteral("componentHealthSection"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    QVBoxLayout *titleColumn = nullptr;
    QHBoxLayout *tabRow = nullptr;
    QVBoxLayout *leftColumn = nullptr;
    QHBoxLayout *body = nullptr;
    if (next) {
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        heading->setContentsMargins(24, 12, 24, 2);
        heading->setSpacing(6);
        outer->addLayout(heading);
        titleColumn = new QVBoxLayout;
        titleColumn->setContentsMargins(0, 0, 0, 0);
        titleColumn->setSpacing(2);
        heading->addLayout(titleColumn, 1);
        heading->addSpacing(12);
        tabRow = new QHBoxLayout;
        tabRow->setContentsMargins(14, 0, 24, 0);
        tabRow->setSpacing(4);
        outer->addLayout(tabRow);
        auto *divider = new QFrame;
        divider->setObjectName(QStringLiteral("healthHeaderDivider"));
        divider->setFrameShape(QFrame::NoFrame);
        divider->setFixedHeight(1);
        outer->addWidget(divider);
        body = new QHBoxLayout;
        body->setContentsMargins(0, 0, 0, 0);
        body->setSpacing(0);
        outer->addLayout(body, 1);
        leftColumn = new QVBoxLayout;
        leftColumn->setContentsMargins(0, 0, 0, 0);
        leftColumn->setSpacing(0);
        body->addLayout(leftColumn, 1);
    }
    auto *title = new QLabel(QStringLiteral("Component health"));
    title->setObjectName(QStringLiteral("pageTitle"));
    title->setToolTip(QStringLiteral(
        "One tiny card per source file: how many lines it holds, how much UI "
        "stall time the diagnostics log attributes to it, and how many "
        "patterns it contains that have frozen this app before — waiting on a "
        "subprocess inside a loop, calling setStyleSheet per widget, building "
        "QSettings or a QRegularExpression per iteration, pumping the event "
        "loop inside a loop body, walking the filesystem per iteration, or "
        "nesting loops three deep. Click a card to see that file's functions. "
        "The score is 0-100 and is always printed, so colour never carries "
        "the meaning on its own."));
    if (next)
        titleColumn->addWidget(title);
    else
        heading->addWidget(title);

    m_componentHealthViews = new QButtonGroup(page);
    m_componentHealthViews->setExclusive(true);
    const struct {
        const char *label;
        const char *tip;
        int view;
    } views[] = {
        {"Source files", "One card per source file: size, stall time and risky patterns",
         ViewSource},
        {"Binary size",
         "A treemap of the compiled app: every component, file and symbol sized by the "
         "bytes it takes up in the binary",
         ViewBinary},
    };
    for (const auto &view : views) {
        auto *button = new QPushButton(QString::fromUtf8(view.label));
        button->setObjectName(QStringLiteral("ghostButton"));
        button->setCheckable(true);
        button->setChecked(view.view == ViewSource);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(QString::fromUtf8(view.tip));
        m_componentHealthViews->addButton(button, view.view);
        if (next) {
            button->setProperty("healthRole", QStringLiteral("tab"));
            tabRow->addWidget(button);
        } else {
            heading->addWidget(button);
        }
    }
    if (next)
        tabRow->addStretch(1);
    connect(m_componentHealthViews, &QButtonGroup::idClicked, this,
            [this](int view) { showComponentHealthView(view); });

    m_componentHealthStatus = new QLabel;
    m_componentHealthStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_componentHealthStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (next) {
        m_componentHealthStatus->setProperty("healthRole", QStringLiteral("summary"));
        m_componentHealthStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        titleColumn->addWidget(m_componentHealthStatus);
    } else {
        heading->addWidget(m_componentHealthStatus);
        heading->addStretch();
    }

    m_componentHealthSourceTools = new QWidget;
    auto *sourceTools = new QHBoxLayout(m_componentHealthSourceTools);
    sourceTools->setContentsMargins(0, 0, 0, 0);
    sourceTools->setSpacing(heading->spacing());
    if (next) {
        m_componentHealthSourceTools->setProperty("healthRole", QStringLiteral("toolbar"));
        m_componentHealthSourceTools->setAttribute(Qt::WA_StyledBackground, true);
        sourceTools->setContentsMargins(24, 8, 24, 8);
        sourceTools->setSpacing(8);
    }

    m_componentHealthBack = new QPushButton(QStringLiteral("All files"));
    m_componentHealthBack->setObjectName(QStringLiteral("ghostButton"));
    m_componentHealthBack->setCursor(Qt::PointingHandCursor);
    m_componentHealthBack->setToolTip(
        QStringLiteral("Back to every file in the codebase"));
    if (next) {
        m_componentHealthBack->setProperty("healthRole", QStringLiteral("action"));
        m_componentHealthBack->setProperty("nextIconRole", int(Theme::Role::FgMuted));
    }
    setOcticon(m_componentHealthBack, "arrow-left", 14);
    m_componentHealthBack->setVisible(false);
    connect(m_componentHealthBack, &QPushButton::clicked, this,
            [this] { showComponentHealthOverview(); });
    sourceTools->addWidget(m_componentHealthBack);

    m_componentHealthFilter = new QLineEdit;
    m_componentHealthFilter->setPlaceholderText(QStringLiteral("Filter…"));
    m_componentHealthFilter->setClearButtonEnabled(true);
    m_componentHealthFilter->setFixedWidth(next ? 224 : 180);
    if (next)
        m_componentHealthFilter->addAction(
            QIcon(kit::tintedIcon(QStringLiteral("search"), Theme::color(Theme::Role::FgSubtle),
                                  14, m_componentHealthFilter->devicePixelRatioF())),
            QLineEdit::LeadingPosition);
    m_componentHealthFilter->setToolTip(QStringLiteral(
        "Show only cards whose name, component or path contains this text"));
    connect(m_componentHealthFilter, &QLineEdit::textChanged, this,
            [this](const QString &text) {
                if (!m_componentHealthBoard)
                    return;
                m_componentHealthBoard->setFilter(text);
                updateComponentHealthStatus();
            });
    sourceTools->addWidget(m_componentHealthFilter);

    m_componentHealthSort = new QComboBox;
    populateSortModes(m_componentHealthSort);
    m_componentHealthSort->setToolTip(QStringLiteral(
        "Order the cards. \"Needs attention\" puts the lowest health score "
        "first; \"Recently changed\" uses each file's modification time, so "
        "it shows what you have just been working on."));
    connect(m_componentHealthSort, &QComboBox::currentIndexChanged, this,
            [this](int) {
                if (m_componentHealthOpenFile.isEmpty())
                    showComponentHealthOverview();
                else
                    showComponentHealthFile(m_componentHealthOpenFile);
            });
    if (next) {
        sourceTools->addStretch(1);
        auto *count = new QLabel;
        count->setObjectName(QStringLiteral("healthCardCount"));
        sourceTools->addWidget(count);
    }
    sourceTools->addWidget(m_componentHealthSort);
    if (next)
        leftColumn->addWidget(m_componentHealthSourceTools);
    else
        heading->addWidget(m_componentHealthSourceTools);

    auto *rescan = new QPushButton(QStringLiteral("Rescan"));
    rescan->setObjectName(QStringLiteral("ghostButton"));
    rescan->setCursor(Qt::PointingHandCursor);
    rescan->setToolTip(QStringLiteral(
        "Read the source tree and the stall log, or the compiled binary, again on a "
        "worker thread"));
    if (next) {
        rescan->setProperty("healthRole", QStringLiteral("action"));
        rescan->setProperty("nextIconRole", int(Theme::Role::FgMuted));
    }
    setOcticon(rescan, "sync", 14);
    connect(rescan, &QPushButton::clicked, this, [this] {
        if (m_componentHealthStack && m_componentHealthStack->currentIndex() == ViewBinary)
            refreshBinarySize(true);
        else
            refreshComponentHealth(/*force=*/true);
    });
    heading->addWidget(rescan);
    if (!next)
        outer->addLayout(heading);

    // The visible-work row. Hidden when idle so the page is not noisy, shown
    // for the whole scan with a real count behind it.
    m_componentHealthProgressRow = new QWidget;
    auto *progressLayout = new QHBoxLayout(m_componentHealthProgressRow);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(8);
    m_componentHealthProgress = new QProgressBar;
    m_componentHealthProgress->setTextVisible(false);
    m_componentHealthProgress->setFixedHeight(6);
    m_componentHealthProgress->setMaximumWidth(260);
    progressLayout->addWidget(m_componentHealthProgress);
    m_componentHealthProgressLabel = new QLabel;
    m_componentHealthProgressLabel->setObjectName(QStringLiteral("mutedLabel"));
    progressLayout->addWidget(m_componentHealthProgressLabel);
    progressLayout->addStretch();
    m_componentHealthProgressRow->setVisible(false);
    if (next) {
        progressLayout->setContentsMargins(24, 10, 24, 0);
        leftColumn->addWidget(m_componentHealthProgressRow);
    } else {
        outer->addWidget(m_componentHealthProgressRow);
    }

    m_componentHealthBoard = new ComponentHealthBoard;
    new NextHealthColorSync(m_componentHealthBoard);
    applyNextHealthColors(m_componentHealthBoard);
    m_componentHealthBoard->setOnActivate([this](const QString &id) {
        if (m_componentHealthOpenFile.isEmpty())
            showComponentHealthFile(id);
    });

    m_componentHealthScroll = new QScrollArea;
    // widgetResizable keeps the board at the viewport width and scrolls on the
    // minimum height the board sets for itself. Deliberately not an event
    // filter on MainWindow: eventFilter() is already one of the hottest paths
    // in the app and has its own stall records.
    m_componentHealthScroll->setWidgetResizable(true);
    m_componentHealthScroll->setFrameShape(QFrame::NoFrame);
    m_componentHealthScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_componentHealthScroll->setWidget(m_componentHealthBoard);

    auto *binaryPage = new QWidget;
    auto *binaryLayout = new QVBoxLayout(binaryPage);
    binaryLayout->setContentsMargins(0, 0, 0, 0);
    binaryLayout->setSpacing(8);
    if (next) {
        binaryLayout->setContentsMargins(24, 20, 24, 24);
        binaryLayout->setSpacing(12);
        auto *tiles = new QFrame;
        tiles->setObjectName(QStringLiteral("healthBinaryTiles"));
        tiles->setFrameShape(QFrame::NoFrame);
        auto *tilesLayout = new QHBoxLayout(tiles);
        tilesLayout->setContentsMargins(0, 0, 0, 4);
        tilesLayout->setSpacing(12);
        tilesLayout->addWidget(healthStatTile(QStringLiteral("total"),
                                              QStringLiteral("Total binary size")), 1);
        tilesLayout->addWidget(healthStatTile(QStringLiteral("symbols"),
                                              QStringLiteral("Symbols")), 1);
        tilesLayout->addWidget(healthStatTile(QStringLiteral("objects"),
                                              QStringLiteral("Object files")), 1);
        tilesLayout->addWidget(healthStatTile(QStringLiteral("largest"),
                                              QStringLiteral("Largest part")), 1);
        tiles->setVisible(false);
        binaryLayout->addWidget(tiles);
    }
    m_binarySizeCrumbs = new QLabel;
    m_binarySizeCrumbs->setObjectName(QStringLiteral("mutedLabel"));
    m_binarySizeCrumbs->setTextFormat(Qt::RichText);
    m_binarySizeCrumbs->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    if (next)
        m_binarySizeCrumbs->setProperty("healthRole", QStringLiteral("crumbs"));
    connect(m_binarySizeCrumbs, &QLabel::linkActivated, this, [this](const QString &link) {
        if (m_binarySizeMap)
            m_binarySizeMap->setFocusNode(link.toInt());
    });
    binaryLayout->addWidget(m_binarySizeCrumbs);
    m_binarySizeMap = new BinarySizeMap;
    m_binarySizeMap->setOnFocusChanged([this](int focus) { updateBinarySizeCrumbs(focus); });
    binaryLayout->addWidget(m_binarySizeMap, 1);

    m_componentHealthStack = new QStackedWidget;
    m_componentHealthStack->insertWidget(ViewSource, m_componentHealthScroll);
    m_componentHealthStack->insertWidget(ViewBinary, binaryPage);
    if (!next) {
        outer->addWidget(m_componentHealthStack, 1);
        return page;
    }
    leftColumn->addWidget(m_componentHealthStack, 1);

    auto *panel = new QFrame;
    panel->setObjectName(QStringLiteral("healthDetailPanel"));
    panel->setFixedWidth(360);
    auto *panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(0);
    auto *head = new QFrame;
    head->setObjectName(QStringLiteral("healthDetailHead"));
    head->setFixedHeight(48);
    auto *headLayout = new QHBoxLayout(head);
    headLayout->setContentsMargins(16, 0, 10, 0);
    headLayout->setSpacing(8);
    auto *icon = new QLabel;
    icon->setObjectName(QStringLiteral("healthDetailIcon"));
    icon->setFixedSize(16, 16);
    headLayout->addWidget(icon);
    auto *name = new QLabel;
    name->setObjectName(QStringLiteral("healthDetailName"));
    name->setTextFormat(Qt::PlainText);
    name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    headLayout->addWidget(name, 1);
    auto *close = new QPushButton;
    close->setObjectName(QStringLiteral("healthDetailClose"));
    close->setCursor(Qt::PointingHandCursor);
    close->setToolTip(QStringLiteral("Back to every file in the codebase"));
    close->setFixedSize(28, 28);
    close->setProperty("nextIconRole", int(Theme::Role::FgMuted));
    setOcticon(close, "x", 14);
    connect(close, &QPushButton::clicked, this, [this] { showComponentHealthOverview(); });
    headLayout->addWidget(close);
    panelLayout->addWidget(head);

    auto *detailScroll = new QScrollArea;
    detailScroll->setObjectName(QStringLiteral("healthDetailScroll"));
    detailScroll->setFrameShape(QFrame::NoFrame);
    detailScroll->setWidgetResizable(true);
    detailScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *detail = new QWidget;
    detail->setObjectName(QStringLiteral("healthDetailBody"));
    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(16, 16, 16, 16);
    detailLayout->setSpacing(20);

    auto *where = new QVBoxLayout;
    where->setSpacing(4);
    auto *path = new QLabel;
    path->setObjectName(QStringLiteral("healthDetailPath"));
    path->setTextFormat(Qt::PlainText);
    path->setWordWrap(true);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setFont(Theme::monospaceFont(path->font()));
    where->addWidget(path);
    auto *meta = new QLabel;
    meta->setObjectName(QStringLiteral("healthDetailMeta"));
    meta->setTextFormat(Qt::PlainText);
    meta->setWordWrap(true);
    where->addWidget(meta);
    detailLayout->addLayout(where);

    auto *scoreRow = new QHBoxLayout;
    scoreRow->setSpacing(12);
    auto *score = new QLabel;
    score->setObjectName(QStringLiteral("healthDetailScore"));
    scoreRow->addWidget(score, 0, Qt::AlignBottom);
    auto *verdict = new QLabel;
    verdict->setObjectName(QStringLiteral("healthDetailVerdict"));
    scoreRow->addWidget(verdict, 1, Qt::AlignBottom);
    detailLayout->addLayout(scoreRow);

    auto *facts = new QFrame;
    facts->setObjectName(QStringLiteral("healthDetailFacts"));
    facts->setProperty("healthCard", true);
    auto *factsLayout = new QVBoxLayout(facts);
    factsLayout->setContentsMargins(0, 0, 0, 0);
    factsLayout->setSpacing(0);
    detailLayout->addWidget(facts);

    auto *flagsBlock = new QVBoxLayout;
    flagsBlock->setSpacing(8);
    auto *flagsHeading = new QLabel;
    flagsHeading->setObjectName(QStringLiteral("healthDetailFlagsHeading"));
    flagsBlock->addWidget(flagsHeading);
    auto *flags = new QFrame;
    flags->setObjectName(QStringLiteral("healthDetailFlags"));
    flags->setProperty("healthCard", true);
    auto *flagsLayout = new QVBoxLayout(flags);
    flagsLayout->setContentsMargins(0, 0, 0, 0);
    flagsLayout->setSpacing(0);
    flagsBlock->addWidget(flags);
    detailLayout->addLayout(flagsBlock);
    detailLayout->addStretch(1);

    detailScroll->setWidget(detail);
    panelLayout->addWidget(detailScroll, 1);
    panel->setVisible(false);
    body->addWidget(panel);
    return page;
}

void MainWindow::showComponentHealthView(int view)
{
    if (!m_componentHealthStack)
        return;
    m_componentHealthStack->setCurrentIndex(view);
    if (m_componentHealthViews && m_componentHealthViews->button(view))
        m_componentHealthViews->button(view)->setChecked(true);
    if (m_componentHealthSourceTools)
        m_componentHealthSourceTools->setVisible(view == ViewSource);
    if (view == ViewBinary)
        refreshBinarySize();
    if (QWidget *page = healthPage(m_componentHealthStack);
        page && page->findChild<QFrame *>(QStringLiteral("healthDetailPanel"))) {
        const FileHealth *open = nullptr;
        if (view == ViewSource && !m_componentHealthOpenFile.isEmpty()) {
            for (const FileHealth &file : std::as_const(m_componentHealthReport.files))
                if (file.path == m_componentHealthOpenFile)
                    open = &file;
        }
        syncHealthDetailPanel(page, open);
    }
    updateComponentHealthStatus();
}

void MainWindow::updateBinarySizeCrumbs(int focus)
{
    if (!m_binarySizeCrumbs || !m_binarySizeMap)
        return;
    const forkmesh::binarysize::Report &report = m_binarySizeMap->report();
    if (report.nodes.isEmpty()) {
        m_binarySizeCrumbs->setText(QString());
        return;
    }
    QStringList parts;
    const QVector<int> chain = m_binarySizeMap->chainTo(focus);
    for (int index : chain) {
        const QString name = report.nodes.at(index).name.toHtmlEscaped();
        parts << (index == focus ? QStringLiteral("<b>%1</b>").arg(name)
                                 : QStringLiteral("<a href=\"%1\">%2</a>").arg(index).arg(name));
    }
    const qint64 bytes = report.nodes.at(focus).bytes;
    m_binarySizeCrumbs->setText(
        parts.join(QStringLiteral(" / ")) +
        QStringLiteral(" &nbsp;·&nbsp; %1 · %2% of the binary · click a block to zoom in, "
                       "right-click to zoom out")
            .arg(forkmesh::binarysize::formatBytes(bytes))
            .arg(report.fileSize > 0 ? 100.0 * double(bytes) / double(report.fileSize) : 0.0,
                 0, 'f', 1));
}

void MainWindow::refreshBinarySize(bool force)
{
    if (m_binarySizeScanning || !m_binarySizeMap)
        return;
    if (!force && !m_binarySizeMap->report().isEmpty())
        return;
    m_binarySizeScanning = true;
    updateComponentHealthStatus();

    forkmesh::binarysize::Options options;
    options.binaryPath = QCoreApplication::applicationFilePath();
    options.buildDir = QFileInfo(options.binaryPath).absolutePath();
    options.targetName = QFileInfo(options.binaryPath).completeBaseName();
    options.sourceRoot = QDir::cleanPath(sourceRoot() + QStringLiteral("/.."));
    options.componentOf = binaryComponent;
    runOffThread<forkmesh::binarysize::Report>(
        [options]() {
            const forkmesh::BackgroundScope activity(
                QStringLiteral("binary size"),
                QStringLiteral("reading %1 for the binary size map")
                    .arg(QFileInfo(options.binaryPath).fileName()));
            return forkmesh::binarysize::analyse(options);
        },
        [this](forkmesh::binarysize::Report report) {
            m_binarySizeScanning = false;
            if (m_binarySizeMap)
                m_binarySizeMap->setReport(std::move(report));
            if (m_binarySizeMap)
                syncBinaryTiles(healthPage(m_componentHealthStack), m_binarySizeMap->report());
            updateComponentHealthStatus();
        });
}

void MainWindow::updateComponentHealthStatus()
{
    if (!m_componentHealthStatus)
        return;
    if (m_componentHealthStack && m_componentHealthStack->currentIndex() == ViewBinary) {
        if (m_binarySizeScanning) {
            m_componentHealthStatus->setText(QStringLiteral("reading the binary…"));
            return;
        }
        const forkmesh::binarysize::Report &binary = m_binarySizeMap->report();
        if (binary.nodes.isEmpty()) {
            m_componentHealthStatus->setText(binary.error);
            return;
        }
        QString text = QStringLiteral("%1 · %2 symbols")
                           .arg(forkmesh::binarysize::formatBytes(binary.fileSize))
                           .arg(binary.symbolCount);
        if (binary.objectsListed > 0) {
            text += QStringLiteral(" · %1 of %2 object files")
                        .arg(binary.objectCount)
                        .arg(binary.objectsListed);
        } else {
            text += QStringLiteral(" · no build folder, so files are named from the symbol "
                                   "table only");
        }
        if (!binary.fromSymtab)
            text += QStringLiteral(" · stripped: exported symbols only");
        text += QStringLiteral(" · read in %1 ms").arg(binary.scanMs);
        m_componentHealthStatus->setText(text);
        return;
    }
    if (m_componentHealthScanning) {
        m_componentHealthStatus->setText(QStringLiteral("scanning…"));
        return;
    }
    const Report &report = m_componentHealthReport;
    if (report.files.isEmpty()) {
        m_componentHealthStatus->setText(QString());
        return;
    }
    QString text =
        QStringLiteral("%1 files · %2 lines · %3 functions · scanned in %4 ms")
            .arg(report.files.size())
            .arg(report.totalLoc)
            .arg(report.totalFunctions)
            .arg(report.scanMs);
    if (report.acknowledgedCount > 0) {
        // Exemptions are counted out loud so "no flags" cannot hide them.
        text += QStringLiteral(" · %1 acknowledged in code")
                    .arg(report.acknowledgedCount);
    }
    if (report.stallRecords > 0) {
        // Say plainly how much of the stall time belongs to nobody: most
        // samples name only Qt and libc frames, and spreading that over the
        // code would invent blame.
        text += QStringLiteral(" · %1 stall records, %2 s total, %3 s with no "
                               "app frame")
                    .arg(report.stallRecords)
                    .arg(report.stallTotalMs / 1000)
                    .arg(report.stallUnattributedMs / 1000);
        if (report.stallStarvedMs > 0) {
            text += QStringLiteral(", %1 s while the machine was starved")
                        .arg(report.stallStarvedMs / 1000);
        }
    }
    if (m_componentHealthBoard && m_componentHealthFilter &&
        !m_componentHealthFilter->text().trimmed().isEmpty()) {
        text += QStringLiteral(" · %1 shown")
                    .arg(m_componentHealthBoard->visibleCount());
    }
    m_componentHealthStatus->setText(text);
    if (QWidget *page = healthPage(m_componentHealthStack); page && m_componentHealthBoard) {
        if (auto *count = page->findChild<QLabel *>(QStringLiteral("healthCardCount"))) {
            const int shown = m_componentHealthBoard->visibleCount();
            const bool functions = !m_componentHealthOpenFile.isEmpty();
            count->setText(QStringLiteral("%1 %2").arg(shown).arg(
                functions ? (shown == 1 ? QStringLiteral("function") : QStringLiteral("functions"))
                          : (shown == 1 ? QStringLiteral("file") : QStringLiteral("files"))));
        }
    }
}

void MainWindow::showComponentHealthOverview()
{
    m_componentHealthOpenFile.clear();
    if (m_componentHealthBack)
        m_componentHealthBack->setVisible(false);
    syncHealthDetailPanel(healthPage(m_componentHealthStack), nullptr);
    if (!m_componentHealthBoard)
        return;
    const int mode = m_componentHealthSort
                         ? m_componentHealthSort->currentData().toInt()
                         : SortNeedsAttention;
    QVector<FileHealth> ordered = m_componentHealthReport.files;
    std::sort(ordered.begin(), ordered.end(),
              [mode](const FileHealth &a, const FileHealth &b) {
                  switch (mode) {
                  case SortStallTime:
                      if (a.stallMs != b.stallMs)
                          return a.stallMs > b.stallMs;
                      break;
                  case SortFlags:
                      if (a.findingCount != b.findingCount)
                          return a.findingCount > b.findingCount;
                      break;
                  case SortLargest:
                      if (a.loc != b.loc)
                          return a.loc > b.loc;
                      break;
                  case SortRecentlyChanged:
                      if (a.modifiedMs != b.modifiedMs)
                          return a.modifiedMs > b.modifiedMs;
                      break;
                  case SortName:
                      if (a.display != b.display)
                          return a.display.compare(b.display,
                                                   Qt::CaseInsensitive) < 0;
                      break;
                  case SortComponent:
                      if (a.component != b.component)
                          return a.component.compare(b.component,
                                                     Qt::CaseInsensitive) < 0;
                      break;
                  default:
                      break;
                  }
                  // Health is the tie-break for every order, so equal cards
                  // still lead with the ones worth looking at.
                  if (a.health != b.health)
                      return a.health < b.health;
                  return a.path < b.path;
              });

    QVector<HealthCard> cards;
    cards.reserve(ordered.size());
    for (const FileHealth &file : std::as_const(ordered)) {
        HealthCard card;
        card.id = file.path;
        card.title = file.display;
        card.subtitle = file.component;
        card.loc = file.loc;
        card.stallMs = file.stallMs;
        card.findings = file.findingCount;
        card.health = file.health;
        card.drillable = true;
        QStringList kinds;
        for (auto it = file.findingKinds.constBegin();
             it != file.findingKinds.constEnd(); ++it) {
            kinds.append(QStringLiteral("%1 × %2").arg(it.value()).arg(it.key()));
        }
        kinds.sort();
        card.tooltip =
            QStringLiteral("%1\n%2 lines · %3 functions · longest %4 lines")
                .arg(file.path)
                .arg(file.loc)
                .arg(file.functionCount)
                .arg(file.longestFunction);
        if (file.modifiedMs > 0) {
            card.tooltip +=
                QStringLiteral("\nlast changed %1")
                    .arg(QDateTime::fromMSecsSinceEpoch(file.modifiedMs)
                             .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        }
        if (file.stallMs > 0) {
            card.tooltip += QStringLiteral("\nstall attributed: %1 ms")
                                .arg(file.stallMs);
        }
        if (!kinds.isEmpty())
            card.tooltip += QStringLiteral("\n") + kinds.join(QStringLiteral("\n"));
        card.tooltip += QStringLiteral("\n\nClick to see this file's functions.");
        cards.append(card);
    }
    m_componentHealthBoard->setCards(cards);
    updateComponentHealthStatus();
}

void MainWindow::showComponentHealthFile(const QString &path)
{
    const FileHealth *found = nullptr;
    for (const FileHealth &file : std::as_const(m_componentHealthReport.files)) {
        if (file.path == path) {
            found = &file;
            break;
        }
    }
    if (!found || !m_componentHealthBoard)
        return;
    m_componentHealthOpenFile = path;
    if (m_componentHealthBack)
        m_componentHealthBack->setVisible(true);

    // Functions have no modification time of their own, so "recently changed"
    // falls back to their order in the file — which is at least stable and
    // says where in the file you are.
    const int mode = m_componentHealthSort
                         ? m_componentHealthSort->currentData().toInt()
                         : SortNeedsAttention;
    QVector<Function> ordered = found->functions;
    std::sort(ordered.begin(), ordered.end(),
              [mode](const Function &a, const Function &b) {
                  switch (mode) {
                  case SortStallTime:
                      if (a.stallMs != b.stallMs)
                          return a.stallMs > b.stallMs;
                      break;
                  case SortFlags:
                      if (a.findings.size() != b.findings.size())
                          return a.findings.size() > b.findings.size();
                      break;
                  case SortLargest:
                      if (a.loc != b.loc)
                          return a.loc > b.loc;
                      break;
                  case SortName:
                      if (a.name != b.name)
                          return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
                      break;
                  case SortRecentlyChanged:
                  case SortComponent:
                      return a.line < b.line;
                  default:
                      break;
                  }
                  if (a.stallMs != b.stallMs)
                      return a.stallMs > b.stallMs;
                  if (a.findings.size() != b.findings.size())
                      return a.findings.size() > b.findings.size();
                  return a.loc > b.loc;
              });

    QVector<HealthCard> cards;
    cards.reserve(ordered.size());
    for (const Function &fn : std::as_const(ordered)) {
        HealthCard card;
        card.id = path + QLatin1Char(':') + fn.name;
        card.title = fn.name;
        card.subtitle = QStringLiteral("line %1").arg(fn.line);
        card.loc = fn.loc;
        card.stallMs = fn.stallMs;
        card.findings = fn.findings.size();
        // A function's own score: findings and stall time, with length only a
        // mild penalty. A long function is not automatically a sick one.
        int score = 100 - qMin(45, fn.findings.size() * 12) -
                    qMin(35, fn.stallMs / 1000);
        if (fn.loc > 400)
            score -= 12;
        else if (fn.loc > 200)
            score -= 6;
        card.health = qMax(0, score);
        card.drillable = false;
        card.tooltip = QStringLiteral("%1\n%2:%3 · %4 lines")
                           .arg(fn.name)
                           .arg(path)
                           .arg(fn.line)
                           .arg(fn.loc);
        if (fn.loopDepth > 0) {
            card.tooltip +=
                QStringLiteral("\ndeepest loop nesting: %1").arg(fn.loopDepth);
        }
        if (fn.stallMs > 0) {
            card.tooltip +=
                QStringLiteral("\nstall attributed: %1 ms").arg(fn.stallMs);
        }
        for (const Finding &finding : fn.findings) {
            card.tooltip += QStringLiteral("\n· %1 (line %2): %3")
                                .arg(finding.kind)
                                .arg(finding.line)
                                .arg(finding.why);
        }
        cards.append(card);
    }
    m_componentHealthBoard->setCards(cards);
    if (m_componentHealthFilter)
        m_componentHealthFilter->clear();
    if (m_componentHealthStatus) {
        m_componentHealthStatus->setText(
            QStringLiteral("%1 · %2 functions · %3 lines · %4 flagged")
                .arg(found->path)
                .arg(found->functionCount)
                .arg(found->loc)
                .arg(found->findingCount));
    }
    if (QWidget *page = healthPage(m_componentHealthStack)) {
        if (auto *count = page->findChild<QLabel *>(QStringLiteral("healthCardCount"))) {
            const int shown = m_componentHealthBoard->visibleCount();
            count->setText(QStringLiteral("%1 %2").arg(shown).arg(
                shown == 1 ? QStringLiteral("function") : QStringLiteral("functions")));
        }
        if (!m_componentHealthStack || m_componentHealthStack->currentIndex() == ViewSource)
            syncHealthDetailPanel(page, found);
    }
}

void MainWindow::refreshComponentHealth(bool force)
{
    if (m_componentHealthScanning)
        return;
    if (!force && !m_componentHealthReport.files.isEmpty())
        return;

    m_componentHealthScanning = true;
    m_componentHealthScanDone.store(0, std::memory_order_relaxed);
    m_componentHealthScanTotal.store(0, std::memory_order_relaxed);
    if (m_componentHealthProgressRow)
        m_componentHealthProgressRow->setVisible(true);
    if (m_componentHealthProgress) {
        m_componentHealthProgress->setRange(0, 0); // indeterminate until counted
        m_componentHealthProgress->setValue(0);
    }
    if (m_componentHealthProgressLabel)
        m_componentHealthProgressLabel->setText(QStringLiteral("Reading the stall log…"));
    updateComponentHealthStatus();

    // Progress crosses threads through two atomics and a GUI-thread timer:
    // no widget is ever touched from the worker, and nothing is posted per
    // file, so a 310-file scan costs a handful of repaints.
    if (!m_componentHealthProgressTimer) {
        m_componentHealthProgressTimer = new QTimer(this);
        m_componentHealthProgressTimer->setInterval(80);
        connect(m_componentHealthProgressTimer, &QTimer::timeout, this, [this] {
            const int total =
                m_componentHealthScanTotal.load(std::memory_order_relaxed);
            const int done =
                m_componentHealthScanDone.load(std::memory_order_relaxed);
            if (total <= 0 || !m_componentHealthProgress)
                return;
            if (m_componentHealthProgress->maximum() != total)
                m_componentHealthProgress->setRange(0, total);
            m_componentHealthProgress->setValue(done);
            if (m_componentHealthProgressLabel) {
                m_componentHealthProgressLabel->setText(
                    QStringLiteral("Scanning %1 of %2 files…")
                        .arg(done)
                        .arg(total));
            }
        });
    }
    m_componentHealthProgressTimer->start();

    const QString root = sourceRoot();
    const QString diagnostics = diagnosticsRoot();
    runOffThread<Report>(
        [this, root, diagnostics]() {
            // Also announce the scan on the footer's background strip, so it
            // stays visible after navigating away from this page and reads as
            // deliberate work rather than as an unexplained busy period.
            const forkmesh::BackgroundScope activity(
                QStringLiteral("code health"),
                QStringLiteral("scanning %1 for component health")
                    .arg(QDir(root).dirName()));
            return analyse(root, {QStringLiteral("src")}, diagnostics,
                           [this](int done, int total, const QString &) {
                               m_componentHealthScanDone.store(
                                   done, std::memory_order_relaxed);
                               m_componentHealthScanTotal.store(
                                   total, std::memory_order_relaxed);
                               return true;
                           });
        },
        [this](Report report) {
            m_componentHealthScanning = false;
            if (m_componentHealthProgressTimer)
                m_componentHealthProgressTimer->stop();
            if (m_componentHealthProgressRow)
                m_componentHealthProgressRow->setVisible(false);
            m_componentHealthReport = std::move(report);
            // Paths come back relative to <checkout>/desktop; show them the
            // way the rest of the app spells them.
            for (FileHealth &file : m_componentHealthReport.files)
                file.path = QStringLiteral("desktop/") + file.path;
            if (!m_componentHealthOpenFile.isEmpty())
                showComponentHealthFile(m_componentHealthOpenFile);
            else
                showComponentHealthOverview();
        });
}
