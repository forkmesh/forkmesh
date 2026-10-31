#include "StartupSplash.h"
#include "Theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QChildEvent>
#include <QCursor>
#include <QEvent>
#include <QEventLoop>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRadialGradient>
#include <QScreen>
#include <QSettings>
#include <QStyleHints>
#include <QTimer>

#include <cmath>

namespace forkmesh::ui {

namespace {

constexpr qreal kShadowMargin = 30.0;
constexpr qreal kCardWidth = 640.0;
constexpr qreal kCardHeight = 620.0;
constexpr qreal kCardRadius = 14.0;
constexpr qreal kHeaderHeight = 268.0;
constexpr qreal kLogoSize = 112.0;
constexpr qreal kLogoDiscRadius = 70.0;
constexpr qreal kOrbitRadius = 82.0;
constexpr qreal kLogoCentreY = 100.0;
constexpr qreal kFooterHeight = 46.0;
constexpr qreal kSidePadding = 22.0;
constexpr qreal kStepRowHeight = 25.0;
constexpr qreal kDetailRowHeight = 21.0;
constexpr qreal kGlyphSize = 15.0;

constexpr qint64 kSlowStepMs = 250;

// The splash repaints synchronously from pump() because there is no event loop
// yet. A full card repaint costs ~10 ms, and a launch calls pump() ~40 times,
// so the un-budgeted version spent ~450 ms of the pre-event-loop window drawing
// the card that is supposed to make launching feel fast. Skip a repaint until
// at least kPaintBudgetRatio times its own last cost has elapsed: the splash
// then costs a bounded ~1/(1+ratio) of launch time on any machine, and it still
// paints at up to 60 fps when the card is cheap to draw.
constexpr qint64 kPaintBudgetRatio = 9;
constexpr qint64 kMinPaintIntervalMs = 16;

constexpr qint64 kFinishHoldMs = 620;
constexpr qint64 kFinishFadeMs = 340;

constexpr int kDeadlineMs = 45000;

constexpr qint64 kIdleTimeoutMs = 15000;

constexpr qreal kDegToRad = 3.14159265358979323846 / 180.0;
constexpr int kArcUnit = 16;

const QString kSplashEnabledSetting = QStringLiteral("ui/startupSplash");
const QString kLastStepCountSetting = QStringLiteral("ui/startupSplashSteps");
const QString kLastDurationSetting = QStringLiteral("ui/startupSplashMs");

QPointer<StartupSplash> g_splash;

bool preferDarkSplash()
{
    const QString pref =
        QSettings().value(QStringLiteral("app/theme"),
                          QStringLiteral("system")).toString();
    if (pref == QLatin1String("light"))
        return false;
    if (pref == QLatin1String("dark") || pref == QLatin1String("halloween"))
        return true;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::styleHints())
        return QGuiApplication::styleHints()->colorScheme() !=
               Qt::ColorScheme::Light;
#endif
    return true;
}

QColor alpha(const QColor &color, int a)
{
    return QColor(color.red(), color.green(), color.blue(), a);
}

} // namespace

