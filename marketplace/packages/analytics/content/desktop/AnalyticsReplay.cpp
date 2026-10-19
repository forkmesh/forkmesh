#include "AnalyticsReplay.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace forkmesh::analytics {
namespace {

constexpr qint64 kTrailMs = 1500;
constexpr qint64 kClickMs = 900;
constexpr qint64 kIdleJumpMs = 2000;
constexpr int kMaxPagesLoaded = 200;

double numberOf(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toDouble();
}

template <typename Item>
int lastAtOrBefore(const QList<Item> &items, qint64 at)
{
    const auto found = std::upper_bound(items.begin(), items.end(), at,
                                        [](qint64 value, const Item &item) { return value < item.at; });
    return int(found - items.begin()) - 1;
}

double scrollAt(const QList<MotionScroll> &scrolls, qint64 at)
{
    const int index = lastAtOrBefore(scrolls, at);
    return index >= 0 ? scrolls.at(index).y : 0;
}

QColor accentColor(const QWidget *widget)
{
    return widget->palette().color(QPalette::Highlight);
}

void drawPlaceholders(QPainter &painter, const QRectF &area, double fromY, double toY, double scale, double offsetY,
                      const QColor &ink)
{
    constexpr double step = 120;
    QColor bar = ink;
    bar.setAlphaF(0.16);
    QColor block = ink;
    block.setAlphaF(0.08);
    painter.setPen(Qt::NoPen);
    for (double y = std::floor(std::max(0.0, fromY) / step) * step; y < toY; y += step) {
        const int index = int(y / step);
        const double top = area.top() + (y - offsetY) * scale;
        if (index % 5 == 2) {
            painter.setBrush(block);
            painter.drawRoundedRect(QRectF(area.left() + area.width() * 0.06, top + 10 * scale,
                                           area.width() * 0.88, 92 * scale), 3, 3);
            continue;
        }
        const double width = 0.3 + double((index * 37) % 50) / 100.0;
        painter.setBrush(bar);
        painter.drawRoundedRect(QRectF(area.left() + area.width() * 0.06, top + 24 * scale,
                                       area.width() * width, std::max(1.5, 14 * scale)), 2, 2);
        painter.drawRoundedRect(QRectF(area.left() + area.width() * 0.06, top + 52 * scale,
                                       area.width() * width * 0.7, std::max(1.0, 9 * scale)), 2, 2);
    }
}

} // namespace

QString MotionTrack::countryFlag(const QString &code)
{
    if (code.size() != 2)
        return QString();
    QString flag;
    for (const QChar c : code.toUpper()) {
        if (c < QLatin1Char('A') || c > QLatin1Char('Z'))
            return QString();
        const char32_t point = 0x1F1E6 + (c.unicode() - 'A');
        flag += QString::fromUcs4(&point, 1);
    }
    return flag;
}

QString MotionTrack::pathOf(const QString &url)
{
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.host().isEmpty())
        return url;
    const QString path = parsed.path();
    return path.isEmpty() ? QStringLiteral("/") : path;
}

void MotionTrack::append(const QJsonArray &events)
{
    for (const QJsonValue &value : events)
        if (value.isObject())
            m_raw.append(value);
}

bool MotionTrack::isEmpty() const
{
    return m_end <= m_start;
}

