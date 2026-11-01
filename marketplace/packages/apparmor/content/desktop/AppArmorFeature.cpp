#include "AppArmorFeature.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryFile>
#include <QThread>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

namespace forkmesh::apparmor {

class ModeBar final : public QWidget
{
public:
    struct Segment {
        QString label;
        int count = 0;
        QColor color;
    };

    explicit ModeBar(const QString &title, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_title(title)
    {
        setMinimumHeight(64);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setSegments(const QList<Segment> &segments)
    {
        m_segments = segments;
        QStringList lines;
        for (const Segment &segment : segments)
            lines << QStringLiteral("%1: %2").arg(segment.label).arg(segment.count);
        setToolTip(lines.join(QLatin1Char('\n')));
        setAccessibleDescription(QStringLiteral("%1. %2").arg(m_title, lines.join(QStringLiteral(", "))));
        update();
    }

    QSize sizeHint() const override { return {360, 64}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QFont bold = font();
        bold.setBold(true);
        painter.setFont(bold);
        painter.setPen(palette().color(QPalette::WindowText));
        int total = 0;
        for (const Segment &segment : m_segments)
            total += segment.count;
        painter.drawText(QRect(0, 0, width(), 18), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("%1 · %2").arg(m_title).arg(total));
        const QRectF bar(0, 22, width(), 12);
        QPainterPath clip;
        clip.addRoundedRect(bar, 6, 6);
        painter.setClipPath(clip);
        painter.fillRect(bar, palette().color(QPalette::Mid));
        qreal x = 0;
        for (const Segment &segment : m_segments) {
            if (total == 0 || segment.count == 0)
                continue;
            const qreal w = bar.width() * segment.count / total;
            painter.fillRect(QRectF(x, bar.top(), w, bar.height()), segment.color);
            x += w;
        }
        painter.setClipping(false);
        painter.setFont(font());
        int lx = 0;
        const QFontMetrics metrics(font());
        for (const Segment &segment : m_segments) {
            const QString text = QStringLiteral("%1 %2").arg(segment.label).arg(segment.count);
            const int needed = 14 + metrics.horizontalAdvance(text) + 14;
            if (lx + needed > width())
                break;
            painter.setBrush(segment.color);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(QRectF(lx, 44, 10, 10), 2, 2);
            painter.setPen(palette().color(QPalette::WindowText));
            painter.drawText(QRect(lx + 14, 40, needed, 18), Qt::AlignLeft | Qt::AlignVCenter, text);
            lx += needed;
        }
    }

private:
    QString m_title;
    QList<Segment> m_segments;
};

class TimelineChart final : public QWidget
{
public:
    explicit TimelineChart(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setMinimumHeight(140);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(QStringLiteral("AppArmor events per hour"));
    }

    void setBuckets(const QList<HourBucket> &buckets)
    {
        m_buckets = buckets;
        int denied = 0;
        int allowed = 0;
        for (const HourBucket &bucket : buckets) {
            denied += bucket.denied;
            allowed += bucket.allowed;
        }
        setAccessibleDescription(QStringLiteral("%1 denied and %2 allowed in complain mode over the last %3 hours.")
                                     .arg(denied)
                                     .arg(allowed)
                                     .arg(buckets.size()));
        update();
    }

    QSize sizeHint() const override { return {480, 140}; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF plot = plotRect();
        const QColor text = palette().color(QPalette::WindowText);
        QColor grid = text;
        grid.setAlphaF(0.15);
        painter.setPen(grid);
        painter.drawLine(plot.bottomLeft(), plot.bottomRight());
        painter.drawLine(plot.topLeft(), plot.topRight());
        const int peak = maximum();
        painter.setPen(text);
        painter.drawText(QRectF(0, plot.top() - 8, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(peak));
        painter.drawText(QRectF(0, plot.bottom() - 8, plot.left() - 6, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("0"));
        if (m_buckets.isEmpty())
            return;
        const qreal slot = plot.width() / m_buckets.size();
        const qreal barWidth = std::max<qreal>(2, slot - 3);
        for (int i = 0; i < m_buckets.size(); ++i) {
            const HourBucket &bucket = m_buckets.at(i);
            const qreal x = plot.left() + i * slot + (slot - barWidth) / 2;
            const qreal deniedHeight = plot.height() * bucket.denied / peak;
            const qreal allowedHeight = plot.height() * bucket.allowed / peak;
            painter.fillRect(QRectF(x, plot.bottom() - deniedHeight, barWidth, deniedHeight), QColor(QStringLiteral("#b42318")));
            painter.fillRect(QRectF(x, plot.bottom() - deniedHeight - allowedHeight, barWidth, allowedHeight),
                             QColor(QStringLiteral("#d08a00")));
            if (i % 6 == 0) {
                painter.setPen(text);
                painter.drawText(QRectF(x - 20, plot.bottom() + 2, 52, 16), Qt::AlignHCenter | Qt::AlignTop,
                                 bucket.start.toString(QStringLiteral("HH:mm")));
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QRectF plot = plotRect();
        if (m_buckets.isEmpty() || !plot.contains(event->position())) {
            QToolTip::hideText();
            return;
        }
        const int index = std::clamp(int((event->position().x() - plot.left()) / (plot.width() / m_buckets.size())), 0,
                                     int(m_buckets.size()) - 1);
        const HourBucket &bucket = m_buckets.at(index);
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("%1 to %2\n%3 denied\n%4 allowed in complain mode")
                               .arg(bucket.start.toString(QStringLiteral("ddd HH:mm")),
                                    bucket.start.addSecs(3600).toString(QStringLiteral("HH:mm")))
                               .arg(bucket.denied)
                               .arg(bucket.allowed),
                           this);
    }

private:
    QRectF plotRect() const { return QRectF(36, 10, std::max(10, width() - 44), std::max(10, height() - 32)); }

    int maximum() const
    {
        int peak = 1;
        for (const HourBucket &bucket : m_buckets)
            peak = std::max(peak, bucket.denied + bucket.allowed);
        return peak;
    }

    QList<HourBucket> m_buckets;
};

namespace {

QColor modeColor(Mode mode)
{
    switch (mode) {
    case Mode::Enforce: return QColor(QStringLiteral("#27834a"));
    case Mode::Complain: return QColor(QStringLiteral("#d08a00"));
    case Mode::Kill: return QColor(QStringLiteral("#b42318"));
    case Mode::Prompt: return QColor(QStringLiteral("#2f6fdb"));
    case Mode::Mixed: return QColor(QStringLiteral("#7c5cc4"));
    case Mode::Unconfined:
    case Mode::Unknown: return QColor(QStringLiteral("#6b7280"));
    }
    return {};
}

QColor disabledColor()
{
    return QColor(QStringLiteral("#a3a9b4"));
}

QLabel *mutedLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *heading(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

QPushButton *ghostButton(const QString &text, const QString &tip)
{
    auto *button = new QPushButton(text);
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    return button;
}

QTreeWidget *table(const QString &name, const QStringList &headers, int stretch)
{
    auto *tree = new QTreeWidget;
    tree->setAccessibleName(name);
    tree->setColumnCount(int(headers.size()));
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->setSortingEnabled(false);
    tree->header()->setStretchLastSection(false);
    for (int i = 0; i < headers.size(); ++i)
        tree->header()->setSectionResizeMode(i, i == stretch ? QHeaderView::Stretch : QHeaderView::ResizeToContents);
    return tree;
}

QFont monoFont()
{
    return QFontDatabase::systemFont(QFontDatabase::FixedFont);
}

std::optional<QString> editInDialog(QWidget *parent, const QString &title, const QString &intro, const QString &initial,
                                    const QString &okLabel)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.resize(820, 640);
    auto *layout = new QVBoxLayout(&dialog);
    auto *label = mutedLabel(intro);
    layout->addWidget(label);
    auto *editor = new QPlainTextEdit;
    editor->setFont(monoFont());
    editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    editor->setPlainText(initial);
    editor->setAccessibleName(QStringLiteral("Profile text"));
    layout->addWidget(editor, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *ok = buttons->addButton(okLabel, QDialogButtonBox::AcceptRole);
    ok->setObjectName(QStringLiteral("primaryButton"));
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;
    return editor->toPlainText();
}

QString changedLines(const QString &before, const QString &after)
{
    const QStringList left = before.split(QLatin1Char('\n'));
    const QStringList right = after.split(QLatin1Char('\n'));
    const QSet<QString> oldLines(left.begin(), left.end());
    const QSet<QString> newLines(right.begin(), right.end());
    QStringList out;
    for (const QString &line : left) {
        if (!newLines.contains(line))
            out << QStringLiteral("- ") + line;
    }
    for (const QString &line : right) {
        if (!oldLines.contains(line))
            out << QStringLiteral("+ ") + line;
    }
    return out.join(QLatin1Char('\n'));
}

}

AppArmorFeature::AppArmorFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent)
    , m_searchIndexChanged(host.searchIndexChanged)
{
    setObjectName(QStringLiteral("appArmorPage"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *headingRow = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("AppArmor"));
    title->setObjectName(QStringLiteral("pageTitle"));
    headingRow->addWidget(title);
    headingRow->addStretch();
    m_newProfile = ghostButton(QStringLiteral("New profile…"), QStringLiteral("Write a profile for a program, starting in complain mode so it only logs"));
    m_reloadAll = ghostButton(QStringLiteral("Reload all"), QStringLiteral("Reload every profile in /etc/apparmor.d (administrator)"));
    m_live = ghostButton(QStringLiteral("Live view"), QStringLiteral("Read the loaded profiles and the denial log once (administrator)"));
    m_refresh = new QPushButton(QStringLiteral("Refresh"));
    m_refresh->setObjectName(QStringLiteral("primaryButton"));
    headingRow->addWidget(m_newProfile);
    headingRow->addWidget(m_reloadAll);
    headingRow->addWidget(m_live);
    headingRow->addWidget(m_refresh);
    outer->addLayout(headingRow);

    m_summary = new QLabel(QStringLiteral("Reading AppArmor…"));
    m_summary->setObjectName(QStringLiteral("appArmorSummary"));
    m_summary->setWordWrap(true);
    outer->addWidget(m_summary);
    outer->addWidget(mutedLabel(QStringLiteral("AppArmor limits what each program may open, run and connect to. This page reads the profiles, "
                                               "processes and log on this computer; it sends nothing anywhere. Every change asks for confirmation "
                                               "and then your administrator password, keeps a copy of the file it replaces, and is undone "
                                               "if AppArmor rejects the new profile.")));
    m_status = mutedLabel({});
    m_status->setObjectName(QStringLiteral("appArmorStatus"));
    m_status->hide();
    outer->addWidget(m_status);

    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->addTab(buildOverview(), QStringLiteral("Overview"));
    m_tabs->addTab(buildProfiles(), QStringLiteral("Profiles"));
    m_tabs->addTab(buildProcesses(), QStringLiteral("Processes"));
    m_tabs->addTab(buildEvents(), QStringLiteral("Events"));
    outer->addWidget(m_tabs, 1);

    connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_live, &QPushButton::clicked, this, [this] { readLive(); });
    connect(m_newProfile, &QPushButton::clicked, this, [this] { newProfile(); });
    connect(m_reloadAll, &QPushButton::clicked, this, [this] { reloadAll(); });
    updateActions();
    refresh();
}

AppArmorFeature::~AppArmorFeature()
{
    if (m_scanThread) {
        m_scanThread->disconnect(this);
        m_scanThread->wait();
        delete m_scanThread;
    }
}

QWidget *AppArmorFeature::buildOverview()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    layout->setSpacing(12);
    auto *bars = new QHBoxLayout;
    bars->setSpacing(24);
    m_profileBar = new ModeBar(QStringLiteral("Profiles"));
    m_profileBar->setAccessibleName(QStringLiteral("Profiles by mode"));
    m_processBar = new ModeBar(QStringLiteral("Running processes"));
    m_processBar->setAccessibleName(QStringLiteral("Processes by confinement"));
    bars->addWidget(m_profileBar, 1);
    bars->addWidget(m_processBar, 1);
    layout->addLayout(bars);
    layout->addWidget(heading(QStringLiteral("Events in the last 24 hours")));
    m_timeline = new TimelineChart;
    layout->addWidget(m_timeline);
    m_timelineCaption = mutedLabel({});
    layout->addWidget(m_timelineCaption);

    auto *split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(heading(QStringLiteral("Status")));
    m_checks = table(QStringLiteral("AppArmor status"), {QStringLiteral("Item"), QStringLiteral("State")}, 1);
    leftLayout->addWidget(m_checks);
    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(heading(QStringLiteral("Worth a look")));
    m_attention = table(QStringLiteral("Items worth a look"), {QStringLiteral("What"), QStringLiteral("Detail")}, 1);
    m_attention->setUniformRowHeights(false);
    rightLayout->addWidget(m_attention);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 3);
    layout->addWidget(split, 1);

    connect(m_attention, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (item)
            navigate(item->data(0, Qt::UserRole).toString());
    });
    connect(m_attention, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        if (item)
            navigate(item->data(0, Qt::UserRole).toString());
    });
    return page;
}

QWidget *AppArmorFeature::buildProfiles()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    auto *filters = new QHBoxLayout;
    m_profileFilter = new QComboBox;
    m_profileFilter->setAccessibleName(QStringLiteral("Filter profiles"));
    m_profileFilter->addItem(QStringLiteral("All profiles"), QStringLiteral("all"));
    m_profileFilter->addItem(QStringLiteral("Enforce"), QStringLiteral("enforce"));
    m_profileFilter->addItem(QStringLiteral("Complain"), QStringLiteral("complain"));
    m_profileFilter->addItem(QStringLiteral("Unconfined flag"), QStringLiteral("unconfined"));
    m_profileFilter->addItem(QStringLiteral("Disabled"), QStringLiteral("disabled"));
    m_profileFilter->addItem(QStringLiteral("Not loaded"), QStringLiteral("unloaded"));
    m_profileFilter->addItem(QStringLiteral("With events"), QStringLiteral("events"));
    m_profileFilter->addItem(QStringLiteral("Running"), QStringLiteral("running"));
    filters->addWidget(m_profileFilter);
    filters->addStretch();
    layout->addLayout(filters);