StartupSplash::StartupSplash(bool dark, const QString &version,
                             const QString &commit)
    : QWidget(nullptr,
              Qt::SplashScreen | Qt::FramelessWindowHint |
                  Qt::WindowStaysOnTopHint),
      m_dark(dark),
      m_version(version),
      m_commit(commit.left(7))
{
    m_clock.start();
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setFixedSize(int(kCardWidth + 2 * kShadowMargin),
                 int(kCardHeight + 2 * kShadowMargin));

    if (dark) {
        m_palette = {QColor(0, 0, 0),         Theme::remapColor(QColor("#161b22")),
                     QColor("#1b2230"),       Theme::remapColor(QColor("#161b22")),
                     Theme::remapColor(QColor("#30363d")),       Theme::remapColor(QColor("#21262d")),
                     Theme::remapColor(QColor("#e6edf3")),       Theme::remapColor(QColor("#8b949e")),
                     Theme::remapColor(QColor("#6e7681")),       Theme::remapColor(QColor("#21262d")),
                     QColor("#58a6ff"),       QColor("#3fb950"),
                     QColor("#e3b341"),       QColor("#f85149")};
    } else {
        m_palette = {QColor(60, 70, 85),      QColor("#ffffff"),
                     Theme::remapColor(QColor("#f6f8fa")),       QColor("#ffffff"),
                     Theme::remapColor(QColor("#d0d7de")),       Theme::remapColor(QColor("#eaeef2")),
                     Theme::remapColor(QColor("#1f2328")),       Theme::remapColor(QColor("#57606a")),
                     Theme::remapColor(QColor("#8c959f")),       Theme::remapColor(QColor("#eaeef2")),
                     QColor("#0969da"),       QColor("#1a7f37"),
                     QColor("#9a6700"),       QColor("#cf222e")};
    }

    syncFonts();

    QSettings settings;
    const int steps = settings.value(kLastStepCountSetting, 0).toInt();
    const qint64 duration = settings.value(kLastDurationSetting, 0).toLongLong();
    if (steps >= 4 && steps <= 500)
        m_expectedSteps = steps;
    if (duration >= 150 && duration <= 120000)
        m_expectedMs = duration;

    m_animation = new QTimer(this);
    m_animation->setInterval(16);
    QObject::connect(m_animation, &QTimer::timeout, this, [this] {
        if (m_finishAtMs < 0 &&
            m_clock.elapsed() - m_lastActivityMs > kIdleTimeoutMs) {
            finishInternal(QStringLiteral("Startup went quiet — hiding this"),
                           false);
            return;
        }
        if (m_finishAtMs >= 0) {
            const qint64 since = m_clock.elapsed() - m_finishAtMs;
            if (since > kFinishHoldMs) {
                const qreal fade =
                    1.0 - qreal(since - kFinishHoldMs) / qreal(kFinishFadeMs);
                if (fade <= 0.0) {
                    dismiss();
                    return;
                }
                m_opacity = fade;
                update();
                return;
            }
        }
        update(damageRect());
    });
    m_animation->start();

    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    m_deadline->setInterval(kDeadlineMs);
    QObject::connect(m_deadline, &QTimer::timeout, this,
                     [this] { dismiss(); });
    m_deadline->start();
}


void StartupSplash::attachTo(QWidget *host)
{
    if (!host || m_host == host)
        return;
    m_host = host;

    setParent(host, Qt::Widget);
    setMinimumSize(0, 0);
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    // WA_TranslucentBackground is only meant for top-level windows and also
    // enables WA_NoSystemBackground. Leaving those attributes on the widget
    // after reparenting makes the overlay depend on compositor-backed alpha;
    // WSLg can then expose the child surface as an opaque grey rectangle.
    // Restore normal child-widget backing-store propagation so the translucent
    // scrim below is blended with the already-painted host window by Qt.
    setAttribute(Qt::WA_TranslucentBackground, false);
    setAttribute(Qt::WA_NoSystemBackground, false);
    setAutoFillBackground(false);

    host->installEventFilter(this);
    followHost();
    show();
    raise();
}

void StartupSplash::followHost()
{
    if (!m_host)
        return;
    setGeometry(m_host->rect());
}

bool StartupSplash::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_host) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Show:
            followHost();
            raise();
            break;
        case QEvent::ChildAdded: {
            const QObject *child = static_cast<QChildEvent *>(event)->child();
            if (child && child->isWidgetType())
                raise();
            break;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}


void StartupSplash::noteActivity()
{
    m_lastActivityMs = m_clock.elapsed();
}

void StartupSplash::beginStep(const QString &label)
{
    if (m_finishAtMs >= 0)
        return;
    noteActivity();
    completeCurrentStep();
    Row row;
    row.text = label;
    row.state = RowState::Running;
    row.startedMs = m_clock.elapsed();
    m_rows.append(row);
    m_runningRow = int(m_rows.size()) - 1;
    pump();
}

void StartupSplash::addDetail(const QString &text)
{
    if (m_finishAtMs >= 0)
        return;
    noteActivity();
    Row row;
    row.text = text;
    row.state = RowState::Detail;
    row.startedMs = m_clock.elapsed();
    row.endedMs = row.startedMs;
    m_rows.append(row);
    pump();
}

void StartupSplash::completeCurrentStep()
{
    if (m_runningRow < 0 || m_runningRow >= m_rows.size())
        return;
    noteActivity();
    Row &row = m_rows[m_runningRow];
    row.state = RowState::Done;
    row.endedMs = m_clock.elapsed();
    m_runningRow = -1;
    ++m_completedSteps;
    pump();
}