void MotionTrack::finish()
{
    QList<std::pair<qint64, QJsonObject>> events;
    events.reserve(m_raw.size());
    for (const QJsonValue &value : std::as_const(m_raw)) {
        const QJsonObject event = value.toObject();
        events.append({qint64(numberOf(event, "t")), event});
    }
    m_raw = QJsonArray();
    std::stable_sort(events.begin(), events.end(),
                     [](const auto &left, const auto &right) { return left.first < right.first; });
    m_moves.clear();
    m_clicks.clear();
    m_scrolls.clear();
    m_viewports.clear();
    m_pages.clear();
    m_activity.clear();
    m_start = events.isEmpty() ? 0 : events.first().first;
    m_end = m_start;
    double document = -1;
    double tallest = 0;
    for (const auto &[at, event] : std::as_const(events)) {
        const QString kind = event.value(QStringLiteral("k")).toString();
        const QJsonObject data = event.value(QStringLiteral("d")).toObject();
        m_end = std::max(m_end, at);
        if (kind == QLatin1String("meta")) {
            tallest = numberOf(data, "pageHeight");
            m_pages.append({at, data.value(QStringLiteral("href")).toString(), tallest});
            m_viewports.append({at, QSize(int(numberOf(data, "width")), int(numberOf(data, "height")))});
            m_activity.append(at);
        } else if (kind == QLatin1String("full")) {
            document = numberOf(data, "doc");
            m_scrolls.append({at, numberOf(data, "y"), tallest});
        } else if (kind == QLatin1String("resize")) {
            m_viewports.append({at, QSize(int(numberOf(data, "width")), int(numberOf(data, "height")))});
            m_activity.append(at);
        } else if (kind == QLatin1String("scroll")) {
            if (numberOf(data, "id") != document)
                continue;
            tallest = std::max(tallest, numberOf(data, "h"));
            m_scrolls.append({at, numberOf(data, "y"), tallest});
            m_activity.append(at);
        } else if (kind == QLatin1String("move")) {
            for (const QJsonValue &sample : data.value(QStringLiteral("positions")).toArray()) {
                const QJsonArray position = sample.toArray();
                const qint64 when = at + qint64(position.at(2).toDouble());
                m_moves.append({when, QPointF(position.at(0).toDouble(), position.at(1).toDouble())});
                m_activity.append(when);
                m_end = std::max(m_end, when);
            }
        } else if (kind == QLatin1String("mouse")) {
            m_clicks.append({at, QPointF(numberOf(data, "x"), numberOf(data, "y")), 0,
                             data.value(QStringLiteral("type")).toString()});
            m_activity.append(at);
        } else if (kind == QLatin1String("input")) {
            m_activity.append(at);
        }
    }
    const auto byTime = [](const auto &left, const auto &right) { return left.at < right.at; };
    std::stable_sort(m_moves.begin(), m_moves.end(), byTime);
    std::stable_sort(m_clicks.begin(), m_clicks.end(), byTime);
    std::sort(m_activity.begin(), m_activity.end());
    for (MotionClick &click : m_clicks)
        click.scrollY = scrollAt(m_scrolls, click.at);
}

qint64 MotionTrack::nextActivity(qint64 at) const
{
    const auto found = std::upper_bound(m_activity.begin(), m_activity.end(), at);
    return found == m_activity.end() ? m_end : *found;
}

MotionState MotionTrack::stateAt(qint64 at) const
{
    MotionState state;
    const int viewport = lastAtOrBefore(m_viewports, at);
    if (viewport >= 0)
        state.viewport = m_viewports.at(viewport).size;
    else if (!m_viewports.isEmpty())
        state.viewport = m_viewports.first().size;
    if (state.viewport.width() <= 0 || state.viewport.height() <= 0)
        state.viewport = QSize(1280, 800);
    state.pageIndex = lastAtOrBefore(m_pages, at);
    if (state.pageIndex < 0 && !m_pages.isEmpty())
        state.pageIndex = 0;
    qint64 pageStart = m_start;
    if (state.pageIndex >= 0) {
        const MotionPage &page = m_pages.at(state.pageIndex);
        state.href = page.href;
        state.pageHeight = page.height;
        pageStart = page.at;
    }
    const int scroll = lastAtOrBefore(m_scrolls, at);
    if (scroll >= 0 && m_scrolls.at(scroll).at >= pageStart) {
        state.scrollY = m_scrolls.at(scroll).y;
        state.pageHeight = std::max(state.pageHeight, m_scrolls.at(scroll).height);
    }
    state.pageHeight = std::max(state.pageHeight, state.scrollY + state.viewport.height());
    const int move = lastAtOrBefore(m_moves, at);
    if (move >= 0) {
        state.hasCursor = true;
        state.cursor = m_moves.at(move).point;
        if (move + 1 < m_moves.size()) {
            const MotionPoint &from = m_moves.at(move);
            const MotionPoint &to = m_moves.at(move + 1);
            const qint64 span = to.at - from.at;
            if (span > 0 && span < 1000) {
                const double share = double(at - from.at) / double(span);
                state.cursor = from.point + (to.point - from.point) * share;
            }
        }
        for (int index = move; index >= 0 && m_moves.at(index).at > at - kTrailMs; --index)
            state.trail.prepend(m_moves.at(index));
    }
    for (int index = lastAtOrBefore(m_clicks, at); index >= 0 && m_clicks.at(index).at > at - kClickMs; --index)
        state.clicks.append(m_clicks.at(index));
    return state;
}