    auto *split = new QSplitter(Qt::Vertical);
    split->setChildrenCollapsible(false);
    m_profiles = table(QStringLiteral("AppArmor profiles"),
                       {QStringLiteral("Profile"), QStringLiteral("Mode"), QStringLiteral("State"), QStringLiteral("Processes"),
                        QStringLiteral("Denied"), QStringLiteral("File")},
                       5);
    split->addWidget(m_profiles);

    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_profileDetails = new QWidget;
    auto *details = new QVBoxLayout(m_profileDetails);
    details->setContentsMargins(4, 8, 4, 4);
    details->setSpacing(6);
    m_profileTitle = heading({});
    m_profileTitle->setWordWrap(true);
    m_profileTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(m_profileTitle);
    m_profileInfo = new QLabel;
    m_profileInfo->setWordWrap(true);
    m_profileInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(m_profileInfo);
    auto *actions = new QHBoxLayout;
    m_enforce = ghostButton(QStringLiteral("Enforce"), QStringLiteral("Block and log anything the profile does not allow"));
    m_complain = ghostButton(QStringLiteral("Complain"), QStringLiteral("Allow everything but log what enforce mode would block"));
    m_toggleDisabled = ghostButton(QStringLiteral("Disable"), QStringLiteral("Unload the profile and keep it off after restarts"));
    m_reload = ghostButton(QStringLiteral("Reload"), QStringLiteral("Compile and load the profile file again"));
    m_unload = ghostButton(QStringLiteral("Unload"), QStringLiteral("Remove the profile from the kernel until the next restart or reload"));
    m_edit = ghostButton(QStringLiteral("Edit…"), QStringLiteral("Edit the profile file, then save and load it"));
    m_profileEvents = ghostButton(QStringLiteral("Show events"), QStringLiteral("Show this profile's log events"));
    for (QPushButton *button : {m_enforce, m_complain, m_toggleDisabled, m_reload, m_unload, m_edit, m_profileEvents})
        actions->addWidget(button);
    actions->addStretch();
    details->addLayout(actions);
    m_profileHint = mutedLabel({});
    details->addWidget(m_profileHint);
    m_profileText = new QPlainTextEdit;
    m_profileText->setReadOnly(true);
    m_profileText->setFont(monoFont());
    m_profileText->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_profileText->setAccessibleName(QStringLiteral("Profile file contents"));
    m_profileText->setMinimumHeight(160);
    details->addWidget(m_profileText, 1);
    scroll->setWidget(m_profileDetails);
    split->addWidget(scroll);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    layout->addWidget(split, 1);