void StartupSplash::failCurrentStep(const QString &reason)
{
    noteActivity();
    if (m_runningRow >= 0 && m_runningRow < m_rows.size()) {
        Row &row = m_rows[m_runningRow];
        row.state = RowState::Failed;
        row.endedMs = m_clock.elapsed();
        m_runningRow = -1;
        ++m_completedSteps;
    }
    if (!reason.isEmpty()) {
        Row row;
        row.text = reason;
        row.state = RowState::Failed;
        row.startedMs = m_clock.elapsed();
        row.endedMs = row.startedMs;
        m_rows.append(row);
    }
    pump(true); // a failure/finish frame must not be skipped by the budget
}

void StartupSplash::noteHostPainted(const QString &label)
{
    if (m_finishAtMs >= 0)
        return;
    m_hostPainting = true;
    beginStep(label);
    completeCurrentStep();
    m_hostPainting = false;
    m_hostPainted = true;
}

void StartupSplash::finish(const QString &label)
{
    finishInternal(label, true);
}

void StartupSplash::finishInternal(const QString &label, bool calibrate)
{
    if (m_finishAtMs >= 0)
        return;
    completeCurrentStep();
    if (!label.isEmpty()) {
        Row row;
        row.text = label;
        row.state = RowState::Done;
        row.startedMs = m_clock.elapsed();
        row.endedMs = row.startedMs;
        m_rows.append(row);
        ++m_completedSteps;
    }
    m_finishAtMs = m_clock.elapsed();
    m_shownProgress = 1.0;

    if (calibrate) {
        QSettings settings;
        settings.setValue(kLastStepCountSetting, m_completedSteps);
        settings.setValue(kLastDurationSetting, m_finishAtMs);
    }

    if (m_deadline)
        m_deadline->stop();
    pump(true); // a failure/finish frame must not be skipped by the budget
}

void StartupSplash::dismiss()
{
    if (m_animation)
        m_animation->stop();
    if (m_deadline)
        m_deadline->stop();
    if (m_host)
        m_host->removeEventFilter(this);
    hide();
    if (g_splash == this)
        g_splash = nullptr;
    deleteLater();
}

QStringList StartupSplash::transcript() const
{
    QStringList lines;
    lines.reserve(int(m_rows.size()));
    for (const Row &row : m_rows) {
        const QString prefix = row.state == RowState::Detail
                                   ? QStringLiteral("    ")
                                   : QString();
        const QString duration =
            row.endedMs >= 0
                ? QStringLiteral(" (%1)").arg(elapsedText(row.endedMs -
                                                          row.startedMs))
                : QStringLiteral(" (running)");
        lines << prefix + row.text + duration;
    }
    return lines;
}


void StartupSplash::pump(bool force)
{
    if (!isVisible())
        return;
    if (m_hostPainting ||
        (m_host && m_host->testAttribute(Qt::WA_WState_InPaintEvent))) {
        update(damageRect());
        return;
    }
    if (m_host && !m_hostPainted) {
        update(damageRect());
        return;
    }
    if (!force && m_lastPaintEndedMs >= 0) {
        const qint64 since = m_clock.elapsed() - m_lastPaintEndedMs;
        if (since < qMax(kMinPaintIntervalMs,
                         m_lastPaintCostMs * kPaintBudgetRatio)) {
            // The row is already recorded; the next repaint that clears the
            // budget draws it along with everything since.
            return;
        }
    }
    QElapsedTimer paintCost;
    paintCost.start();
    if (m_host)
        repaint(damageRect());
    else
        repaint(damageRect());
    QCoreApplication::sendPostedEvents(this, 0);
    m_lastPaintCostMs = paintCost.elapsed();
    m_lastPaintEndedMs = m_clock.elapsed();
}

// code-health: allow(blocking-wait) startup splash pump before the main event loop; processEvents capped at 4 ms, only while visible
void StartupSplash::pumpEvents()
{
    if (!isVisible())
        return;
    pump(true); // picks posted vs synchronous by where we are in the launch
    // Keep the splash dismissible and the native window movable even while a
    // startup step is using the synchronous paint pump.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 4);
}

void StartupSplash::mousePressEvent(QMouseEvent *event)
{
    Q_UNUSED(event);
    dismiss();
}


QRectF StartupSplash::cardRect() const
{
    return QRectF(std::round((qreal(width()) - kCardWidth) / 2.0),
                  std::round((qreal(height()) - kCardHeight) / 2.0), kCardWidth,
                  kCardHeight);
}