class ReplayStage final : public QWidget
{
public:
    explicit ReplayStage(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("analyticsReplayStage"));
        setAccessibleName(QStringLiteral("Recording playback"));
        setMinimumSize(420, 280);
    }

    void present(const MotionTrack *track, const MotionState &state, qint64 at)
    {
        m_track = track;
        m_state = state;
        m_at = at;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QColor ink = palette().color(QPalette::WindowText);
        const QColor base = palette().color(QPalette::Base);
        const QColor accent = accentColor(this);
        QColor frame = palette().color(QPalette::PlaceholderText);
        frame.setAlphaF(0.45);
        const QSize viewport = m_state.viewport;
        const QRectF bounds = QRectF(rect()).adjusted(4, 4, -4, -4);
        constexpr double mapWidth = 46;
        constexpr double barHeight = 24;
        const QRectF main(bounds.left(), bounds.top(), bounds.width() - mapWidth - 12, bounds.height());
        const double scale = std::min(main.width() / viewport.width(),
                                      (main.height() - barHeight) / viewport.height());
        const QSizeF shown(viewport.width() * scale, viewport.height() * scale);
        const QRectF window(main.left() + (main.width() - shown.width()) / 2, main.top(), shown.width(),
                            shown.height() + barHeight);
        const QRectF page(window.left(), window.top() + barHeight, shown.width(), shown.height());

        painter.setPen(QPen(frame, 1));
        painter.setBrush(palette().color(QPalette::Window).darker(104));
        painter.drawRoundedRect(window, 6, 6);
        painter.setPen(Qt::NoPen);
        painter.setBrush(base);
        painter.drawRect(page);
        painter.setPen(palette().color(QPalette::PlaceholderText));
        const QRectF address = QRectF(window.left() + 10, window.top(), window.width() - 20, barHeight);
        painter.drawText(address, Qt::AlignVCenter | Qt::AlignLeft,
                         painter.fontMetrics().elidedText(m_state.href.isEmpty() ? QStringLiteral("—") : m_state.href,
                                                          Qt::ElideMiddle, int(address.width())));

        painter.save();
        painter.setClipRect(page);
        drawPlaceholders(painter, page, m_state.scrollY, m_state.scrollY + viewport.height(), scale,
                         m_state.scrollY, ink);
        for (const MotionClick &click : std::as_const(m_state.clicks)) {
            const double age = double(m_at - click.at) / double(kClickMs);
            QColor ring = accent;
            ring.setAlphaF(std::clamp(1.0 - age, 0.0, 1.0));
            painter.setPen(QPen(ring, 2));
            painter.setBrush(Qt::NoBrush);
            const QPointF centre = page.topLeft() + click.point * scale;
            const double radius = 6 + age * 22;
            painter.drawEllipse(centre, radius, radius);
        }
        if (m_state.trail.size() > 1) {
            for (int index = 1; index < m_state.trail.size(); ++index) {
                QColor stroke = accent;
                stroke.setAlphaF(0.55 * double(index) / double(m_state.trail.size()));
                painter.setPen(QPen(stroke, 2, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(page.topLeft() + m_state.trail.at(index - 1).point * scale,
                                 page.topLeft() + m_state.trail.at(index).point * scale);
            }
        }
        if (m_state.hasCursor) {
            const QPointF tip = page.topLeft() + m_state.cursor * scale;
            QPainterPath arrow;
            arrow.moveTo(tip);
            arrow.lineTo(tip + QPointF(0, 17));
            arrow.lineTo(tip + QPointF(4.5, 13));
            arrow.lineTo(tip + QPointF(8, 20));
            arrow.lineTo(tip + QPointF(10.5, 19));
            arrow.lineTo(tip + QPointF(7.2, 12));
            arrow.lineTo(tip + QPointF(12.5, 12));
            arrow.closeSubpath();
            painter.setPen(QPen(Qt::white, 1.4));
            painter.setBrush(QColor(20, 20, 20));
            painter.drawPath(arrow);
        }
        painter.restore();

        const QRectF map(bounds.right() - mapWidth, bounds.top(), mapWidth, bounds.height());
        const double total = std::max(1.0, m_state.pageHeight);
        const double mapScale = map.height() / total;
        painter.setPen(QPen(frame, 1));
        painter.setBrush(base);
        painter.drawRoundedRect(map, 4, 4);
        painter.save();
        painter.setClipRect(map);
        drawPlaceholders(painter, map, 0, total, mapScale, 0, ink);
        if (m_track && m_state.pageIndex >= 0) {
            const qint64 since = m_track->pages().at(m_state.pageIndex).at;
            QColor dot = accent;
            dot.setAlphaF(0.7);
            painter.setPen(Qt::NoPen);
            painter.setBrush(dot);
            for (const MotionClick &click : m_track->clicks()) {
                if (click.at < since || click.at > m_at)
                    continue;
                const QPointF at(map.left() + click.point.x() / viewport.width() * map.width(),
                                 map.top() + (click.point.y() + click.scrollY) * mapScale);
                painter.drawEllipse(at, 2.2, 2.2);
            }
        }
        QColor fill = accent;
        fill.setAlphaF(0.16);
        painter.setPen(QPen(accent, 1.5));
        painter.setBrush(fill);
        painter.drawRect(QRectF(map.left() + 1, map.top() + m_state.scrollY * mapScale, map.width() - 2,
                                std::max(4.0, viewport.height() * mapScale)));
        painter.restore();
    }

private:
    const MotionTrack *m_track = nullptr;
    MotionState m_state;
    qint64 m_at = 0;
};

class ReplayTimeline final : public QWidget
{
public:
    explicit ReplayTimeline(std::function<void(qint64)> seek, QWidget *parent = nullptr)
        : QWidget(parent), m_seek(std::move(seek))
    {
        setObjectName(QStringLiteral("analyticsReplayTimeline"));
        setAccessibleName(QStringLiteral("Seek"));
        setFixedHeight(24);
        setCursor(Qt::PointingHandCursor);
    }

    void setTrack(const MotionTrack *track)
    {
        m_track = track;
        update();
    }

    void setPosition(qint64 at)
    {
        m_at = at;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!m_track || m_track->isEmpty())
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF track = trackRect();
        QColor rail = palette().color(QPalette::PlaceholderText);
        rail.setAlphaF(0.3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(rail);
        painter.drawRoundedRect(QRectF(track.left(), track.center().y() - 2, track.width(), 4), 2, 2);
        const QColor accent = accentColor(this);
        painter.setBrush(accent);
        const double played = xFor(m_at);
        painter.drawRoundedRect(QRectF(track.left(), track.center().y() - 2, played - track.left(), 4), 2, 2);
        painter.setPen(QPen(palette().color(QPalette::WindowText), 1.5));
        for (const MotionPage &page : m_track->pages()) {
            const double x = xFor(page.at);
            painter.drawLine(QPointF(x, track.top() + 2), QPointF(x, track.bottom() - 2));
        }
        QColor click = accent;
        click.setAlphaF(0.8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(click);
        for (const MotionClick &item : m_track->clicks())
            painter.drawEllipse(QPointF(xFor(item.at), track.top() + 3), 2, 2);
        painter.setBrush(palette().color(QPalette::Base));
        painter.setPen(QPen(accent, 2));
        painter.drawEllipse(QPointF(played, track.center().y()), 6, 6);
    }

    void mousePressEvent(QMouseEvent *event) override { seekTo(event->position().x()); }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons() & Qt::LeftButton)
            seekTo(event->position().x());
    }

private:
    QRectF trackRect() const { return QRectF(8, 2, std::max(1, width() - 16), height() - 4); }

    double xFor(qint64 at) const
    {
        const QRectF track = trackRect();
        const double span = double(std::max<qint64>(1, m_track->end() - m_track->start()));
        return track.left() + track.width() * std::clamp(double(at - m_track->start()) / span, 0.0, 1.0);
    }

    void seekTo(double x)
    {
        if (!m_track || m_track->isEmpty())
            return;
        const QRectF track = trackRect();
        const double share = std::clamp((x - track.left()) / track.width(), 0.0, 1.0);
        m_seek(m_track->start() + qint64(share * double(m_track->end() - m_track->start())));
    }

    std::function<void(qint64)> m_seek;
    const MotionTrack *m_track = nullptr;
    qint64 m_at = 0;
};