    connect(m_profileFilter, &QComboBox::currentIndexChanged, this, [this] { renderProfiles(); });
    connect(m_profiles, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item)
            showProfile(item->data(0, Qt::UserRole).toString());
    });
    connect(m_enforce, &QPushButton::clicked, this, [this] { setMode(Mode::Enforce); });
    connect(m_complain, &QPushButton::clicked, this, [this] { setMode(Mode::Complain); });
    connect(m_toggleDisabled, &QPushButton::clicked, this, [this] { toggleDisabled(); });
    connect(m_reload, &QPushButton::clicked, this, [this] { reloadProfile(); });
    connect(m_unload, &QPushButton::clicked, this, [this] { unloadProfile(); });
    connect(m_edit, &QPushButton::clicked, this, [this] { editProfile(); });
    connect(m_profileEvents, &QPushButton::clicked, this, [this] { showEventsFor(m_selectedProfile); });
    return page;
}

QWidget *AppArmorFeature::buildProcesses()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    auto *filters = new QHBoxLayout;
    m_processFilter = new QComboBox;
    m_processFilter->setAccessibleName(QStringLiteral("Filter processes"));
    m_processFilter->addItem(QStringLiteral("All processes"), QStringLiteral("all"));
    m_processFilter->addItem(QStringLiteral("Confined"), QStringLiteral("confined"));
    m_processFilter->addItem(QStringLiteral("Enforce"), QStringLiteral("enforce"));
    m_processFilter->addItem(QStringLiteral("Complain"), QStringLiteral("complain"));
    m_processFilter->addItem(QStringLiteral("Unconfined"), QStringLiteral("unconfined"));
    m_processFilter->addItem(QStringLiteral("Unconfined, but a profile exists"), QStringLiteral("missed"));
    filters->addWidget(m_processFilter);
    filters->addStretch();
    layout->addLayout(filters);
    m_processes = table(QStringLiteral("Processes and their AppArmor confinement"),
                        {QStringLiteral("PID"), QStringLiteral("Program"), QStringLiteral("Profile"), QStringLiteral("Mode"),
                         QStringLiteral("Note"), QStringLiteral("Command")},
                        5);
    layout->addWidget(m_processes, 1);
    m_processNote = mutedLabel(QStringLiteral("Double-click a confined process to open its profile. A process started before its profile "
                                              "was loaded stays unconfined until it is restarted."));
    layout->addWidget(m_processNote);
    connect(m_processFilter, &QComboBox::currentIndexChanged, this, [this] { renderProcesses(); });
    connect(m_processes, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty())
            selectProfile(item->data(0, Qt::UserRole).toString());
    });
    return page;
}

QWidget *AppArmorFeature::buildEvents()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 10, 0, 0);
    auto *filters = new QHBoxLayout;
    m_eventFilter = new QComboBox;
    m_eventFilter->setAccessibleName(QStringLiteral("Filter events"));
    m_eventFilter->addItem(QStringLiteral("Denied and allowed"), QStringLiteral("access"));
    m_eventFilter->addItem(QStringLiteral("Denied"), QStringLiteral("DENIED"));
    m_eventFilter->addItem(QStringLiteral("Allowed in complain mode"), QStringLiteral("ALLOWED"));
    m_eventFilter->addItem(QStringLiteral("Profile loads and other"), QStringLiteral("other"));
    m_eventFilter->addItem(QStringLiteral("Everything"), QStringLiteral("all"));
    filters->addWidget(m_eventFilter);
    m_eventScope = mutedLabel({});
    m_eventScope->setWordWrap(false);
    filters->addWidget(m_eventScope);
    m_eventScopeClear = ghostButton(QStringLiteral("Show all profiles"), QStringLiteral("Stop filtering by profile"));
    filters->addWidget(m_eventScopeClear);
    filters->addStretch();
    layout->addLayout(filters);
    m_eventEmpty = mutedLabel({});
    layout->addWidget(m_eventEmpty);

    auto *split = new QSplitter(Qt::Vertical);
    split->setChildrenCollapsible(false);
    m_events = table(QStringLiteral("AppArmor log events"),
                     {QStringLiteral("Time"), QStringLiteral("Result"), QStringLiteral("Profile"), QStringLiteral("Operation"),
                      QStringLiteral("Target"), QStringLiteral("Access"), QStringLiteral("Program")},
                     4);
    m_events->setSelectionMode(QAbstractItemView::ExtendedSelection);
    split->addWidget(m_events);
    auto *detail = new QWidget;
    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(4, 8, 4, 4);
    m_eventRule = new QLabel;
    m_eventRule->setWordWrap(true);
    m_eventRule->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLayout->addWidget(m_eventRule);
    auto *actions = new QHBoxLayout;
    m_allow = new QPushButton(QStringLiteral("Allow…"));
    m_allow->setObjectName(QStringLiteral("primaryButton"));
    m_allow->setToolTip(QStringLiteral("Add rules for the selected events to the profile and reload it"));
    actions->addWidget(m_allow);
    actions->addStretch();
    detailLayout->addLayout(actions);
    m_eventDetail = mutedLabel({});
    m_eventDetail->setFont(monoFont());
    m_eventDetail->setTextInteractionFlags(Qt::TextSelectableByMouse);
    detailLayout->addWidget(m_eventDetail);
    detailLayout->addStretch();
    split->addWidget(detail);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);

    connect(m_eventFilter, &QComboBox::currentIndexChanged, this, [this] { renderEvents(); });
    connect(m_eventScopeClear, &QPushButton::clicked, this, [this] { showEventsFor({}); });
    connect(m_events, &QTreeWidget::itemSelectionChanged, this, [this] { showEventDetails(); });
    connect(m_allow, &QPushButton::clicked, this, [this] { allowSelected(); });
    return page;
}

void AppArmorFeature::refresh()
{
    if (m_scanThread)
        return;
    m_refresh->setEnabled(false);
    m_refresh->setText(QStringLiteral("Reading…"));
    auto result = std::make_shared<Snapshot>();
    const bool readLogs = !m_liveEvents.has_value();
    m_scanThread = QThread::create([result, readLogs] { *result = readSnapshot(QString(), readLogs); });
    connect(m_scanThread, &QThread::finished, this, [this, result, readLogs] {
        m_scanThread->deleteLater();
        m_scanThread = nullptr;
        QList<Event> previous = m_snapshot.events;
        QString previousSource = m_snapshot.eventSource;
        m_snapshot = *result;
        if (!readLogs) {
            m_snapshot.events = previous;
            m_snapshot.eventSource = previousSource;
        }
        m_scanned = true;
        m_refresh->setText(QStringLiteral("Refresh"));
        m_refresh->setEnabled(true);
        rebuildRows();
        render();
        if (m_searchIndexChanged)
            m_searchIndexChanged();
    });
    m_scanThread->start(QThread::LowPriority);
}

const QList<Event> &AppArmorFeature::events() const
{
    return m_liveEvents ? *m_liveEvents : m_snapshot.events;
}