QRect StartupSplash::damageRect() const
{
    return cardRect()
        .adjusted(-kShadowMargin, -kShadowMargin, kShadowMargin, kShadowMargin)
        .toAlignedRect();
}

qreal StartupSplash::rowHeight(const Row &row) const
{
    return row.state == RowState::Detail ? kDetailRowHeight : kStepRowHeight;
}

qreal StartupSplash::contentHeight() const
{
    qreal total = 0.0;
    for (const Row &row : m_rows)
        total += rowHeight(row);
    return total;
}

qreal StartupSplash::progressTarget(qint64 now) const
{
    if (m_finishAtMs >= 0)
        return 1.0;
    const qreal byStep =
        qreal(m_completedSteps) / qreal(qMax(m_expectedSteps, 1));
    const qreal byTime = qreal(now) / qreal(qMax<qint64>(m_expectedMs, 1));
    return qBound(0.02, qMax(byStep, byTime * 0.92), 0.97);
}

QString StartupSplash::elapsedText(qint64 ms) const
{
    if (ms < 1000)
        return QStringLiteral("%1 ms").arg(ms);
    return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 2);
}

void StartupSplash::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    const qint64 now = m_clock.elapsed();

    syncFonts();
    ensureLogo();

    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    if (m_opacity < 1.0)
        p.setOpacity(m_opacity);

    const QRectF card = cardRect();
    if (m_host) {
        p.fillRect(rect(), alpha(QColor(0, 0, 0), m_dark ? 130 : 96));
    }
    ensureShadow();
    if (!m_shadow.isNull())
        p.drawPixmap(QPointF(card.left() - kShadowMargin,
                             card.top() - kShadowMargin),
                     m_shadow);

    QPainterPath cardPath;
    cardPath.addRoundedRect(card, kCardRadius, kCardRadius);
    p.save();
    p.setClipPath(cardPath);

    QLinearGradient body(card.topLeft(), QPointF(card.left(), card.top() + 160));
    body.setColorAt(0.0, m_palette.headerTop);
    body.setColorAt(1.0, m_palette.card);
    p.fillPath(cardPath, body);

    paintHeader(p, card, now);
    paintProgress(p, card, now);

    const QRectF list(card.left() + kSidePadding,
                      card.top() + kHeaderHeight + 10.0,
                      card.width() - 2 * kSidePadding,
                      card.height() - kHeaderHeight - kFooterHeight - 16.0);
    paintRows(p, list, now);
    paintFooter(p, card);
    p.restore();

    QPen border(m_palette.border);
    border.setWidthF(1.0);
    p.setPen(border);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), kCardRadius,
                      kCardRadius);
}

void StartupSplash::syncFonts()
{
    const QString family = QApplication::font().family();
    if (family == m_fontFamily && !m_fontFamily.isEmpty())
        return;
    m_fontFamily = family;
    const auto sized = [&family](int pixels, QFont::Weight weight) {
        QFont font(family);
        font.setPixelSize(pixels);
        font.setWeight(weight);
        return font;
    };
    m_titleFont = sized(24, QFont::Bold);
    m_subtitleFont = sized(11, QFont::Normal);
    m_rowFont = sized(13, QFont::Normal);
    m_detailFont = sized(12, QFont::Normal);
    m_metaFont = sized(11, QFont::Normal);
}

void StartupSplash::ensureLogo()
{
    const qreal dpr = devicePixelRatioF();
    if (!m_logo.isNull() && qFuzzyCompare(m_logoDpr, dpr))
        return;
    const QPixmap source(QStringLiteral(":/app/forkmesh.png"));
    if (source.isNull()) {
        m_logoDpr = dpr;
        return;
    }
    const int edge = int(kLogoSize * dpr);
    m_logo = source.scaled(edge, edge, Qt::KeepAspectRatio,
                           Qt::SmoothTransformation);
    m_logo.setDevicePixelRatio(dpr);
    m_logoDpr = dpr;
}

void StartupSplash::ensureShadow()
{
    const qreal dpr = devicePixelRatioF();
    if (!m_shadow.isNull() && qFuzzyCompare(m_shadowDpr, dpr))
        return;
    m_shadow = QPixmap(int((kCardWidth + 2 * kShadowMargin) * dpr),
                       int((kCardHeight + 2 * kShadowMargin) * dpr));
    m_shadow.setDevicePixelRatio(dpr);
    m_shadow.fill(Qt::transparent);
    QPainter shadow(&m_shadow);
    shadow.setRenderHint(QPainter::Antialiasing, true);
    paintShadow(shadow,
                QRectF(kShadowMargin, kShadowMargin, kCardWidth, kCardHeight));
    m_shadowDpr = dpr;
}

