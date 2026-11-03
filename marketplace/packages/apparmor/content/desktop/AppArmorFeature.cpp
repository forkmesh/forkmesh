#include "AppArmorFeature.h"

#include "DesignTokens.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
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
#include <QStyle>
#include <QTemporaryFile>
#include <QThread>
#include <QToolTip>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace forkmesh::apparmor {

QFont scaledFont(const QFont &base, qreal factor);

struct Segment {
    QString label;
    int count = 0;
    QColor color;
};

class ModeBar final : public QWidget
{
public:
    explicit ModeBar(const QString &title, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_title(title)
    {
        setObjectName(QStringLiteral("appArmorChart"));
        setMinimumHeight(50);
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

    QSize sizeHint() const override { return {280, 50}; }

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
        painter.drawText(QRect(0, 0, width(), 16), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("%1 · %2").arg(m_title).arg(total));
        const QRectF bar(0, 19, width(), 10);
        QPainterPath clip;
        clip.addRoundedRect(bar, 5, 5);
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
            const int needed = 14 + metrics.horizontalAdvance(text) + 12;
            if (lx + needed > width())
                break;
            painter.setBrush(segment.color);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(QRectF(lx, 37, 9, 9), 2, 2);
            painter.setPen(palette().color(QPalette::WindowText));
            painter.drawText(QRect(lx + 13, 33, needed, 17), Qt::AlignLeft | Qt::AlignVCenter, text);
            lx += needed;
        }
    }

private:
    QString m_title;
    QList<Segment> m_segments;
};

class CoverageRing final : public QWidget
{
public:
    explicit CoverageRing(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("appArmorChart"));
        setFixedSize(104, 104);
        setAccessibleName(QStringLiteral("Share of running processes AppArmor confines"));
    }

    void setSegments(const QList<Segment> &segments, int covered)
    {
        m_segments = segments;
        m_covered = covered;
        m_total = 0;
        for (const Segment &segment : segments)
            m_total += segment.count;
        const QString text = QStringLiteral("%1 of %2 running processes are confined by AppArmor").arg(covered).arg(m_total);
        setToolTip(text);
        setAccessibleDescription(text);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal thickness = 11;
        const QRectF ring = QRectF(rect()).adjusted(thickness / 2 + 1, thickness / 2 + 1, -thickness / 2 - 1, -thickness / 2 - 1);
        QPen track(palette().color(QPalette::Mid), thickness, Qt::SolidLine, Qt::FlatCap);
        painter.setPen(track);
        painter.drawEllipse(ring);
        if (m_total > 0) {
            qreal start = 90 * 16;
            for (const Segment &segment : m_segments) {
                if (segment.count == 0)
                    continue;
                const qreal span = -360.0 * 16 * segment.count / m_total;
                painter.setPen(QPen(segment.color, thickness, Qt::SolidLine, Qt::FlatCap));
                painter.drawArc(ring, int(start), int(std::min(-16.0, span)));
                start += span;
            }
        }
        const int percent = m_total > 0 ? int(std::lround(100.0 * m_covered / m_total)) : 0;
        QFont big = scaledFont(font(), 1.45);
        big.setBold(true);
        painter.setFont(big);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(QRectF(0, height() / 2.0 - 20, width(), 24), Qt::AlignCenter, QStringLiteral("%1%").arg(percent));
        const QFont small = scaledFont(font(), 0.85);
        painter.setFont(small);
        QColor muted = palette().color(QPalette::WindowText);
        muted.setAlphaF(0.65);
        painter.setPen(muted);
        painter.drawText(QRectF(0, height() / 2.0 + 4, width(), 16), Qt::AlignCenter, QStringLiteral("confined"));
    }

private:
    QList<Segment> m_segments;
    int m_covered = 0;
    int m_total = 0;
};