void AppArmorFeature::rebuildRows()
{
    m_rows.clear();
    QHash<QString, int> index;
    for (int f = 0; f < m_snapshot.files.size(); ++f) {
        const ProfileFile &file = m_snapshot.files.at(f);
        for (const ProfileDeclaration &declaration : file.profiles) {
            if (index.contains(declaration.name))
                continue;
            ProfileRow row;
            row.name = declaration.name;
            row.attachment = declaration.attachment;
            row.fileIndex = f;
            row.declared = declaredMode(declaration.flags);
            index.insert(row.name, int(m_rows.size()));
            m_rows << row;
        }
    }
    if (m_liveProfiles) {
        for (const LoadedProfile &loaded : *m_liveProfiles) {
            if (loaded.name.contains(QStringLiteral("//")))
                continue;
            auto it = index.constFind(loaded.name);
            if (it == index.constEnd()) {
                ProfileRow row;
                row.name = loaded.name;
                index.insert(row.name, int(m_rows.size()));
                m_rows << row;
                it = index.constFind(loaded.name);
            }
            ProfileRow &row = m_rows[*it];
            row.loaded = true;
            row.live = loaded.mode;
        }
    }
    for (const ProcessInfo &process : m_snapshot.processes) {
        const auto it = index.constFind(topLevelProfile(process.label.profile));
        if (!process.label.profile.isEmpty() && it != index.constEnd())
            ++m_rows[*it].processes;
    }
    for (const Event &event : events()) {
        const auto it = index.constFind(topLevelProfile(event.profile));
        if (it == index.constEnd())
            continue;
        if (event.result == QStringLiteral("DENIED"))
            ++m_rows[*it].denied;
        else if (event.result == QStringLiteral("ALLOWED"))
            ++m_rows[*it].allowed;
    }
    std::sort(m_rows.begin(), m_rows.end(), [](const ProfileRow &left, const ProfileRow &right) {
        return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
    });
    QList<QPair<QString, QRegularExpression>> attachments;
    for (const ProfileRow &row : m_rows) {
        const ProfileFile *file = fileFor(&row);
        if (!file || file->disabled || row.declared == Mode::Unconfined || !row.attachment.startsWith(QLatin1Char('/')))
            continue;
        const QString pattern = globPattern(row.attachment);
        const QRegularExpression expression(pattern);
        if (!pattern.isEmpty() && expression.isValid())
            attachments.append({row.name, expression});
    }
    m_missed.clear();
    for (const ProcessInfo &process : m_snapshot.processes) {
        if (process.label.mode != Mode::Unconfined || !process.executable.startsWith(QLatin1Char('/')))
            continue;
        for (const auto &attachment : attachments) {
            if (attachment.second.match(process.executable).hasMatch()) {
                m_missed.insert(process.pid, attachment.first);
                break;
            }
        }
    }
}

const ProfileRow *AppArmorFeature::rowByName(const QString &name) const
{
    for (const ProfileRow &row : m_rows) {
        if (row.name == name)
            return &row;
    }
    return nullptr;
}

const ProfileFile *AppArmorFeature::fileFor(const ProfileRow *row) const
{
    if (!row || row->fileIndex < 0 || row->fileIndex >= m_snapshot.files.size())
        return nullptr;
    return &m_snapshot.files.at(row->fileIndex);
}

QString AppArmorFeature::displayMode(const ProfileRow &row) const
{
    const ProfileFile *file = fileFor(&row);
    if (m_liveProfiles && row.loaded)
        return modeName(row.live);
    if (file && file->disabled)
        return QStringLiteral("disabled");
    if (file && file->forceComplain)
        return modeName(Mode::Complain);
    return modeName(row.declared);
}

QString AppArmorFeature::displayState(const ProfileRow &row) const
{
    const ProfileFile *file = fileFor(&row);
    if (file && file->disabled)
        return row.loaded ? QStringLiteral("Disabled, still loaded") : QStringLiteral("Disabled");
    if (!m_liveProfiles)
        return file ? QStringLiteral("In policy") : QStringLiteral("Unknown");
    if (!file)
        return QStringLiteral("Loaded, no file");
    return row.loaded ? QStringLiteral("Loaded") : QStringLiteral("Not loaded");
}

QString AppArmorFeature::profileFor(const ProcessInfo &process) const
{
    return m_missed.value(process.pid);
}

bool AppArmorFeature::matchesQuery(const QStringList &values) const
{
    if (m_query.isEmpty())
        return true;
    for (const QString &value : values) {
        if (value.contains(m_query, Qt::CaseInsensitive))
            return true;
    }
    return false;
}

void AppArmorFeature::render()
{
    renderOverview();
    renderProfiles();
    renderProcesses();
    renderEvents();
    updateActions();
}

void AppArmorFeature::renderOverview()
{
    if (!m_scanned)
        return;
    if (!m_snapshot.supported) {
        m_summary->setText(QStringLiteral("AppArmor is a Linux security module. This page works on Linux only."));
        m_checks->clear();
        m_attention->clear();
        return;
    }
    QHash<QString, int> modes;
    int disabled = 0;
    for (const ProfileRow &row : m_rows) {
        const ProfileFile *file = fileFor(&row);
        if (file && file->disabled && !row.loaded)
            ++disabled;
        else
            ++modes[displayMode(row)];
    }
    QList<ModeBar::Segment> profileSegments;
    for (Mode mode : {Mode::Enforce, Mode::Complain, Mode::Kill, Mode::Prompt, Mode::Unconfined, Mode::Mixed, Mode::Unknown}) {
        const int count = modes.value(modeName(mode));
        if (count > 0)
            profileSegments << ModeBar::Segment{modeName(mode), count, modeColor(mode)};
    }
    if (disabled > 0)
        profileSegments << ModeBar::Segment{QStringLiteral("disabled"), disabled, disabledColor()};
    m_profileBar->setSegments(profileSegments);

    int enforced = 0;
    int complaining = 0;
    int other = 0;
    int unconfined = 0;
    int missed = 0;
    for (const ProcessInfo &process : m_snapshot.processes) {
        switch (process.label.mode) {
        case Mode::Enforce: ++enforced; break;
        case Mode::Complain: ++complaining; break;
        case Mode::Unconfined:
            ++unconfined;
            if (!profileFor(process).isEmpty())
                ++missed;
            break;
        default: ++other; break;
        }
    }
    QList<ModeBar::Segment> processSegments{{QStringLiteral("enforce"), enforced, modeColor(Mode::Enforce)},
                                            {QStringLiteral("complain"), complaining, modeColor(Mode::Complain)}};
    if (other > 0)
        processSegments << ModeBar::Segment{QStringLiteral("other"), other, modeColor(Mode::Mixed)};
    processSegments << ModeBar::Segment{QStringLiteral("unconfined"), unconfined, modeColor(Mode::Unconfined)};
    m_processBar->setSegments(processSegments);

    const QList<Event> &all = events();
    int denied = 0;
    int allowed = 0;
    for (const Event &event : all) {
        if (event.result == QStringLiteral("DENIED"))
            ++denied;
        else if (event.result == QStringLiteral("ALLOWED"))
            ++allowed;
    }
    m_timeline->setBuckets(hourlyBuckets(all, QDateTime::currentDateTime(), 24));
    const QString source = m_liveEvents ? QStringLiteral("the administrator read at %1").arg(m_liveReadAt.toString(QStringLiteral("HH:mm")))
                                        : m_snapshot.eventSource;
    if (source.isEmpty())
        m_timelineCaption->setText(QStringLiteral("The AppArmor log is only readable by an administrator on this computer. Press Live view to read it once."));
    else
        m_timelineCaption->setText(QStringLiteral("Red bars are denials, amber bars are accesses complain mode let through. %1 denied and %2 "
                                                  "allowed in the last 7 days, from %3.")
                                       .arg(denied)
                                       .arg(allowed)
                                       .arg(source));

    QString state;
    if (m_snapshot.enabled && m_snapshot.inLsmList)
        state = QStringLiteral("AppArmor is on");
    else if (m_snapshot.moduleLoaded)
        state = QStringLiteral("AppArmor is built in but turned off");
    else
        state = QStringLiteral("AppArmor is not available in this kernel");
    const int confined = enforced + complaining + other;
    m_summary->setText(QStringLiteral("%1 · %2 profiles (%3 enforce, %4 complain) · %5 of %6 processes confined · %7 denials in 7 days")
                           .arg(state)
                           .arg(m_rows.size())
                           .arg(modes.value(modeName(Mode::Enforce)))
                           .arg(modes.value(modeName(Mode::Complain)))
                           .arg(confined)
                           .arg(m_snapshot.processes.size())
                           .arg(denied));

    m_checks->clear();
    auto check = [this](const QString &item, const QString &value, bool good) {
        auto *row = new QTreeWidgetItem(m_checks, {item, value});
        row->setForeground(1, good ? modeColor(Mode::Enforce) : modeColor(Mode::Kill));
        row->setToolTip(1, value);
    };
    check(QStringLiteral("Kernel"), m_snapshot.enabled ? QStringLiteral("Enabled")
                                    : m_snapshot.moduleLoaded ? QStringLiteral("Built in, turned off (add apparmor=1 security=apparmor to the boot line)")
                                                              : QStringLiteral("Not in this kernel"),
          m_snapshot.enabled);
    check(QStringLiteral("Security modules"), m_snapshot.lsm.isEmpty() ? QStringLiteral("Unknown") : m_snapshot.lsm, m_snapshot.inLsmList);
    check(QStringLiteral("Policy compiler"),
          m_snapshot.parserPath.isEmpty() ? QStringLiteral("apparmor_parser is not installed; install the apparmor package") : m_snapshot.parserPath,
          !m_snapshot.parserPath.isEmpty());
    int fileCount = 0;
    int snapCount = 0;
    for (const ProfileFile &file : m_snapshot.files)
        (file.source == QStringLiteral("snap") ? snapCount : fileCount)++;
    check(QStringLiteral("Policy files"),
          QStringLiteral("%1 in %2%3").arg(fileCount).arg(m_snapshot.policyDir, snapCount ? QStringLiteral(", %1 from Snap").arg(snapCount) : QString()),
          fileCount > 0);
    check(QStringLiteral("Loaded profiles"),
          m_liveProfiles ? QStringLiteral("%1, read at %2").arg(m_liveProfiles->size()).arg(m_liveReadAt.toString(QStringLiteral("HH:mm")))
                         : QStringLiteral("Only an administrator can list them; press Live view"),
          m_liveProfiles.has_value());
    check(QStringLiteral("Log"), source.isEmpty() ? QStringLiteral("Not readable without administrator rights") : source, !source.isEmpty());

    m_attention->clear();
    auto attention = [this](const QString &what, const QString &detail, const QString &target, Mode tone) {
        auto *row = new QTreeWidgetItem(m_attention, {what, detail});
        row->setData(0, Qt::UserRole, target);
        row->setForeground(0, modeColor(tone));
        row->setToolTip(1, detail);
    };
    if (!m_snapshot.enabled)
        attention(QStringLiteral("AppArmor is off"), QStringLiteral("No profile protects anything until it is turned on at boot."), {}, Mode::Kill);
    QList<const ProfileRow *> noisy;
    for (const ProfileRow &row : m_rows) {
        if (row.denied > 0)
            noisy << &row;
    }
    std::sort(noisy.begin(), noisy.end(), [](const ProfileRow *left, const ProfileRow *right) { return left->denied > right->denied; });
    for (qsizetype i = 0; i < std::min<qsizetype>(noisy.size(), 6); ++i)
        attention(noisy.at(i)->name, QStringLiteral("%1 denials. If the program misbehaves, select them in Events and allow them.").arg(noisy.at(i)->denied),
                  QStringLiteral("events:") + noisy.at(i)->name, Mode::Kill);
    for (const ProcessInfo &process : m_snapshot.processes) {
        const QString profile = profileFor(process);
        if (!profile.isEmpty())
            attention(QStringLiteral("%1 (pid %2)").arg(process.name).arg(process.pid),
                      QStringLiteral("Runs unconfined although profile %1 matches it. Restart the program to confine it.").arg(profile),
                      QStringLiteral("profile:") + profile, Mode::Complain);
    }
    QStringList complainProfiles;
    for (const ProfileRow &row : m_rows) {
        if (displayMode(row) == modeName(Mode::Complain))
            complainProfiles << row.name;
    }
    if (complainProfiles.size() == 1)
        attention(complainProfiles.first(), QStringLiteral("In complain mode: it logs but blocks nothing. Switch to enforce once the log is quiet."),
                  QStringLiteral("profile:") + complainProfiles.first(), Mode::Complain);
    else if (complainProfiles.size() > 1)
        attention(QStringLiteral("%1 profiles in complain mode").arg(complainProfiles.size()),
                  QStringLiteral("They log but block nothing: %1. Switch each to enforce once its log is quiet.")
                      .arg(complainProfiles.mid(0, 6).join(QStringLiteral(", ")) + (complainProfiles.size() > 6 ? QStringLiteral("…") : QString())),
                  QStringLiteral("filter:complain"), Mode::Complain);
    for (const ProfileRow &row : m_rows) {
        if (m_liveProfiles && fileFor(&row) && !fileFor(&row)->disabled && !row.loaded)
            attention(row.name, QStringLiteral("Has a file but is not loaded. Reload it to apply it."), QStringLiteral("profile:") + row.name,
                      Mode::Unknown);
    }
    if (m_attention->topLevelItemCount() == 0)
        new QTreeWidgetItem(m_attention, {QStringLiteral("Nothing stands out"), QStringLiteral("No denials, unconfined matches or complain-mode profiles.")});
}