QString AnalyticsReplayDialog::clockText(qint64 milliseconds)
{
    const qint64 seconds = std::max<qint64>(0, milliseconds / 1000);
    if (seconds >= 3600)
        return QStringLiteral("%1:%2:%3")
            .arg(seconds / 3600)
            .arg(seconds % 3600 / 60, 2, 10, QLatin1Char('0'))
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

AnalyticsReplayDialog::AnalyticsReplayDialog(const QJsonObject &recording, Fetch fetch, QWidget *parent)
    : QDialog(parent), m_recording(recording), m_fetch(std::move(fetch))
{
    setObjectName(QStringLiteral("analyticsReplayDialog"));
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(QStringLiteral("Recording"));
    resize(1080, 720);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(8);

    const QJsonArray trail = recording.value(QStringLiteral("pages")).toArray();
    const QString entry = trail.isEmpty() ? recording.value(QStringLiteral("startUrl")).toString()
                                          : trail.first().toString();
    const QString flag = MotionTrack::countryFlag(recording.value(QStringLiteral("country")).toString());
    auto *title = new QLabel((flag.isEmpty() ? QString() : flag + QLatin1Char(' ')) + MotionTrack::pathOf(entry));
    title->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 15px;"));
    title->setToolTip(entry);
    layout->addWidget(title);
    const QJsonObject device = recording.value(QStringLiteral("device")).toObject();
    QStringList facts{
        QLocale().toString(QDateTime::fromMSecsSinceEpoch(qint64(numberOf(recording, "startedAt"))),
                           QLocale::ShortFormat),
        clockText(qint64(numberOf(recording, "durationMs"))),
        QStringLiteral("%1 pages").arg(std::max<qsizetype>(1, trail.size())),
        QStringLiteral("%1 clicks").arg(qint64(numberOf(recording, "clicks")))};
    QStringList deviceParts;
    for (const char *key : {"type", "browser", "os"}) {
        const QString part = device.value(QLatin1String(key)).toString().trimmed();
        if (!part.isEmpty())
            deviceParts.append(part);
    }
    const QString deviceText = deviceParts.join(QLatin1Char(' '));
    if (!deviceText.isEmpty())
        facts.append(deviceText);
    if (!recording.value(QStringLiteral("country")).toString().isEmpty())
        facts.append(recording.value(QStringLiteral("country")).toString());
    auto *meta = new QLabel(facts.join(QStringLiteral(" · ")));
    meta->setObjectName(QStringLiteral("mutedLabel"));
    layout->addWidget(meta);

    auto *middle = new QHBoxLayout;
    middle->setSpacing(12);
    m_stage = new ReplayStage;
    middle->addWidget(m_stage, 1);
    auto *side = new QVBoxLayout;
    side->setSpacing(4);
    auto *pagesTitle = new QLabel(QStringLiteral("Pages viewed"));
    pagesTitle->setStyleSheet(QStringLiteral("font-weight: 600;"));
    side->addWidget(pagesTitle);
    m_pages = new QListWidget;
    m_pages->setObjectName(QStringLiteral("analyticsReplayPages"));
    m_pages->setAccessibleName(QStringLiteral("Pages viewed"));
    m_pages->setFixedWidth(230);
    side->addWidget(m_pages, 1);
    middle->addLayout(side);
    layout->addLayout(middle, 1);

    m_note = new QLabel(QStringLiteral("Loading the recording…"));
    m_note->setObjectName(QStringLiteral("mutedLabel"));
    layout->addWidget(m_note);

    m_timeline = new ReplayTimeline([this](qint64 at) { seek(at); });
    layout->addWidget(m_timeline);

    auto *controls = new QHBoxLayout;
    controls->setSpacing(8);
    m_play = new QPushButton(QStringLiteral("Play"));
    m_play->setObjectName(QStringLiteral("primaryButton"));
    m_play->setEnabled(false);
    m_play->setToolTip(QStringLiteral("Play or pause (Space)"));
    controls->addWidget(m_play);
    m_time = new QLabel(QStringLiteral("0:00 / 0:00"));
    m_time->setObjectName(QStringLiteral("mutedLabel"));
    controls->addWidget(m_time);
    controls->addStretch();
    controls->addWidget(new QLabel(QStringLiteral("Speed")));
    m_speed = new QComboBox;
    m_speed->setAccessibleName(QStringLiteral("Playback speed"));
    for (const double speed : {0.5, 1.0, 2.0, 4.0, 8.0})
        m_speed->addItem(QStringLiteral("%1×").arg(speed), speed);
    m_speed->setCurrentIndex(1);
    controls->addWidget(m_speed);
    m_skip = new QCheckBox(QStringLiteral("Skip inactivity"));
    m_skip->setChecked(true);
    controls->addWidget(m_skip);
    auto *close = new QPushButton(QStringLiteral("Close"));
    close->setObjectName(QStringLiteral("ghostButton"));
    controls->addWidget(close);
    layout->addLayout(controls);

    m_timer = new QTimer(this);
    m_timer->setInterval(33);
    connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
    connect(m_play, &QPushButton::clicked, this, [this] { setPlaying(!m_playing); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);
    connect(m_pages, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        seek(item->data(Qt::UserRole).toLongLong());
    });
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(space, &QShortcut::activated, this, [this] {
        if (m_play->isEnabled())
            setPlaying(!m_playing);
    });

    loadFrom(-1);
}

void AnalyticsReplayDialog::loadFrom(int afterSeq)
{
    const QPointer<AnalyticsReplayDialog> guard(this);
    m_fetch(QStringLiteral("?afterSeq=%1&view=motion").arg(afterSeq),
            [guard, afterSeq](bool ok, const QJsonObject &payload, const QString &error) {
        if (!guard)
            return;
        if (!ok) {
            guard->m_note->setText(QStringLiteral("The recording could not load: %1").arg(error));
            return;
        }
        for (const QJsonValue &chunk : payload.value(QStringLiteral("chunks")).toArray())
            guard->m_track.append(chunk.toObject().value(QStringLiteral("events")).toArray());
        const int next = payload.value(QStringLiteral("nextAfterSeq")).toInt(-1);
        if (next > afterSeq && ++guard->m_parts < kMaxPagesLoaded) {
            guard->m_note->setText(QStringLiteral("Loading the recording… part %1").arg(next + 2));
            guard->loadFrom(next);
            return;
        }
        guard->ready();
    });
}

void AnalyticsReplayDialog::ready()
{
    m_track.finish();
    if (m_track.isEmpty()) {
        m_note->setText(QStringLiteral("This recording has no movement to play back yet."));
        return;
    }
    m_note->setText(QStringLiteral(
        "Mouse movement, clicks and scrolling over an outline of the page. The web dashboard's "
        "Recordings tab replays the page itself."));
    for (const MotionPage &page : m_track.pages()) {
        auto *item = new QListWidgetItem(QStringLiteral("%1  %2")
                                             .arg(clockText(page.at - m_track.start()), MotionTrack::pathOf(page.href)));
        item->setToolTip(page.href);
        item->setData(Qt::UserRole, page.at);
        m_pages->addItem(item);
    }
    m_timeline->setTrack(&m_track);
    m_play->setEnabled(true);
    m_at = m_track.start();
    paintAt();
    setPlaying(true);
}

void AnalyticsReplayDialog::seek(qint64 at)
{
    m_at = std::clamp(at, m_track.start(), m_track.end());
    m_lastTick = QDateTime::currentMSecsSinceEpoch();
    paintAt();
}

void AnalyticsReplayDialog::setPlaying(bool playing)
{
    if (playing && m_at >= m_track.end())
        m_at = m_track.start();
    m_playing = playing;
    m_play->setText(playing ? QStringLiteral("Pause") : QStringLiteral("Play"));
    m_lastTick = QDateTime::currentMSecsSinceEpoch();
    if (playing)
        m_timer->start();
    else
        m_timer->stop();
    paintAt();
}

void AnalyticsReplayDialog::tick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 elapsed = std::clamp<qint64>(now - m_lastTick, 0, 250);
    m_lastTick = now;
    m_at += qint64(double(elapsed) * m_speed->currentData().toDouble());
    if (m_skip->isChecked()) {
        const qint64 next = m_track.nextActivity(m_at);
        if (next - m_at > kIdleJumpMs)
            m_at = next - 400;
    }
    if (m_at >= m_track.end()) {
        m_at = m_track.end();
        setPlaying(false);
        return;
    }
    paintAt();
}

void AnalyticsReplayDialog::paintAt()
{
    const MotionState state = m_track.stateAt(m_at);
    m_stage->present(&m_track, state, m_at);
    m_timeline->setPosition(m_at);
    m_time->setText(QStringLiteral("%1 / %2").arg(clockText(m_at - m_track.start()),
                                                  clockText(m_track.end() - m_track.start())));
    if (state.pageIndex >= 0 && state.pageIndex < m_pages->count() && m_pages->currentRow() != state.pageIndex)
        m_pages->setCurrentRow(state.pageIndex);
}

} // namespace forkmesh::analytics