class TimelineChart final : public QWidget
{
public:
    explicit TimelineChart(int height, int labelEvery, const QString &labelFormat, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_labelEvery(labelEvery)
        , m_labelFormat(labelFormat)
    {
        setObjectName(QStringLiteral("appArmorChart"));
        setMouseTracking(true);
        setFixedHeight(height);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(QStringLiteral("AppArmor events per hour"));
    }

    void setColors(const QColor &denied, const QColor &allowed)
    {
        m_denied = denied;
        m_allowed = allowed;
        update();
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

    QSize sizeHint() const override { return {360, height()}; }

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
        QColor muted = text;
        muted.setAlphaF(0.65);
        painter.setPen(muted);
        painter.drawText(QRectF(0, plot.top() - 8, plot.left() - 5, 16), Qt::AlignRight | Qt::AlignVCenter, QString::number(peak));
        painter.drawText(QRectF(0, plot.bottom() - 8, plot.left() - 5, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("0"));
        if (m_buckets.isEmpty())
            return;
        const qreal slot = plot.width() / m_buckets.size();
        const qreal barWidth = std::max<qreal>(1.5, slot - (slot > 6 ? 3 : 0.5));
        for (int i = 0; i < m_buckets.size(); ++i) {
            const HourBucket &bucket = m_buckets.at(i);
            const qreal x = plot.left() + i * slot + (slot - barWidth) / 2;
            const qreal deniedHeight = plot.height() * bucket.denied / peak;
            const qreal allowedHeight = plot.height() * bucket.allowed / peak;
            painter.fillRect(QRectF(x, plot.bottom() - deniedHeight, barWidth, deniedHeight), m_denied);
            painter.fillRect(QRectF(x, plot.bottom() - deniedHeight - allowedHeight, barWidth, allowedHeight), m_allowed);
            if (m_labelEvery > 0 && i % m_labelEvery == 0) {
                painter.setPen(muted);
                painter.drawText(QRectF(x - 2, plot.bottom() + 2, 52, 14), Qt::AlignLeft | Qt::AlignTop, bucket.start.toString(m_labelFormat));
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
    QRectF plotRect() const { return QRectF(28, 6, std::max(10, width() - 32), std::max(10, height() - 22)); }

    int maximum() const
    {
        int peak = 1;
        for (const HourBucket &bucket : m_buckets)
            peak = std::max(peak, bucket.denied + bucket.allowed);
        return peak;
    }

    int m_labelEvery = 6;
    QString m_labelFormat;
    QColor m_denied;
    QColor m_allowed;
    QList<HourBucket> m_buckets;
};

class RankBars final : public QWidget
{
public:
    struct Entry {
        QString name;
        int denied = 0;
        int allowed = 0;
    };

    explicit RankBars(std::function<void(const QString &)> activated, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_activated(std::move(activated))
    {
        setObjectName(QStringLiteral("appArmorChart"));
        setMouseTracking(true);
        setFixedHeight(5 * kRow);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName(QStringLiteral("Profiles with the most events in 7 days"));
    }

    void setEntries(const QList<Entry> &entries, const QColor &denied, const QColor &allowed, const QString &empty)
    {
        m_empty = empty;
        m_entries = entries.mid(0, 5);
        m_denied = denied;
        m_allowed = allowed;
        QStringList lines;
        for (const Entry &entry : m_entries)
            lines << QStringLiteral("%1: %2 denied, %3 allowed").arg(entry.name).arg(entry.denied).arg(entry.allowed);
        setAccessibleDescription(lines.join(QStringLiteral("; ")));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor text = palette().color(QPalette::WindowText);
        QColor muted = text;
        muted.setAlphaF(0.65);
        if (m_entries.isEmpty()) {
            painter.setPen(muted);
            painter.drawText(rect(), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_empty);
            return;
        }
        int peak = 1;
        for (const Entry &entry : m_entries)
            peak = std::max(peak, entry.denied + entry.allowed);
        const QFontMetrics metrics(font());
        const int nameWidth = std::min(width() * 2 / 5, 180);
        const int countWidth = 36;
        const qreal barSpace = std::max(10, width() - nameWidth - countWidth - 8);
        for (int i = 0; i < m_entries.size(); ++i) {
            const Entry &entry = m_entries.at(i);
            const int y = i * kRow;
            if (i == m_hover) {
                QColor hover = palette().color(QPalette::Highlight);
                hover.setAlphaF(0.12);
                painter.fillRect(QRect(0, y, width(), kRow), hover);
            }
            painter.setPen(text);
            painter.drawText(QRect(2, y, nameWidth - 6, kRow), Qt::AlignLeft | Qt::AlignVCenter,
                             metrics.elidedText(entry.name, Qt::ElideMiddle, nameWidth - 6));
            const qreal deniedWidth = barSpace * entry.denied / peak;
            const qreal allowedWidth = barSpace * entry.allowed / peak;
            const QRectF bar(nameWidth, y + kRow / 2.0 - 5, deniedWidth + allowedWidth, 10);
            QPainterPath clip;
            clip.addRoundedRect(bar, 3, 3);
            painter.save();
            painter.setClipPath(clip);
            painter.fillRect(QRectF(bar.left(), bar.top(), deniedWidth, bar.height()), m_denied);
            painter.fillRect(QRectF(bar.left() + deniedWidth, bar.top(), allowedWidth, bar.height()), m_allowed);
            painter.restore();
            painter.setPen(muted);
            painter.drawText(QRectF(bar.right() + 6, y, countWidth, kRow), Qt::AlignLeft | Qt::AlignVCenter,
                             QString::number(entry.denied + entry.allowed));
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int row = rowAt(event->position().y());
        if (row != m_hover) {
            m_hover = row;
            setCursor(row >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
        if (row >= 0) {
            const Entry &entry = m_entries.at(row);
            QToolTip::showText(event->globalPosition().toPoint(),
                               QStringLiteral("%1\n%2 denied, %3 allowed in complain mode\nClick to open it")
                                   .arg(entry.name)
                                   .arg(entry.denied)
                                   .arg(entry.allowed),
                               this);
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = -1;
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int row = rowAt(event->position().y());
        if (row >= 0 && m_activated)
            m_activated(m_entries.at(row).name);
    }

private:
    static constexpr int kRow = 21;

    int rowAt(qreal y) const
    {
        const int row = int(y / kRow);
        return y >= 0 && row < m_entries.size() ? row : -1;
    }

    std::function<void(const QString &)> m_activated;
    QList<Entry> m_entries;
    QString m_empty;
    QColor m_denied;
    QColor m_allowed;
    int m_hover = -1;
};

QFont scaledFont(const QFont &base, qreal factor)
{
    QFont font = base;
    if (base.pixelSize() > 0)
        font.setPixelSize(std::max(1, int(std::lround(base.pixelSize() * factor))));
    else if (base.pointSizeF() > 0)
        font.setPointSizeF(base.pointSizeF() * factor);
    return font;
}

namespace {

QColor modeColor(Mode mode)
{
    switch (mode) {
    case Mode::Enforce: return Theme::color(Theme::Role::Success);
    case Mode::Complain: return Theme::color(Theme::Role::Warning);
    case Mode::Kill: return Theme::color(Theme::Role::Danger);
    case Mode::Prompt: return Theme::color(Theme::Role::Accent);
    case Mode::Mixed: return QColor(QStringLiteral("#7c5cc4"));
    case Mode::Unconfined:
    case Mode::Unknown: return Theme::color(Theme::Role::FgMuted);
    }
    return {};
}

QColor deniedColor()
{
    return Theme::color(Theme::Role::Danger);
}

QColor allowedColor()
{
    return Theme::color(Theme::Role::Warning);
}

QColor disabledColor()
{
    return Theme::color(Theme::Role::FgSubtle);
}

QColor toneColor(AdviceTone tone)
{
    switch (tone) {
    case AdviceTone::Good: return Theme::color(Theme::Role::Success);
    case AdviceTone::Warning: return Theme::color(Theme::Role::Warning);
    case AdviceTone::Problem: return Theme::color(Theme::Role::Danger);
    case AdviceTone::Info: break;
    }
    return Theme::color(Theme::Role::Accent);
}

QIcon dotIcon(const QColor &color)
{
    QPixmap pixmap(20, 20);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(1.5, 1.5, 7, 7));
    return QIcon(pixmap);
}

QString dotHtml(const QColor &color)
{
    return QStringLiteral("<span style=\"color:%1\">&#9679;</span>").arg(color.name());
}

QString linkFor(const QString &target)
{
    return QString::fromLatin1(QUrl::toPercentEncoding(target));
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

QLabel *richLabel()
{
    auto *label = new QLabel;
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
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
    tree->setFrameShape(QFrame::NoFrame);
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

QFrame *pane(const QString &title, const QString &explanation, QLabel **count, QWidget *tools = nullptr)
{
    auto *frame = new QFrame;
    frame->setObjectName(QStringLiteral("appArmorPane"));
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(4);
    auto *header = new QHBoxLayout;
    header->setSpacing(8);
    header->addWidget(heading(title));
    if (count) {
        *count = mutedLabel({});
        (*count)->setWordWrap(false);
        header->addWidget(*count);
    }
    header->addStretch();
    if (tools)
        header->addWidget(tools);
    layout->addLayout(header);
    if (!explanation.isEmpty())
        layout->addWidget(mutedLabel(explanation));
    return frame;
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
    outer->setContentsMargins(16, 12, 16, 12);
    outer->setSpacing(8);

    auto *headingRow = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("AppArmor"));
    title->setObjectName(QStringLiteral("pageTitle"));
    headingRow->addWidget(title);
    m_summary = new QLabel(QStringLiteral("Reading AppArmor…"));
    m_summary->setObjectName(QStringLiteral("appArmorSummary"));
    m_summary->setTextFormat(Qt::RichText);
    headingRow->addSpacing(8);
    headingRow->addWidget(m_summary, 1);
    m_newProfile = ghostButton(QStringLiteral("New profile…"), QStringLiteral("Write a profile for a program, starting in complain mode so it only logs"));
    m_reloadAll = ghostButton(QStringLiteral("Reload all"), QStringLiteral("Reload every profile in /etc/apparmor.d (administrator)"));
    m_live = ghostButton(QStringLiteral("Live view"), QStringLiteral("Read the loaded profiles and the denial log once (administrator); changes nothing"));
    m_refresh = new QPushButton(QStringLiteral("Refresh"));
    m_refresh->setObjectName(QStringLiteral("primaryButton"));
    headingRow->addWidget(m_newProfile);
    headingRow->addWidget(m_reloadAll);
    headingRow->addWidget(m_live);
    headingRow->addWidget(m_refresh);
    outer->addLayout(headingRow);

    auto *intro = mutedLabel(QStringLiteral("AppArmor gives each program a profile that limits what it may open, run and connect to. "
                                            "<b>Enforce</b> blocks and logs, <b>complain</b> only logs, <b>unconfined</b> means no limits. "
                                            "Nothing here leaves this computer; every change asks first, keeps a backup and is undone if "
                                            "AppArmor rejects it."));
    intro->setTextFormat(Qt::RichText);
    outer->addWidget(intro);
    m_status = mutedLabel({});
    m_status->setObjectName(QStringLiteral("appArmorStatus"));
    m_status->hide();
    outer->addWidget(m_status);

    outer->addWidget(buildOverview());

    auto *columns = new QSplitter(Qt::Horizontal);
    columns->setChildrenCollapsible(false);
    columns->setHandleWidth(8);
    auto *left = new QSplitter(Qt::Vertical);
    left->setChildrenCollapsible(false);
    left->setHandleWidth(8);
    left->addWidget(buildProfiles());
    left->addWidget(buildProcesses());
    left->setSizes({600, 400});
    auto *middle = new QSplitter(Qt::Vertical);
    middle->setChildrenCollapsible(false);
    middle->setHandleWidth(8);
    middle->addWidget(buildDetails());
    middle->addWidget(buildEvents());
    middle->setSizes({500, 500});
    columns->addWidget(left);
    columns->addWidget(middle);
    columns->addWidget(buildSuggestions());
    columns->setSizes({400, 500, 380});
    outer->addWidget(columns, 1);

    connect(m_refresh, &QPushButton::clicked, this, [this] { refresh(); });
    connect(m_live, &QPushButton::clicked, this, [this] { readLive(); });
    connect(m_newProfile, &QPushButton::clicked, this, [this] { newProfile(); });
    connect(m_reloadAll, &QPushButton::clicked, this, [this] { reloadAll(); });
    applyStyle();
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

void AppArmorFeature::applyStyle()
{
    setStyleSheet(QStringLiteral("QFrame#appArmorPane { background: %1; border: 1px solid %2; border-radius: 8px; }"
                                 "QFrame#appArmorPane QLabel { background: transparent; border: none; }"
                                 "QFrame#appArmorPane QScrollArea, QFrame#appArmorPane QScrollArea > QWidget > QWidget,"
                                 " QWidget#appArmorChart { background: transparent; }")
                      .arg(Theme::hex(Theme::Role::Surface), Theme::hex(Theme::Role::Border)));
    m_timeline->setColors(deniedColor(), allowedColor());
    m_profileTimeline->setColors(deniedColor(), allowedColor());
}

void AppArmorFeature::themeChanged()
{
    applyStyle();
    render();
}

QWidget *AppArmorFeature::buildOverview()
{
    auto *strip = new QHBoxLayout;
    strip->setSpacing(8);
    strip->setContentsMargins(0, 0, 0, 0);

    QFrame *coverage = pane(QStringLiteral("Coverage"), {}, nullptr);
    auto *coverageRow = new QHBoxLayout;
    coverageRow->setSpacing(12);
    m_coverage = new CoverageRing;
    coverageRow->addWidget(m_coverage, 0, Qt::AlignTop);
    auto *bars = new QVBoxLayout;
    bars->setSpacing(4);
    m_profileBar = new ModeBar(QStringLiteral("Profiles"));
    m_profileBar->setAccessibleName(QStringLiteral("Profiles by mode"));
    m_processBar = new ModeBar(QStringLiteral("Running processes"));
    m_processBar->setAccessibleName(QStringLiteral("Processes by confinement"));
    bars->addWidget(m_profileBar);
    bars->addWidget(m_processBar);
    bars->addStretch();
    coverageRow->addLayout(bars, 1);
    static_cast<QVBoxLayout *>(coverage->layout())->addLayout(coverageRow);
    static_cast<QVBoxLayout *>(coverage->layout())->addStretch();

    QFrame *timeline = pane(QStringLiteral("Last 24 hours"), {}, nullptr);
    m_timeline = new TimelineChart(86, 6, QStringLiteral("HH:mm"));
    timeline->layout()->addWidget(m_timeline);
    m_timelineCaption = mutedLabel({});
    m_timelineCaption->setTextFormat(Qt::RichText);
    timeline->layout()->addWidget(m_timelineCaption);
    static_cast<QVBoxLayout *>(timeline->layout())->addStretch();

    QFrame *busiest = pane(QStringLiteral("Busiest profiles"), {}, nullptr);
    m_busiest = new RankBars([this](const QString &name) {
        selectProfile(name);
        showEventsFor(name);
    });
    busiest->layout()->addWidget(m_busiest);
    m_busiestCaption = mutedLabel({});
    busiest->layout()->addWidget(m_busiestCaption);
    static_cast<QVBoxLayout *>(busiest->layout())->addStretch();

    strip->addWidget(coverage, 4);
    strip->addWidget(timeline, 5);
    strip->addWidget(busiest, 3);
    auto *holder = new QWidget;
    holder->setLayout(strip);
    holder->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    return holder;
}

QWidget *AppArmorFeature::buildProfiles()
{
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
    QFrame *frame = pane(QStringLiteral("Profiles"), {}, &m_profilesCount, m_profileFilter);
    m_profiles = table(QStringLiteral("AppArmor profiles"),
                       {QStringLiteral("Profile"), QStringLiteral("Mode"), QStringLiteral("Running"), QStringLiteral("Denied")}, 0);
    frame->layout()->addWidget(m_profiles);
    connect(m_profileFilter, &QComboBox::currentIndexChanged, this, [this] { renderProfiles(); });
    connect(m_profiles, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item)
            showProfile(item->data(0, Qt::UserRole).toString());
    });
    return frame;
}

QWidget *AppArmorFeature::buildDetails()
{
    QFrame *frame = pane(QStringLiteral("Selected profile"), {}, nullptr);
    m_profileEmpty = mutedLabel(QStringLiteral("Pick a profile on the left to see what it does, how it is doing, and what to do next."));
    frame->layout()->addWidget(m_profileEmpty);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_profileDetails = new QWidget;
    auto *details = new QVBoxLayout(m_profileDetails);
    details->setContentsMargins(0, 0, 4, 0);
    details->setSpacing(5);
    m_profileTitle = heading({});
    m_profileTitle->setWordWrap(true);
    m_profileTitle->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(m_profileTitle);
    m_profileMode = new QLabel;
    m_profileMode->setTextFormat(Qt::RichText);
    m_profileMode->setWordWrap(true);
    details->addWidget(m_profileMode);
    m_profileAbout = mutedLabel({});
    m_profileAbout->setTextFormat(Qt::RichText);
    details->addWidget(m_profileAbout);
    m_profileAdvice = richLabel();
    m_profileAdvice->setAccessibleName(QStringLiteral("Suggestions for this profile"));
    details->addWidget(m_profileAdvice);
    m_profileTimeline = new TimelineChart(58, 24, QStringLiteral("ddd"));
    m_profileTimeline->setAccessibleName(QStringLiteral("This profile's events per hour over 7 days"));
    details->addWidget(m_profileTimeline);
    m_enforce = ghostButton(QStringLiteral("Enforce"), QStringLiteral("Block and log anything the profile does not allow"));
    m_complain = ghostButton(QStringLiteral("Complain"), QStringLiteral("Allow everything but log what enforce mode would block"));
    m_toggleDisabled = ghostButton(QStringLiteral("Disable"), QStringLiteral("Unload the profile and keep it off after restarts"));
    m_reload = ghostButton(QStringLiteral("Reload"), QStringLiteral("Compile and load the profile file again"));
    m_unload = ghostButton(QStringLiteral("Unload"), QStringLiteral("Remove the profile from the kernel until the next restart or reload"));
    m_edit = ghostButton(QStringLiteral("Edit…"), QStringLiteral("Edit the profile file, then save and load it"));
    m_profileEvents = ghostButton(QStringLiteral("Show events"), QStringLiteral("Show only this profile's log events below"));
    for (const QList<QPushButton *> &group : {QList<QPushButton *>{m_enforce, m_complain, m_toggleDisabled, m_profileEvents},
                                              QList<QPushButton *>{m_reload, m_unload, m_edit}}) {
        auto *actions = new QHBoxLayout;
        actions->setSpacing(4);
        for (QPushButton *button : group)
            actions->addWidget(button);
        actions->addStretch();
        details->addLayout(actions);
    }
    m_profileHint = mutedLabel({});
    details->addWidget(m_profileHint);
    m_profileInfo = mutedLabel({});
    m_profileInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->addWidget(m_profileInfo);
    m_showFile = ghostButton(QStringLiteral("Show file"), QStringLiteral("Show the profile's rules"));
    m_showFile->setCheckable(true);
    details->addWidget(m_showFile, 0, Qt::AlignLeft);
    m_profileText = new QPlainTextEdit;
    m_profileText->setReadOnly(true);
    m_profileText->setFont(monoFont());
    m_profileText->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_profileText->setAccessibleName(QStringLiteral("Profile file contents"));
    m_profileText->setMinimumHeight(180);
    m_profileText->hide();
    details->addWidget(m_profileText, 1);
    details->addStretch();
    scroll->setWidget(m_profileDetails);
    frame->layout()->addWidget(scroll);

    connect(m_enforce, &QPushButton::clicked, this, [this] { setMode(Mode::Enforce); });
    connect(m_complain, &QPushButton::clicked, this, [this] { setMode(Mode::Complain); });
    connect(m_toggleDisabled, &QPushButton::clicked, this, [this] { toggleDisabled(); });
    connect(m_reload, &QPushButton::clicked, this, [this] { reloadProfile(); });
    connect(m_unload, &QPushButton::clicked, this, [this] { unloadProfile(); });
    connect(m_edit, &QPushButton::clicked, this, [this] { editProfile(); });
    connect(m_profileEvents, &QPushButton::clicked, this, [this] { showEventsFor(m_selectedProfile); });
    connect(m_showFile, &QPushButton::toggled, this, [this](bool on) {
        m_showFile->setText(on ? QStringLiteral("Hide file") : QStringLiteral("Show file"));
        m_profileText->setVisible(on);
    });
    return frame;
}

QWidget *AppArmorFeature::buildProcesses()
{
    m_processFilter = new QComboBox;
    m_processFilter->setAccessibleName(QStringLiteral("Filter processes"));
    m_processFilter->addItem(QStringLiteral("All processes"), QStringLiteral("all"));
    m_processFilter->addItem(QStringLiteral("Confined"), QStringLiteral("confined"));
    m_processFilter->addItem(QStringLiteral("Enforce"), QStringLiteral("enforce"));
    m_processFilter->addItem(QStringLiteral("Complain"), QStringLiteral("complain"));
    m_processFilter->addItem(QStringLiteral("Unconfined"), QStringLiteral("unconfined"));
    m_processFilter->addItem(QStringLiteral("Unconfined, but a profile exists"), QStringLiteral("missed"));
    QFrame *frame = pane(QStringLiteral("Processes"), QStringLiteral("Double-click a confined process to open its profile."), &m_processesCount,
                         m_processFilter);
    m_processes = table(QStringLiteral("Processes and their AppArmor confinement"),
                        {QStringLiteral("PID"), QStringLiteral("Program"), QStringLiteral("Mode"), QStringLiteral("Profile"),
                         QStringLiteral("Note")},
                        3);
    m_processes->header()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_processes->setColumnWidth(1, 150);
    frame->layout()->addWidget(m_processes);
    connect(m_processFilter, &QComboBox::currentIndexChanged, this, [this] { renderProcesses(); });
    connect(m_processes, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item && !item->data(0, Qt::UserRole).toString().isEmpty())
            selectProfile(item->data(0, Qt::UserRole).toString());
    });
    return frame;
}

QWidget *AppArmorFeature::buildEvents()
{
    auto *tools = new QWidget;
    auto *filters = new QHBoxLayout(tools);
    filters->setContentsMargins(0, 0, 0, 0);
    filters->setSpacing(6);
    m_eventScope = mutedLabel({});
    m_eventScope->setWordWrap(false);
    filters->addWidget(m_eventScope);
    m_eventScopeClear = ghostButton(QStringLiteral("All profiles"), QStringLiteral("Stop filtering by profile"));
    filters->addWidget(m_eventScopeClear);
    m_eventFilter = new QComboBox;
    m_eventFilter->setAccessibleName(QStringLiteral("Filter events"));
    m_eventFilter->addItem(QStringLiteral("Denied and allowed"), QStringLiteral("access"));
    m_eventFilter->addItem(QStringLiteral("Denied"), QStringLiteral("DENIED"));
    m_eventFilter->addItem(QStringLiteral("Allowed in complain mode"), QStringLiteral("ALLOWED"));
    m_eventFilter->addItem(QStringLiteral("Profile loads and other"), QStringLiteral("other"));
    m_eventFilter->addItem(QStringLiteral("Everything"), QStringLiteral("all"));
    filters->addWidget(m_eventFilter);
    QFrame *frame = pane(QStringLiteral("Events"), {}, &m_eventsCount, tools);
    m_eventEmpty = mutedLabel({});
    frame->layout()->addWidget(m_eventEmpty);
    m_events = table(QStringLiteral("AppArmor log events"),
                     {QStringLiteral("Time"), QStringLiteral("Result"), QStringLiteral("Profile"), QStringLiteral("Operation"),
                      QStringLiteral("Target"), QStringLiteral("Access"), QStringLiteral("Program")},
                     4);
    m_events->setSelectionMode(QAbstractItemView::ExtendedSelection);
    frame->layout()->addWidget(m_events);
    auto *actions = new QHBoxLayout;
    m_eventRule = new QLabel;
    m_eventRule->setWordWrap(true);
    m_eventRule->setTextInteractionFlags(Qt::TextSelectableByMouse);
    actions->addWidget(m_eventRule, 1);
    m_allow = new QPushButton(QStringLiteral("Allow…"));
    m_allow->setObjectName(QStringLiteral("primaryButton"));
    m_allow->setToolTip(QStringLiteral("Add rules for the selected events to the profile and reload it"));
    actions->addWidget(m_allow, 0, Qt::AlignTop);
    static_cast<QVBoxLayout *>(frame->layout())->addLayout(actions);

    connect(m_eventFilter, &QComboBox::currentIndexChanged, this, [this] { renderEvents(); });
    connect(m_eventScopeClear, &QPushButton::clicked, this, [this] { showEventsFor({}); });
    connect(m_events, &QTreeWidget::itemSelectionChanged, this, [this] { showEventDetails(); });
    connect(m_allow, &QPushButton::clicked, this, [this] { allowSelected(); });
    return frame;
}

QWidget *AppArmorFeature::buildSuggestions()
{
    auto *column = new QWidget;
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    QFrame *attention = pane(QStringLiteral("Suggestions"), {}, &m_attentionCount);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_attention = richLabel();
    m_attention->setAccessibleName(QStringLiteral("Suggestions"));
    m_attention->setContentsMargins(0, 0, 4, 0);
    scroll->setWidget(m_attention);
    attention->layout()->addWidget(scroll);

    QFrame *status = pane(QStringLiteral("System"), {}, nullptr);
    m_checks = richLabel();
    m_checks->setAccessibleName(QStringLiteral("AppArmor status"));
    status->layout()->addWidget(m_checks);
    status->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    layout->addWidget(attention, 1);
    layout->addWidget(status);

    connect(m_attention, &QLabel::linkActivated, this, [this](const QString &link) { navigate(QUrl::fromPercentEncoding(link.toUtf8())); });
    return column;
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

ProfileFacts AppArmorFeature::factsFor(const ProfileRow &row) const
{
    const ProfileFile *file = fileFor(&row);
    ProfileFacts facts;
    facts.hasFile = file != nullptr;
    facts.editable = file && file->editable;
    facts.disabled = file && file->disabled;
    facts.liveKnown = m_liveProfiles.has_value();
    facts.loaded = row.loaded;
    facts.logKnown = m_liveEvents.has_value() || !m_snapshot.eventSource.isEmpty();
    if (m_liveProfiles && row.loaded)
        facts.mode = row.live;
    else if (file && file->forceComplain)
        facts.mode = Mode::Complain;
    else
        facts.mode = row.declared;
    if (file) {
        for (const ProfileDeclaration &declaration : file->profiles)
            facts.unconfinedFlag = facts.unconfinedFlag || declaredMode(declaration.flags) == Mode::Unconfined;
    }
    facts.processes = row.processes;
    facts.denied = row.denied;
    facts.allowed = row.allowed;
    for (const QString &profile : m_missed) {
        if (profile == row.name)
            ++facts.missed;
    }
    return facts;
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
        m_attention->setText(QStringLiteral("Nothing to show: AppArmor runs on Linux only."));
        m_attentionCount->clear();
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
    QList<Segment> profileSegments;
    for (Mode mode : {Mode::Enforce, Mode::Complain, Mode::Kill, Mode::Prompt, Mode::Unconfined, Mode::Mixed, Mode::Unknown}) {
        const int count = modes.value(modeName(mode));
        if (count > 0)
            profileSegments << Segment{modeName(mode), count, modeColor(mode)};
    }
    if (disabled > 0)
        profileSegments << Segment{QStringLiteral("disabled"), disabled, disabledColor()};
    m_profileBar->setSegments(profileSegments);

    int enforced = 0;
    int complaining = 0;
    int other = 0;
    int unconfined = 0;
    QHash<QString, int> unconfinedPrograms;
    for (const ProcessInfo &process : m_snapshot.processes) {
        switch (process.label.mode) {
        case Mode::Enforce: ++enforced; break;
        case Mode::Complain: ++complaining; break;
        case Mode::Unconfined:
            ++unconfined;
            if (profileFor(process).isEmpty() && !process.name.isEmpty())
                ++unconfinedPrograms[process.name];
            break;
        default: ++other; break;
        }
    }
    QList<Segment> processSegments{{QStringLiteral("enforce"), enforced, modeColor(Mode::Enforce)},
                                   {QStringLiteral("complain"), complaining, modeColor(Mode::Complain)}};
    if (other > 0)
        processSegments << Segment{QStringLiteral("other"), other, modeColor(Mode::Mixed)};
    processSegments << Segment{QStringLiteral("unconfined"), unconfined, modeColor(Mode::Unconfined)};
    m_processBar->setSegments(processSegments);
    const int confined = enforced + complaining + other;
    m_coverage->setSegments(processSegments, confined);

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
    const bool logKnown = !source.isEmpty();
    if (!logKnown)
        m_timelineCaption->setText(QStringLiteral("The log is only readable by an administrator. Press Live view to read it once."));
    else
        m_timelineCaption->setText(QStringLiteral("%1 denied &nbsp;%2 let through by complain mode &nbsp;·&nbsp; 7 days: %3 denied, %4 let through")
                                       .arg(dotHtml(deniedColor()), dotHtml(allowedColor()))
                                       .arg(denied)
                                       .arg(allowed));

    QList<RankBars::Entry> busiest;
    for (const ProfileRow &row : m_rows) {
        if (row.denied + row.allowed > 0)
            busiest << RankBars::Entry{row.name, row.denied, row.allowed};
    }
    std::sort(busiest.begin(), busiest.end(), [](const RankBars::Entry &left, const RankBars::Entry &right) {
        return left.denied + left.allowed > right.denied + right.allowed;
    });
    m_busiest->setEntries(busiest, deniedColor(), allowedColor(),
                          logKnown ? QStringLiteral("No profile blocked or logged anything in 7 days.")
                                   : QStringLiteral("The log is only readable by an administrator. Press Live view to see which profiles "
                                                    "block the most."));
    m_busiestCaption->setText(QStringLiteral("Events per profile in 7 days. Click one to open it."));
    m_busiestCaption->setVisible(!busiest.isEmpty());

    QString state;
    AdviceTone stateTone = AdviceTone::Good;
    if (m_snapshot.enabled && m_snapshot.inLsmList)
        state = QStringLiteral("AppArmor is on");
    else if (m_snapshot.moduleLoaded) {
        state = QStringLiteral("AppArmor is built in but turned off");
        stateTone = AdviceTone::Problem;
    } else {
        state = QStringLiteral("AppArmor is not available in this kernel");
        stateTone = AdviceTone::Problem;
    }
    m_summary->setText(QStringLiteral("%1 <b>%2</b> · %3 profiles · %4 of %5 processes confined · %6")
                           .arg(dotHtml(toneColor(stateTone)), state)
                           .arg(m_rows.size())
                           .arg(confined)
                           .arg(m_snapshot.processes.size())
                           .arg(logKnown ? QStringLiteral("%1 denials in 7 days").arg(denied) : QStringLiteral("log not read yet")));

    QStringList checks;
    auto check = [&checks](const QString &item, const QString &value, bool good) {
        checks << QStringLiteral("<tr><td style=\"color:%1\">%2</td><td style=\"padding:0 8px 0 4px\"><b>%3</b></td><td>%4</td></tr>")
                      .arg((good ? toneColor(AdviceTone::Good) : toneColor(AdviceTone::Problem)).name(),
                           good ? QStringLiteral("&#10003;") : QStringLiteral("&#10007;"), item.toHtmlEscaped(), value.toHtmlEscaped());
    };
    check(QStringLiteral("Kernel"), m_snapshot.enabled ? QStringLiteral("Enabled")
                                    : m_snapshot.moduleLoaded ? QStringLiteral("Built in, turned off (add apparmor=1 security=apparmor to the boot line)")
                                                              : QStringLiteral("Not in this kernel"),
          m_snapshot.enabled);
    const QStringList modules = m_snapshot.lsm.split(QLatin1Char(','), Qt::SkipEmptyParts);
    check(QStringLiteral("Modules"),
          modules.isEmpty()           ? QStringLiteral("Unknown")
          : m_snapshot.inLsmList      ? QStringLiteral("AppArmor is one of %1 active").arg(modules.size())
                                      : QStringLiteral("AppArmor is not among the %1 active").arg(modules.size()),
          m_snapshot.inLsmList);
    check(QStringLiteral("Compiler"),
          m_snapshot.parserPath.isEmpty() ? QStringLiteral("apparmor_parser is not installed; install the apparmor package") : m_snapshot.parserPath,
          !m_snapshot.parserPath.isEmpty());
    int fileCount = 0;
    int snapCount = 0;
    for (const ProfileFile &file : m_snapshot.files)
        (file.source == QStringLiteral("snap") ? snapCount : fileCount)++;
    check(QStringLiteral("Files"),
          QStringLiteral("%1 in %2%3").arg(fileCount).arg(m_snapshot.policyDir, snapCount ? QStringLiteral(", %1 from Snap").arg(snapCount) : QString()),
          fileCount > 0);
    check(QStringLiteral("Loaded"),
          m_liveProfiles ? QStringLiteral("%1, read at %2").arg(m_liveProfiles->size()).arg(m_liveReadAt.toString(QStringLiteral("HH:mm")))
                         : QStringLiteral("Needs Live view"),
          m_liveProfiles.has_value());
    check(QStringLiteral("Log"), logKnown ? source : QStringLiteral("Needs Live view"), logKnown);
    m_checks->setText(QStringLiteral("<table cellspacing=\"0\" cellpadding=\"1\">%1</table>").arg(checks.join(QString())));
    m_checks->setToolTip(modules.isEmpty() ? QString() : QStringLiteral("Active security modules: %1").arg(modules.join(QStringLiteral(", "))));

    QStringList items;
    auto attention = [&items](AdviceTone tone, const QString &what, const QString &detail, const QString &target, const QString &linkText) {
        QString link;
        if (!target.isEmpty())
            link = QStringLiteral(" <a href=\"%1\">%2</a>").arg(linkFor(target), linkText.toHtmlEscaped());
        items << QStringLiteral("<p style=\"margin:0 0 8px 0\">%1 <b>%2</b><br>%3%4</p>")
                     .arg(dotHtml(toneColor(tone)), what.toHtmlEscaped(), detail.toHtmlEscaped(), link);
    };
    if (!m_snapshot.enabled)
        attention(AdviceTone::Problem, QStringLiteral("AppArmor is off"),
                  QStringLiteral("No profile protects anything until it is turned on at boot: add apparmor=1 security=apparmor to the kernel "
                                 "command line and restart."),
                  {}, {});
    if (m_snapshot.enabled && m_snapshot.parserPath.isEmpty())
        attention(AdviceTone::Problem, QStringLiteral("apparmor_parser is missing"),
                  QStringLiteral("Profiles cannot be loaded or changed without it. Install the apparmor package."), {}, {});
    if (!logKnown)
        attention(AdviceTone::Info, QStringLiteral("Read the log"),
                  QStringLiteral("Only an administrator can read what AppArmor blocked. Live view reads it once and changes nothing."),
                  QStringLiteral("live:"), QStringLiteral("Live view"));
    QList<const ProfileRow *> noisy;
    for (const ProfileRow &row : m_rows) {
        if (row.denied > 0)
            noisy << &row;
    }
    std::sort(noisy.begin(), noisy.end(), [](const ProfileRow *left, const ProfileRow *right) { return left->denied > right->denied; });
    for (qsizetype i = 0; i < std::min<qsizetype>(noisy.size(), 5); ++i) {
        const ProfileRow *row = noisy.at(i);
        const QString about = programDescription(row->name);
        attention(AdviceTone::Problem, row->name,
                  QStringLiteral("%1Blocked %2 %3 in 7 days. If the program works, the profile is doing its job; if something broke, "
                                 "allow the denials it needs.")
                      .arg(about.isEmpty() ? QString() : about + QLatin1Char(' '))
                      .arg(row->denied)
                      .arg(row->denied == 1 ? QStringLiteral("access") : QStringLiteral("accesses")),
                  QStringLiteral("events:") + row->name, QStringLiteral("Review denials"));
    }
    QStringList learning;
    QStringList readyToEnforce;
    QStringList complainProfiles;
    for (const ProfileRow &row : m_rows) {
        if (displayMode(row) != modeName(Mode::Complain))
            continue;
        complainProfiles << row.name;
        if (row.allowed > 0)
            learning << row.name;
        else if (logKnown && row.processes > 0)
            readyToEnforce << row.name;
    }
    for (const QString &name : learning.mid(0, 3)) {
        const ProfileRow *row = rowByName(name);
        attention(AdviceTone::Warning, name,
                  QStringLiteral("Complain mode let %1 %2 through. Allow what the program needs, then switch it to enforce.")
                      .arg(row->allowed)
                      .arg(row->allowed == 1 ? QStringLiteral("access") : QStringLiteral("accesses")),
                  QStringLiteral("events:") + name, QStringLiteral("Review"));
    }
    for (const QString &name : readyToEnforce.mid(0, 3))
        attention(AdviceTone::Warning, name,
                  QStringLiteral("Running in complain mode with a quiet log: enforce mode would block nothing it does. Switch it to enforce "
                                 "to protect it."),
                  QStringLiteral("profile:") + name, QStringLiteral("Open"));
    QList<const ProcessInfo *> missed;
    for (const ProcessInfo &process : m_snapshot.processes) {
        if (!profileFor(process).isEmpty())
            missed << &process;
    }
    for (qsizetype i = 0; i < std::min<qsizetype>(missed.size(), 3); ++i)
        attention(AdviceTone::Warning, QStringLiteral("%1 (pid %2)").arg(missed.at(i)->name).arg(missed.at(i)->pid),
                  QStringLiteral("Runs unconfined although profile %1 matches it, because it started first. Restart the program to confine it.")
                      .arg(profileFor(*missed.at(i))),
                  QStringLiteral("profile:") + profileFor(*missed.at(i)), QStringLiteral("Open profile"));
    if (missed.size() > 3)
        attention(AdviceTone::Warning, QStringLiteral("%1 more unconfined processes with a profile").arg(missed.size() - 3),
                  QStringLiteral("Restart them to confine them."), QStringLiteral("processes:missed"), QStringLiteral("Show them"));
    const int otherComplain = int(complainProfiles.size() - learning.mid(0, 3).size() - readyToEnforce.mid(0, 3).size());
    if (otherComplain > 0)
        attention(AdviceTone::Info, QStringLiteral("%1 profiles only log").arg(complainProfiles.size()),
                  QStringLiteral("Complain mode blocks nothing: %1. Switch each to enforce once its program has run a while with a quiet log.")
                      .arg(complainProfiles.mid(0, 5).join(QStringLiteral(", ")) + (complainProfiles.size() > 5 ? QStringLiteral("…") : QString())),
                  QStringLiteral("filter:complain"), QStringLiteral("List them"));
    for (const ProfileRow &row : m_rows) {
        if (m_liveProfiles && fileFor(&row) && !fileFor(&row)->disabled && !row.loaded)
            attention(AdviceTone::Problem, row.name, QStringLiteral("Has a file but is not loaded, so it protects nothing. Reload it."),
                      QStringLiteral("profile:") + row.name, QStringLiteral("Open"));
    }
    QList<QPair<QString, int>> topUnconfined;
    for (auto it = unconfinedPrograms.constBegin(); it != unconfinedPrograms.constEnd(); ++it)
        topUnconfined.append({it.key(), it.value()});
    std::sort(topUnconfined.begin(), topUnconfined.end(), [](const QPair<QString, int> &left, const QPair<QString, int> &right) {
        return left.second != right.second ? left.second > right.second : left.first < right.first;
    });
    if (!topUnconfined.isEmpty()) {
        QStringList names;
        for (qsizetype i = 0; i < std::min<qsizetype>(topUnconfined.size(), 5); ++i)
            names << QStringLiteral("%1 (%2)").arg(topUnconfined.at(i).first).arg(topUnconfined.at(i).second);
        attention(AdviceTone::Info, QStringLiteral("Most running programs have no profile"),
                  QStringLiteral("Normal on most systems: distributions confine network services and a few apps. Busiest unconfined: %1. "
                                 "A profile helps most for programs that open files or pages from others (browsers, chat, PDF viewers).")
                      .arg(names.join(QStringLiteral(", "))),
                  QStringLiteral("new:"), QStringLiteral("New profile…"));
    }
    if (items.isEmpty())
        attention(AdviceTone::Good, QStringLiteral("Nothing stands out"), QStringLiteral("No denials, unconfined matches or complain-mode profiles."),
                  {}, {});
    m_attention->setText(items.join(QString()));
    const int count = int(items.size());
    m_attentionCount->setText(count == 1 ? QStringLiteral("1 item") : QStringLiteral("%1 items").arg(count));
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
        const QString about = programDescription(row.name);
        if (!show || !matchesQuery({row.name, row.attachment, file ? file->fileName : QString(), mode, about}))
            continue;
        const QString state = displayState(row);
        const bool usual = state == QStringLiteral("In policy") || state == QStringLiteral("Loaded");
        auto *item = new QTreeWidgetItem(m_profiles, {row.name, usual ? mode : QStringLiteral("%1 · %2").arg(mode, state.toLower()),
                                                      row.processes ? QString::number(row.processes) : QString(),
                                                      row.denied ? QString::number(row.denied) : QString()});
        item->setData(0, Qt::UserRole, row.name);
        const QColor color = disabled ? disabledColor() : modeColor(modeFromText(mode));
        item->setIcon(1, dotIcon(color));
        QStringList tip{row.name};
        if (!about.isEmpty())
            tip << about;
        if (!row.attachment.isEmpty() && row.attachment != row.name)
            tip << QStringLiteral("Attaches to %1").arg(row.attachment);
        if (file)
            tip << file->path;
        item->setToolTip(0, tip.join(QLatin1Char('\n')));
        item->setToolTip(1, QStringLiteral("%1\n%2").arg(state, modeExplanation(modeFromText(mode))).trimmed());
        if (row.denied)
            item->setForeground(3, deniedColor());
        if (row.name == keep)
            selected = item;
    }
    m_profilesCount->setText(m_profiles->topLevelItemCount() == m_rows.size()
                                 ? QString::number(m_rows.size())
                                 : QStringLiteral("%1 of %2").arg(m_profiles->topLevelItemCount()).arg(m_rows.size()));
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
    m_profileEmpty->setVisible(row == nullptr);
    m_recommended = AdviceAction::None;
    if (!row) {
        updateActions();
        return;
    }
    const ProfileFile *file = fileFor(row);
    const ProfileFacts facts = factsFor(*row);
    m_profileTitle->setText(row->name);
    const QColor color = facts.disabled ? disabledColor() : modeColor(facts.mode);
    m_profileMode->setText(QStringLiteral("%1 %2 · %3").arg(dotHtml(color), displayMode(*row).toHtmlEscaped(), displayState(*row).toHtmlEscaped()));
    QStringList about;
    const QString description = programDescription(row->name);
    if (!description.isEmpty())
        about << description.toHtmlEscaped();
    if (facts.disabled)
        about << QStringLiteral("Disabled: AppArmor does not load this profile, so the program runs without limits.");
    else if (!modeExplanation(facts.mode).isEmpty())
        about << modeExplanation(facts.mode).toHtmlEscaped();
    m_profileAbout->setText(about.join(QStringLiteral("<br>")));
    m_profileAbout->setVisible(!about.isEmpty());

    const QList<Advice> advice = profileAdvice(facts);
    QStringList lines;
    for (const Advice &entry : advice) {
        lines << QStringLiteral("<p style=\"margin:0 0 4px 0\">%1 %2</p>").arg(dotHtml(toneColor(entry.tone)), entry.text.toHtmlEscaped());
        if (m_recommended == AdviceAction::None)
            m_recommended = entry.action;
    }
    m_profileAdvice->setText(lines.join(QString()));
    m_profileAdvice->setVisible(!lines.isEmpty());

    QList<Event> own;
    for (const Event &event : events()) {
        if (topLevelProfile(event.profile) == row->name)
            own << event;
    }
    m_profileTimeline->setBuckets(hourlyBuckets(own, QDateTime::currentDateTime(), 24 * 7));
    m_profileTimeline->setToolTip(QStringLiteral("This profile's events per hour over 7 days"));
    m_profileTimeline->setVisible(facts.logKnown);

    QStringList info;
    info << QStringLiteral("Running processes: %1 · Denied: %2 · Let through in complain mode: %3").arg(row->processes).arg(row->denied).arg(row->allowed);
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
    m_profileInfo->setText(info.join(QLatin1Char('\n')));
    m_profileText->setPlainText(file ? file->text : QString());
    m_showFile->setVisible(file != nullptr);
    m_profileText->setVisible(file != nullptr && m_showFile->isChecked());
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
        const QString note = missed.isEmpty() ? QString() : QStringLiteral("Restart to confine");
        auto *item = new QTreeWidgetItem(m_processes, {QString::number(process.pid), program, modeName(mode),
                                                       missed.isEmpty() ? process.label.profile : missed, note});
        item->setData(0, Qt::UserRole, missed.isEmpty() ? topLevelProfile(process.label.profile) : missed);
        item->setIcon(2, dotIcon(modeColor(mode)));
        const QString tip = process.command.isEmpty() ? process.executable : process.command;
        for (int column = 0; column < 4; ++column)
            item->setToolTip(column, tip);
        if (!missed.isEmpty()) {
            item->setForeground(4, modeColor(Mode::Complain));
            item->setToolTip(4, QStringLiteral("Profile %1 matches this program, but the process started before it was loaded. Restart the "
                                               "program to confine it.").arg(missed));
        }
    }
    const int total = int(m_snapshot.processes.size());
    m_processesCount->setText(m_processes->topLevelItemCount() == total ? QString::number(total)
                                                                        : QStringLiteral("%1 of %2").arg(m_processes->topLevelItemCount()).arg(total));
}

void AppArmorFeature::renderEvents()
{
    const QString filter = m_eventFilter->currentData().toString();
    m_eventScope->setText(m_eventProfile.isEmpty() ? QString() : QStringLiteral("Only %1").arg(m_eventProfile));
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
        const QColor color = event.result == QStringLiteral("DENIED") ? deniedColor()
                             : event.result == QStringLiteral("ALLOWED") ? allowedColor()
                                                                         : modeColor(Mode::Unknown);
        item->setIcon(1, dotIcon(color));
        item->setToolTip(0, event.raw);
        item->setToolTip(4, event.target);
        ++shown;
    }
    m_eventsCount->setText(shown == 5000 ? QStringLiteral("first 5,000") : QString::number(shown));
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
        m_eventRule->setToolTip({});
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
    m_eventRule->setToolTip(first >= 0 && first < all.size() ? all.at(first).raw : QString());
    if (rules.isEmpty())
        m_eventRule->setText(QStringLiteral("No rule can be suggested for this event. Use Edit on the profile if it needs one."));
    else if (profiles.size() > 1)
        m_eventRule->setText(QStringLiteral("Select events from one profile at a time to allow them."));
    else
        m_eventRule->setText(QStringLiteral("Rule to add to %1:\n%2").arg(*profiles.begin(), rules.join(QLatin1Char('\n'))));
    updateActions();
}

void AppArmorFeature::selectProfile(const QString &name)
{
    m_selectedProfile = name;
    m_profileFilter->blockSignals(true);
    m_profileFilter->setCurrentIndex(0);
    m_profileFilter->blockSignals(false);
    renderProfiles();
    m_profiles->setFocus();
}

void AppArmorFeature::showEventsFor(const QString &profile)
{
    m_eventProfile = profile;
    renderEvents();
}

void AppArmorFeature::navigate(const QString &target)
{
    if (target.startsWith(QStringLiteral("profile:")))
        selectProfile(target.mid(8));
    else if (target.startsWith(QStringLiteral("events:"))) {
        selectProfile(target.mid(7));
        showEventsFor(target.mid(7));
    } else if (target.startsWith(QStringLiteral("filter:")))
        m_profileFilter->setCurrentIndex(std::max(0, m_profileFilter->findData(target.mid(7))));
    else if (target.startsWith(QStringLiteral("processes:")))
        m_processFilter->setCurrentIndex(std::max(0, m_processFilter->findData(target.mid(10))));
    else if (target == QStringLiteral("live:"))
        readLive();
    else if (target == QStringLiteral("new:"))
        newProfile();
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
    m_profileHint->setText(file && m_snapshot.parserPath.isEmpty() ? QStringLiteral("apparmor_parser is not installed, so profiles cannot be loaded from here.")
                                                                   : QString());
    m_profileHint->setVisible(!m_profileHint->text().isEmpty());

    QPushButton *recommended = nullptr;
    switch (m_recommended) {
    case AdviceAction::Enforce: recommended = m_enforce; break;
    case AdviceAction::Complain: recommended = m_complain; break;
    case AdviceAction::Enable: recommended = m_toggleDisabled; break;
    case AdviceAction::Reload: recommended = m_reload; break;
    case AdviceAction::Events: recommended = m_profileEvents; break;
    case AdviceAction::LiveView:
    case AdviceAction::None: break;
    }
    for (QPushButton *button : {m_enforce, m_complain, m_toggleDisabled, m_reload, m_profileEvents}) {
        const QString name = button == recommended && button->isEnabled() ? QStringLiteral("primaryButton") : QStringLiteral("ghostButton");
        if (button->objectName() == name)
            continue;
        button->setObjectName(name);
        button->style()->unpolish(button);
        button->style()->polish(button);
    }

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
            error = QStringLiteral("%1/%2 already exists; select it in Profiles and use Edit instead.").arg(policyDir, profile);
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
        setStatus(QStringLiteral("%1 is a child profile inside another one. Add the rule to it with Edit.").arg(profile));
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
             adminInstallScript(),
             {temp->fileName(), dest, backupDir(), load,
              QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex())},
             true, temp);
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