void AppArmorFeature::renderProfiles()
{
    const QString filter = m_profileFilter->currentData().toString();
    const QString keep = m_selectedProfile;
    m_profiles->blockSignals(true);
    m_profiles->clear();
    QTreeWidgetItem *selected = nullptr;
    for (const ProfileRow &row : m_rows) {
        const ProfileFile *file = fileFor(&row);
        const QString mode = displayMode(row);
        const bool disabled = file && file->disabled;
        bool show = true;
        if (filter == QStringLiteral("enforce") || filter == QStringLiteral("complain") || filter == QStringLiteral("unconfined"))
            show = mode == filter;
        else if (filter == QStringLiteral("disabled"))
            show = disabled;
        else if (filter == QStringLiteral("unloaded"))
            show = m_liveProfiles && !row.loaded;
        else if (filter == QStringLiteral("events"))
            show = row.denied + row.allowed > 0;
        else if (filter == QStringLiteral("running"))
            show = row.processes > 0;
        if (!show || !matchesQuery({row.name, row.attachment, file ? file->fileName : QString(), mode}))
            continue;
        auto *item = new QTreeWidgetItem(m_profiles, {row.name, mode, displayState(row), row.processes ? QString::number(row.processes) : QString(),
                                                      row.denied ? QString::number(row.denied) : QString(), file ? file->path : QString()});
        item->setData(0, Qt::UserRole, row.name);
        item->setForeground(1, disabled ? disabledColor() : modeColor(modeFromText(mode)));
        item->setToolTip(0, row.attachment.isEmpty() ? row.name : QStringLiteral("%1\nAttaches to %2").arg(row.name, row.attachment));
        if (row.denied)
            item->setForeground(4, modeColor(Mode::Kill));
        if (row.name == keep)
            selected = item;
    }
    if (!selected)
        selected = m_profiles->topLevelItem(0);
    if (selected) {
        m_profiles->setCurrentItem(selected);
        m_profiles->scrollToItem(selected);
    }
    m_profiles->blockSignals(false);
    showProfile(selected ? selected->data(0, Qt::UserRole).toString() : QString());
}

void AppArmorFeature::showProfile(const QString &name)
{
    m_selectedProfile = name;
    const ProfileRow *row = rowByName(name);
    m_profileDetails->setVisible(row != nullptr);
    if (!row) {
        updateActions();
        return;
    }
    const ProfileFile *file = fileFor(row);
    m_profileTitle->setText(row->name);
    QStringList info;
    info << QStringLiteral("Mode: %1 · %2").arg(displayMode(*row), displayState(*row));
    if (!row->attachment.isEmpty() && row->attachment != row->name)
        info << QStringLiteral("Attaches to: %1").arg(row->attachment);
    if (file) {
        info << QStringLiteral("File: %1").arg(file->path);
        if (file->profiles.size() > 1)
            info << QStringLiteral("This file holds %1 profiles; mode changes apply to all of them.").arg(file->profiles.size());
        for (const ProfileDeclaration &declaration : file->profiles) {
            if (declaration.name == row->name && !declaration.localInclude.isEmpty())
                info << QStringLiteral("Local additions: %1/local/%2").arg(m_snapshot.policyDir, declaration.localInclude);
            if (declaration.name == row->name && !declaration.flags.isEmpty())
                info << QStringLiteral("Flags: %1").arg(declaration.flags.join(QStringLiteral(", ")));
        }
    }
    info << QStringLiteral("Running processes: %1 · Denied: %2 · Allowed in complain mode: %3").arg(row->processes).arg(row->denied).arg(row->allowed);
    m_profileInfo->setText(info.join(QLatin1Char('\n')));
    m_profileText->setPlainText(file ? file->text : QString());
    m_profileText->setVisible(file != nullptr);
    updateActions();
}