void StartupSplash::paintShadow(QPainter &p, const QRectF &card) const
{
    p.setPen(Qt::NoPen);
    for (int i = int(kShadowMargin); i > 0; i -= 2) {
        const qreal spread = qreal(i);
        const int a = int(46.0 * std::pow(1.0 - spread / kShadowMargin, 2.2));
        if (a <= 0)
            continue;
        p.setBrush(alpha(m_palette.shadow, a));
        p.drawRoundedRect(card.adjusted(-spread, -spread + 3.0, spread,
                                        spread + 3.0),
                          kCardRadius + spread, kCardRadius + spread);
    }
}

void StartupSplash::paintHeader(QPainter &p, const QRectF &card,
                                qint64 now) const
{
    const QPointF mark(card.center().x(), card.top() + kLogoCentreY);
    paintMeshOrbit(p, mark, now);

    p.setPen(QPen(alpha(m_palette.border, 200), 1.0));
    p.setBrush(QColor(255, 255, 255));
    p.drawEllipse(mark, kLogoDiscRadius, kLogoDiscRadius);
    if (!m_logo.isNull()) {
        const QSizeF logical = QSizeF(m_logo.size()) / m_logo.devicePixelRatio();
        p.drawPixmap(QPointF(mark.x() - logical.width() / 2.0,
                             mark.y() - logical.height() / 2.0),
                     m_logo);
    }

    const QRectF titleBox(card.left(), card.top() + kLogoCentreY + kOrbitRadius + 8.0,
                          card.width(), 32.0);
    p.setFont(m_titleFont);
    p.setPen(m_palette.text);
    p.drawText(titleBox, Qt::AlignHCenter | Qt::AlignVCenter,
               QStringLiteral("ForkMesh"));

    p.setFont(m_subtitleFont);
    p.setPen(m_finishAtMs >= 0 ? m_palette.ok : m_palette.muted);
    p.drawText(titleBox.translated(0.0, 26.0),
               Qt::AlignHCenter | Qt::AlignVCenter,
               m_finishAtMs >= 0
                   ? QStringLiteral("Ready")
                   : QStringLiteral("Starting the desktop node…"));

    const qint64 shown = m_finishAtMs >= 0 ? m_finishAtMs : now;
    QFont clock = m_titleFont;
    clock.setWeight(QFont::Normal);
    p.setFont(clock);
    p.setPen(m_finishAtMs >= 0 ? m_palette.ok : m_palette.muted);
    const QRectF clockBox(card.right() - 160.0, card.top() + 24.0, 136.0, 24.0);
    p.drawText(clockBox, Qt::AlignRight | Qt::AlignVCenter, elapsedText(shown));
    p.setFont(m_metaFont);
    p.setPen(m_palette.faint);
    p.drawText(QRectF(card.right() - 160.0, card.top() + 48.0, 136.0, 14.0),
               Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("elapsed"));
}