void AppArmorFeature::renderProcesses()
{
    const QString filter = m_processFilter->currentData().toString();
    m_processes->clear();
    for (const ProcessInfo &process : m_snapshot.processes) {
        const Mode mode = process.label.mode;
        const QString missed = profileFor(process);
        bool show = true;
        if (filter == QStringLiteral("confined"))
            show = mode != Mode::Unconfined;
        else if (filter == QStringLiteral("enforce"))
            show = mode == Mode::Enforce;
        else if (filter == QStringLiteral("complain"))
            show = mode == Mode::Complain;
        else if (filter == QStringLiteral("unconfined"))
            show = mode == Mode::Unconfined;
        else if (filter == QStringLiteral("missed"))
            show = !missed.isEmpty();
        const QString program = process.name;
        if (!show || !matchesQuery({program, process.executable, process.label.profile, process.command, QString::number(process.pid)}))
            continue;
        const QString note = missed.isEmpty() ? QString() : QStringLiteral("Profile %1 exists; restart to confine").arg(missed);
        auto *item = new QTreeWidgetItem(m_processes, {QString::number(process.pid), program, process.label.profile, modeName(mode), note, process.command});
        item->setData(0, Qt::UserRole, missed.isEmpty() ? topLevelProfile(process.label.profile) : missed);
        item->setForeground(3, modeColor(mode));
        if (!missed.isEmpty())
            item->setForeground(4, modeColor(Mode::Complain));
        item->setToolTip(1, process.executable);
        item->setToolTip(5, process.command);
    }
}

void AppArmorFeature::renderEvents()
{
    const QString filter = m_eventFilter->currentData().toString();
    m_eventScope->setText(m_eventProfile.isEmpty() ? QString() : QStringLiteral("Profile: %1").arg(m_eventProfile));
    m_eventScope->setVisible(!m_eventProfile.isEmpty());
    m_eventScopeClear->setVisible(!m_eventProfile.isEmpty());
    m_events->clear();
    const QList<Event> &all = events();
    int shown = 0;
    for (int i = 0; i < all.size() && shown < 5000; ++i) {
        const Event &event = all.at(i);
        const bool access = event.result == QStringLiteral("DENIED") || event.result == QStringLiteral("ALLOWED");
        if (filter == QStringLiteral("access") && !access)
            continue;
        if ((filter == QStringLiteral("DENIED") || filter == QStringLiteral("ALLOWED")) && event.result != filter)
            continue;
        if (filter == QStringLiteral("other") && access)
            continue;
        if (!m_eventProfile.isEmpty() && topLevelProfile(event.profile) != m_eventProfile)
            continue;
        if (!matchesQuery({event.profile, event.target, event.comm, event.operation}))
            continue;
        const QString mask = event.denied.isEmpty() ? event.requested : event.denied;
        auto *item = new QTreeWidgetItem(m_events, {event.time.isValid() ? event.time.toString(QStringLiteral("MMM d HH:mm:ss")) : QString(),
                                                    event.result.toLower(), event.profile, event.operation, event.target, mask,
                                                    event.pid ? QStringLiteral("%1 (%2)").arg(event.comm).arg(event.pid) : event.comm});
        item->setData(0, Qt::UserRole, i);
        item->setForeground(1, event.result == QStringLiteral("DENIED") ? modeColor(Mode::Kill)
                               : event.result == QStringLiteral("ALLOWED") ? modeColor(Mode::Complain)
                                                                           : modeColor(Mode::Unknown));
        item->setToolTip(4, event.target);
        ++shown;
    }
    QString empty;
    if (all.isEmpty())
        empty = m_liveEvents || !m_snapshot.eventSource.isEmpty()
            ? QStringLiteral("The AppArmor log has no entries from the last 7 days.")
            : QStringLiteral("The AppArmor log is only readable by an administrator. Press Live view to read it once; nothing is changed.");
    else if (shown == 0)
        empty = QStringLiteral("No events match the filter.");
    m_eventEmpty->setText(empty);
    m_eventEmpty->setVisible(!empty.isEmpty());
    showEventDetails();
}

void AppArmorFeature::showEventDetails()
{
    const QList<QTreeWidgetItem *> selected = m_events->selectedItems();
    const QList<Event> &all = events();
    if (selected.isEmpty()) {
        m_eventRule->setText(QStringLiteral("Select one or more denials to see the rule that would allow them."));
        m_eventDetail->clear();
        updateActions();
        return;
    }
    QStringList rules;
    QSet<QString> profiles;
    for (QTreeWidgetItem *item : selected) {
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0 || index >= all.size())
            continue;
        profiles.insert(all.at(index).profile);
        const QString rule = suggestedRule(all.at(index));
        if (!rule.isEmpty() && !rules.contains(rule))
            rules << rule;
    }
    const int first = selected.first()->data(0, Qt::UserRole).toInt();
    m_eventDetail->setText(first >= 0 && first < all.size() ? all.at(first).raw : QString());
    if (rules.isEmpty())
        m_eventRule->setText(QStringLiteral("No rule can be suggested for this event. Edit the profile in Profiles if it needs one."));
    else if (profiles.size() > 1)
        m_eventRule->setText(QStringLiteral("Select events from one profile at a time to allow them."));
    else
        m_eventRule->setText(QStringLiteral("Rule to add to %1:\n%2").arg(*profiles.begin(), rules.join(QLatin1Char('\n'))));
    updateActions();
}

void AppArmorFeature::selectProfile(const QString &name)
{
    m_selectedProfile = name;
    m_profileFilter->setCurrentIndex(0);
    renderProfiles();
    m_tabs->setCurrentIndex(1);
}

void AppArmorFeature::showEventsFor(const QString &profile)
{
    m_eventProfile = profile;
    renderEvents();
    if (!profile.isEmpty())
        m_tabs->setCurrentIndex(3);
}

void AppArmorFeature::navigate(const QString &target)
{
    if (target.startsWith(QStringLiteral("profile:")))
        selectProfile(target.mid(8));
    else if (target.startsWith(QStringLiteral("events:")))
        showEventsFor(target.mid(7));
    else if (target.startsWith(QStringLiteral("filter:"))) {
        m_profileFilter->setCurrentIndex(std::max(0, m_profileFilter->findData(target.mid(7))));
        m_tabs->setCurrentIndex(1);
    }
}

void AppArmorFeature::setHostSearchQuery(const QString &query)
{
    m_query = query.trimmed();
    renderProfiles();
    renderProcesses();
    renderEvents();
}

QList<forkmesh::market::NativePackageSearchHit> AppArmorFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty() || limit <= 0)
        return hits;
    for (const ProfileRow &row : m_rows) {
        if (!row.name.contains(needle, Qt::CaseInsensitive) && !row.attachment.contains(needle, Qt::CaseInsensitive))
            continue;
        hits << forkmesh::market::NativePackageSearchHit{QStringLiteral("profile:") + row.name, row.name,
                                                          QStringLiteral("AppArmor profile · %1 · %2").arg(displayMode(row), displayState(row))};
        if (hits.size() == limit)
            break;
    }
    return hits;
}

void AppArmorFeature::activateSearchHit(const QString &id)
{
    navigate(id);
}

void AppArmorFeature::updateActions()
{
    const bool ready = m_snapshot.supported && !m_snapshot.parserPath.isEmpty();
    const bool idle = !m_busy;
    m_live->setEnabled(idle && m_snapshot.supported);
    m_newProfile->setEnabled(idle && ready);
    m_reloadAll->setEnabled(idle && ready);
    const ProfileRow *row = rowByName(m_selectedProfile);
    const ProfileFile *file = fileFor(row);
    const bool editable = idle && ready && file && file->editable;
    bool unconfinedFlag = false;
    if (file) {
        for (const ProfileDeclaration &declaration : file->profiles)
            unconfinedFlag = unconfinedFlag || declaredMode(declaration.flags) == Mode::Unconfined;
    }
    const Mode mode = file ? fileMode(file->profiles) : Mode::Unknown;
    m_enforce->setEnabled(editable && !unconfinedFlag && mode != Mode::Enforce);
    m_complain->setEnabled(editable && !unconfinedFlag && mode != Mode::Complain);
    m_toggleDisabled->setEnabled(editable);
    m_toggleDisabled->setText(file && file->disabled ? QStringLiteral("Enable") : QStringLiteral("Disable"));
    m_reload->setEnabled(idle && ready && file && !file->disabled);
    m_unload->setEnabled(idle && ready && file && (!m_liveProfiles || (row && row->loaded)));
    m_edit->setEnabled(editable);
    m_profileEvents->setEnabled(row && row->denied + row->allowed > 0);
    QString hint;
    if (file && !file->editable)
        hint = QStringLiteral("Snap writes this profile itself and replaces any change, so it can only be viewed here.");
    else if (unconfinedFlag)
        hint = QStringLiteral("This profile is marked unconfined on purpose: it only names the program and allows everything. Enforce or "
                              "complain would apply a profile with almost no rules and break the program.");
    else if (row && !file)
        hint = QStringLiteral("This profile is loaded in the kernel but has no file in %1 (a container, Snap or another tool loaded it).").arg(m_snapshot.policyDir);
    else if (file && m_snapshot.parserPath.isEmpty())
        hint = QStringLiteral("apparmor_parser is not installed, so profiles cannot be loaded from here.");
    m_profileHint->setText(hint);
    m_profileHint->setVisible(!hint.isEmpty());

    bool allowable = false;
    const QList<QTreeWidgetItem *> selected = m_events->selectedItems();
    QSet<QString> profiles;
    for (QTreeWidgetItem *item : selected) {
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0 || index >= events().size())
            continue;
        profiles.insert(events().at(index).profile);
        allowable = allowable || !suggestedRule(events().at(index)).isEmpty();
    }
    m_allow->setEnabled(idle && ready && allowable && profiles.size() == 1);
}

void AppArmorFeature::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

void AppArmorFeature::readLive()
{
    runAdmin(QStringLiteral("Live view"), QString(), QString(), adminLiveScript(), {}, false);
}

void AppArmorFeature::setMode(Mode mode)
{
    const ProfileRow *row = rowByName(m_selectedProfile);
    const ProfileFile *file = fileFor(row);
    if (!file)
        return;
    const QString text = withMode(file->text, mode);
    if (text == file->text) {
        setStatus(QStringLiteral("%1 is already in %2 mode.").arg(file->fileName, modeName(mode)));
        return;
    }
    const QString description = mode == Mode::Enforce
        ? QStringLiteral("Put %1 in enforce mode? AppArmor will block, and log, anything the profile does not allow. If the program "
                         "stops working, switch back to complain or allow the denials in Events.").arg(row->name)
        : QStringLiteral("Put %1 in complain mode? AppArmor will allow everything and only log what enforce mode would block. "
                         "Use it to learn which rules the program needs.").arg(row->name);
    writePolicy(mode == Mode::Enforce ? QStringLiteral("Enforce") : QStringLiteral("Complain"), description, file->path, text,
                file->disabled ? QString() : file->path);
}

void AppArmorFeature::toggleDisabled()
{
    const ProfileRow *row = rowByName(m_selectedProfile);
    const ProfileFile *file = fileFor(row);
    if (!file || !file->editable)
        return;
    if (file->disabled) {
        runAdmin(QStringLiteral("Enable"),
                 QStringLiteral("Enable %1 again? The disable link is moved to %2 and the profile is loaded.").arg(file->fileName, backupDir()),
                 QStringLiteral("Folder: %1\nFile: %2").arg(m_snapshot.policyDir, file->fileName), adminEnableScript(),
                 {m_snapshot.policyDir, file->fileName, backupDir()}, true);
    } else {
        runAdmin(QStringLiteral("Disable"),
                 QStringLiteral("Disable %1? It is unloaded now and stays off after restarts; the programs it covered run unconfined. "
                                "The file itself is kept, so you can enable it again.").arg(file->fileName),
                 QStringLiteral("Link %1/disable/%2 to the file, then unload it.").arg(m_snapshot.policyDir, file->fileName),
                 adminDisableScript(), {m_snapshot.policyDir, file->fileName}, true);
    }
}

void AppArmorFeature::reloadProfile()
{
    const ProfileFile *file = fileFor(rowByName(m_selectedProfile));
    if (!file)
        return;
    runAdmin(QStringLiteral("Reload"), QStringLiteral("Compile and load %1 again?").arg(file->path), QStringLiteral("apparmor_parser -r -W %1").arg(file->path),
             adminReloadScript(), {file->path}, true);
}

void AppArmorFeature::unloadProfile()
{
    const ProfileFile *file = fileFor(rowByName(m_selectedProfile));
    if (!file)
        return;
    runAdmin(QStringLiteral("Unload"),
             QStringLiteral("Unload the profiles in %1 until the next restart or reload? Programs they covered run unconfined. Use Disable "
                            "to keep them off.").arg(file->fileName),
             QStringLiteral("apparmor_parser -R %1").arg(file->path), adminUnloadScript(), {file->path}, true);
}

void AppArmorFeature::editProfile()
{
    const ProfileRow *row = rowByName(m_selectedProfile);
    const ProfileFile *file = fileFor(row);
    if (!file || !file->editable)
        return;
    const std::optional<QString> text = editInDialog(
        this, QStringLiteral("Edit %1").arg(file->fileName),
        QStringLiteral("Saving checks the profile with AppArmor, keeps the old file in %1 and loads the new one. If AppArmor rejects it, "
                       "the old file is put back.").arg(backupDir()),
        file->text, QStringLiteral("Save and load"));
    if (!text || *text == file->text)
        return;
    writePolicy(QStringLiteral("Save"), QStringLiteral("Save your changes to %1 and load them?").arg(file->path), file->path, *text,
                file->disabled ? QString() : file->path);
}