void StartupSplash::paintMeshOrbit(QPainter &p, const QPointF &centre,
                                   qint64 now) const
{
    const bool done = m_finishAtMs >= 0;
    const QColor accent = done ? m_palette.ok : m_palette.accent;

    const qreal pulse = 0.5 + 0.5 * std::sin(qreal(now) / 380.0);
    const qreal glowRadius = kOrbitRadius + 14.0;
    const qreal innerRadius = kOrbitRadius - 7.0;
    QRadialGradient glow(centre, glowRadius);
    glow.setColorAt(0.0, alpha(accent, int(34 + 30 * pulse)));
    glow.setColorAt(0.62, alpha(accent, int(12 + 12 * pulse)));
    glow.setColorAt(1.0, alpha(accent, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(centre, glowRadius, glowRadius);

    p.setBrush(Qt::NoBrush);

    QPen track(alpha(accent, 46));
    track.setWidthF(2.4);
    p.setPen(track);
    p.drawEllipse(centre, kOrbitRadius, kOrbitRadius);

    const QRectF outer(centre.x() - kOrbitRadius, centre.y() - kOrbitRadius,
                       2.0 * kOrbitRadius, 2.0 * kOrbitRadius);
    const QRectF inner(centre.x() - innerRadius, centre.y() - innerRadius,
                       2.0 * innerRadius, 2.0 * innerRadius);

    QPen sweep(accent);
    sweep.setWidthF(2.4);
    sweep.setCapStyle(Qt::RoundCap);
    p.setPen(sweep);
    p.drawArc(outer, int(-(now * 0.20)) % 360 * kArcUnit,
              (done ? 360 : 105) * kArcUnit);

    QPen counter(alpha(accent, 130));
    counter.setWidthF(1.8);
    counter.setCapStyle(Qt::RoundCap);
    p.setPen(counter);
    p.drawArc(inner, int(now * 0.13) % 360 * kArcUnit,
              (done ? 360 : 78) * kArcUnit);

    const QColor peers[3] = {accent, m_palette.ok, QColor("#bc8cff")};
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 3; ++i) {
        const qreal angle = (qreal(now) * 0.075 + i * 120.0) * kDegToRad;
        const QPointF dot(centre.x() + kOrbitRadius * std::cos(angle),
                          centre.y() - kOrbitRadius * std::sin(angle));
        p.setBrush(alpha(peers[i], 90));
        p.drawEllipse(dot, 6.0, 6.0);
        p.setBrush(peers[i]);
        p.drawEllipse(dot, 3.4, 3.4);
    }
}

void StartupSplash::paintProgress(QPainter &p, const QRectF &card, qint64 now)
{
    const qreal target = progressTarget(now);
    const qint64 delta = qMax<qint64>(0, now - m_lastPaintMs);
    m_lastPaintMs = now;
    m_shownProgress +=
        (target - m_shownProgress) * qMin(1.0, qreal(delta) / 220.0);
    m_shownProgress = qBound(0.0, m_shownProgress, 1.0);

    const QRectF bar(card.left() + kSidePadding, card.top() + kHeaderHeight - 12.0,
                     card.width() - 2 * kSidePadding, 4.0);
    p.setPen(Qt::NoPen);
    p.setBrush(m_palette.track);
    p.drawRoundedRect(bar, 2.0, 2.0);

    const qreal filledWidth = bar.width() * m_shownProgress;
    if (filledWidth > 1.0) {
        const QRectF filled(bar.left(), bar.top(), filledWidth, bar.height());
        const QColor head = m_finishAtMs >= 0 ? m_palette.ok : m_palette.accent;
        QLinearGradient fill(filled.topLeft(), filled.topRight());
        fill.setColorAt(0.0, alpha(head, 150));
        fill.setColorAt(1.0, head);
        p.setBrush(fill);
        p.drawRoundedRect(filled, 2.0, 2.0);

        if (m_finishAtMs < 0) {
            p.save();
            QPainterPath clip;
            clip.addRoundedRect(filled, 2.0, 2.0);
            p.setClipPath(clip);
            const qreal span = filled.width() + 160.0;
            const qreal x =
                filled.left() + std::fmod(qreal(now) * 0.34, span) - 80.0;
            const QColor sheen(255, 255, 255);
            QLinearGradient shimmer(QPointF(x, 0), QPointF(x + 80.0, 0));
            shimmer.setColorAt(0.0, alpha(sheen, 0));
            shimmer.setColorAt(0.5, alpha(sheen, 70));
            shimmer.setColorAt(1.0, alpha(sheen, 0));
            p.fillRect(filled, shimmer);
            p.restore();
        }
    }

    p.setPen(QPen(m_palette.separator, 1.0));
    p.drawLine(QPointF(card.left(), card.top() + kHeaderHeight),
               QPointF(card.right(), card.top() + kHeaderHeight));
}

void StartupSplash::paintRows(QPainter &p, const QRectF &list, qint64 now) const
{
    p.save();
    p.setClipRect(list);

    const qreal offset = qMax(0.0, contentHeight() - list.height());
    qreal y = list.top() - offset;

    const QFontMetricsF stepMetrics(m_rowFont);
    const QFontMetricsF detailMetrics(m_detailFont);
    const QFontMetricsF metaMetrics(m_metaFont);

    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        const qreal height = rowHeight(row);
        const QRectF band(list.left(), y, list.width(), height);
        y += height;
        if (band.bottom() < list.top() - 2.0 || band.top() > list.bottom() + 2.0)
            continue;

        const bool detail = row.state == RowState::Detail;
        const qreal indent = detail ? 22.0 : 0.0;
        const QRectF glyph(band.left() + indent + 3.0,
                           band.center().y() - kGlyphSize / 2.0, kGlyphSize,
                           kGlyphSize);
        paintRowGlyph(p, glyph, row, now);

        QString duration;
        QColor durationColor = m_palette.faint;
        if (row.state == RowState::Running) {
            duration = elapsedText(now - row.startedMs);
            durationColor = m_palette.accent;
        } else if (!detail && row.endedMs >= 0) {
            const qint64 took = row.endedMs - row.startedMs;
            if (took >= 1) {
                duration = elapsedText(took);
                if (took >= kSlowStepMs)
                    durationColor = m_palette.warn;
            }
        }
        qreal durationWidth = 0.0;
        if (!duration.isEmpty()) {
            p.setFont(m_metaFont);
            p.setPen(durationColor);
            durationWidth = metaMetrics.horizontalAdvance(duration) + 12.0;
            p.drawText(QRectF(list.right() - durationWidth + 12.0, band.top(),
                              durationWidth - 12.0, height),
                       Qt::AlignRight | Qt::AlignVCenter, duration);
        }

        const qreal textLeft = glyph.right() + 9.0;
        const qreal textWidth =
            qMax(20.0, list.right() - durationWidth - textLeft);
        QColor textColor;
        switch (row.state) {
        case RowState::Running:
            textColor = m_palette.text;
            break;
        case RowState::Failed:
            textColor = m_palette.bad;
            break;
        case RowState::Detail:
            textColor = m_palette.faint;
            break;
        case RowState::Done:
            textColor = m_palette.muted;
            break;
        }
        QFont font = detail ? m_detailFont : m_rowFont;
        if (row.state == RowState::Running)
            font.setWeight(QFont::DemiBold);
        p.setFont(font);
        p.setPen(textColor);
        const QFontMetricsF &metrics = detail ? detailMetrics : stepMetrics;
        p.drawText(QRectF(textLeft, band.top(), textWidth, height),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   metrics.elidedText(row.text, Qt::ElideRight, textWidth));
    }

    if (offset > 0.0) {
        QLinearGradient fade(list.topLeft(),
                             QPointF(list.left(), list.top() + 26.0));
        fade.setColorAt(0.0, m_palette.card);
        fade.setColorAt(1.0, alpha(m_palette.card, 0));
        p.fillRect(QRectF(list.left(), list.top(), list.width(), 26.0), fade);
    }
    p.restore();
}