void AppArmorFeature::newProfile()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New AppArmor profile"));
    dialog.resize(760, 560);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(mutedLabel(QStringLiteral("The new profile starts in complain mode: the program keeps working and every access it "
                                                "makes is logged. Use it for a while, allow what it needs in Events, then switch it to enforce.")));
    auto *form = new QFormLayout;
    auto *programRow = new QHBoxLayout;
    auto *program = new QLineEdit;
    program->setPlaceholderText(QStringLiteral("/usr/bin/program"));
    auto *browse = ghostButton(QStringLiteral("Browse…"), QStringLiteral("Pick the program file"));
    programRow->addWidget(program, 1);
    programRow->addWidget(browse);
    form->addRow(QStringLiteral("Program"), programRow);
    auto *name = new QLineEdit;
    name->setPlaceholderText(QStringLiteral("profile name"));
    form->addRow(QStringLiteral("Profile name"), name);
    layout->addLayout(form);
    auto *preview = new QPlainTextEdit;
    preview->setFont(monoFont());
    preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    preview->setAccessibleName(QStringLiteral("New profile text"));
    layout->addWidget(preview, 1);
    auto *problem = mutedLabel({});
    layout->addWidget(problem);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *create = buttons->addButton(QStringLiteral("Create and load"), QDialogButtonBox::AcceptRole);
    create->setObjectName(QStringLiteral("primaryButton"));
    layout->addWidget(buttons);
    const QString abi = m_snapshot.abiVersions.contains(QStringLiteral("4.0")) ? QStringLiteral("4.0")
        : m_snapshot.abiVersions.contains(QStringLiteral("3.0"))               ? QStringLiteral("3.0")
                                                                               : QString();
    QStringList existing;
    for (const ProfileFile &file : m_snapshot.files)
        existing << file.fileName;
    const QString policyDir = m_snapshot.policyDir;
    auto validate = [=] {
        const QString path = program->text().trimmed();
        const QString profile = name->text().trimmed();
        QString error;
        if (!path.startsWith(QLatin1Char('/')))
            error = QStringLiteral("Give the program's full path.");
        else if (!isSafeProfileName(profile))
            error = QStringLiteral("Use letters, digits, dots, dashes or underscores for the name.");
        else if (existing.contains(profile))
            error = QStringLiteral("%1/%2 already exists; edit it in Profiles instead.").arg(policyDir, profile);
        problem->setText(error);
        create->setEnabled(error.isEmpty());
    };
    auto regenerate = [=] {
        const QString path = program->text().trimmed();
        const QString profile = name->text().trimmed();
        preview->setPlainText(newProfileTemplate(profile.isEmpty() ? QStringLiteral("name") : profile,
                                                 path.isEmpty() ? QStringLiteral("/usr/bin/program") : path, abi));
        validate();
    };
    connect(program, &QLineEdit::textEdited, &dialog, [=](const QString &text) {
        const QString base = QFileInfo(text.trimmed()).fileName();
        if (name->isModified() == false && isSafeProfileName(base))
            name->setText(base);
        regenerate();
    });
    connect(name, &QLineEdit::textEdited, &dialog, [=] { regenerate(); });
    connect(browse, &QPushButton::clicked, &dialog, [=, &dialog] {
        const QString chosen = QFileDialog::getOpenFileName(&dialog, QStringLiteral("Choose a program"), QStringLiteral("/usr/bin"));
        if (chosen.isEmpty())
            return;
        program->setText(chosen);
        if (!name->isModified() && isSafeProfileName(QFileInfo(chosen).fileName()))
            name->setText(QFileInfo(chosen).fileName());
        regenerate();
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    regenerate();
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString profile = name->text().trimmed();
    if (!isSafeFileName(profile) || existing.contains(profile))
        return;
    const QString dest = m_snapshot.policyDir + QLatin1Char('/') + profile;
    m_selectedProfile = profile;
    writePolicy(QStringLiteral("Create"), QStringLiteral("Create %1 and load it in complain mode?").arg(dest), dest, preview->toPlainText(), dest);
}

void AppArmorFeature::reloadAll()
{
    runAdmin(QStringLiteral("Reload all"),
             QStringLiteral("Reload every AppArmor profile from %1? Running programs keep their current confinement.").arg(m_snapshot.policyDir),
             QStringLiteral("systemctl reload apparmor"), adminReloadAllScript(), {}, true);
}

void AppArmorFeature::allowSelected()
{
    const QList<Event> &all = events();
    QStringList rules;
    QString profile;
    for (QTreeWidgetItem *item : m_events->selectedItems()) {
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0 || index >= all.size())
            continue;
        const QString rule = suggestedRule(all.at(index));
        if (rule.isEmpty())
            continue;
        if (!profile.isEmpty() && profile != all.at(index).profile)
            return;
        profile = all.at(index).profile;
        if (!rules.contains(rule))
            rules << rule;
    }
    if (rules.isEmpty())
        return;
    if (profile.contains(QStringLiteral("//"))) {
        setStatus(QStringLiteral("%1 is a child profile inside another one. Add the rule to it with Edit in Profiles.").arg(profile));
        return;
    }
    const ProfileRow *row = rowByName(profile);
    const ProfileFile *file = fileFor(row);
    if (!file || !file->editable) {
        setStatus(QStringLiteral("No editable file holds the profile %1, so the rule cannot be added from here.").arg(profile));
        return;
    }
    QString local;
    for (const ProfileDeclaration &declaration : file->profiles) {
        if (declaration.name == profile)
            local = declaration.localInclude;
    }
    const QString load = file->disabled ? QString() : file->path;
    if (!local.isEmpty() && isSafeFileName(local)) {
        const QString dest = m_snapshot.policyDir + QStringLiteral("/local/") + local;
        QFile current(dest);
        QString before;
        if (current.exists()) {
            if (!current.open(QIODevice::ReadOnly)) {
                setStatus(QStringLiteral("%1 is not readable, so the rule cannot be added safely.").arg(dest));
                return;
            }
            before = QString::fromUtf8(current.readAll());
        }
        QString after = before;
        for (const QString &rule : rules)
            after = withLocalRule(after, rule);
        if (after == before) {
            setStatus(QStringLiteral("%1 already allows these.").arg(dest));
            return;
        }
        writePolicy(QStringLiteral("Allow"),
                    QStringLiteral("Add %1 rule(s) to %2, the local additions file of %3, and reload the profile?")
                        .arg(rules.size())
                        .arg(dest, profile),
                    dest, after, load);
        return;
    }
    QString after = file->text;
    for (const QString &rule : rules) {
        if (containsRule(after, rule))
            continue;
        const QString next = withRule(after, profile, rule);
        if (next.isEmpty()) {
            setStatus(QStringLiteral("Could not find where the profile %1 ends in %2. Add the rule with Edit.").arg(profile, file->path));
            return;
        }
        after = next;
    }
    if (after == file->text) {
        setStatus(QStringLiteral("%1 already allows these.").arg(file->path));
        return;
    }
    writePolicy(QStringLiteral("Allow"),
                QStringLiteral("Add %1 rule(s) to the profile %2 in %3 and reload it?").arg(rules.size()).arg(profile, file->path), file->path,
                after, load);
}

void AppArmorFeature::writePolicy(const QString &title, const QString &description, const QString &dest, const QString &text,
                                  const QString &load)
{
    auto temp = std::make_shared<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/forkmesh-apparmor-XXXXXX"));
    if (!temp->open()) {
        setStatus(QStringLiteral("Could not write a temporary file for the new profile."));
        return;
    }
    temp->write(text.toUtf8());
    temp->flush();
    temp->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    QString before;
    QFile current(dest);
    if (current.open(QIODevice::ReadOnly))
        before = QString::fromUtf8(current.readAll());
    const QString changes = before.isEmpty() ? text : changedLines(before, text);
    runAdmin(title, description,
             QStringLiteral("File: %1\nOld copy kept in: %2\n%3\nChanges:\n%4")
                 .arg(dest, backupDir(), load.isEmpty() ? QStringLiteral("Not loaded (profile is disabled).") : QStringLiteral("Then load: ") + load,
                      changes),
             adminInstallScript(), {temp->fileName(), dest, backupDir(), load}, true, temp);
}

void AppArmorFeature::runAdmin(const QString &title, const QString &description, const QString &details, const QString &script,
                               const QStringList &arguments, bool confirm, std::shared_ptr<QTemporaryFile> keep)
{
    if (m_busy)
        return;
    if (confirm) {
        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(QStringLiteral("AppArmor: %1").arg(title));
        box.setText(description);
        box.setInformativeText(QStringLiteral("Your computer will ask for your administrator password."));
        if (!details.isEmpty())
            box.setDetailedText(details);
        QPushButton *apply = box.addButton(title, QMessageBox::AcceptRole);
        QPushButton *cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() != apply)
            return;
    }
#if !defined(Q_OS_LINUX)
    setStatus(QStringLiteral("AppArmor runs on Linux only."));
    return;
#else
    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pkexec.isEmpty()) {
        setStatus(QStringLiteral("The administrator prompt (pkexec, from polkit) is not installed, so AppArmor cannot be managed from here."));
        return;
    }
    m_busy = true;
    setStatus(QStringLiteral("%1: waiting for the administrator password…").arg(title));
    updateActions();
    auto *process = new QProcess(this);
    connect(process, &QProcess::finished, this, [this, process, title, keep](int exitCode, QProcess::ExitStatus status) {
        process->deleteLater();
        m_busy = false;
        const QString output = QString::fromUtf8(process->readAllStandardOutput());
        const QString errors = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
        const QHash<QString, QString> sections = splitSections(output);
        const bool cancelled = exitCode == 126 || exitCode == 127;
        if (!cancelled && sections.contains(QStringLiteral("profiles")) && !sections.value(QStringLiteral("profiles")).trimmed().isEmpty()) {
            m_liveProfiles = parseLoadedProfiles(sections.value(QStringLiteral("profiles")));
            m_liveReadAt = QDateTime::currentDateTime();
        }
        if (!cancelled && sections.contains(QStringLiteral("events"))) {
            m_liveEvents = parseEvents(sections.value(QStringLiteral("events")));
            m_liveReadAt = QDateTime::currentDateTime();
        }
        if (status == QProcess::NormalExit && exitCode == 0)
            setStatus(QStringLiteral("%1: done.").arg(title));
        else if (cancelled)
            setStatus(QStringLiteral("The administrator prompt was cancelled, so nothing was changed."));
        else if (exitCode == 3)
            setStatus(QStringLiteral("%1: AppArmor rejected the profile, so the previous file was kept. %2").arg(title, errors.right(600)));
        else
            setStatus(QStringLiteral("%1 did not finish (exit code %2)%3").arg(title).arg(exitCode).arg(errors.isEmpty() ? QStringLiteral(".") : QStringLiteral(": ") + errors.right(600)));
        rebuildRows();
        render();
        refresh();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        m_busy = false;
        setStatus(QStringLiteral("The administrator prompt could not be started."));
        updateActions();
    });
    process->start(pkexec, QStringList{QStringLiteral("/bin/sh"), QStringLiteral("-c"), script, QStringLiteral("sh")} + arguments);
#endif
}

}

namespace {
const forkmesh::market::NativePackageRegistration appArmorRegistration({
    QStringLiteral("builtin:apparmor"),
    {QStringLiteral("extension.native")},
    QStringLiteral("AppArmor"),
    QStringLiteral("shield-check"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::apparmor::AppArmorFeature(host, parent);
    },
});
}