void StartupSplash::paintRowGlyph(QPainter &p, const QRectF &box,
                                  const Row &row, qint64 now) const
{
    const QPointF centre = box.center();
    p.setBrush(Qt::NoBrush);
    switch (row.state) {
    case RowState::Running: {
        QPen track(alpha(m_palette.accent, 60));
        track.setWidthF(1.8);
        p.setPen(track);
        p.drawEllipse(centre, 5.6, 5.6);
        QPen arc(m_palette.accent);
        arc.setWidthF(1.8);
        arc.setCapStyle(Qt::RoundCap);
        p.setPen(arc);
        p.drawArc(QRectF(centre.x() - 5.6, centre.y() - 5.6, 11.2, 11.2),
                  int(-(now * 0.30)) % 360 * 16, 100 * 16);
        break;
    }
    case RowState::Done: {
        p.setPen(Qt::NoPen);
        p.setBrush(alpha(m_palette.ok, 42));
        p.drawEllipse(centre, 6.4, 6.4);
        QPen tick(m_palette.ok);
        tick.setWidthF(1.7);
        tick.setCapStyle(Qt::RoundCap);
        tick.setJoinStyle(Qt::RoundJoin);
        p.setPen(tick);
        p.setBrush(Qt::NoBrush);
        QPainterPath check;
        check.moveTo(centre.x() - 3.1, centre.y() + 0.2);
        check.lineTo(centre.x() - 0.9, centre.y() + 2.5);
        check.lineTo(centre.x() + 3.3, centre.y() - 2.5);
        p.drawPath(check);
        break;
    }
    case RowState::Failed: {
        p.setPen(Qt::NoPen);
        p.setBrush(alpha(m_palette.bad, 46));
        p.drawEllipse(centre, 6.4, 6.4);
        QPen cross(m_palette.bad);
        cross.setWidthF(1.7);
        cross.setCapStyle(Qt::RoundCap);
        p.setPen(cross);
        p.drawLine(QPointF(centre.x() - 2.8, centre.y() - 2.8),
                   QPointF(centre.x() + 2.8, centre.y() + 2.8));
        p.drawLine(QPointF(centre.x() + 2.8, centre.y() - 2.8),
                   QPointF(centre.x() - 2.8, centre.y() + 2.8));
        break;
    }
    case RowState::Detail: {
        p.setPen(Qt::NoPen);
        p.setBrush(alpha(m_palette.faint, 150));
        p.drawEllipse(centre, 1.9, 1.9);
        break;
    }
    }
}

void StartupSplash::paintFooter(QPainter &p, const QRectF &card) const
{
    const qreal top = card.bottom() - kFooterHeight;
    p.setPen(QPen(m_palette.separator, 1.0));
    p.drawLine(QPointF(card.left(), top), QPointF(card.right(), top));

    const QRectF band(card.left() + kSidePadding, top, card.width() - 2 * kSidePadding,
                      kFooterHeight);

    QString status;
    QColor statusColor = m_palette.muted;
    if (m_finishAtMs >= 0) {
        status = QStringLiteral("Ready — %1 steps in %2")
                     .arg(m_completedSteps)
                     .arg(elapsedText(m_finishAtMs));
        statusColor = m_palette.ok;
    } else if (m_runningRow >= 0 && m_runningRow < m_rows.size()) {
        status = m_rows.at(m_runningRow).text;
    } else {
        status = QStringLiteral("Working…");
    }

    QString build = m_version.isEmpty() ? QString()
                                        : QStringLiteral("v") + m_version;
    if (!m_commit.isEmpty() && m_commit != QLatin1String("unknown"))
        build += QStringLiteral(" · ") + m_commit;
    if (m_finishAtMs < 0) {
        const QString skip = QStringLiteral("Click anywhere to skip");
        build = build.isEmpty() ? skip
                                : skip + QStringLiteral(" · ") + build;
    }

    p.setFont(m_metaFont);
    const QFontMetricsF metrics(m_metaFont);
    const qreal buildWidth =
        build.isEmpty() ? 0.0 : metrics.horizontalAdvance(build) + 16.0;
    p.setPen(m_palette.faint);
    if (!build.isEmpty())
        p.drawText(band, Qt::AlignRight | Qt::AlignVCenter, build);
    const qreal statusWidth = qMax(40.0, band.width() - buildWidth);
    p.setPen(statusColor);
    p.drawText(QRectF(band.left(), band.top(), statusWidth, band.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               metrics.elidedText(status, Qt::ElideRight, statusWidth));
}


StartupSplash *activeStartupSplash()
{
    return g_splash.data();
}

StartupSplash *showStartupSplash(const QString &version, const QString &commit)
{
    if (g_splash)
        return g_splash.data();
    if (!qobject_cast<QApplication *>(QCoreApplication::instance()))
        return nullptr;
    if (qEnvironmentVariableIsSet("FORKMESH_NO_SPLASH"))
        return nullptr;
    if (!QSettings().value(kSplashEnabledSetting, true).toBool())
        return nullptr;

    auto *splash = new StartupSplash(preferDarkSplash(), version, commit);
    const QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry();
        splash->move(available.center() -
                     QPoint(splash->width() / 2, splash->height() / 2));
    }
    splash->show();
    g_splash = splash;
    splash->pumpEvents();
    splash->pumpEvents();
    return splash;
}

void attachStartupSplashTo(QWidget *host)
{
    if (g_splash)
        g_splash->attachTo(host);
}

void startupStep(const QString &label)
{
    if (g_splash)
        g_splash->beginStep(label);
}

void startupDetail(const QString &text)
{
    if (g_splash)
        g_splash->addDetail(text);
}

void startupStepFailed(const QString &reason)
{
    if (g_splash)
        g_splash->failCurrentStep(reason);
}

void startupWindowPainted(const QString &label)
{
    if (g_splash)
        g_splash->noteHostPainted(label);
}

void finishStartupSplash(const QString &label)
{
    if (g_splash)
        g_splash->finish(label);
}

void dismissStartupSplash()
{
    if (g_splash)
        g_splash->dismiss();
}

} // namespace forkmesh::ui
