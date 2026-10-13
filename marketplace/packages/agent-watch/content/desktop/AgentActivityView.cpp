#include "AgentActivityView.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRadialGradient>
#include <QScreen>
#include <QtMath>
#include <QTextBoundaryFinder>

#ifdef FORKMESH_HAVE_OPENGL
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#endif

#include <algorithm>
#include <cmath>
#include <initializer_list>

using namespace forkmesh::agentvis;

namespace {

// Thirty frames while anything in the field is moving, none at all when
// nothing is: a quiet field is drawn once and then left still. The idle timer
// only asks whether something has started, it paints nothing, so a window
// left open on an idle fleet costs nothing. On the GPU the busy field keeps
// up with the display instead, up to kFastestIntervalMs. A field where only
// the displays under the bodies scroll keeps up with the display on either:
// those frames are a blit and a few strips.
constexpr int kBusyIntervalMs = 33;
constexpr int kIdleIntervalMs = 100;
constexpr int kFastestIntervalMs = 7;
// How long a body takes to fade in or out when "working only" changes its mind.
constexpr double kFadeSeconds = 0.9;
// How long a row of the sound list stays lit after its note, as a time
// constant: bright on the beat, gone in a couple of seconds.
constexpr double kRowGlowSeconds = 0.8;
// How many of an agent's latest marks circle it.
constexpr int kRingMarks = 40;
// Your mirror nodes, and the pipes agents open to them: green, where every
// other host is blue.
constexpr double kMirrorHue = 0.38;
// The relay's clients: web visitors teal, other desktops the desktop's own
// blue, phones violet. Past kClientRows rows they are counted, not drawn.
constexpr int kClientRows = 3;
double clientHue(ClientKind kind)
{
    switch (kind) {
    case ClientKind::Visitor:
        return 0.46;
    case ClientKind::Phone:
        return 0.80;
    case ClientKind::Node:
        break;
    }
    return 0.58;
}
// The words circling a body - its project over the top, its task under the
// bottom - start this far outside it, clear of its running ring (radius + 15)
// and the marks round it (radius + 9); kLabelPixels is their size.
constexpr double kLabelGap = 19.0;
constexpr int kLabelPixels = 11;
// The display under a body: a line of dots kLedPitch apart, one for each
// pixel of a small font, in a panel kLedPad deep round them. A new reply
// holds still for kLedHold seconds, then scrolls at kLedSpeed pixels a second
// - a dot a frame at sixty frames - round and round, kLedGapDots dark dots
// between its last word and its first.
constexpr double kLedPitch = 2.5;
constexpr int kLedFontPixels = 8;
constexpr double kLedPad = 3.0;
constexpr double kLedHold = 0.6;
constexpr double kLedSpeed = 150.0;
constexpr int kLedGapDots = 14;
constexpr double kLedMaxWidth = 220.0;
// How long the still field under the displays is trusted before it is
// painted again: what changes without moving the field shows within this.
constexpr double kStillSeconds = 1.0;

QFont ledFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(kLedFontPixels);
    font.setStyleStrategy(QFont::NoAntialias);
    font.setHintingPreference(QFont::PreferFullHinting);
    return font;
}

int ledRows()
{
    static const int rows = qMax(6, QFontMetrics(ledFont()).height());
    return rows;
}

// The display's depth, and the room under a body it takes with its gap.
double ledHeight()
{
    return ledRows() * kLedPitch + 2 * kLedPad;
}

double ledRoom()
{
    return ledHeight() + 2.0;
}

// One lit or unlit dot centred on `at`, `scale` device pixels to a pixel.
void drawLedDot(QPainter &painter, const QPointF &at, double scale, bool lit)
{
    const double dot = kLedPitch * 0.36 * scale;
    if (lit) {
        painter.setBrush(QColor(255, 140, 0, 50));
        painter.drawEllipse(at, dot * 1.5, dot * 1.5);
        painter.setBrush(QColor(255, 190, 60));
    } else {
        painter.setBrush(QColor(255, 150, 40, 36));
    }
    painter.drawEllipse(at, dot, dot);
}

void paintLedDot(QPainter &painter, int column, int row, bool lit)
{
    drawLedDot(painter, QPointF((column + 0.5) * kLedPitch, (row + 0.5) * kLedPitch), 1.0,
               lit);
}

// A lit dot for a display `ratio` device pixels to a pixel, sitting `phase`
// quarters of a device pixel right of and below a whole one. A line is
// stamped from these: a copy each instead of two antialiased ellipses.
const QImage &litDot(qreal ratio, int phaseX, int phaseY)
{
    static QHash<quint64, QImage> dots;
    const quint64 key = (quint64(qRound(ratio * 1000)) << 8) | quint64(phaseX << 4 | phaseY);
    QImage &dot = dots[key];
    if (dot.isNull()) {
        const double reach = kLedPitch * 0.36 * 1.5 * ratio;
        const int side = qCeil(2 * reach + 2);
        dot = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
        dot.fill(Qt::transparent);
        QPainter p(&dot);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        drawLedDot(p, QPointF(reach + 0.5 + phaseX / 4.0, reach + 0.5 + phaseY / 4.0), ratio,
                   true);
    }
    return dot;
}

// Where litDot's image goes for a dot centred `at` device pixels in: the
// whole pixel it starts on, and its quarter-pixel phase past it.
std::pair<int, int> litDotPlace(double at, qreal ratio)
{
    const double from = at - (kLedPitch * 0.36 * 1.5 * ratio + 0.5);
    int whole = int(std::floor(from));
    int quarter = qRound((from - whole) * 4);
    if (quarter == 4) {
        whole += 1;
        quarter = 0;
    }
    return {whole, quarter};
}

QPointF onQuad(const QPointF &from, const QPointF &control, const QPointF &to,
               double t)
{
    const double u = 1 - t;
    return u * u * from + 2 * u * t * control + t * t * to;
}

double easeOut(double t)
{
    t = qBound(0.0, t, 1.0);
    return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
}

double easeInOut(double t)
{
    t = qBound(0.0, t, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

QPointF polar(const QPointF &centre, double radius, double angle)
{
    return centre + QPointF(std::cos(angle), std::sin(angle)) * radius;
}

QString formatDuration(double seconds)
{
    if (seconds < 60)
        return QStringLiteral("%1 s").arg(qMax(0, int(seconds)));
    if (seconds < 3600)
        return QStringLiteral("%1 min").arg(int(seconds / 60));
    return QStringLiteral("%1 h %2 min").arg(int(seconds / 3600)).arg(int(seconds / 60) % 60);
}

QString formatAgo(double seconds)
{
    if (seconds < 1.5)
        return QStringLiteral("just now");
    if (seconds < 90)
        return QStringLiteral("%1 s ago").arg(qRound(seconds));
    return QStringLiteral("%1 min ago").arg(qRound(seconds / 60));
}

// A stable pseudo-random number in 0..1, for the stars:
// the sky is the same sky every time the window opens.
double scatter(quint32 seed)
{
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    return double(seed % 100000u) / 100000.0;
}

// The kinds the list shows: every one that sounds, the most frequent on top.
// Kinds with the same tally keep the enum's order.
QVector<Motion> listedMotions(const Scene *scene)
{
    QVector<Motion> motions;
    for (const Motion motion : forkmesh::sonify::kMotions) {
        if (motion != Motion::Rest)
            motions.append(motion);
    }
    if (scene) {
        std::stable_sort(motions.begin(), motions.end(), [scene](Motion a, Motion b) {
            return scene->landCount(a) > scene->landCount(b);
        });
    }
    return motions;
}

// A name as a body carries it: without the "#N" its tag already shows.
QString withoutNumber(const Body &body, const QString &text)
{
    QString title = text.simplified();
    const QString number = QStringLiteral("#%1").arg(body.sessionId);
    if (title.startsWith(number))
        title = title.mid(number.size()).trimmed();
    while (title.startsWith(QLatin1Char('-')) || title.startsWith(QLatin1Char(':')))
        title = title.mid(1).trimmed();
    return title;
}

// What an agent is called round its body: the project it works in.
QString shortTitle(const Body &body)
{
    return withoutNumber(body, body.title);
}

// And what it was asked to do, under that.
QString shortTask(const Body &body)
{
    return withoutNumber(body, body.task);
}

// Writes `text` round `centre`, its letters starting `inner` pixels out and
// standing away from it, centred over the top or under the bottom. Both read
// left to right the right way up: over the top the words run clockwise,
// under it anticlockwise. Longer than `sweep` radians, it is cut short.
// Returns the radians it took.
double drawRingText(QPainter &painter, const QPointF &centre, double inner, bool top,
                  const QString &text, const QFontMetricsF &metrics, double sweep)
{
    // Over the top the letters stand on the circle; under it they hang from it.
    const double baseline = inner + (top ? metrics.descent() : metrics.ascent());
    const QString shown = metrics.elidedText(text, Qt::ElideRight, sweep * baseline);
    if (shown.isEmpty())
        return 0;
    const double direction = top ? 1.0 : -1.0;
    const double taken = metrics.horizontalAdvance(shown) / baseline;
    double at = (top ? -M_PI / 2 : M_PI / 2) - direction * taken / 2;
    QTextBoundaryFinder letters(QTextBoundaryFinder::Grapheme, shown);
    qsizetype from = 0;
    for (qsizetype to = letters.toNextBoundary(); to != -1; to = letters.toNextBoundary()) {
        const QString letter = shown.mid(from, to - from);
        from = to;
        const double width = metrics.horizontalAdvance(letter);
        const double angle = at + direction * width / baseline / 2;
        at += direction * width / baseline;
        if (letter.trimmed().isEmpty())
            continue;
        painter.save();
        painter.translate(polar(centre, baseline, angle));
        painter.rotate((angle + direction * M_PI / 2) * 180.0 / M_PI);
        painter.drawText(QPointF(-width / 2, 0), letter);
        painter.restore();
    }
    return taken;
}

// The figure of a cue (sonify::isCue) - a log line's family, a new host,
// background work - centred on `at`, `size` its half-width. The same drawing
// is the mark in the field and the icon on the list, so the two always match.
// `turn` spins the ones that move (0..1).
void paintCueGlyph(QPainter &painter, Motion motion, const QPointF &at, double size,
                   const QColor &colour, double turn)
{
    const QPen pen(colour, qMax(1.0, size * 0.26), Qt::SolidLine, Qt::RoundCap,
                   Qt::RoundJoin);
    QColor fill = colour;
    fill.setAlphaF(fill.alphaF() * 0.3f);
    painter.save();
    painter.translate(at);
    switch (motion) {
    case Motion::Connect: {
        // A signal: a dot and two arcs opening upwards from it.
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(QPointF(0, size * 0.55), size * 0.22, size * 0.22);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (int k = 1; k <= 2; ++k) {
            const double radius = size * 0.5 * k;
            painter.drawArc(QRectF(-radius, size * 0.55 - radius, radius * 2, radius * 2),
                            45 * 16, 90 * 16);
        }
        break;
    }
    case Motion::Task: {
        // A box turning over: work picked up.
        painter.rotate(turn * 180.0);
        painter.setPen(pen);
        painter.setBrush(fill);
        painter.drawRect(QRectF(-size * 0.6, -size * 0.6, size * 1.2, size * 1.2));
        break;
    }
    case Motion::LogError: {
        // A cross, struck.
        painter.rotate(turn * 30.0);
        painter.setPen(QPen(colour, qMax(1.2, size * 0.34), Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(-size * 0.7, -size * 0.7), QPointF(size * 0.7, size * 0.7));
        painter.drawLine(QPointF(-size * 0.7, size * 0.7), QPointF(size * 0.7, -size * 0.7));
        break;
    }
    case Motion::LogStall: {
        // An hourglass.
        painter.setPen(pen);
        painter.setBrush(fill);
        QPolygonF glass;
        glass << QPointF(-size * 0.6, -size * 0.8) << QPointF(size * 0.6, -size * 0.8)
              << QPointF(-size * 0.6, size * 0.8) << QPointF(size * 0.6, size * 0.8);
        painter.drawPolygon(glass);
        break;
    }
    case Motion::LogSync: {
        // Two arrows chasing round a circle.
        painter.rotate(turn * 180.0);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const QRectF box(-size * 0.75, -size * 0.75, size * 1.5, size * 1.5);
        for (int k = 0; k < 2; ++k) {
            painter.drawArc(box, (k * 180 + 20) * 16, 130 * 16);
            const double tip = (k * 180 + 150) * M_PI / 180.0;
            const QPointF end(std::cos(tip) * size * 0.75, -std::sin(tip) * size * 0.75);
            const QPointF back(std::cos(tip - 0.5) * size * 1.0, -std::sin(tip - 0.5) * size * 1.0);
            painter.drawLine(end, back);
        }
        break;
    }
    case Motion::LogGit: {
        // A branch: a stem forking to two tips.
        painter.setPen(pen);
        painter.drawLine(QPointF(-size * 0.4, size * 0.8), QPointF(-size * 0.4, -size * 0.8));
        painter.drawLine(QPointF(-size * 0.4, size * 0.2), QPointF(size * 0.45, -size * 0.3));
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        for (const QPointF &node : {QPointF(-size * 0.4, size * 0.8), QPointF(-size * 0.4, -size * 0.8),
                                    QPointF(size * 0.45, -size * 0.3)})
            painter.drawEllipse(node, size * 0.24, size * 0.24);
        break;
    }
    case Motion::LogCloud: {
        // A cloud: three puffs on a flat base.
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(QPointF(-size * 0.45, size * 0.15), size * 0.42, size * 0.38);
        painter.drawEllipse(QPointF(0, -size * 0.15), size * 0.5, size * 0.48);
        painter.drawEllipse(QPointF(size * 0.45, size * 0.15), size * 0.42, size * 0.38);
        painter.drawRect(QRectF(-size * 0.45, size * 0.1, size * 0.9, size * 0.43));
        break;
    }
    case Motion::LogTask: {
        // A note: a page with two lines on it.
        painter.setPen(pen);
        painter.setBrush(fill);
        painter.drawRect(QRectF(-size * 0.55, -size * 0.75, size * 1.1, size * 1.5));
        painter.drawLine(QPointF(-size * 0.25, -size * 0.25), QPointF(size * 0.25, -size * 0.25));
        painter.drawLine(QPointF(-size * 0.25, size * 0.2), QPointF(size * 0.25, size * 0.2));
        break;
    }
    case Motion::LogAccount: {
        // A key.
        painter.rotate(-30.0 + turn * 40.0);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(-size * 0.45, 0), size * 0.35, size * 0.35);
        painter.drawLine(QPointF(-size * 0.1, 0), QPointF(size * 0.8, 0));
        painter.drawLine(QPointF(size * 0.55, 0), QPointF(size * 0.55, size * 0.3));
        painter.drawLine(QPointF(size * 0.8, 0), QPointF(size * 0.8, size * 0.3));
        break;
    }
    case Motion::LogInfo: {
        // An i: a dot over a stem.
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(QPointF(0, -size * 0.6), size * 0.2, size * 0.2);
        painter.setPen(pen);
        painter.drawLine(QPointF(0, -size * 0.15), QPointF(0, size * 0.75));
        break;
    }
    case Motion::Visitor: {
        // A globe: somebody on the web. Its meridian turns.
        painter.setPen(pen);
        painter.setBrush(fill);
        painter.drawEllipse(QPointF(0, 0), size * 0.8, size * 0.8);
        painter.setBrush(Qt::NoBrush);
        const double meridian = size * 0.8 * std::abs(std::cos(turn * M_PI));
        painter.drawEllipse(QPointF(0, 0), qMax(0.5, meridian), size * 0.8);
        painter.drawLine(QPointF(-size * 0.8, 0), QPointF(size * 0.8, 0));
        break;
    }
    case Motion::Client: {
        // A screen on its stand: another desktop, or a phone.
        painter.setPen(pen);
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(-size * 0.8, -size * 0.7, size * 1.6, size * 1.1),
                                size * 0.15, size * 0.15);
        painter.drawLine(QPointF(0, size * 0.4), QPointF(0, size * 0.75));
        painter.drawLine(QPointF(-size * 0.4, size * 0.75), QPointF(size * 0.4, size * 0.75));
        break;
    }
    case Motion::Socket: {
        // Frames on the wire, the beads of the field: a warm one going out
        // and a cool one coming in, riding the same line.
        painter.setPen(pen);
        painter.drawLine(QPointF(-size * 0.85, 0), QPointF(size * 0.85, 0));
        painter.setPen(Qt::NoPen);
        const double x = size * (-0.5 + turn);
        painter.setBrush(QColor::fromHslF(0.08f, 0.9f, 0.62f, colour.alphaF()));
        painter.drawEllipse(QPointF(x, -size * 0.28), size * 0.3, size * 0.3);
        painter.setBrush(QColor::fromHslF(0.52f, 0.9f, 0.62f, colour.alphaF()));
        painter.drawEllipse(QPointF(-x, size * 0.28), size * 0.3, size * 0.3);
        break;
    }
    default:
        break;
    }
    painter.restore();
}

// `text` after `label`, broken between words over lines of about `width`
// characters - the card cuts a longer one short - at most `most` of them.
void wrapInto(QStringList &lines, const QString &label, const QString &text, int width = 64,
              int most = 4)
{
    QString line = label;
    bool fresh = true; // nothing on the line yet but its label or indent
    int used = 0;
    for (const QString &word : text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
        if (!fresh && line.size() + word.size() + 1 > width) {
            if (++used == most) {
                lines << line + QChar(0x2026);
                return;
            }
            lines << line;
            line = QStringLiteral("    ");
            fresh = true;
        }
        line += (line.isEmpty() || line.endsWith(QLatin1Char(' ')) ? QString()
                                                                   : QStringLiteral(" "))
                + word;
        fresh = false;
    }
    if (!fresh)
        lines << line;
}

// What the relay's kinds look like on the field, for the list's card: the
// row beside the desktop has no words of its own.
QStringList relayKey(Motion motion)
{
    switch (motion) {
    case Motion::Visitor:
        return {QStringLiteral("on the field: a flag in the row left of this desktop, the "
                               "country the site's edge placed the browser in; a globe "
                               "where it did not say"),
                QStringLiteral("a ring opens as one arrives; dashed, it fades as it leaves"),
                QStringLiteral("an amber warning sign: worth watching - a fast rate, a "
                               "probe for a path like /.env, many refusals"),
                QStringLiteral("a red warning sign: danger - an attack tool, a flood, or a "
                               "crowd out of one range or network"),
                QStringLiteral("a red bar through it: blocked at the edge by this desktop"),
                QStringLiteral("click a visitor from the Worker's log to block it")};
    case Motion::Client:
        return {QStringLiteral("on the field: a diamond for another desktop, a slate for a "
                               "phone, nearest this desktop in the row left of it")};
    case Motion::Socket:
        return {QStringLiteral("on the field: the beads flying between this desktop and the "
                               "row beside it"),
                QStringLiteral("warm (orange): a frame this desktop sent - presence, chat, "
                               "typing, keys. One sent to the whole room flies to everybody "
                               "in it at once, which is the stream of beads out to the "
                               "visitors"),
                QStringLiteral("cool (teal): a frame coming in from somebody in the room, or "
                               "a site visitor's request reaching the Worker"),
                QStringLiteral("a site visitor seen only in the Worker's log is not in the "
                               "room: nothing is sent back to it")};
    default:
        return {};
    }
}

} // namespace

#ifdef FORKMESH_HAVE_OPENGL
// The view's own painting, done on an OpenGL surface laid over it. It takes
// no input - the mouse goes through to the view underneath - and holds no
// state: every frame is the view's paintField, on the GPU's paint engine.
class AgentActivityView::GpuSurface : public QOpenGLWidget
{
public:
    explicit GpuSurface(AgentActivityView *view)
        : QOpenGLWidget(view)
        , m_view(view)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        // The GL paint engine only smooths edges by multisampling.
        QSurfaceFormat surface = format();
        surface.setSamples(4);
        setFormat(surface);
    }

protected:
    void paintGL() override
    {
        QPainter painter(this);
        m_view->paintField(painter);
    }

private:
    AgentActivityView *m_view;
};
#endif

AgentActivityView::AgentActivityView(Scene *scene, QWidget *parent)
    : QWidget(parent)
    , m_scene(scene)
{
    setObjectName(QStringLiteral("agentActivityView"));
    setMouseTracking(true);
    setMinimumSize(360, 240);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_stars.reserve(460);
    for (quint32 i = 0; i < 460; ++i) {
        Star star;
        star.x = scatter(i * 7 + 1);
        star.y = scatter(i * 7 + 2);
        // Most stars are faint pinpricks; a few are bright enough to glow.
        const double r = scatter(i * 7 + 3);
        star.size = 0.5 + 1.9 * r * r * r;
        star.brightness = 0.25 + 0.75 * scatter(i * 7 + 4);
        star.phase = 6.2832 * scatter(i * 7 + 6);
        m_stars.append(star);
    }
    applyTheme();
    layoutField();
}

QStringList AgentActivityView::backdropNames()
{
    return {QStringLiteral("Theme"), QStringLiteral("Space"), QStringLiteral("Aurora"),
            QStringLiteral("Ocean")};
}

void AgentActivityView::setBackdrop(Backdrop backdrop)
{
    if (m_backdrop == backdrop)
        return;
    m_backdrop = backdrop;
    applyTheme();
    redraw();
}

void AgentActivityView::setShowNetwork(bool show)
{
    if (m_network == show)
        return;
    m_network = show;
    redraw();
}

void AgentActivityView::setShowProcesses(bool show)
{
    if (m_processes == show)
        return;
    m_processes = show;
    redraw();
}

void AgentActivityView::setShowLogs(bool show)
{
    if (m_logs == show)
        return;
    m_logs = show;
    layoutField();
    redraw();
}

void AgentActivityView::setHideOutput(bool hide)
{
    if (m_hideOutput == hide)
        return;
    m_hideOutput = hide;
    redraw();
}

void AgentActivityView::setShowSounds(bool show)
{
    if (m_sounds == show)
        return;
    m_sounds = show;
    layoutField();
    redraw();
}

QPointF AgentActivityView::hostPosition(const QString &name) const
{
    const Host *host = m_scene ? m_scene->host(name) : nullptr;
    if (!host)
        return {};
    return QPointF(m_field.left() + host->x * m_field.width(), m_field.top() + 10);
}

QString AgentActivityView::hostAt(const QPointF &point) const
{
    if (!m_network || !m_scene)
        return {};
    QString best;
    double bestDistance = 12;
    for (const Host &host : m_scene->hosts()) {
        const QPointF p = hostPosition(host.name);
        const double distance = std::hypot(p.x() - point.x(), p.y() - point.y());
        if (distance <= bestDistance) {
            best = host.name;
            bestDistance = distance;
        }
    }
    return best;
}

AgentActivityView::FlowHit AgentActivityView::flowAt(const QPointF &point) const
{
    FlowHit best;
    if (!m_network || !m_scene)
        return best;
    double bestDistance = 6;
    for (const Flow &flow : m_scene->flows()) {
        const QPointF from = flowEnd(flow.sessionId);
        const QPointF to = hostPosition(flow.host);
        if (from.isNull() || to.isNull())
            continue;
        if (flow.sessionId != Scene::kDesktop && presence(flow.sessionId) <= 0.01)
            continue;
        const QPointF control = flowControl(from, to);
        QPointF previous = from;
        for (int i = 1; i <= 32; ++i) {
            const QPointF next = onQuad(from, control, to, i / 32.0);
            const QPointF span = next - previous;
            const double length = QPointF::dotProduct(span, span);
            const double t = length > 0
                                 ? qBound(0.0, QPointF::dotProduct(point - previous, span) / length,
                                          1.0)
                                 : 0.0;
            const QPointF nearest = previous + span * t;
            const double distance = std::hypot(nearest.x() - point.x(), nearest.y() - point.y());
            if (distance < bestDistance) {
                best = {flow.sessionId, flow.host};
                bestDistance = distance;
            }
            previous = next;
        }
    }
    return best;
}

QPointF AgentActivityView::flowEnd(int sessionId) const
{
    // A mirror's own marks and sparks rise off its seat along the top - or
    // off the desktop's node, the machine that took the line, when the
    // network layer that draws the seats is switched off.
    if (const Host *mirror = m_scene ? m_scene->mirrorForSource(sessionId) : nullptr) {
        if (m_network)
            return hostPosition(mirror->name);
        sessionId = Scene::kDesktop;
    }
    if (sessionId == Scene::kDesktop) {
        // Along the bottom, under the rows of agents: the machine they all
        // run on, with the hosts they talk to along the top.
        return QPointF(m_field.center().x(),
                       m_field.bottom() - 34.0 * qMax(0.8, m_unit / 520.0));
    }
    const Body *body = m_scene ? m_scene->body(sessionId) : nullptr;
    return body ? pointFor(*body) : QPointF();
}

QPointF AgentActivityView::flowControl(const QPointF &from, const QPointF &to) const
{
    const QPointF span = to - from;
    const double length = std::hypot(span.x(), span.y());
    if (length < 1)
        return (from + to) / 2.0;
    const QPointF normal(-span.y() / length, span.x() / length);
    return (from + to) / 2.0 + normal * (length * 0.12);
}

QPointF AgentActivityView::joinControl(const QPointF &a, const QPointF &b) const
{
    const QPointF span = b - a;
    const double length = std::hypot(span.x(), span.y());
    if (length < 1)
        return (a + b) / 2.0;
    // Bowed away from the straight line, so three agents joined to one
    // another are three readable arcs rather than one thicket.
    const QPointF normal(-span.y() / length, span.x() / length);
    return (a + b) / 2.0 + normal * (length * 0.16 + 12.0);
}

void AgentActivityView::applyTheme()
{
    // The night backdrops are dark whatever the app's theme is, and the marks
    // on them are drawn lit.
    m_dark = m_backdrop != Backdrop::Theme
             || palette().color(QPalette::Window).lightness() < 128;
    m_background = m_dark ? QColor(6, 9, 16) : QColor(248, 249, 252);
    m_foreground = m_dark ? QColor(236, 242, 250) : QColor(24, 28, 36);
    m_muted = m_dark ? QColor(140, 152, 172) : QColor(96, 104, 118);
    m_guide = m_dark ? QColor(255, 255, 255, 16) : QColor(16, 22, 34, 20);
}

void AgentActivityView::setWorkingOnly(bool on)
{
    if (m_workingOnly == on)
        return;
    m_workingOnly = on;
    if (const Body *hovered = m_scene ? m_scene->body(m_hover) : nullptr;
        hovered && !shownNow(*hovered))
        m_hover = -1;
    retimeAnimation();
    redraw();
}

bool AgentActivityView::shownNow(const Body &body) const
{
    return !m_workingOnly || body.working(m_scene->now());
}

double AgentActivityView::presence(int sessionId) const
{
    const auto it = m_presence.constFind(sessionId);
    if (it != m_presence.constEnd())
        return it.value();
    // Not painted yet: it is where it will be once it is, so an agent added a
    // moment ago can already be hovered and clicked.
    const Body *body = m_scene ? m_scene->body(sessionId) : nullptr;
    return body && (!m_workingOnly || body->working(m_scene->now())) ? 1.0 : 0.0;
}

bool AgentActivityView::easePresence()
{
    if (!m_scene)
        return false;
    const double now = m_scene->now();
    // The first frame shows the field as it is; after that, arrivals and
    // departures fade.
    const bool first = m_presenceClock < 0;
    const double elapsed = first ? 0.0 : qMax(0.0, now - m_presenceClock);
    m_presenceClock = now;
    const double step = elapsed / kFadeSeconds;
    bool moving = false;
    QHash<int, double> next;
    for (const Body &body : m_scene->bodies()) {
        const double target = !m_workingOnly || body.working(now) ? 1.0 : 0.0;
        double value = m_presence.value(body.sessionId, first ? target : 0.0);
        if (value < target)
            value = qMin(target, value + step);
        else if (value > target)
            value = qMax(target, value - step);
        if (!qFuzzyCompare(1.0 + value, 1.0 + target))
            moving = true;
        next.insert(body.sessionId, value);
    }
    m_presence = next;
    return moving;
}

void AgentActivityView::layoutField()
{
    const double margin = 14;
    // The log takes a column on the left when it is asked for and there is
    // room; it wants the room first, because it was asked for by name.
    m_logRect = QRectF();
    if (m_logs && width() >= 760) {
        const double logs = qBound(240.0, width() * 0.24, 360.0);
        m_logRect = QRectF(10, 10, logs, height() - 20);
    }
    // The list of sounds takes a column on the right when there is room for it
    // and the field keeps the rest; on a narrow window the field matters more.
    m_listRect = QRectF();
    double list = 0;
    if (m_sounds && width() >= (m_logRect.isEmpty() ? 700 : 1100)) {
        list = qBound(230.0, width() * 0.24, 330.0);
        m_listRect = QRectF(width() - list - 10, 10, list, height() - 20);
        // As much of each row as there is room to write: the name, what it
        // stands for, and what you hear.
        const int rows = int(listedMotions(m_scene).size());
        const double room = m_listRect.height() - 12;
        m_rowLines = room >= rows * 50.0 ? 3 : (room >= rows * 36.0 ? 2 : 1);
        m_rowHeight = qMin(room / rows, m_rowLines == 3 ? 58.0 : (m_rowLines == 2 ? 42.0 : 26.0));
    }
    const double right = list > 0 ? m_listRect.left() - 12 : width() - margin;
    const double left = m_logRect.isEmpty() ? margin : m_logRect.right() + 12;
    m_field = QRectF(left + 12, margin,
                     qMax(60.0, right - left - 24),
                     qMax(60.0, height() - margin * 2));
    m_unit = qMin(m_field.width(), m_field.height());
}

QRectF AgentActivityView::soundRowRect(Motion motion) const
{
    if (m_listRect.isEmpty())
        return {};
    const QVector<Motion> motions = listedMotions(m_scene);
    const int index = int(motions.indexOf(motion));
    if (index < 0)
        return {};
    return QRectF(m_listRect.left() + 6, m_listRect.top() + 6 + index * m_rowHeight,
                  m_listRect.width() - 12, m_rowHeight - 3);
}

double AgentActivityView::soundGlow(Motion motion) const
{
    if (!m_scene)
        return 0;
    const double now = m_scene->now();
    const auto glow = [now](double at) {
        const double since = now - at;
        return since < 0 ? 0.0 : std::exp(-since / kRowGlowSeconds);
    };
    // A note you hear lights the row; a mark for an agent nobody is listening
    // to only warms it, so the list tells sound from silence.
    return qMax(glow(m_scene->lastSounded(motion)), 0.18 * glow(m_scene->lastLanded(motion)));
}

void AgentActivityView::setMotionShown(Motion motion, bool shown)
{
    const int index = int(motion);
    if (index <= 0 || index >= forkmesh::sonify::kMotionCount || m_hidden[index] == !shown)
        return;
    m_hidden[index] = !shown;
    redraw();
}

bool AgentActivityView::motionShown(Motion motion) const
{
    const int index = int(motion);
    return index <= 0 || index >= forkmesh::sonify::kMotionCount || !m_hidden[index];
}

void AgentActivityView::setMotionHeard(Motion motion, bool heard)
{
    const int index = int(motion);
    if (index <= 0 || index >= forkmesh::sonify::kMotionCount || m_silenced[index] == !heard)
        return;
    m_silenced[index] = !heard;
    redraw();
}

bool AgentActivityView::motionHeard(Motion motion) const
{
    const int index = int(motion);
    return index <= 0 || index >= forkmesh::sonify::kMotionCount || !m_silenced[index];
}

QRectF AgentActivityView::kindSwitchRect(Motion motion, bool sound) const
{
    const QRectF row = soundRowRect(motion);
    if (row.isEmpty())
        return {};
    // Two small switches at the right of the row's first line: the figure,
    // then the sound.
    const double side = qMin(16.0, row.height() - 4);
    const double top = row.top() + qMin((row.height() - side) / 2, 4.0);
    const double right = row.right() - 4 - (sound ? 0 : side + 6);
    return QRectF(right - side, top, side, side);
}

void AgentActivityView::setShowLinks(bool show)
{
    if (m_links == show)
        return;
    m_links = show;
    redraw();
}

void AgentActivityView::setShowRings(bool show)
{
    if (m_rings == show)
        return;
    m_rings = show;
    redraw();
}

QColor AgentActivityView::ink(double hue, double saturation, double lightness,
                              double alpha) const
{
    // The same hue reads differently on the two fields: on the dark one the
    // marks are lit, on the light one they are inked.
    const double light = m_dark ? lightness : qBound(0.18, lightness - 0.22, 0.82);
    QColor colour = QColor::fromHslF(qBound(0.0, hue, 0.9999),
                                     qBound(0.0, saturation, 1.0),
                                     qBound(0.0, light, 1.0));
    colour.setAlphaF(float(qBound(0.0, alpha, 1.0)));
    return colour;
}

void AgentActivityView::drawGlow(QPainter &painter, const QPointF &centre, double radius,
                                 const QColor &colour) const
{
    const double alpha = colour.alphaF();
    if (alpha <= 0.004 || radius <= 0.2)
        return;
    // The sprite is painted at the size it is drawn, in device pixels, so
    // laying it down is a plain blend with no filtering. Close colours share
    // one (a step of four in each channel is below what a soft glow shows),
    // and a big glow's size goes in steps of four pixels, so a body that
    // breathes uses a handful of sprites rather than one a frame.
    const double ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    int diameter = qMax(2, qRound(radius * 2 * ratio));
    if (diameter > 80)
        diameter = (diameter + 2) / 4 * 4;
    const QRgb rgb = qRgb(colour.red() & 0xfc, colour.green() & 0xfc, colour.blue() & 0xfc);
    const quint64 key = (quint64(diameter) << 32) | rgb;
    QImage sprite;
    if (const QImage *cached = m_glows.object(key)) {
        sprite = *cached;
    } else {
        sprite = QImage(diameter, diameter, QImage::Format_ARGB32_Premultiplied);
        sprite.fill(Qt::transparent);
        QPainter p(&sprite);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QPointF mid(diameter / 2.0, diameter / 2.0);
        QColor solid = QColor::fromRgb(rgb);
        QRadialGradient gradient(mid, diameter / 2.0);
        gradient.setColorAt(0.0, solid);
        solid.setAlpha(0);
        gradient.setColorAt(1.0, solid);
        p.setPen(Qt::NoPen);
        p.setBrush(gradient);
        p.drawEllipse(mid, diameter / 2.0, diameter / 2.0);
        p.end();
        sprite.setDevicePixelRatio(ratio);
        m_glows.insert(key, new QImage(sprite), int(sprite.sizeInBytes() / 1024) + 1);
    }
    const double was = painter.opacity();
    painter.setOpacity(was * alpha);
    const double half = diameter / 2.0 / ratio;
    painter.drawImage(centre - QPointF(half, half), sprite);
    painter.setOpacity(was);
}

QPointF AgentActivityView::pointFor(const Body &body) const
{
    // Its seat, plus where the float and its last note have carried it: the
    // middle of its row, its names circling it, lifted by half the room its
    // display takes under it.
    return QPointF(m_field.left() + (body.x + body.dx) * m_field.width(),
                   m_field.top() + (body.y + body.dy) * m_field.height() - ledRoom() / 2);
}

double AgentActivityView::radiusFor(const Body &body) const
{
    const double scale = m_unit / 520.0;
    const double base = 9.0 + 17.0 * body.size + 9.0 * body.energy;
    // No bigger than its row leaves room for: its running ring and the words
    // circling it, above it and below, and its display under those.
    double most = 64.0;
    if (m_scene && !m_scene->rows().isEmpty())
        most = qMax(8.0, m_scene->rows().first().height * m_field.height() / 2 - kLabelGap
                             - kLabelPixels - 3.0 - ledRoom() / 2);
    return qBound(5.0, base * scale, most);
}

QPointF AgentActivityView::bodyPosition(int sessionId) const
{
    if (const Body *body = m_scene ? m_scene->body(sessionId) : nullptr)
        return pointFor(*body);
    return {};
}

double AgentActivityView::bodyRadius(int sessionId) const
{
    if (const Body *body = m_scene ? m_scene->body(sessionId) : nullptr)
        return radiusFor(*body);
    return 0;
}

void AgentActivityView::setFace(int sessionId, const QString &key, const QIcon &face)
{
    if (face.isNull()) {
        if (m_faces.remove(sessionId))
            redraw();
        return;
    }
    const auto it = m_faces.constFind(sessionId);
    if (it != m_faces.constEnd() && it->first == key)
        return;
    m_faces.insert(sessionId, {key, face});
    redraw();
}

QPixmap AgentActivityView::faceArt(int sessionId, double radius, qreal ratio) const
{
    const auto it = m_faces.constFind(sessionId);
    if (it == m_faces.constEnd())
        return QPixmap();
    // Rounded up to a multiple of 8 device pixels and scaled down to fit when
    // drawn, so a body that grows a little does not cut a new face.
    const int pixels = qMax(8, (int(std::ceil(radius * 2 * ratio)) + 7) / 8 * 8);
    const QString cacheKey = it->first + QLatin1Char('|') + QString::number(pixels);
    if (const QPixmap *cached = m_faceArt.object(cacheKey))
        return *cached;
    const QPixmap art = it->second.pixmap(QSize(pixels, pixels));
    if (art.isNull())
        return QPixmap();
    QPixmap cut(pixels, pixels);
    cut.fill(Qt::transparent);
    {
        QPainter p(&cut);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        QPainterPath round;
        round.addEllipse(QRectF(0, 0, pixels, pixels));
        p.setClipPath(round);
        p.drawPixmap(QRect(0, 0, pixels, pixels), art);
    }
    m_faceArt.insert(cacheKey, new QPixmap(cut), qMax(1, pixels * pixels * 4 / 1024));
    return cut;
}

int AgentActivityView::sessionAt(const QPointF &point) const
{
    if (!m_scene)
        return -1;
    int best = -1;
    double bestDistance = 0;
    for (const Body &body : m_scene->bodies()) {
        if (!shownNow(body))
            continue; // filtered out: not there to be clicked, even mid-fade
        const QPointF centre = pointFor(body);
        const double dx = centre.x() - point.x();
        const double dy = centre.y() - point.y();
        const double distance = std::sqrt(dx * dx + dy * dy);
        // A little wider than the body itself, so a small idle agent is still
        // easy to hit.
        const double reach = qMax(radiusFor(body) + 6.0, 14.0);
        if (distance > reach)
            continue;
        if (best < 0 || distance < bestDistance) {
            best = body.sessionId;
            bestDistance = distance;
        }
    }
    return best;
}

QString AgentActivityView::headline() const
{
    if (!m_scene)
        return {};
    const int agents = m_scene->bodies().size();
    const int working = m_scene->activeCount();
    QString text = QStringLiteral("%1 agent%2")
                       .arg(agents)
                       .arg(agents == 1 ? QString() : QStringLiteral("s"));
    if (m_workingOnly)
        text += QStringLiteral(" (%1 shown)").arg(m_scene->workingCount());
    const double down = m_scene->downRate();
    const double up = m_scene->upRate();
    if (m_network && (down >= 1 || up >= 1))
        text += QStringLiteral(" · network ↓ %1 ↑ %2").arg(formatRate(down), formatRate(up));
    if (m_network) {
        int mirrors = 0;
        int busy = 0;
        for (const Host &host : m_scene->hosts()) {
            if (!host.mirror)
                continue;
            ++mirrors;
            if (Scene::rateOf(host.upRecent + host.downRecent) >= 1)
                ++busy;
        }
        if (mirrors > 0)
            text += QStringLiteral(" · %1 mirror%2 (%3 in use)")
                        .arg(mirrors)
                        .arg(mirrors == 1 ? QString() : QStringLiteral("s"))
                        .arg(busy);
    }
    if (m_network) {
        const int visitors = m_scene->clientCount(ClientKind::Visitor);
        const int others = m_scene->clientCount(ClientKind::Node)
                           + m_scene->clientCount(ClientKind::Phone);
        if (visitors > 0)
            text += QStringLiteral(" · %1 web visitor%2")
                        .arg(visitors)
                        .arg(visitors == 1 ? QString() : QStringLiteral("s"));
        if (others > 0)
            text += QStringLiteral(" · %1 other client%2")
                        .arg(others)
                        .arg(others == 1 ? QString() : QStringLiteral("s"));
        if (const int frames = m_scene->frameRate())
            text += QStringLiteral(" · %1 relay frame%2 a minute")
                        .arg(frames)
                        .arg(frames == 1 ? QString() : QStringLiteral("s"));
        if (const Threat threat = m_scene->siteThreat(); threat != Threat::None)
            text += QStringLiteral(" · ⚠ %1: %2")
                        .arg(threatLabel(threat),
                             m_scene->siteThreatReasons().join(QStringLiteral(", ")));
    }
    text += QStringLiteral(" · %1 working now").arg(working);
    if (const int tasks = m_scene->runningTasks())
        text += QStringLiteral(" · %1 background task%2")
                    .arg(tasks)
                    .arg(tasks == 1 ? QString() : QStringLiteral("s"));
    if (const int processes = m_scene->runningProcesses(); m_processes && processes > 0)
        text += QStringLiteral(" · %1 process%2 (%3 busy)")
                    .arg(processes)
                    .arg(processes == 1 ? QString() : QStringLiteral("es"))
                    .arg(m_scene->busyProcesses());
    text += QStringLiteral(" · %1 action%2 a minute")
                .arg(m_scene->eventRate())
                .arg(m_scene->eventRate() == 1 ? QString() : QStringLiteral("s"));
    if (m_logs)
        text += QStringLiteral(" · %1 log line%2 a minute")
                    .arg(m_scene->logRate())
                    .arg(m_scene->logRate() == 1 ? QString() : QStringLiteral("s"));
    if (const Body *body = m_scene->body(m_hover)) {
        text += QStringLiteral("    ") + describeBody(*body);
        if (!body->doing.isEmpty())
            text += QStringLiteral(" — ") + body->doing;
    }
    return text;
}

QStringList AgentActivityView::hoverLines() const
{
    QStringList lines;
    if (!m_scene || !m_mouseInside || m_logRect.contains(m_mouse))
        return lines;
    const double now = m_scene->now();
    const QPointF at = m_mouse;

    if (const Body *body = m_scene->body(m_hover); body && shownNow(*body)) {
        QStringList names{shortTitle(*body), shortTask(*body)};
        names.removeAll(QString());
        const QString name = names.join(QStringLiteral(" · "));
        lines << (name.isEmpty() ? QStringLiteral("#%1").arg(body->sessionId)
                                 : QStringLiteral("#%1 %2").arg(QString::number(body->sessionId),
                                                                name));
        static const char *states[] = {"idle", "running", "waiting", "done", "failed"};
        QString status = QString::fromLatin1(states[qBound(0, int(body->state), 4)]);
        status += body->listening ? QStringLiteral(" · playing as ")
                                  : QStringLiteral(" · would sound as ");
        status += forkmesh::sonify::voiceDescription(body->voice);
        lines << status;
        if (body->turns > 0 || body->tokens > 0 || body->events > 0)
            lines << QStringLiteral("%1 turns · %2k tokens · %3 events")
                         .arg(body->turns)
                         .arg(body->tokens / 1000)
                         .arg(body->events);
        // What it was asked: the session's prompt, and the latest one sent
        // to it if that is something else, wrapped over a few lines.
        const auto wrapped = [&lines](const QString &label, const QString &text) {
            const QString flat = text.simplified();
            constexpr int kWidth = 64;
            constexpr int kMaxLines = 4;
            QString line = label;
            int used = 0;
            for (const QString &word : flat.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                if (line.size() + word.size() + 1 > kWidth && line.size() > label.size()) {
                    if (++used == kMaxLines) {
                        lines << line + QChar(0x2026);
                        return;
                    }
                    lines << line;
                    line = QStringLiteral("    ");
                }
                line += (line.endsWith(QLatin1Char(' ')) ? QString() : QStringLiteral(" ")) + word;
            }
            if (line.trimmed().size() > 0)
                lines << line;
        };
        if (!body->prompt.isEmpty())
            wrapped(QStringLiteral("prompt:"), body->prompt);
        if (!body->lastPrompt.isEmpty()
            && body->lastPrompt.simplified() != body->prompt.simplified())
            wrapped(QStringLiteral("latest:"), body->lastPrompt);
        if (!body->said.isEmpty())
            wrapped(QStringLiteral("said:"), plainProse(body->said, 600));
        if (!body->doing.isEmpty())
            lines << QStringLiteral("now: ") + body->doing;
        if (!body->subject.isEmpty())
            lines << QStringLiteral("on: ") + body->subject;
        if (body->netUp > 0 || body->netDown > 0) {
            lines << QStringLiteral("network: ↑ %1 ↓ %2 (now ↑ %3 ↓ %4)")
                         .arg(formatBytes(body->netUp), formatBytes(body->netDown),
                              formatRate(Scene::rateOf(body->netUpRecent)),
                              formatRate(Scene::rateOf(body->netDownRecent)));
            // Each connection: the domain it resolved to, then the address,
            // port and kind of network it is on.
            int shown = 0;
            for (const Flow &flow : m_scene->flows()) {
                if (flow.sessionId != m_hover)
                    continue;
                for (const Endpoint &endpoint : flow.endpoints) {
                    if (shown++ >= 4)
                        break;
                    lines << (flow.host == endpoint.address
                                  ? QStringLiteral("→ ") + describeEndpoint(endpoint.address,
                                                                           endpoint.port)
                                  : QStringLiteral("→ %1 (%2)")
                                        .arg(flow.host, describeEndpoint(endpoint.address,
                                                                         endpoint.port)));
                }
            }
        }
        for (const Join &join : m_scene->joins()) {
            if (join.a == m_hover || join.b == m_hover)
                lines << QStringLiteral("with #%1: %2")
                             .arg(QString::number(join.a == m_hover ? join.b : join.a),
                                  join.why);
        }
        // What the picture of it means.
        lines << QStringLiteral("size: tokens burned · glow: how busy it is now");
        QString rings = QStringLiteral(
            "icons round it: its latest marks, as the list of sounds draws them, newest at the top");
        if (body->state == State::Running)
            rings += QStringLiteral(" · outer ring: running");
        else if (body->state == State::Waiting)
            rings += QStringLiteral(" · dotted ring: waiting on you");
        else if (body->state == State::Failed)
            rings += QStringLiteral(" · red ring: failed");
        lines << rings;
        lines << QStringLiteral("click to open · double-click to %1")
                     .arg(body->listening ? QStringLiteral("stop listening")
                                          : QStringLiteral("listen"));
        return lines.mid(0, 22);
    }

    // A switch in the list of sounds.
    if (m_listRect.contains(at)) {
        for (const Motion motion : listedMotions(m_scene)) {
            for (const bool sound : {false, true}) {
                if (!kindSwitchRect(motion, sound).adjusted(-2, -2, 2, 2).contains(at))
                    continue;
                const QString label = QString::fromLatin1(styleFor(motion).label);
                const bool on = sound ? motionHeard(motion) : motionShown(motion);
                lines << (sound ? QStringLiteral("%1 sound: %2") : QStringLiteral("%1 figure: %2"))
                             .arg(label, on ? QStringLiteral("on") : QStringLiteral("off"));
                lines << (on ? (sound ? QStringLiteral("click to silence it")
                                      : QStringLiteral("click to hide it"))
                             : (sound ? QStringLiteral("click to hear it again")
                                      : QStringLiteral("click to show it again")));
                return lines;
            }
        }
        // The rest of a row: what the kind is, and how much of it there has been.
        for (const Motion motion : listedMotions(m_scene)) {
            if (!soundRowRect(motion).contains(at))
                continue;
            const MotionStyle &style = styleFor(motion);
            lines << QString::fromLatin1(style.label);
            wrapInto(lines, QStringLiteral("stands for:"), QString::fromLatin1(style.description));
            for (const QString &key : relayKey(motion))
                wrapInto(lines, QString(), key);
            lines << QStringLiteral("sounds like: ") + QString::fromLatin1(style.sound);
            lines << QStringLiteral("%1 seen · %2 heard")
                         .arg(m_scene->landCount(motion))
                         .arg(m_scene->soundCount(motion));
            const QString last = m_scene->lastText(motion);
            if (!last.isEmpty())
                lines << QStringLiteral("latest: ") + last.left(120);
            return lines;
        }
        return lines;
    }

    // A background task round an agent or the desktop.
    for (const Task &task : m_scene->tasks()) {
        const QPointF p = taskPosition(task);
        if (p.isNull() || std::hypot(p.x() - at.x(), p.y() - at.y()) > 8)
            continue;
        lines << (task.kind == TaskKind::Desktop ? QStringLiteral("background: ") + task.word
                                                 : task.word);
        if (!task.detail.isEmpty())
            lines << task.detail.left(160);
        lines << (task.running()
                      ? QStringLiteral("running for %1 s").arg(now - task.bornSeconds, 0, 'f', 1)
                      : (task.failed ? QStringLiteral("failed") : QStringLiteral("done")));
        lines << (task.sessionId == Scene::kDesktop
                      ? QStringLiteral("for this desktop")
                      : QStringLiteral("for #%1").arg(task.sessionId));
        return lines;
    }

    // A process in an agent's tree.
    if (m_processes) {
        const Process *nearest = nullptr;
        double best = 7.0;
        for (const Process &process : m_scene->processes()) {
            const QPointF p = processPosition(process);
            const double d = p.isNull() ? 1e9 : std::hypot(p.x() - at.x(), p.y() - at.y());
            if (d <= best) {
                best = d;
                nearest = &process;
            }
        }
        if (nearest) {
            lines << QStringLiteral("process: %1 (pid %2)").arg(nearest->name).arg(nearest->pid);
            if (!nearest->command.isEmpty())
                lines << nearest->command.left(160);
            if (!nearest->running())
                lines << QStringLiteral("ended");
            else if (nearest->busy())
                lines << QStringLiteral("busy, %1% of a core").arg(qRound(nearest->cpu * 100));
            else
                lines << QStringLiteral("sleeping");
            lines << QStringLiteral("seen for %1").arg(formatDuration(now - nearest->bornSeconds));
            const Process *parent = nearest->parentPid
                                        ? m_scene->process(nearest->sessionId, nearest->parentPid)
                                        : nullptr;
            lines << (parent ? QStringLiteral("started by %1 (pid %2), for #%3")
                                   .arg(parent->name)
                                   .arg(parent->pid)
                                   .arg(nearest->sessionId)
                             : QStringLiteral("started by #%1").arg(nearest->sessionId));
            return lines;
        }
    }

    // A host along the top, or the desktop's own node.
    if (m_network) {
        for (const Host &host : m_scene->hosts()) {
            const QPointF p = hostPosition(host.name);
            if (std::hypot(p.x() - at.x(), p.y() - at.y()) > 12)
                continue;
            lines << host.name;
            lines << hostPlaceLabel(hostPlace(host.name, host.mirror));
            // The domain it resolved to, and where each connection really went.
            const bool named = std::none_of(host.endpoints.cbegin(), host.endpoints.cend(),
                                            [&](const Endpoint &endpoint) {
                                                return endpoint.address == host.name;
                                            });
            if (host.name == QLatin1String("this machine"))
                lines << QStringLiteral("domain: localhost");
            else if (named && !host.mirror)
                lines << QStringLiteral("domain: ") + host.name;
            for (const Endpoint &endpoint : host.endpoints)
                lines << QStringLiteral("  ") + describeEndpoint(endpoint.address, endpoint.port);
            // No name (the host is its address): who holds it, from whois.
            if (!named && !host.mirror && host.name != QLatin1String("this machine")) {
                const QStringList network = m_scene->addressNetwork(host.name);
                lines << (network.isEmpty() ? QStringList{QStringLiteral("whois: looking up…")}
                                            : network);
            }
            lines << QStringLiteral("now ↓ %1  ↑ %2")
                         .arg(formatRate(Scene::rateOf(host.downRecent)),
                              formatRate(Scene::rateOf(host.upRecent)));
            lines << QStringLiteral("since it appeared ↓ %1  ↑ %2")
                         .arg(formatBytes(host.down), formatBytes(host.up));
            QStringList talking;
            QStringList connections;
            for (const Flow &flow : m_scene->flows()) {
                if (flow.host != host.name)
                    continue;
                const QString who = flow.sessionId == Scene::kDesktop
                                        ? QStringLiteral("this desktop")
                                        : QStringLiteral("#%1").arg(flow.sessionId);
                talking << who;
                for (const Endpoint &endpoint : flow.endpoints)
                    connections << QStringLiteral("  %1 → %2:%3  ↓ %4  ↑ %5")
                                       .arg(who, endpoint.address.contains(QLatin1Char(':'))
                                                     ? QLatin1Char('[') + endpoint.address
                                                           + QLatin1Char(']')
                                                     : endpoint.address)
                                       .arg(endpoint.port)
                                       .arg(formatBytes(endpoint.down), formatBytes(endpoint.up));
            }
            if (!talking.isEmpty())
                lines << QStringLiteral("talking: ") + talking.join(QStringLiteral(", "));
            else if (host.mirror)
                lines << QStringLiteral("nobody is talking to it right now");
            // Its own work: the journal it streams into the desktop's log.
            if (host.mirror) {
                if (host.lines > 0) {
                    lines << QStringLiteral("its log: %1 line%2 a minute, %3 since it "
                                            "took its seat")
                                 .arg(m_scene->lineRate(host.source))
                                 .arg(m_scene->lineRate(host.source) == 1
                                          ? QString()
                                          : QStringLiteral("s"))
                                 .arg(host.lines);
                    QString newest = host.lastLine.simplified();
                    if (newest.size() > 72)
                        newest = newest.left(71) + QChar(0x2026);
                    lines << QStringLiteral("  ") + newest;
                } else {
                    lines << QStringLiteral("its log: nothing yet");
                }
            }
            if (!connections.isEmpty()) {
                lines << QStringLiteral("connections:");
                lines << connections.mid(0, 8);
            }
            lines << QStringLiteral("beads on a line: warm going out, cool coming in");
            QStringList shown = lines.mid(0, 27);
            if (m_onConnectionClicked)
                shown << QStringLiteral("click to watch its traffic live");
            return shown;
        }
    }
    // A client of the relay, in the row beside the desktop.
    if (const QString id = clientAt(at); !id.isEmpty()) {
        const Client &client = *m_scene->client(id);
        QString name = client.info.name.trimmed();
        if (name.isEmpty())
            name = client.info.nodeName.trimmed();
        lines << (name.isEmpty() ? client.info.id.left(16) : name);
        switch (client.kind) {
        case ClientKind::Visitor:
            lines << (!client.web ? QStringLiteral("web visitor: a browser on the site, "
                                                   "in the relay room")
                      : client.tool.isEmpty()
                          ? QStringLiteral("web visitor: a browser on the site, "
                                           "from the Worker's live log")
                          : QStringLiteral("web visitor: a script (%1), from the "
                                           "Worker's live log")
                                .arg(client.tool));
            break;
        case ClientKind::Phone:
            lines << QStringLiteral("phone: the mobile app, in the relay room");
            break;
        case ClientKind::Node:
            lines << QStringLiteral("desktop node, in the relay room");
            break;
        }
        if (client.blocked)
            lines << QStringLiteral("⊘ blocked at the edge by this desktop");
        if (client.threat != Threat::None) {
            wrapInto(lines,
                     QStringLiteral("⚠ %1:").arg(threatLabel(client.threat)),
                     client.threatReasons.join(QStringLiteral("; ")));
        }
        if (!client.info.country.isEmpty())
            lines << QStringLiteral("from: %1 (%2)")
                         .arg(countryName(client.info.country), client.info.country);
        QStringList facts;
        if (!client.info.nodeName.isEmpty() && client.info.nodeName != name)
            facts << QStringLiteral("node ") + client.info.nodeName;
        if (!client.info.platform.isEmpty())
            facts << client.info.platform;
        if (!client.info.version.isEmpty())
            facts << QStringLiteral("ForkMesh ") + client.info.version;
        if (!client.info.accountKind.isEmpty())
            facts << client.info.accountKind + QStringLiteral(" account");
        if (!facts.isEmpty() && !client.web)
            lines << facts.join(QStringLiteral(" · "));
        if (client.web) {
            const QString &agent = client.info.userAgent;
            const QString browser = browserForUserAgent(agent);
            const QString system = systemForUserAgent(agent);
            QString runs = browser.isEmpty() ? client.tool : browser;
            if (!system.isEmpty())
                runs += (runs.isEmpty() ? QString() : QStringLiteral(" on ")) + system;
            if (!runs.isEmpty())
                lines << QStringLiteral("runs: ") + runs;
            if (!agent.isEmpty())
                wrapInto(lines, QStringLiteral("agent:"), agent, 64, 3);
        }
        if (!client.info.detail.isEmpty())
            lines << client.info.detail;
        if (client.web) {
            lines << (client.present()
                          ? QStringLiteral("on the site for %1")
                                .arg(formatDuration(now - client.bornSeconds))
                          : QStringLiteral("left %1").arg(formatAgo(now - client.leftSeconds)));
            const int rate = client.requestRate(now);
            lines << QStringLiteral("%1 request%2 · %3 in the last minute")
                         .arg(client.requests)
                         .arg(client.requests == 1 ? QString() : QStringLiteral("s"))
                         .arg(rate);
            if (!client.types.isEmpty()) {
                QStringList answers;
                for (auto it = client.types.cbegin(); it != client.types.cend(); ++it)
                    answers << QStringLiteral("%1 %2").arg(it.key()).arg(it.value());
                lines << QStringLiteral("answered: ") + answers.join(QStringLiteral(" · "));
            }
            lines << QStringLiteral("latest: %1, %2")
                         .arg(client.lastType, formatAgo(now - client.lastFrameSeconds));
            for (qsizetype r = 1; r < client.recent.size(); ++r)
                lines << QStringLiteral("  %1, %2")
                             .arg(client.recent.at(r).first,
                                  formatAgo(now - client.recent.at(r).second));
            lines << QStringLiteral("beads: cool, its requests reaching the site; it is "
                                    "not in the relay room, so none go back to it");
            if (!client.blocked && !client.info.address.isEmpty()) {
                for (const BlockRule &rule : m_scene->blockRules(client.info.id)) {
                    if (rule.recommended)
                        lines << QStringLiteral("click to block it - best: ") + rule.label;
                }
            }
            return lines;
        }
        lines << (client.present()
                      ? QStringLiteral("seen for %1").arg(formatDuration(now - client.bornSeconds))
                      : QStringLiteral("left %1").arg(formatAgo(now - client.leftSeconds)));
        lines << QStringLiteral("frames: ↓ %1 from it (%2) · ↑ %3 to it (%4)")
                     .arg(client.framesIn)
                     .arg(formatBytes(client.bytesIn))
                     .arg(client.framesOut)
                     .arg(formatBytes(client.bytesOut));
        if (!client.types.isEmpty()) {
            // Most first: what fills the socket to it.
            QVector<std::pair<QString, int>> kinds;
            for (auto it = client.types.cbegin(); it != client.types.cend(); ++it)
                kinds.append({it.key(), it.value()});
            std::stable_sort(kinds.begin(), kinds.end(),
                             [](const auto &a, const auto &b) { return a.second > b.second; });
            QStringList parts;
            for (qsizetype k = 0; k < qMin<qsizetype>(kinds.size(), 5); ++k)
                parts << QStringLiteral("%1 %2").arg(kinds.at(k).first).arg(kinds.at(k).second);
            lines << QStringLiteral("by kind: ") + parts.join(QStringLiteral(" · "));
        }
        if (!client.lastType.isEmpty())
            lines << QStringLiteral("latest frame: %1, %2")
                         .arg(client.lastType, formatAgo(now - client.lastFrameSeconds));
        lines << QStringLiteral("beads: warm, this desktop's frames to it or to the whole "
                                "room; cool, its frames to this desktop");
        return lines;
    }
    const QPointF desktop = flowEnd(Scene::kDesktop);
    if (std::hypot(desktop.x() - at.x(), desktop.y() - at.y()) < 12) {
        lines << QStringLiteral("this desktop");
        lines << QStringLiteral("its own traffic: the relay and the API behind every page");
        lines << QStringLiteral("round it: its background work · sparks: its log");
        if (const int tasks = m_scene->runningTasks())
            lines << QStringLiteral("%1 background task%2 running")
                         .arg(tasks)
                         .arg(tasks == 1 ? QString() : QStringLiteral("s"));
        // The relay's websocket, and who is on the other end of it.
        const RelaySocket &socket = m_scene->relaySocket();
        if (socket.connected)
            lines << QStringLiteral("relay websocket: connected %1%2")
                         .arg(socket.endpoint.isEmpty() ? QString()
                                                        : QStringLiteral("to ") + socket.endpoint
                                                              + QLatin1Char(' '),
                              socket.connectedSeconds > 0
                                  ? QStringLiteral("for ") + formatDuration(socket.connectedSeconds)
                                  : QString());
        else
            lines << QStringLiteral("relay websocket: not connected");
        if (!socket.room.isEmpty())
            lines << QStringLiteral("  ") + socket.room;
        if (socket.framesIn > 0 || socket.framesOut > 0)
            lines << QStringLiteral("  frames ↓ %1 (%2) · ↑ %3 (%4)")
                         .arg(socket.framesIn)
                         .arg(formatBytes(socket.bytesIn))
                         .arg(socket.framesOut)
                         .arg(formatBytes(socket.bytesOut));
        if (socket.controlIn > 0 || socket.controlOut > 0)
            lines << QStringLiteral("  pings and pongs ↓ %1 · ↑ %2")
                         .arg(socket.controlIn)
                         .arg(socket.controlOut);
        if (socket.buffered > 0)
            lines << QStringLiteral("  %1 waiting to send").arg(formatBytes(socket.buffered));
        if (!socket.lastInType.isEmpty() || !socket.lastOutType.isEmpty())
            lines << QStringLiteral("  latest ↓ %1 · ↑ %2")
                         .arg(socket.lastInType.isEmpty() ? QStringLiteral("-") : socket.lastInType,
                              socket.lastOutType.isEmpty() ? QStringLiteral("-")
                                                           : socket.lastOutType);
        lines << QStringLiteral("  %1 frame%2 a minute while watched")
                     .arg(m_scene->frameRate())
                     .arg(m_scene->frameRate() == 1 ? QString() : QStringLiteral("s"));
        const int visitors = m_scene->clientCount(ClientKind::Visitor);
        const int nodes = m_scene->clientCount(ClientKind::Node);
        const int phones = m_scene->clientCount(ClientKind::Phone);
        lines << QStringLiteral("in the room: %1 web visitor%2 · %3 desktop node%4 · %5 phone%6")
                     .arg(visitors)
                     .arg(visitors == 1 ? QString() : QStringLiteral("s"))
                     .arg(nodes)
                     .arg(nodes == 1 ? QString() : QStringLiteral("s"))
                     .arg(phones)
                     .arg(phones == 1 ? QString() : QStringLiteral("s"));
        // Where the site's visitors are, most first.
        QHash<QString, int> countries;
        for (const Client &client : m_scene->clients()) {
            if (client.present() && !client.info.country.isEmpty())
                ++countries[client.info.country];
        }
        if (!countries.isEmpty()) {
            QVector<std::pair<QString, int>> ranked;
            for (auto it = countries.cbegin(); it != countries.cend(); ++it)
                ranked.append({it.key(), it.value()});
            std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            QStringList parts;
            for (qsizetype k = 0; k < qMin<qsizetype>(ranked.size(), 6); ++k)
                parts << QStringLiteral("%1 %2").arg(countryName(ranked.at(k).first))
                             .arg(ranked.at(k).second);
            if (ranked.size() > 6)
                parts << QStringLiteral("%1 more").arg(ranked.size() - 6);
            wrapInto(lines, QStringLiteral("visitors from:"), parts.join(QStringLiteral(" · ")));
        }
        lines << QStringLiteral("  %1 newcomer%2 in the last minute")
                     .arg(m_scene->arrivalRate())
                     .arg(m_scene->arrivalRate() == 1 ? QString() : QStringLiteral("s"));
        if (const Threat threat = m_scene->siteThreat(); threat != Threat::None)
            wrapInto(lines, QStringLiteral("⚠ %1:").arg(threatLabel(threat)),
                     m_scene->siteThreatReasons().join(QStringLiteral("; ")));
        return lines;
    }

    if (const FlowHit hit = flowAt(at); !hit.host.isEmpty()) {
        const QString who = hit.sessionId == Scene::kDesktop
                                ? QStringLiteral("this desktop")
                                : QStringLiteral("#%1").arg(hit.sessionId);
        lines << QStringLiteral("%1 → %2").arg(who, hit.host);
        for (const Flow &flow : m_scene->flows()) {
            if (flow.sessionId != hit.sessionId || flow.host != hit.host)
                continue;
            lines << QStringLiteral("now ↓ %1  ↑ %2")
                         .arg(formatRate(Scene::rateOf(flow.downRecent)),
                              formatRate(Scene::rateOf(flow.upRecent)));
            for (const Endpoint &endpoint : flow.endpoints)
                lines << QStringLiteral("  ") + describeEndpoint(endpoint.address, endpoint.port);
            lines << QStringLiteral("last traffic %1").arg(formatAgo(now - flow.lastSeconds));
        }
        if (m_onConnectionClicked)
            lines << QStringLiteral("click to watch its traffic live");
        return lines;
    }

    // A link between two agents.
    for (const Join &join : m_scene->joins()) {
        const Body *a = m_scene->body(join.a);
        const Body *b = m_scene->body(join.b);
        if (!a || !b || qMin(presence(join.a), presence(join.b)) <= 0.01)
            continue;
        const QPointF pa = pointFor(*a);
        const QPointF pb = pointFor(*b);
        const QPointF control = joinControl(pa, pb);
        for (int i = 1; i < 20; ++i) {
            const QPointF p = onQuad(pa, control, pb, i / 20.0);
            if (std::hypot(p.x() - at.x(), p.y() - at.y()) < 7) {
                lines << join.why;
                lines << QStringLiteral("#%1 and #%2").arg(join.a).arg(join.b);
                return lines;
            }
        }
    }
    return lines;
}

void AgentActivityView::retimeAnimation()
{
    if (!isVisible()) {
        m_timer.stop();
        m_intervalMs = 0;
        return;
    }
    // Anything still moving keeps the full frame rate: an agent running or
    // acting, a figure or packet in flight, background work, a link whose
    // sparks are still running, a body gliding to a note or fading in or out.
    // Nothing moving, nothing painted: the idle timer only watches for a start.
    // Only the displays moving: a frame of those alone, stepped once a
    // refresh, costs a blit and a few strips.
    const int wanted = m_fading || (m_scene && m_scene->moving())
                           ? busyIntervalMs()
                           : (ledsMoving() ? ledIntervalMs() : kIdleIntervalMs);
    if (m_timer.isActive() && m_intervalMs == wanted)
        return;
    m_intervalMs = wanted;
    m_timer.start(wanted, Qt::PreciseTimer, this);
}

int AgentActivityView::ledIntervalMs() const
{
    const QScreen *where = screen();
    const double hz = where ? where->refreshRate() : 60.0;
    if (hz < 1)
        return 16;
    return qBound(kFastestIntervalMs, int(1000.0 / hz), kBusyIntervalMs);
}

int AgentActivityView::busyIntervalMs() const
{
    if (!m_surface)
        return kBusyIntervalMs;
    // One frame per refresh of the screen the window is on.
    const QScreen *where = screen();
    const double hz = where ? where->refreshRate() : 60.0;
    if (hz < 1)
        return 16;
    return qBound(kFastestIntervalMs, int(1000.0 / hz), kBusyIntervalMs);
}

bool AgentActivityView::gpuAvailable()
{
#ifdef FORKMESH_HAVE_OPENGL
    static const bool available = [] {
        if (qEnvironmentVariable("FORKMESH_AGENT_ACTIVITY_GPU") == QLatin1String("0"))
            return false;
        const QString platform = QGuiApplication::platformName();
        if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal"))
            return false;
        // A context that will not start here would leave the surface black:
        // try one first, and paint in software if it fails.
        QOpenGLContext probe;
        return probe.create() && probe.isValid();
    }();
    return available;
#else
    return false;
#endif
}

void AgentActivityView::setGpuRendering(bool on)
{
    on = on && gpuAvailable();
    if (on == (m_surface != nullptr))
        return;
#ifdef FORKMESH_HAVE_OPENGL
    if (on) {
        m_surface = new GpuSurface(this);
        m_surface->setGeometry(rect());
        m_surface->show();
    } else
#endif
    {
        delete m_surface;
        m_surface = nullptr;
    }
    m_frameTimes.clear();
    m_intervalMs = 0;
    retimeAnimation();
    update();
}

void AgentActivityView::setShowFrameRate(bool show)
{
    if (m_showFrameRate == show)
        return;
    m_showFrameRate = show;
    redraw();
}

bool AgentActivityView::onlyLedsMoving() const
{
    return m_scene && !m_fading && !m_scene->moving() && ledsMoving();
}

void AgentActivityView::redraw()
{
    // Something changed. While only the displays move, the still under them
    // is painted again and the frame is theirs; otherwise a whole frame.
    m_ledFrame = onlyLedsMoving();
    if (m_ledFrame) {
        m_stillStale = true;
        m_ledRegion = QRegion(rect());
    }
    if (m_surface)
        m_surface->update();
    else
        update();
}

int AgentActivityView::framesPerSecond() const
{
    if (!m_frameClock.isValid())
        return 0;
    const qint64 since = m_frameClock.nsecsElapsed() - 1000000000LL;
    int frames = 0;
    for (const qint64 at : m_frameTimes)
        frames += at > since ? 1 : 0;
    return frames;
}

QString AgentActivityView::frameRateText() const
{
    const QString engine = m_surface ? QStringLiteral("GPU") : QStringLiteral("CPU");
    // A still field paints nothing, so a count of its last second would be
    // the rate it was moving at, not the rate it is at.
    if (!animating())
        return QStringLiteral("%1 · idle").arg(engine);
    return QStringLiteral("%1 · %2 fps · %3 ms")
        .arg(engine)
        .arg(framesPerSecond())
        .arg(m_paintMs, 0, 'f', 1);
}

void AgentActivityView::timerEvent(QTimerEvent *event)
{
    if (event->timerId() != m_timer.timerId()) {
        QWidget::timerEvent(event);
        return;
    }
    if (m_scene)
        m_scene->tick();
    retimeAnimation();
    // One last frame after the field comes to rest, then none until it
    // moves - bar the displays, which go on scrolling over it.
    const bool busy = m_fading || (m_scene && m_scene->moving());
    if (busy || m_paintedMoving)
        redraw();
    else if (ledsMoving())
        redrawLeds();
    m_paintedMoving = busy;
}

void AgentActivityView::redrawLeds()
{
    if (m_scene && m_scene->now() - m_stillSeconds > kStillSeconds)
        m_stillStale = true;
    // Where every display is: a frame where none has moved a dot is not
    // painted at all.
    size_t seen = m_stillStale ? 1 : 0;
    QRegion region;
    for (const Body &body : m_scene->bodies()) {
        if (body.said.isEmpty() || presence(body.sessionId) <= 0.01)
            continue;
        const QRectF box = ledRect(body, pointFor(body), radiusFor(body));
        region += box.toAlignedRect().adjusted(-1, -1, 1, 1);
        const auto led = m_leds.constFind(body.sessionId);
        const bool drawn = led != m_leds.constEnd()
                           && led->source.constData() == body.said.constData()
                           && led->source.size() == body.said.size();
        seen = qHashMulti(seen, body.sessionId, box.left(), box.top(),
                          drawn ? ledScroll(body, led->width, box.width() - 2 * kLedPad)
                                : -1.0);
    }
    if (seen == m_ledSeen && !m_showFrameRate)
        return;
    m_ledSeen = seen;
    if (m_surface) {
        m_ledFrame = true;
        m_surface->update();
        return;
    }
    // Painting a new still shows the whole of it.
    if (m_stillStale)
        region = QRegion(rect());
    else if (m_showFrameRate)
        region += frameRateRect(QFont(), frameRateText()).toAlignedRect().adjusted(-40, -2, 2, 2);
    m_ledFrame = true;
    m_ledRegion = region;
    update(region);
}

bool AgentActivityView::animating() const
{
    if (m_fading || (m_scene && m_scene->moving()))
        return true;
    if (!m_scene)
        return false;
    return ledsMoving();
}

bool AgentActivityView::ledsMoving() const
{
    if (!m_scene)
        return false;
    for (const Body &body : m_scene->bodies()) {
        if (body.said.isEmpty() || presence(body.sessionId) <= 0.01)
            continue;
        // A reply not drawn yet, or one too long to fit going round.
        const auto led = m_leds.constFind(body.sessionId);
        if (led == m_leds.constEnd() || led->source.constData() != body.said.constData()
            || led->source.size() != body.said.size())
            return true;
        if (led->width > ledRect(body, QPointF(), 0).width() - 2 * kLedPad)
            return true;
    }
    return false;
}

const AgentActivityView::Led &AgentActivityView::ledFor(const Body &body, qreal ratio) const
{
    Led &led = m_leds[body.sessionId];
    // The same reply as last frame is the same buffer: no need to read it
    // again, let alone draw it.
    if (led.ratio == ratio && led.source.size() == body.said.size()
        && (led.source.constData() == body.said.constData() || led.source == body.said)) {
        led.source = body.said;
        return led;
    }
    led.source = body.said;
    const QString text = plainProse(body.said);
    if (led.text == text && led.ratio == ratio && (text.isEmpty() || !led.strip.isNull()))
        return led;
    led.text = text;
    led.ratio = ratio;
    led.strip = QPixmap();
    led.width = 0;
    if (text.isEmpty())
        return led;
    // The words one pixel a dot, then every lit pixel stamped as a dot.
    const QFont font = ledFont();
    const QFontMetrics metrics(font);
    const int rows = ledRows();
    const int columns = qMax(1, metrics.horizontalAdvance(text) + 1);
    QImage mask(columns, rows, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter p(&mask);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(QPointF(0, metrics.ascent()), text);
    }
    led.width = columns * kLedPitch;
    QImage strip(qCeil(led.width * ratio), qCeil(rows * kLedPitch * ratio),
                 QImage::Format_ARGB32_Premultiplied);
    strip.fill(Qt::transparent);
    {
        QPainter p(&strip);
        for (int row = 0; row < rows; ++row) {
            const QRgb *line = reinterpret_cast<const QRgb *>(mask.constScanLine(row));
            const auto [y, phaseY] = litDotPlace((row + 0.5) * kLedPitch * ratio, ratio);
            for (int column = 0; column < columns; ++column) {
                if (qAlpha(line[column]) <= 110)
                    continue;
                const auto [x, phaseX] = litDotPlace((column + 0.5) * kLedPitch * ratio, ratio);
                p.drawImage(QPoint(x, y), litDot(ratio, phaseX, phaseY));
            }
        }
    }
    strip.setDevicePixelRatio(ratio);
    led.strip = QPixmap::fromImage(std::move(strip));
    return led;
}

QRectF AgentActivityView::ledRect(const Body &body, const QPointF &centre, double radius) const
{
    // As wide as its place in the row allows, a whole number of dots across.
    const double room = qBound(64.0, body.slot * m_field.width() - 12, kLedMaxWidth);
    const double width = std::floor((room - 2 * kLedPad) / kLedPitch) * kLedPitch + 2 * kLedPad;
    return QRectF(std::round(centre.x() - width / 2),
                  std::round(centre.y() + radius + kLabelGap + kLabelPixels + 2), width,
                  ledHeight());
}

double AgentActivityView::ledScroll(const Body &body, double width, double window) const
{
    if (width <= window || !m_scene)
        return 0;
    const double t = m_scene->now() - body.saidSeconds - kLedHold;
    if (t <= 0)
        return 0;
    // It moves a whole dot at a time, the way a display does, and comes
    // round to its first word again after the gap.
    const double moved = std::fmod(t * kLedSpeed, width + kLedGapDots * kLedPitch);
    return std::floor(moved / kLedPitch) * kLedPitch;
}

void AgentActivityView::paintLed(QPainter &painter, const Body &body, const QPointF &centre,
                                 double radius)
{
    if (body.said.isEmpty())
        return;
    const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const Led &led = ledFor(body, ratio);
    if (led.strip.isNull())
        return;
    const QRectF box = ledRect(body, centre, radius);
    painter.setPen(QPen(ink(body.hue, 0.6, 0.5, 0.7), 1.0));
    painter.setBrush(QColor(14, 10, 6, 235));
    painter.drawRoundedRect(box, 3, 3);

    const QRectF window = box.adjusted(kLedPad, kLedPad, -kLedPad, -kLedPad);
    const int rows = ledRows();
    if (m_ledDark.isNull() || m_ledDark.devicePixelRatio() != ratio) {
        // Every unlit dot a display can have, drawn once.
        const int columns = int(kLedMaxWidth / kLedPitch);
        m_ledDark = QPixmap(QSize(qCeil(columns * kLedPitch * ratio),
                                  qCeil(rows * kLedPitch * ratio)));
        m_ledDark.setDevicePixelRatio(ratio);
        m_ledDark.fill(Qt::transparent);
        QPainter p(&m_ledDark);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        for (int row = 0; row < rows; ++row) {
            for (int column = 0; column < columns; ++column)
                paintLedDot(p, column, row, false);
        }
    }
    painter.save();
    painter.setClipRect(window);
    painter.drawPixmap(window.topLeft(), m_ledDark);
    // A line that fits sits in the middle; a longer one starts at its first
    // word and scrolls, its first word coming in again behind its last.
    if (led.width <= window.width()) {
        painter.drawPixmap(
            QPointF(window.left()
                        + std::floor((window.width() - led.width) / 2 / kLedPitch) * kLedPitch,
                    window.top()),
            led.strip);
    } else {
        const double left = window.left() - ledScroll(body, led.width, window.width());
        painter.drawPixmap(QPointF(left, window.top()), led.strip);
        const double again = left + led.width + kLedGapDots * kLedPitch;
        if (again < window.right())
            painter.drawPixmap(QPointF(again, window.top()), led.strip);
    }
    painter.restore();
}

void AgentActivityView::paintLeds(QPainter &painter)
{
    for (const Body &body : m_scene->bodies()) {
        const double shown = presence(body.sessionId);
        if (shown <= 0.01 || body.said.isEmpty())
            continue;
        painter.setOpacity(shown);
        paintLed(painter, body, pointFor(body), radiusFor(body));
    }
    painter.setOpacity(1.0);
}

void AgentActivityView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    retimeAnimation();
}

void AgentActivityView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_timer.stop();
    m_intervalMs = 0;
}

void AgentActivityView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        applyTheme();
        redraw();
    }
}

void AgentActivityView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    layoutField();
    if (m_surface)
        m_surface->setGeometry(rect());
}

void AgentActivityView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    m_mouseInside = false;
    if (m_hover != -1)
        m_hover = -1;
    redraw();
}

void AgentActivityView::mouseMoveEvent(QMouseEvent *event)
{
    m_mouse = event->position();
    m_mouseInside = true;
    const int hit = sessionAt(m_mouse);
    const bool overLogs = m_onOpenLogs && m_logRect.contains(m_mouse);
    bool overSwitch = false;
    if (m_listRect.contains(m_mouse)) {
        for (const Motion motion : listedMotions(m_scene))
            overSwitch = overSwitch
                         || kindSwitchRect(motion, false).adjusted(-2, -2, 2, 2).contains(m_mouse)
                         || kindSwitchRect(motion, true).adjusted(-2, -2, 2, 2).contains(m_mouse);
    }
    const bool overConnection =
        hit < 0 && !overLogs && !m_listRect.contains(m_mouse) && m_onConnectionClicked
        && (!hostAt(m_mouse).isEmpty() || !flowAt(m_mouse).host.isEmpty());
    setCursor(hit >= 0 || overLogs || overSwitch || overConnection ? Qt::PointingHandCursor
                                                                    : Qt::ArrowCursor);
    // The card follows the cursor over the same thing; the field under it
    // only changes when the body it lights does.
    const bool same = hit == m_hover;
    m_hover = hit;
    if (same && !m_stillStale && onlyLedsMoving()) {
        m_ledFrame = true;
        m_ledRegion = QRegion(rect());
        if (m_surface)
            m_surface->update();
        else
            update();
        return;
    }
    redraw();
}

void AgentActivityView::mousePressEvent(QMouseEvent *event)
{
    if (m_onOpenLogs && m_logRect.contains(event->position())) {
        m_onOpenLogs();
        return;
    }
    if (m_listRect.contains(event->position())) {
        for (const Motion motion : listedMotions(m_scene)) {
            for (const bool sound : {false, true}) {
                if (!kindSwitchRect(motion, sound).adjusted(-2, -2, 2, 2).contains(
                        event->position()))
                    continue;
                const bool on = !(sound ? motionHeard(motion) : motionShown(motion));
                if (sound)
                    setMotionHeard(motion, on);
                else
                    setMotionShown(motion, on);
                if (m_onToggleKind)
                    m_onToggleKind(motion, sound, on);
                return;
            }
        }
        return;
    }
    const int hit = sessionAt(event->position());
    if (hit < 0) {
        if (const QString host = hostAt(event->position());
            !host.isEmpty() && m_onConnectionClicked) {
            m_onConnectionClicked(Scene::kAnySession, host);
            return;
        }
        if (const QString client = clientAt(event->position());
            !client.isEmpty() && m_onClientClicked) {
            m_onClientClicked(client, event->globalPosition().toPoint());
            return;
        }
        if (const FlowHit flow = flowAt(event->position());
            !flow.host.isEmpty() && m_onConnectionClicked) {
            m_onConnectionClicked(flow.sessionId, flow.host);
            return;
        }
        QWidget::mousePressEvent(event);
        return;
    }
    m_hover = hit;
    if (m_onSelect)
        m_onSelect(hit);
    redraw();
}

void AgentActivityView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Clicking a switch twice quickly is two clicks, not a double click.
    if (m_listRect.contains(event->position())) {
        mousePressEvent(event);
        return;
    }
    const int hit = sessionAt(event->position());
    if (hit < 0) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    if (m_onToggleListen)
        m_onToggleListen(hit);
    redraw();
}

void AgentActivityView::paintEvent(QPaintEvent *event)
{
    // The OpenGL surface covers the view and paints the field itself.
    if (m_surface)
        return;
    // A paint asked for by more than a display frame is a whole one.
    if (m_ledFrame && !(event->region() - m_ledRegion).isEmpty())
        m_ledFrame = false;
    QPainter painter(this);
    paintField(painter);
}

void AgentActivityView::paintField(QPainter &painter)
{
    if (!m_frameClock.isValid())
        m_frameClock.start();
    const qint64 began = m_frameClock.nsecsElapsed();
    m_frameTimes.append(began);
    const qint64 oldest = began - 1000000000LL;
    qsizetype stale = 0;
    while (stale < m_frameTimes.size() && m_frameTimes.at(stale) <= oldest)
        ++stale;
    m_frameTimes.remove(0, stale);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    const bool ledFrame = m_ledFrame && m_scene;
    m_ledFrame = false;
    if (ledFrame) {
        const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
        const QSize pixels(qCeil(width() * ratio), qCeil(height() * ratio));
        if (m_stillStale || m_still.size() != pixels || m_still.devicePixelRatio() != ratio) {
            QImage still(pixels, QImage::Format_ARGB32_Premultiplied);
            still.setDevicePixelRatio(ratio);
            QPainter p(&still);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setRenderHint(QPainter::TextAntialiasing, true);
            p.setFont(painter.font());
            paintLayers(p);
            p.end();
            m_still = std::move(still);
            m_stillStale = false;
            m_stillSeconds = m_scene->now();
        }
        painter.drawImage(QPointF(0, 0), m_still);
    } else {
        paintLayers(painter);
        // Whatever changed to call for a whole frame is not on the still.
        m_stillStale = true;
    }
    if (!m_scene)
        return;
    paintLeds(painter);
    paintFrameRate(painter);
    paintHoverCard(painter);
    if (!ledFrame)
        setAccessibleDescription(headline());
    // What a frame costs, smoothed so the readout can be read.
    const double took = (m_frameClock.nsecsElapsed() - began) / 1e6;
    m_paintMs = m_paintMs <= 0 ? took : m_paintMs * 0.9 + took * 0.1;
}

void AgentActivityView::paintLayers(QPainter &painter)
{
    paintBackdrop(painter);
    if (!m_scene)
        return;
    m_fading = easePresence();
    paintRows(painter);
    paintJoins(painter);
    paintNetwork(painter);
    paintDesktopNode(painter);
    paintClients(painter);
    paintPulses(painter);
    paintProcesses(painter);
    paintBodies(painter);
    paintTasks(painter);
    paintNetworkMeter(painter);
    paintLogSparks(painter);
    paintSoundList(painter);
    paintLogs(painter);
}

QRectF AgentActivityView::frameRateRect(const QFont &base, const QString &text) const
{
    QFont font = base;
    font.setPixelSize(11);
    const QFontMetricsF metrics(font);
    return QRectF(m_field.right() - metrics.horizontalAdvance(text) - 12,
                  m_field.bottom() - metrics.height() - 6,
                  metrics.horizontalAdvance(text) + 12, metrics.height() + 6);
}

void AgentActivityView::paintFrameRate(QPainter &painter)
{
    if (!m_showFrameRate)
        return;
    painter.setOpacity(1.0);
    QFont font = painter.font();
    font.setPixelSize(11);
    painter.setFont(font);
    const QString text = frameRateText();
    const QRectF box = frameRateRect(font, text);
    painter.setPen(Qt::NoPen);
    QColor back = m_background;
    back.setAlphaF(0.7);
    painter.setBrush(back);
    painter.drawRoundedRect(box, 4, 4);
    painter.setPen(m_muted);
    painter.drawText(box, Qt::AlignCenter, text);
}

void AgentActivityView::paintJoins(QPainter &painter)
{
    if (!m_links)
        return;
    const double now = m_scene->now();
    const QVector<Join> joins = m_scene->joins();
    for (const Join &join : joins) {
        const Body *a = m_scene->body(join.a);
        const Body *b = m_scene->body(join.b);
        if (!a || !b)
            continue;
        const double shown = qMin(presence(join.a), presence(join.b));
        if (shown <= 0.01)
            continue;
        painter.setOpacity(shown);
        const QPointF pa = pointFor(*a);
        const QPointF pb = pointFor(*b);
        if (std::hypot(pb.x() - pa.x(), pb.y() - pa.y()) < 1)
            continue;
        const QPointF control = joinControl(pa, pb);

        QPainterPath path(pa);
        path.quadTo(control, pb);

        QLinearGradient gradient(pa, pb);
        const double alpha = 0.10 + 0.55 * join.strength;
        gradient.setColorAt(0.0, ink(a->hue, 0.85, 0.62, alpha));
        gradient.setColorAt(0.5, ink((a->hue + b->hue) / 2.0, 0.95, 0.70, alpha * 0.9));
        gradient.setColorAt(1.0, ink(b->hue, 0.85, 0.62, alpha));
        QPen pen(QBrush(gradient), 1.0 + 4.0 * join.strength);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }
    painter.setOpacity(1.0);
}

void AgentActivityView::paintFigure(QPainter &painter, Motion motion, const QPointF &c,
                                    double r, double t, double angle, double s) const
{
    const MotionStyle &style = styleFor(motion);
    const double hue = style.hue;
    const double fade = std::pow(1.0 - t, 1.2) * (0.35 + 0.65 * s);
    if (fade <= 0.01)
        return;
    const double a = angle * 2.0 * M_PI;
    const QPointF dir(std::cos(a), std::sin(a));
    const QPointF normal(-dir.y(), dir.x());
    const QColor line = ink(hue, 0.92, 0.66, fade);
    const QColor lit = ink(hue, 1.0, 0.80, fade);
    const double width = qMax(0.8, style.weight * (0.9 + 1.4 * s) * (1.0 - 0.5 * t));
    QPen pen(line, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);

    switch (motion) {
    case Motion::Rest:
        return;
    case Motion::Start: {
        // A flower opening: petals turning outwards, and a ring behind them.
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink(hue, 0.9, 0.66, fade * 0.8));
        const double reach = r * (1.0 + 1.2 * easeOut(t));
        const int petals = 6;
        for (int i = 0; i < petals; ++i) {
            const double at = a + i * 2.0 * M_PI / petals + t * 0.6;
            painter.save();
            painter.translate(polar(c, reach, at));
            painter.rotate(at * 180.0 / M_PI);
            const double len = r * 0.42 * (1.0 - 0.4 * t);
            painter.drawEllipse(QPointF(0, 0), len, len * 0.38);
            painter.restore();
        }
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const double ring = r * (1.0 + 1.9 * easeOut(t));
        painter.drawEllipse(c, ring, ring);
        return;
    }
    case Motion::Speak: {
        // Sound going out: three wave fronts, one after another.
        painter.setBrush(Qt::NoBrush);
        for (int k = 0; k < 3; ++k) {
            const double tk = (t - k * 0.14) / (1.0 - 0.28);
            if (tk <= 0 || tk >= 1)
                continue;
            QPen wave = pen;
            wave.setColor(ink(hue, 0.92, 0.70, fade * (1.0 - tk)));
            painter.setPen(wave);
            const double radius = r * (1.15 + 1.6 * tk);
            const QRectF box(c.x() - radius, c.y() - radius, radius * 2, radius * 2);
            const double mid = -a * 180.0 / M_PI;
            painter.drawArc(box, int((mid - 32) * 16), int(64 * 16));
        }
        return;
    }
    case Motion::Think: {
        // Motes orbiting slowly, each with a tail: turning something over.
        const int motes = 2;
        for (int k = 0; k < motes; ++k) {
            const double at = a + k * 2.0 * M_PI / motes + t * 2.4;
            const double radius = r * (1.35 + 0.22 * std::sin(t * 6.0 + k * 2.0));
            QPen tail(ink(hue, 0.8, 0.72, fade * 0.6), 1.4, Qt::SolidLine, Qt::RoundCap);
            painter.setPen(tail);
            painter.setBrush(Qt::NoBrush);
            const QRectF box(c.x() - radius, c.y() - radius, radius * 2, radius * 2);
            painter.drawArc(box, int(-at * 180.0 / M_PI * 16), int(40 * 16));
            painter.setPen(Qt::NoPen);
            painter.setBrush(lit);
            const double dot = 1.8 + 1.6 * s;
            painter.drawEllipse(polar(c, radius, at), dot, dot);
        }
        return;
    }
    case Motion::Read: {
        // A beam reaching out to a page, and the page drawn back in.
        const double out = r * (1.0 + 1.7 * easeOut(qMin(1.0, t * 2.2)));
        QPointF tip = c + dir * out;
        if (t > 0.5)
            tip = c + dir * (r + (out - r) * (1.0 - easeInOut((t - 0.5) * 2.0)));
        painter.setPen(QPen(ink(hue, 0.9, 0.7, fade * 0.7), 1.2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(c + dir * r, tip);
        painter.save();
        painter.translate(tip);
        painter.rotate(a * 180.0 / M_PI + t * 90.0);
        const double half = 2.5 + 2.5 * s;
        painter.setPen(QPen(lit, 1.0));
        painter.setBrush(ink(hue, 0.9, 0.55, fade * 0.5));
        painter.drawRect(QRectF(-half, -half * 1.3, half * 2, half * 2.6));
        painter.restore();
        return;
    }
    case Motion::Search: {
        // A radar sweep: a wedge turning, and a dashed ring reaching out.
        const double radius = r * (1.2 + 1.3 * easeOut(t));
        const double start = -a * 180.0 / M_PI - t * 220.0;
        QRadialGradient wedge(c, radius);
        wedge.setColorAt(0.0, ink(hue, 0.9, 0.6, 0.0));
        wedge.setColorAt(1.0, ink(hue, 0.95, 0.66, fade * 0.55));
        painter.setPen(Qt::NoPen);
        painter.setBrush(wedge);
        const QRectF box(c.x() - radius, c.y() - radius, radius * 2, radius * 2);
        painter.drawPie(box, int(start * 16), int(34 * 16));
        QPen ring = pen;
        ring.setStyle(Qt::DashLine);
        ring.setWidthF(qMax(0.8, width * 0.7));
        painter.setPen(ring);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(c, radius, radius);
        return;
    }
    case Motion::Web: {
        // Out to the web: a globe flung off the body on a thread, turning,
        // and pulled back in.
        const double out = r * (1.2 + 2.0 * easeOut(qMin(1.0, t * 2.0)));
        const QPointF at = c + dir * (t < 0.6 ? out : r + (out - r) * (1.0 - easeInOut((t - 0.6) / 0.4)));
        QPen thread(ink(hue, 0.9, 0.7, fade * 0.6), 1.0, Qt::DotLine, Qt::RoundCap);
        painter.setPen(thread);
        painter.drawLine(c + dir * r, at);
        const double g = 3.5 + 3.0 * s;
        painter.setPen(pen);
        painter.setBrush(ink(hue, 0.9, 0.5, fade * 0.25));
        painter.drawEllipse(at, g, g);
        painter.setBrush(Qt::NoBrush);
        const double turn = std::abs(std::cos(t * 6.0));
        painter.drawEllipse(at, g * turn, g);
        painter.drawLine(at + QPointF(-g, 0), at + QPointF(g, 0));
        return;
    }
    case Motion::Plan: {
        // A checklist beside the body: rows appearing, each ticked off.
        const QPointF origin = polar(c, r * 1.5, a) + QPointF(0, -r * 0.5);
        const double row = 4.0 + 1.5 * s;
        for (int k = 0; k < 3; ++k) {
            const double appear = qBound(0.0, t * 3.2 - k * 0.6, 1.0);
            if (appear <= 0)
                continue;
            const QPointF at = origin + QPointF(0, k * row * 1.5);
            painter.setPen(QPen(ink(hue, 0.9, 0.7, fade * appear), 1.1, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(at.x(), at.y() - row * 0.5, row, row));
            painter.drawLine(at + QPointF(row * 1.5, 0),
                             at + QPointF(row * 1.5 + row * 2.2 * appear, 0));
            if (appear >= 1.0) {
                painter.setPen(QPen(lit, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                QPolygonF tick;
                tick << at + QPointF(row * 0.15, 0) << at + QPointF(row * 0.45, row * 0.35)
                     << at + QPointF(row * 1.0, -row * 0.45);
                painter.drawPolyline(tick);
            }
        }
        return;
    }
    case Motion::Write: {
        // A diamond set down and spreading, ink falling from it.
        const double half = r * (0.8 + 0.9 * easeOut(t));
        painter.save();
        painter.translate(c);
        painter.rotate(45.0 + t * 35.0 + angle * 90.0);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(-half, -half, half * 2, half * 2));
        painter.setPen(Qt::NoPen);
        painter.setBrush(lit);
        for (int i = 0; i < 4; ++i) {
            const QPointF corner(i % 2 ? half : -half, i / 2 ? half : -half);
            const double spark = 1.4 + 1.8 * s * (1.0 - t);
            painter.drawEllipse(corner, spark, spark);
        }
        painter.restore();
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink(hue, 0.9, 0.7, fade * 0.8));
        for (int i = 0; i < 3; ++i) {
            const QPointF drop(c.x() + (i - 1) * r * 0.45,
                               c.y() + r + t * r * (1.2 + 0.4 * i));
            painter.drawEllipse(drop, 1.6, 2.2);
        }
        return;
    }
    case Motion::Run: {
        // A bolt: a zigzag struck outwards, flickering as it goes.
        const double reach = r * (1.0 + 1.9 * easeOut(qMin(1.0, t * 3.0)));
        const int flicker = int(t * 14.0);
        QPainterPath bolt(c + dir * r);
        for (int i = 1; i <= 5; ++i) {
            const double u = i / 5.0;
            const double side = i == 5 ? 0.0
                                       : ((i + flicker) % 2 ? 1.0 : -1.0) * r
                                             * (0.16 + 0.1 * scatter(quint32(i * 31 + flicker)));
            bolt.lineTo(c + dir * (r + (reach - r) * u) + normal * side);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ink(hue, 1.0, 0.85, fade * 0.35), width * 3.0, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(bolt);
        painter.setPen(pen);
        painter.drawPath(bolt);
        painter.setPen(Qt::NoPen);
        painter.setBrush(lit);
        const QPointF tip = c + dir * reach;
        for (int i = 0; i < 3; ++i) {
            const double at = a + (i - 1) * 0.7;
            painter.drawEllipse(polar(tip, r * 0.5 * easeOut(t), at), 1.3, 1.3);
        }
        return;
    }
    case Motion::Delegate: {
        // A little one launched off the body on a curve, trailing, and gone
        // with a pop where it lands.
        const QPointF from = c + dir * r;
        const QPointF to = c + dir * r * 3.4 + normal * r * 0.8;
        const QPointF control = c + dir * r * 2.0 - normal * r * 1.2;
        painter.setPen(Qt::NoPen);
        for (int k = 6; k >= 0; --k) {
            const double u = easeOut(t) - k * 0.035;
            if (u <= 0)
                continue;
            painter.setBrush(ink(hue, 0.9, 0.72, fade * (1.0 - k / 7.0)));
            const double dot = (2.2 + 2.2 * s) * (1.0 - k / 9.0);
            painter.drawEllipse(onQuad(from, control, to, u), dot, dot);
        }
        if (t > 0.75) {
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const double pop = r * 0.9 * (t - 0.75) * 4.0;
            painter.drawEllipse(to, pop, pop);
        }
        return;
    }
    case Motion::Arrive: {
        // An answer coming back: motes falling in from all round.
        const double radius = r * (1.0 + 2.1 * (1.0 - easeOut(t)));
        const int motes = 6;
        for (int i = 0; i < motes; ++i) {
            const double at = a + i * 2.0 * M_PI / motes;
            const QPointF head = polar(c, radius, at);
            painter.setPen(QPen(ink(hue, 0.9, 0.7, fade * 0.55), 1.1, Qt::SolidLine,
                                Qt::RoundCap));
            painter.drawLine(head, polar(c, radius + r * 0.45 * (1.0 - t), at));
            painter.setPen(Qt::NoPen);
            painter.setBrush(lit);
            painter.drawEllipse(head, 1.4 + 1.2 * s, 1.4 + 1.2 * s);
        }
        return;
    }
    case Motion::Prompt: {
        // You, steering: a comet falling onto the body from above, and the
        // ring it makes when it lands.
        const double fall = easeInOut(qMin(1.0, t * 1.7));
        const QPointF from = c + QPointF(-r * 1.2 + 2.4 * r * angle, -r * 4.5);
        const QPointF head = from + (c - from) * fall;
        if (fall < 1.0) {
            QLinearGradient tail(from, head);
            tail.setColorAt(0.0, ink(hue, 1.0, 0.8, 0.0));
            tail.setColorAt(1.0, ink(hue, 1.0, 0.8, fade));
            painter.setPen(QPen(QBrush(tail), 2.2 + 1.6 * s, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(from + (head - from) * 0.4, head);
            painter.setPen(Qt::NoPen);
            drawGlow(painter, head, 7 + 5 * s, ink(hue, 1.0, 0.85, fade));
        } else {
            const double u = (t - 1.0 / 1.7) / (1.0 - 1.0 / 1.7);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const double ring = r * (1.0 + 2.2 * easeOut(u));
            painter.drawEllipse(c, ring, ring);
        }
        return;
    }
    case Motion::Notice: {
        // A warning: a triangle flaring and shaking.
        const double shake = std::sin(t * 70.0) * 2.5 * (1.0 - t);
        const double radius = r * (1.25 + 1.2 * easeOut(t));
        QPolygonF triangle;
        for (int i = 0; i < 3; ++i)
            triangle << polar(c + QPointF(shake, 0), radius, -M_PI / 2 + i * 2.0 * M_PI / 3.0);
        painter.setPen(pen);
        painter.setBrush(ink(hue, 0.9, 0.5, fade * 0.18));
        painter.drawPolygon(triangle);
        return;
    }
    case Motion::Finish: {
        // A firework: sparks bursting out and drooping, and a ring.
        const double burst = r * (1.0 + 3.0 * easeOut(t));
        painter.setPen(Qt::NoPen);
        const int sparks = 12;
        for (int i = 0; i < sparks; ++i) {
            const double at = a + i * 2.0 * M_PI / sparks;
            const QPointF p = polar(c, burst * (0.8 + 0.2 * scatter(quint32(i * 13))), at)
                              + QPointF(0, t * t * r * 1.4);
            painter.setBrush(ink(hue + 0.04 * (i % 3), 1.0, 0.78, fade));
            painter.drawEllipse(p, 1.5 + 1.4 * s, 1.5 + 1.4 * s);
        }
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const double ring = r * (1.0 + 1.6 * easeOut(t));
        painter.drawEllipse(c, ring, ring);
        return;
    }
    case Motion::Frame: {
        // Framing: a pair of brackets closing round a point just off the body,
        // a message or block opening and shutting.
        const QPointF at = polar(c, r * 1.35, a);
        const double half = (3.0 + 2.0 * s) * (1.0 - 0.3 * t);
        const double gap = half * (1.4 - 0.8 * easeOut(t));
        painter.save();
        painter.translate(at);
        painter.rotate(a * 180.0 / M_PI);
        painter.setPen(QPen(lit, 1.1, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        painter.setBrush(Qt::NoBrush);
        for (const double side : {-1.0, 1.0}) {
            QPolygonF bracket;
            bracket << QPointF(side * (gap - half * 0.4), -half) << QPointF(side * gap, -half)
                    << QPointF(side * gap, half) << QPointF(side * (gap - half * 0.4), half);
            painter.drawPolyline(bracket);
        }
        painter.restore();
        return;
    }
    case Motion::Compose: {
        // A tool call being written: three dots typing out along an arc.
        painter.setPen(Qt::NoPen);
        const int shown = qBound(1, int(t * 4.0) + 1, 3);
        for (int k = 0; k < shown; ++k) {
            const QPointF at = polar(c, r * (1.3 + 0.1 * k), a + k * 0.28);
            painter.setBrush(ink(hue, 0.9, 0.74, fade * (1.0 - 0.2 * k)));
            const double dot = 1.3 + 0.9 * s;
            painter.drawEllipse(at, dot, dot);
        }
        return;
    }
    case Motion::Usage: {
        // The meter: three bars rising under the body, filled as far as the
        // report was big, then draining.
        const double rise = easeOut(qMin(1.0, t * 2.5));
        const QPointF base = c + QPointF(-r * 0.5, r * 1.25);
        painter.setPen(Qt::NoPen);
        for (int k = 0; k < 3; ++k) {
            const double height = r * (0.25 + 0.2 * k) * rise * (0.5 + 0.5 * s);
            painter.setBrush(ink(hue, 0.9, 0.62 + 0.06 * k, fade * 0.9));
            painter.drawRect(QRectF(base.x() + k * r * 0.38, base.y() + r * 0.7 - height,
                                    qMax(1.6, r * 0.24), height));
        }
        return;
    }
    case Motion::System: {
        // The machinery: a small cog turning just off the body.
        const QPointF at = polar(c, r * 1.5, a);
        const double radius = (3.2 + 2.0 * s) * (1.0 + 0.3 * easeOut(t));
        painter.save();
        painter.translate(at);
        painter.rotate(t * 120.0);
        painter.setPen(QPen(lit, 1.1, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QPointF(0, 0), radius * 0.55, radius * 0.55);
        for (int i = 0; i < 6; ++i) {
            const double at2 = i * M_PI / 3.0;
            painter.drawLine(polar(QPointF(0, 0), radius * 0.6, at2),
                             polar(QPointF(0, 0), radius, at2));
        }
        painter.restore();
        return;
    }
    case Motion::Tick: {
        // A line of bookkeeping: a small twinkle just off the body.
        const QPointF at = polar(c, r * 1.4, a);
        const double size = (2.5 + 2.5 * s) * (1.0 - 0.5 * t);
        painter.save();
        painter.translate(at);
        painter.rotate(t * 90.0);
        painter.setPen(QPen(lit, 1.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(-size, 0), QPointF(size, 0));
        painter.drawLine(QPointF(0, -size), QPointF(0, size));
        painter.restore();
        return;
    }
    case Motion::Network: {
        // Bytes on the wire: two beads spiralling off the body.
        painter.setPen(Qt::NoPen);
        for (int k = 0; k < 2; ++k) {
            const double at = a + k * M_PI + t * 5.0;
            const double radius = r * (1.1 + 1.2 * t);
            painter.setBrush(ink(hue, 0.9, 0.78, fade));
            painter.drawEllipse(polar(c, radius, at), 1.6, 1.6);
        }
        return;
    }
    case Motion::Scan: {
        // A listing read off: a row of dots lighting one after another round
        // an arc outside the body.
        const int dots = 5;
        painter.setPen(Qt::NoPen);
        for (int k = 0; k < dots; ++k) {
            const double appear = qBound(0.0, t * (dots + 2) * 0.5 - k * 0.5, 1.0);
            if (appear <= 0)
                continue;
            const double at = a - 0.6 + 1.2 * k / qMax(1, dots - 1);
            painter.setBrush(ink(hue, 0.9, 0.72, fade * appear));
            painter.drawEllipse(polar(c, r * (1.7 + 0.5 * easeOut(t)), at), 1.5 + s, 1.5 + s);
        }
        return;
    }
    case Motion::Create: {
        // Something new: a four-pointed star born out beyond the body and
        // growing, tied back to it by a line.
        const QPointF at = c + dir * r * (1.6 + 1.3 * easeOut(t));
        painter.setPen(QPen(ink(hue, 0.9, 0.7, fade * 0.5), 1.0, Qt::DotLine));
        painter.drawLine(c + dir * r, at);
        const double size = (4.0 + 5.0 * s) * easeOut(qMin(1.0, t * 2.5));
        QPainterPath star;
        for (int i = 0; i < 8; ++i) {
            const double at2 = a + t * 1.5 + i * M_PI / 4.0;
            const QPointF p = polar(at, i % 2 ? size * 0.3 : size, at2);
            if (i == 0)
                star.moveTo(p);
            else
                star.lineTo(p);
        }
        star.closeSubpath();
        painter.setPen(pen);
        painter.setBrush(ink(hue, 1.0, 0.75, fade * 0.7));
        painter.drawPath(star);
        return;
    }
    case Motion::Test: {
        // Tests: a pendulum swinging fast back and forth outside the body,
        // and a tick at the end of each swing.
        const double swing = std::sin(t * 2.0 * M_PI * 3.0) * 0.55;
        const QPointF pivot = c + dir * r;
        const QPointF bob = polar(pivot, r * (0.9 + 0.5 * s), a + swing);
        painter.setPen(QPen(ink(hue, 0.8, 0.7, fade * 0.6), 1.0));
        painter.drawLine(pivot, bob);
        painter.setPen(Qt::NoPen);
        painter.setBrush(lit);
        painter.drawEllipse(bob, 2.4 + 1.6 * s, 2.4 + 1.6 * s);
        painter.setPen(QPen(lit, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        for (const double side : {-0.55, 0.55}) {
            const QPointF end = polar(pivot, r * 1.7, a + side);
            QPolygonF tick;
            tick << end + QPointF(-2.5, 0) << end + QPointF(-0.5, 2.2) << end + QPointF(3.0, -2.6);
            painter.drawPolyline(tick);
        }
        return;
    }
    case Motion::Build: {
        // A build: bricks stacking up beside the body, each landing with a
        // thud-ring under it.
        const int bricks = 3;
        const double w = r * 0.55;
        const double hgt = r * 0.24;
        const QPointF base = c + dir * r * 1.9;
        for (int k = 0; k < bricks; ++k) {
            const double land = qBound(0.0, t * bricks * 1.6 - k, 1.0);
            if (land <= 0)
                continue;
            const double drop = (1.0 - easeOut(land)) * r * 1.2;
            const QRectF brick(base.x() - w / 2 + (k % 2) * w * 0.25,
                               base.y() - (k + 1) * hgt * 1.15 - drop, w, hgt);
            painter.setPen(QPen(lit, 1.0));
            painter.setBrush(ink(hue, 0.8, 0.5, fade * 0.7));
            painter.drawRect(brick);
        }
        painter.setPen(QPen(ink(hue, 0.9, 0.65, fade * (1.0 - t)), 2.0));
        painter.setBrush(Qt::NoBrush);
        const double ring = r * (1.0 + 0.6 * easeOut(std::fmod(t * bricks, 1.0)));
        painter.drawEllipse(c, ring, ring);
        return;
    }
    case Motion::Git: {
        // History: a line out of the body forking into branches, a commit
        // dot at each tip.
        const double grow = easeOut(qMin(1.0, t * 1.8));
        const QPointF root = c + dir * r;
        const QPointF fork = root + dir * r * 0.9 * grow;
        painter.setPen(pen);
        painter.drawLine(root, fork);
        painter.setBrush(lit);
        const int branches = 2;
        for (int k = 0; k < branches; ++k) {
            const double spread = (k - (branches - 1) / 2.0) * 0.6;
            const QPointF tip = polar(fork, r * 1.1 * grow, a + spread);
            painter.setPen(pen);
            painter.drawLine(fork, tip);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(tip, 2.2 + s, 2.2 + s);
        }
        painter.drawEllipse(fork, 1.8 + s, 1.8 + s);
        return;
    }
    case Motion::Install: {
        // A package arriving: a box dropping in from outside onto the body.
        const double fall = easeInOut(qMin(1.0, t * 1.5));
        const QPointF from = c + dir * r * 3.4;
        const QPointF at = from + (c + dir * r * 1.1 - from) * fall;
        const double size = 3.5 + 3.0 * s;
        painter.save();
        painter.translate(at);
        painter.rotate(a * 180.0 / M_PI + (1.0 - fall) * 120.0);
        painter.setPen(pen);
        painter.setBrush(ink(hue, 0.8, 0.5, fade * 0.5));
        painter.drawRect(QRectF(-size, -size, size * 2, size * 2));
        painter.drawLine(QPointF(-size, 0), QPointF(size, 0));
        painter.restore();
        return;
    }
    case Motion::Spawn: {
        // Sent off to run on its own: a little copy of the body flung out on
        // a spiral, settling into an orbit of its own.
        const double u = easeOut(t);
        const double at = a + u * 2.4;
        const QPointF p = polar(c, r * (1.2 + 2.2 * u), at);
        painter.setPen(QPen(ink(hue, 0.9, 0.7, fade * 0.4), 1.0, Qt::DotLine));
        painter.setBrush(Qt::NoBrush);
        QPainterPath trail(polar(c, r * 1.2, a));
        for (int k = 1; k <= 12; ++k) {
            const double v = u * k / 12.0;
            trail.lineTo(polar(c, r * (1.2 + 2.2 * v), a + v * 2.4));
        }
        painter.drawPath(trail);
        painter.setPen(pen);
        painter.setBrush(ink(hue, 0.9, 0.6, fade * 0.6));
        painter.drawEllipse(p, r * 0.28, r * 0.28);
        return;
    }
    case Motion::Wait: {
        // Waiting: a dashed ring slowly filling round the body, like a timer.
        const double radius = r * 1.55;
        const QRectF box(c.x() - radius, c.y() - radius, radius * 2, radius * 2);
        QPen ring(ink(hue, 0.7, 0.6, fade * 0.35), 1.0, Qt::DotLine);
        painter.setPen(ring);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(c, radius, radius);
        QPen fill = pen;
        fill.setWidthF(width * 1.4);
        painter.setPen(fill);
        painter.drawArc(box, 90 * 16, int(-360.0 * easeInOut(t) * 16));
        return;
    }
    case Motion::Ask: {
        // A question to you: a speech bubble rising off the body with a "?".
        const QPointF at = c + QPointF(r * 0.6, -r * (1.6 + 1.2 * easeOut(t)));
        const double size = 6.0 + 4.0 * s;
        painter.save();
        painter.setPen(pen);
        painter.setBrush(ink(hue, 0.8, 0.5, fade * 0.35));
        painter.drawRoundedRect(QRectF(at.x() - size, at.y() - size * 0.8, size * 2, size * 1.6),
                                size * 0.5, size * 0.5);
        QFont font = painter.font();
        font.setPixelSize(int(size * 1.4));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(lit);
        painter.drawText(QRectF(at.x() - size, at.y() - size * 0.8, size * 2, size * 1.6),
                         Qt::AlignCenter, QStringLiteral("?"));
        painter.restore();
        return;
    }
    case Motion::Skill: {
        // A skill: a five-pointed star wheeling out and back.
        const double out = r * (1.3 + 1.4 * std::sin(M_PI * t));
        const QPointF at = c + dir * out;
        const double size = 4.0 + 4.0 * s;
        QPainterPath star;
        for (int i = 0; i < 10; ++i) {
            const QPointF p = polar(at, i % 2 ? size * 0.42 : size, -M_PI / 2 + t * 5.0 + i * M_PI / 5);
            if (i == 0)
                star.moveTo(p);
            else
                star.lineTo(p);
        }
        star.closeSubpath();
        painter.setPen(pen);
        painter.setBrush(ink(hue, 1.0, 0.7, fade * 0.6));
        painter.drawPath(star);
        return;
    }
    case Motion::Mcp: {
        // A remote tool: a cable run out to a socket, a square node at its
        // end that lights when the call connects.
        const QPointF from = c + dir * r;
        const QPointF to = c + dir * r * 3.0 + normal * r * 0.6;
        const QPointF control = c + dir * r * 2.0 - normal * r * 0.8;
        const double u = easeOut(qMin(1.0, t * 1.8));
        QPainterPath cable(from);
        for (int k = 1; k <= 12; ++k)
            cable.lineTo(onQuad(from, control, to, u * k / 12.0));
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(cable);
        const QPointF end = onQuad(from, control, to, u);
        const double size = 3.0 + 2.5 * s;
        painter.setBrush(u >= 1.0 ? lit : ink(hue, 0.8, 0.45, fade * 0.5));
        painter.drawRect(QRectF(end.x() - size, end.y() - size, size * 2, size * 2));
        return;
    }
    case Motion::Todo: {
        // The list updated: short bars beside the body, each struck through
        // in turn.
        const QPointF origin = polar(c, r * 1.6, a) + QPointF(0, -r * 0.4);
        const int rows = 3;
        const double row = 3.5 + 1.5 * s;
        for (int k = 0; k < rows; ++k) {
            const double appear = qBound(0.0, t * rows * 1.4 - k * 0.8, 1.0);
            if (appear <= 0)
                continue;
            const QPointF at = origin + QPointF(0, k * row * 1.6);
            painter.setPen(Qt::NoPen);
            painter.setBrush(ink(hue, 0.9, 0.7, fade * appear));
            painter.drawEllipse(at, 1.4, 1.4);
            painter.setPen(QPen(ink(hue, 0.8, 0.65, fade * appear), 1.0));
            painter.drawLine(at + QPointF(row, 0), at + QPointF(row * 4, 0));
            if (appear >= 1.0 && k < rows - 1) {
                painter.setPen(QPen(lit, 1.3));
                painter.drawLine(at + QPointF(row * 0.6, -row * 0.35),
                                 at + QPointF(row * 4.4, row * 0.35));
            }
        }
        return;
    }
    case Motion::Fumble: {
        // An error: a ring that cracks into shaking shards.
        const double shake = std::sin(t * 60.0) * 2.0 * (1.0 - t);
        const double radius = r * (1.2 + 0.9 * easeOut(t));
        const int shards = 5;
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (int k = 0; k < shards; ++k) {
            const double start = k * 360.0 / shards + t * 25.0 * (k % 2 ? 1 : -1);
            const QPointF centre = c + QPointF(shake, 0)
                                   + polar(QPointF(0, 0), r * 0.3 * t, (k + 0.5) * 2 * M_PI / shards);
            const QRectF box(centre.x() - radius, centre.y() - radius, radius * 2, radius * 2);
            painter.drawArc(box, int(-start * 16), int(-(360.0 / shards - 12) * 16));
        }
        return;
    }
    case Motion::Denied: {
        // Refused: a no-entry sign flaring over the body.
        const double radius = r * (1.15 + 0.8 * easeOut(t));
        QPen sign = pen;
        sign.setWidthF(width * 1.6);
        painter.setPen(sign);
        painter.setBrush(ink(hue, 0.9, 0.45, fade * 0.15));
        painter.drawEllipse(c, radius, radius);
        painter.drawLine(polar(c, radius * 0.72, -M_PI * 0.75), polar(c, radius * 0.72, M_PI * 0.25));
        return;
    }
    case Motion::Image: {
        // A picture coming back: a framed landscape falling in to the body.
        const double fall = easeInOut(t);
        const QPointF at = c + dir * r * (3.0 - 1.8 * fall);
        const double w = 7.0 + 5.0 * s;
        const QRectF frame(at.x() - w, at.y() - w * 0.7, w * 2, w * 1.4);
        painter.setPen(pen);
        painter.setBrush(ink(hue, 0.6, 0.3, fade * 0.6));
        painter.drawRect(frame);
        painter.setPen(Qt::NoPen);
        painter.setBrush(lit);
        QPolygonF hills;
        hills << frame.bottomLeft() << QPointF(frame.left() + w * 0.7, frame.top() + w * 0.6)
              << QPointF(frame.left() + w * 1.1, frame.bottom() - w * 0.4)
              << QPointF(frame.left() + w * 1.5, frame.top() + w * 0.8) << frame.bottomRight();
        painter.drawPolygon(hills);
        painter.drawEllipse(QPointF(frame.right() - w * 0.4, frame.top() + w * 0.35), 1.5, 1.5);
        return;
    }
    case Motion::Compact: {
        // Folding down: rings closing in on the body from far out.
        painter.setBrush(Qt::NoBrush);
        for (int k = 0; k < 3; ++k) {
            const double tk = qBound(0.0, t * 1.4 - k * 0.2, 1.0);
            if (tk <= 0 || tk >= 1)
                continue;
            QPen ring = pen;
            ring.setColor(ink(hue, 0.9, 0.7, fade * (0.3 + 0.7 * tk)));
            painter.setPen(ring);
            const double radius = r * (4.0 - 3.0 * easeInOut(tk));
            painter.drawEllipse(c, radius, radius * (1.0 - 0.3 * tk));
        }
        return;
    }
    case Motion::Retry: {
        // Again: an arrow chasing round the body.
        const double radius = r * 1.5;
        const double start = a * 180.0 / M_PI + t * 540.0;
        const QRectF box(c.x() - radius, c.y() - radius, radius * 2, radius * 2);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(box, int(-start * 16), int(-220 * 16));
        const double tipAngle = (start + 220.0) * M_PI / 180.0;
        const QPointF tip = polar(c, radius, tipAngle);
        const QPointF back = polar(c, radius, tipAngle - 0.3);
        const QPointF side = (tip - back) * 0.8;
        painter.drawLine(tip, back + QPointF(-side.y(), side.x()));
        painter.drawLine(tip, back - QPointF(-side.y(), side.x()));
        return;
    }
    case Motion::Fail: {
        // A failed turn: a cross struck through the body, and sparks
        // raining down off it.
        const double grow = easeOut(qMin(1.0, t * 2.5));
        const double arm = r * (1.0 + 0.8 * grow);
        QPen cross = pen;
        cross.setWidthF(width * 1.8);
        painter.setPen(cross);
        painter.drawLine(c + QPointF(-arm, -arm) * 0.7, c + QPointF(arm, arm) * 0.7);
        painter.drawLine(c + QPointF(arm, -arm) * 0.7, c + QPointF(-arm, arm) * 0.7);
        painter.setPen(Qt::NoPen);
        const int drops = 8;
        for (int i = 0; i < drops; ++i) {
            const double x = (double(i) / (drops - 1) - 0.5) * r * 3.0;
            const double y = r + t * t * r * (2.5 + scatter(quint32(i * 7)) * 2.0);
            painter.setBrush(ink(hue, 1.0, 0.65, fade));
            painter.drawEllipse(c + QPointF(x, y), 1.4 + s, 1.4 + s);
        }
        return;
    }
    case Motion::Close: {
        // A block closing: two brackets snapping shut onto a bar.
        const QPointF at = polar(c, r * 1.35, a);
        const double half = (3.0 + 2.0 * s) * (1.0 - 0.3 * t);
        const double gap = half * (0.4 + 1.0 * (1.0 - easeOut(t)));
        painter.save();
        painter.translate(at);
        painter.rotate(a * 180.0 / M_PI);
        painter.setPen(QPen(lit, 1.1, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
        painter.setBrush(Qt::NoBrush);
        for (const double side : {-1.0, 1.0}) {
            QPolygonF bracket;
            bracket << QPointF(side * (gap + half * 0.4), -half) << QPointF(side * gap, -half)
                    << QPointF(side * gap, half) << QPointF(side * (gap + half * 0.4), half);
            painter.drawPolyline(bracket);
        }
        painter.drawLine(QPointF(0, -half * 0.7), QPointF(0, half * 0.7));
        painter.restore();
        return;
    }
    case Motion::Seal: {
        // A seal pressed just off the body, a glint crossing it.
        const QPointF at = polar(c, r * 1.45, a);
        const double size = (2.4 + 1.8 * s) * (1.0 + 0.3 * easeOut(t));
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink(hue, 0.9, 0.55, fade));
        painter.drawEllipse(at, size, size);
        painter.setPen(QPen(ink(hue, 0.4, 0.95, fade), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(at, size * 1.5, size * 1.5);
        const double glint = -size + 2.0 * size * t;
        painter.drawLine(at + QPointF(glint - 1.5, -size), at + QPointF(glint + 1.5, size));
        return;
    }
    case Motion::Connect:
    case Motion::Task:
    case Motion::LogError:
    case Motion::LogStall:
    case Motion::LogSync:
    case Motion::LogGit:
    case Motion::LogCloud:
    case Motion::LogTask:
    case Motion::LogAccount:
    case Motion::LogInfo:
    case Motion::Visitor:
    case Motion::Client:
    case Motion::Socket: {
        // Its glyph thrown off the body - outwards, or falling in for the
        // kinds that report something coming back - and a thin ring behind
        // it, so a log line reads as a line in the log and not an agent's work.
        const double travel = style.inbound ? 1.0 - 0.6 * easeOut(t) : easeOut(t);
        const QPointF at = polar(c, r * (1.3 + 0.8 * travel), a);
        const double size = (3.2 + 2.4 * s) * (1.0 + 0.25 * easeOut(t));
        paintCueGlyph(painter, motion, at, size, lit, t);
        painter.setPen(QPen(line, qMax(0.8, width * 0.6)));
        painter.setBrush(Qt::NoBrush);
        const double ring = r * (1.0 + (style.reach - 1.0) * easeOut(t));
        painter.drawEllipse(c, ring, ring);
        return;
    }
    case Motion::Hook: {
        // A hook swinging off the body on its line.
        const double swing = std::sin(t * 2.0 * M_PI * 1.5) * 0.35 * (1.0 - t);
        const QPointF pivot = polar(c, r * 1.1, a);
        const QPointF end = polar(pivot, r * 0.9, a + swing);
        painter.setPen(QPen(lit, 1.2, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(pivot, end);
        const double size = 2.5 + 1.5 * s;
        const QRectF box(end.x() - size, end.y() - size, size * 2, size * 2);
        painter.drawArc(box, int((-(a + swing) * 180.0 / M_PI + 90) * 16), 200 * 16);
        return;
    }
    }
}

void AgentActivityView::paintIcon(QPainter &painter, Motion motion, const QPointF &c,
                                  double size, double alpha, double tint) const
{
    const double hue = tint < 0 ? styleFor(motion).hue : tint;
    const QColor colour = ink(hue, 0.92, 0.68, alpha);
    const QPen pen(colour, qMax(1.0, size * 0.28), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.save();
    switch (motion) {
    case Motion::Rest:
        break;
    case Motion::Start:
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        for (int i = 0; i < 5; ++i)
            painter.drawEllipse(polar(c, size * 0.62, -M_PI / 2 + i * 2 * M_PI / 5),
                                size * 0.34, size * 0.34);
        break;
    case Motion::Speak: {
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (int k = 0; k < 2; ++k) {
            const double radius = size * (0.55 + 0.55 * k);
            const QRectF box(c.x() - size - radius, c.y() - radius, radius * 2, radius * 2);
            painter.drawArc(box.translated(size * 0.5, 0), -40 * 16, 80 * 16);
        }
        break;
    }
    case Motion::Think:
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        for (int i = 0; i < 3; ++i)
            painter.drawEllipse(polar(c, size * 0.6, -M_PI / 2 + i * 2 * M_PI / 3),
                                size * 0.3, size * 0.3);
        break;
    case Motion::Read:
        painter.setPen(pen);
        painter.setBrush(ink(hue, 0.9, 0.55, alpha * 0.4));
        painter.drawRect(QRectF(c.x() - size * 0.6, c.y() - size * 0.8, size * 1.2, size * 1.6));
        break;
    case Motion::Search:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(c + QPointF(-size * 0.2, -size * 0.2), size * 0.6, size * 0.6);
        painter.drawLine(c + QPointF(size * 0.25, size * 0.25), c + QPointF(size * 0.9, size * 0.9));
        break;
    case Motion::Web:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(c, size * 0.85, size * 0.85);
        painter.drawEllipse(c, size * 0.38, size * 0.85);
        painter.drawLine(c + QPointF(-size * 0.85, 0), c + QPointF(size * 0.85, 0));
        break;
    case Motion::Plan: {
        QPen thin = pen;
        thin.setWidthF(qMax(1.0, size * 0.2));
        painter.setPen(thin);
        painter.setBrush(Qt::NoBrush);
        for (int k = -1; k <= 1; ++k) {
            const double y = c.y() + k * size * 0.62;
            painter.drawLine(QPointF(c.x() - size * 0.8, y), QPointF(c.x() - size * 0.45, y + size * 0.2));
            painter.drawLine(QPointF(c.x() - size * 0.45, y + size * 0.2), QPointF(c.x() - size * 0.1, y - size * 0.25));
            painter.drawLine(QPointF(c.x() + size * 0.15, y), QPointF(c.x() + size * 0.9, y));
        }
        break;
    }
    case Motion::Write: {
        painter.setPen(pen);
        painter.setBrush(ink(hue, 0.9, 0.55, alpha * 0.4));
        QPolygonF diamond;
        diamond << c + QPointF(0, -size) << c + QPointF(size * 0.8, 0) << c + QPointF(0, size)
                << c + QPointF(-size * 0.8, 0);
        painter.drawPolygon(diamond);
        break;
    }
    case Motion::Run: {
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        QPolygonF bolt;
        bolt << c + QPointF(size * 0.3, -size) << c + QPointF(-size * 0.35, size * 0.1)
             << c + QPointF(size * 0.35, -size * 0.1) << c + QPointF(-size * 0.3, size);
        painter.drawPolyline(bolt);
        break;
    }
    case Motion::Delegate:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(c + QPointF(-size * 0.35, size * 0.2), size * 0.5, size * 0.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(c + QPointF(size * 0.6, -size * 0.55), size * 0.3, size * 0.3);
        break;
    case Motion::Arrive:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (int i = 0; i < 4; ++i) {
            const double at = M_PI / 4 + i * M_PI / 2;
            painter.drawLine(polar(c, size, at), polar(c, size * 0.45, at));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(c, size * 0.25, size * 0.25);
        break;
    case Motion::Prompt:
        painter.setPen(pen);
        painter.drawLine(c + QPointF(-size * 0.7, -size), c + QPointF(size * 0.1, size * 0.2));
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(c + QPointF(size * 0.2, size * 0.35), size * 0.45, size * 0.45);
        break;
    case Motion::Notice: {
        painter.setPen(pen);
        painter.setBrush(ink(hue, 0.9, 0.5, alpha * 0.3));
        QPolygonF triangle;
        for (int i = 0; i < 3; ++i)
            triangle << polar(c, size, -M_PI / 2 + i * 2 * M_PI / 3);
        painter.drawPolygon(triangle);
        break;
    }
    case Motion::Finish:
        painter.setPen(pen);
        for (int i = 0; i < 8; ++i) {
            const double at = i * M_PI / 4;
            painter.drawLine(polar(c, size * 0.35, at), polar(c, size, at));
        }
        break;
    case Motion::Frame:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (const double side : {-1.0, 1.0}) {
            QPolygonF bracket;
            bracket << c + QPointF(side * size * 0.3, -size * 0.8)
                    << c + QPointF(side * size * 0.7, -size * 0.8)
                    << c + QPointF(side * size * 0.7, size * 0.8)
                    << c + QPointF(side * size * 0.3, size * 0.8);
            painter.drawPolyline(bracket);
        }
        break;
    case Motion::Compose:
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        for (int k = -1; k <= 1; ++k)
            painter.drawEllipse(c + QPointF(k * size * 0.6, 0), size * 0.22, size * 0.22);
        break;
    case Motion::Usage:
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        for (int k = 0; k < 3; ++k) {
            const double height = size * (0.6 + 0.5 * k);
            painter.drawRect(QRectF(c.x() - size * 0.8 + k * size * 0.6,
                                    c.y() + size * 0.8 - height, size * 0.4, height));
        }
        break;
    case Motion::System:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(c, size * 0.45, size * 0.45);
        for (int i = 0; i < 6; ++i) {
            const double at = i * M_PI / 3.0;
            painter.drawLine(polar(c, size * 0.5, at), polar(c, size * 0.9, at));
        }
        break;
    case Motion::Tick:
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(c, size * 0.3, size * 0.3);
        break;
    case Motion::Network:
        painter.setPen(pen);
        painter.drawLine(c + QPointF(-size * 0.4, -size * 0.7), c + QPointF(-size * 0.4, size * 0.7));
        painter.drawLine(c + QPointF(-size * 0.75, -size * 0.3), c + QPointF(-size * 0.4, -size * 0.7));
        painter.drawLine(c + QPointF(size * 0.4, -size * 0.7), c + QPointF(size * 0.4, size * 0.7));
        painter.drawLine(c + QPointF(size * 0.75, size * 0.3), c + QPointF(size * 0.4, size * 0.7));
        break;
    case Motion::Scan:
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        for (int k = -2; k <= 2; ++k)
            painter.drawEllipse(polar(c + QPointF(0, size * 0.6), size, -M_PI / 2 + k * 0.4),
                                size * 0.18, size * 0.18);
        break;
    case Motion::Create:
    case Motion::Skill: {
        const int points = motion == Motion::Create ? 4 : 5;
        QPainterPath star;
        for (int i = 0; i < points * 2; ++i) {
            const QPointF p = polar(c, i % 2 ? size * 0.4 : size, -M_PI / 2 + i * M_PI / points);
            if (i == 0)
                star.moveTo(p);
            else
                star.lineTo(p);
        }
        star.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawPath(star);
        break;
    }
    case Motion::Test: {
        painter.setPen(pen);
        QPolygonF tick;
        tick << c + QPointF(-size * 0.7, 0) << c + QPointF(-size * 0.2, size * 0.5)
             << c + QPointF(size * 0.8, -size * 0.6);
        painter.drawPolyline(tick);
        break;
    }
    case Motion::Build:
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        for (int k = 0; k < 3; ++k)
            painter.drawRect(QRectF(c.x() - size * 0.8 + (k % 2) * size * 0.3,
                                    c.y() + size * 0.5 - k * size * 0.55, size * 1.3, size * 0.42));
        break;
    case Motion::Git:
        painter.setPen(pen);
        painter.drawLine(c + QPointF(-size * 0.4, size), c + QPointF(-size * 0.4, -size));
        painter.drawLine(c + QPointF(-size * 0.4, size * 0.2), c + QPointF(size * 0.5, -size * 0.5));
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(c + QPointF(size * 0.5, -size * 0.5), size * 0.28, size * 0.28);
        painter.drawEllipse(c + QPointF(-size * 0.4, -size), size * 0.28, size * 0.28);
        break;
    case Motion::Install:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(c.x() - size * 0.7, c.y() - size * 0.3, size * 1.4, size * 1.1));
        painter.drawLine(c + QPointF(0, -size), c + QPointF(0, size * 0.3));
        break;
    case Motion::Spawn:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(c + QPointF(-size * 0.3, size * 0.3), size * 0.45, size * 0.45);
        painter.drawArc(QRectF(c.x() - size, c.y() - size, size * 2, size * 2), 10 * 16, 80 * 16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(c + QPointF(size * 0.7, -size * 0.7), size * 0.25, size * 0.25);
        break;
    case Motion::Wait:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(c, size * 0.85, size * 0.85);
        painter.drawLine(c, c + QPointF(0, -size * 0.6));
        painter.drawLine(c, c + QPointF(size * 0.45, 0));
        break;
    case Motion::Ask: {
        QFont font = painter.font();
        font.setPixelSize(int(size * 2.2));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(colour);
        painter.drawText(QRectF(c.x() - size, c.y() - size * 1.2, size * 2, size * 2.4),
                         Qt::AlignCenter, QStringLiteral("?"));
        break;
    }
    case Motion::Mcp:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(c.x() - size * 0.3, c.y() - size * 0.5, size * 1.1, size));
        painter.drawLine(c + QPointF(-size * 0.3, -size * 0.25), c + QPointF(-size, -size * 0.25));
        painter.drawLine(c + QPointF(-size * 0.3, size * 0.25), c + QPointF(-size, size * 0.25));
        break;
    case Motion::Todo:
        painter.setPen(pen);
        for (int k = -1; k <= 1; ++k) {
            const double y = c.y() + k * size * 0.62;
            painter.drawLine(QPointF(c.x() - size * 0.4, y), QPointF(c.x() + size * 0.9, y));
            painter.drawPoint(QPointF(c.x() - size * 0.85, y));
        }
        break;
    case Motion::Fumble:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(c.x() - size * 0.8, c.y() - size * 0.8, size * 1.6, size * 1.6),
                        20 * 16, 140 * 16);
        painter.drawArc(QRectF(c.x() - size * 0.7, c.y() - size * 0.7, size * 1.6, size * 1.6),
                        200 * 16, 130 * 16);
        break;
    case Motion::Denied:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(c, size * 0.85, size * 0.85);
        painter.drawLine(polar(c, size * 0.6, -M_PI * 0.75), polar(c, size * 0.6, M_PI * 0.25));
        break;
    case Motion::Image: {
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const QRectF frame(c.x() - size * 0.9, c.y() - size * 0.7, size * 1.8, size * 1.4);
        painter.drawRect(frame);
        QPolygonF hill;
        hill << frame.bottomLeft() << QPointF(c.x() - size * 0.2, c.y() - size * 0.1)
             << frame.bottomRight();
        painter.drawPolyline(hill);
        break;
    }
    case Motion::Compact:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(c + QPointF(-size, -size * 0.6), c + QPointF(-size * 0.2, 0));
        painter.drawLine(c + QPointF(-size, size * 0.6), c + QPointF(-size * 0.2, 0));
        painter.drawLine(c + QPointF(size, -size * 0.6), c + QPointF(size * 0.2, 0));
        painter.drawLine(c + QPointF(size, size * 0.6), c + QPointF(size * 0.2, 0));
        break;
    case Motion::Retry:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(c.x() - size * 0.8, c.y() - size * 0.8, size * 1.6, size * 1.6),
                        60 * 16, 270 * 16);
        painter.drawLine(polar(c, size * 0.8, -M_PI / 3), polar(c, size * 0.8, -M_PI / 3) + QPointF(size * 0.4, 0));
        break;
    case Motion::Fail:
        painter.setPen(pen);
        painter.drawLine(c + QPointF(-size * 0.7, -size * 0.7), c + QPointF(size * 0.7, size * 0.7));
        painter.drawLine(c + QPointF(size * 0.7, -size * 0.7), c + QPointF(-size * 0.7, size * 0.7));
        break;
    case Motion::Close:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (const double side : {-1.0, 1.0}) {
            QPolygonF bracket;
            bracket << c + QPointF(side * size * 0.8, -size * 0.8)
                    << c + QPointF(side * size * 0.4, -size * 0.8)
                    << c + QPointF(side * size * 0.4, size * 0.8)
                    << c + QPointF(side * size * 0.8, size * 0.8);
            painter.drawPolyline(bracket);
        }
        painter.drawLine(c + QPointF(0, -size * 0.5), c + QPointF(0, size * 0.5));
        break;
    case Motion::Seal:
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(c, size * 0.55, size * 0.55);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(c, size * 0.9, size * 0.9);
        break;
    case Motion::Connect:
    case Motion::Task:
    case Motion::LogError:
    case Motion::LogStall:
    case Motion::LogSync:
    case Motion::LogGit:
    case Motion::LogCloud:
    case Motion::LogTask:
    case Motion::LogAccount:
    case Motion::LogInfo:
    case Motion::Visitor:
    case Motion::Client:
    case Motion::Socket:
        paintCueGlyph(painter, motion, c, size, colour, 0.0);
        break;
    case Motion::Hook:
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(c + QPointF(size * 0.3, -size), c + QPointF(size * 0.3, size * 0.2));
        painter.drawArc(QRectF(c.x() - size * 0.5, c.y() - size * 0.2, size * 0.8, size * 0.9),
                        180 * 16, 180 * 16);
        break;
    }
    painter.restore();
}

AgentActivityView::HostPlace AgentActivityView::hostPlace(const QString &name, bool mirror)
{
    if (mirror)
        return HostPlace::Mirror;
    const QString host = name.toLower();
    if (host == QLatin1String("this machine") || host == QLatin1String("localhost"))
        return HostPlace::Machine;
    const auto any = [&host](std::initializer_list<const char *> parts) {
        return std::any_of(parts.begin(), parts.end(), [&host](const char *part) {
            return host.contains(QLatin1String(part));
        });
    };
    if (any({"forkmesh"}))
        return HostPlace::Mesh;
    if (any({"anthropic", "claude.ai", "claude.com", "openai", "chatgpt", "generativelanguage",
             "gemini", "mistral.ai", "groq", "openrouter", "cohere"}))
        return HostPlace::Ai;
    if (any({"github", "gitlab", "bitbucket", "codeberg", "sr.ht"}))
        return HostPlace::Git;
    if (any({"npmjs", "yarnpkg", "pypi", "pythonhosted", "crates.io", "rubygems", "maven",
             "nuget", "packagist", "proxy.golang", "pub.dev", "debian.org", "ubuntu.com",
             "fedoraproject", "archlinux", "flathub", "cocoapods"}))
        return HostPlace::Package;
    if (any({"cloudflare", "workers.dev", "pages.dev", "r2.dev", "amazonaws", "cloudfront",
             "googleapis", "googleusercontent", "gstatic", "azure", "windows.net", "vultr",
             "digitalocean", "fastly", "akamai", "vercel", "netlify", "fly.dev", "herokuapp"}))
        return HostPlace::Cloud;
    return HostPlace::Web;
}

QString AgentActivityView::hostPlaceLabel(HostPlace place)
{
    switch (place) {
    case HostPlace::Machine:
        return QStringLiteral("a screen: this machine");
    case HostPlace::Mirror:
        return QStringLiteral("a server: one of your mirror nodes");
    case HostPlace::Mesh:
        return QStringLiteral("a mesh: ForkMesh");
    case HostPlace::Ai:
        return QStringLiteral("a sparkle: an AI service");
    case HostPlace::Git:
        return QStringLiteral("a branch: a git host");
    case HostPlace::Package:
        return QStringLiteral("a box: a package registry");
    case HostPlace::Cloud:
        return QStringLiteral("a cloud: a cloud or CDN provider");
    case HostPlace::Web:
        break;
    }
    return QStringLiteral("a globe: somewhere on the web");
}

void AgentActivityView::paintPlace(QPainter &painter, HostPlace place, const QPointF &c,
                                   double size, double hue, double alpha) const
{
    const QColor colour = ink(hue, 0.6, 0.78, alpha);
    QColor fill = colour;
    fill.setAlphaF(fill.alphaF() * 0.3f);
    const QPen pen(colour, qMax(1.0, size * 0.2), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.save();
    switch (place) {
    case HostPlace::Machine:
        painter.setPen(pen);
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(c.x() - size, c.y() - size * 0.8, size * 2, size * 1.25),
                                size * 0.18, size * 0.18);
        painter.drawLine(c + QPointF(0, size * 0.45), c + QPointF(0, size * 0.8));
        painter.drawLine(c + QPointF(-size * 0.5, size * 0.8), c + QPointF(size * 0.5, size * 0.8));
        break;
    case HostPlace::Mirror:
        painter.setPen(pen);
        painter.setBrush(fill);
        for (const double y : {-size * 0.85, size * 0.1})
            painter.drawRoundedRect(QRectF(c.x() - size * 0.9, c.y() + y, size * 1.8, size * 0.75),
                                    size * 0.15, size * 0.15);
        for (const double y : {-size * 0.475, size * 0.475})
            painter.drawLine(c + QPointF(-size * 0.6, y), c + QPointF(size * 0.1, y));
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        for (const double y : {-size * 0.475, size * 0.475})
            painter.drawEllipse(c + QPointF(size * 0.5, y), size * 0.13, size * 0.13);
        break;
    case HostPlace::Mesh: {
        const QPointF nodes[] = {polar(c, size * 0.75, -M_PI / 2),
                                 polar(c, size * 0.75, M_PI / 6),
                                 polar(c, size * 0.75, 5 * M_PI / 6)};
        painter.setPen(pen);
        painter.drawLine(nodes[0], nodes[1]);
        painter.drawLine(nodes[1], nodes[2]);
        painter.drawLine(nodes[2], nodes[0]);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        for (const QPointF &node : nodes)
            painter.drawEllipse(node, size * 0.26, size * 0.26);
        break;
    }
    case HostPlace::Ai: {
        const auto sparkle = [&painter](const QPointF &at, double outer, double inner) {
            QPainterPath path;
            for (int i = 0; i < 8; ++i) {
                const QPointF p = polar(at, i % 2 ? inner : outer, -M_PI / 2 + i * M_PI / 4);
                if (i == 0)
                    path.moveTo(p);
                else
                    path.lineTo(p);
            }
            path.closeSubpath();
            painter.drawPath(path);
        };
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        sparkle(c + QPointF(-size * 0.15, size * 0.1), size * 0.85, size * 0.22);
        sparkle(c + QPointF(size * 0.6, -size * 0.6), size * 0.35, size * 0.1);
        break;
    }
    case HostPlace::Cloud:
        paintCueGlyph(painter, Motion::LogCloud, c, size, colour, 0.0);
        break;
    case HostPlace::Git:
        paintIcon(painter, Motion::Git, c, size * 0.85, alpha, hue);
        break;
    case HostPlace::Package:
        paintIcon(painter, Motion::Install, c, size * 0.85, alpha, hue);
        break;
    case HostPlace::Web:
        paintIcon(painter, Motion::Web, c, size * 0.9, alpha, hue);
        break;
    }
    painter.restore();
}

const QPixmap &AgentActivityView::markIcon(Motion motion, double size, qreal ratio) const
{
    const double step = qMax(1.0, qRound(size * 2.0) / 2.0);
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(int(motion))
                            .arg(step)
                            .arg(ratio)
                            .arg(m_dark ? 1 : 0);
    auto it = m_markIcons.find(key);
    if (it != m_markIcons.end())
        return *it;
    const double side = std::ceil(step * 3.0);
    QPixmap art(QSize(qCeil(side * ratio), qCeil(side * ratio)));
    art.setDevicePixelRatio(ratio);
    art.fill(Qt::transparent);
    QPainter p(&art);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    paintIcon(p, motion, QPointF(side / 2.0, side / 2.0), step, 1.0);
    p.end();
    if (m_markIcons.size() > 600)
        m_markIcons.clear();
    return *m_markIcons.insert(key, art);
}

void AgentActivityView::paintMark(QPainter &painter, Motion motion, const QPointF &centre,
                                  double size, double alpha) const
{
    if (alpha <= 0.01)
        return;
    const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const QPixmap &art = markIcon(motion, size, ratio);
    const QSizeF extent = art.deviceIndependentSize();
    const double was = painter.opacity();
    painter.setOpacity(was * qMin(1.0, alpha));
    painter.drawPixmap(QRectF(centre - QPointF(extent.width() / 2.0, extent.height() / 2.0),
                              extent),
                       art, QRectF(art.rect()));
    painter.setOpacity(was);
}

void AgentActivityView::paintPulses(QPainter &painter)
{
    const double now = m_scene->now();
    for (const Pulse &pulse : m_scene->pulses()) {
        if (pulse.pending(now) || !motionShown(pulse.motion))
            continue; // its note has not sounded yet, or its kind is hidden
        const bool desktop = pulse.sessionId == Scene::kDesktop;
        // With the network layer off there is no seat to burst off: the
        // mark lands on the desktop's node, and is drawn as the desktop's.
        const Host *mirror = m_network ? m_scene->mirrorForSource(pulse.sessionId) : nullptr;
        const Body *body = desktop || mirror ? nullptr : m_scene->body(pulse.sessionId);
        if (!desktop && !mirror && !body)
            continue;
        const double shown = body ? presence(pulse.sessionId) : 1.0;
        if (shown <= 0.01)
            continue;
        painter.setOpacity(shown);
        // The desktop's marks burst off its node at the foot of the field,
        // all the way round and drawn wider than an agent's, so they read
        // clear of the rows of bodies above it. A mirror's burst off its
        // seat, smaller, so a busy fleet does not swamp the rows below.
        const QPointF centre = body ? pointFor(*body) : flowEnd(pulse.sessionId);
        const double radius = body ? radiusFor(*body)
                                   : (mirror ? 11.0 : 16.0) * qMax(0.8, m_unit / 520.0);
        const Pulse &drawn = pulse;
        paintFigure(painter, pulse.motion, centre, radius, pulse.life(now), drawn.angle,
                    pulse.strength);
    }
    painter.setOpacity(1.0);
}

void AgentActivityView::paintHistoryRing(QPainter &painter, const Body &body,
                                         const QPointF &centre, double radius)
{
    // The agent's own log, round it like a clock: the newest mark at the top,
    // the older ones following round and fading.
    if (!m_rings)
        return;
    const QVector<Mark> &history = m_scene->history();
    const double now = m_scene->now();
    const double ring = radius + 10.0;
    const double icon = qBound(2.6, 2.2 + radius * 0.06, 4.0);
    const int marks = qBound(6, int(2.0 * M_PI * ring / (icon * 3.0)), kRingMarks);
    int drawn = 0;
    for (int i = history.size() - 1; i >= 0 && drawn < marks; --i) {
        const Mark &mark = history.at(i);
        if (mark.sessionId != body.sessionId || !motionShown(mark.motion))
            continue;
        const double at = -M_PI / 2 + drawn * (2.0 * M_PI / marks);
        const double age = qBound(0.0, (now - mark.seconds) / Scene::kHistorySeconds, 1.0);
        const double alpha = (1.0 - age) * (1.0 - double(drawn) / marks) * 0.9;
        const double size = forkmesh::sonify::isFaint(mark.motion)
                                ? icon * 0.8
                                : icon * (0.85 + 0.3 * mark.strength);
        paintMark(painter, mark.motion, polar(centre, ring, at), size, alpha);
        ++drawn;
    }
}

void AgentActivityView::paintBodies(QPainter &painter)
{
    const double now = m_scene->now();
    const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const QFont was = painter.font();
    QFont number = was;
    number.setPixelSize(10);
    number.setBold(true);
    QFont title = was;
    title.setPixelSize(kLabelPixels);
    const QFontMetricsF titleMetrics(title);
    QFont projectFont = title;
    projectFont.setBold(true);
    projectFont.setLetterSpacing(QFont::PercentageSpacing, 110);
    const QFontMetricsF projectMetrics(projectFont);
    for (const Body &body : m_scene->bodies()) {
        const double shown = presence(body.sessionId);
        if (shown <= 0.01)
            continue;
        painter.setOpacity(shown);
        const QPointF centre = pointFor(body);
        const double radius = radiusFor(body);
        const bool hovered = body.sessionId == m_hover;

        // The glow: how busy it is right now, in its own colour.
        const double glowAlpha = 0.10 + 0.50 * body.energy + (hovered ? 0.12 : 0.0);
        const double glowRadius = radius * (2.0 + 1.2 * body.energy);
        painter.setPen(Qt::NoPen);
        drawGlow(painter, centre, glowRadius, ink(body.hue, 0.95, 0.60, glowAlpha));

        // The body itself: lit from one side, like a planet. A run that has
        // finished is dim: still in the field, simply not making anything.
        const bool ended = body.state == State::Done || body.state == State::Failed;
        QRadialGradient core(centre + QPointF(-radius * 0.35, -radius * 0.4), radius * 1.4);
        core.setColorAt(0.0, ink(body.hue, ended ? 0.35 : 0.75,
                                 ended ? 0.45 : 0.70 + 0.15 * body.energy, ended ? 0.8 : 1.0));
        core.setColorAt(1.0, ink(body.hue, ended ? 0.3 : 0.7, ended ? 0.18 : 0.30,
                                 ended ? 0.8 : 1.0));
        painter.setBrush(core);
        painter.setPen(QPen(ink(body.hue, 0.95, 0.76, hovered ? 1.0 : 0.7),
                            hovered ? 2.2 : 1.2));
        painter.drawEllipse(centre, radius, radius);

        // Its face, the one the agent list shows, inside its own outline.
        const QPixmap face = faceArt(body.sessionId, radius, ratio);
        if (!face.isNull()) {
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.setOpacity(shown * (ended ? 0.55 : 1.0));
            const double inner = radius - (hovered ? 1.1 : 0.6);
            painter.drawPixmap(QRectF(centre.x() - inner, centre.y() - inner, inner * 2,
                                      inner * 2),
                               face, QRectF(face.rect()));
            painter.restore();
            painter.setPen(QPen(ink(body.hue, 0.95, 0.76, hovered ? 1.0 : 0.7),
                                hovered ? 2.2 : 1.2));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(centre, radius, radius);
        }

        paintHistoryRing(painter, body, centre, radius);

        // Being heard: a ring that beats with its notes.
        if (body.listening) {
            const double beat = std::exp(-(now - body.lastNoteSeconds) / 0.35);
            QPen pen(ink(body.hue, 1.0, 0.82, 0.45 + 0.5 * beat), 1.2 + 1.4 * beat);
            pen.setStyle(Qt::DotLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const double r = radius + 4 + 3 * beat;
            painter.drawEllipse(centre, r, r);
        }

        // Still running (solid) or waiting on you (dotted): a still ring. It
        // does not turn - only events move the field.
        if (body.state == State::Running || body.state == State::Waiting) {
            QPen pen(ink(body.hue, 1.0, 0.85, 0.55), 2.0);
            pen.setCapStyle(Qt::RoundCap);
            if (body.state == State::Waiting)
                pen.setStyle(Qt::DotLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const double r = radius + 15;
            painter.drawEllipse(centre, r, r);
        }
        if (body.state == State::Failed) {
            painter.setPen(QPen(QColor(248, 81, 73, 220), 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(centre, radius + 3, radius + 3);
        }

        // Its number, the one the agent list gives it, on a tag sitting on
        // top of the body in its own colour.
        const QString tag = QString::number(body.sessionId);
        const QFontMetricsF tagMetrics(number);
        const double tagHeight = tagMetrics.height() + 2;
        const double tagWidth = qMax(tagHeight, tagMetrics.horizontalAdvance(tag) + 8);
        const QRectF tagBox(centre.x() - tagWidth / 2, centre.y() - radius - tagHeight * 0.6,
                            tagWidth, tagHeight);
        painter.setPen(QPen(ink(body.hue, 0.95, 0.76, hovered ? 1.0 : 0.8), 1.0));
        painter.setBrush(m_dark ? QColor(8, 11, 20, 225) : QColor(255, 255, 255, 235));
        painter.drawRoundedRect(tagBox, tagHeight / 2, tagHeight / 2);
        painter.setFont(number);
        painter.setPen(hovered ? m_foreground : ink(body.hue, 0.9, m_dark ? 0.82 : 0.38));
        painter.drawText(tagBox, Qt::AlignCenter, tag);

        // Its names circling it, clear of its rings: the project it works in
        // over the top, in its own colour, and its task under the bottom.
        const QString project = shortTitle(body);
        const QString task = shortTask(body);
        const double inner = radius + kLabelGap;
        // The project has up to half the circle; the task the rest of it,
        // a gap short of the project at either end.
        constexpr double kGap = 0.45;
        double projectSweep = 0;
        const QColor words = hovered ? m_foreground
                                     : (ended ? m_muted
                                              : ink(body.hue, 0.5, m_dark ? 0.80 : 0.32));
        if (!project.isEmpty()) {
            painter.setFont(projectFont);
            painter.setPen(hovered || ended ? words
                                            : ink(body.hue, 0.9, m_dark ? 0.78 : 0.36));
            projectSweep =
                drawRingText(painter, centre, inner, true, project, projectMetrics, M_PI);
        }
        if (!task.isEmpty()) {
            painter.setFont(title);
            painter.setPen(words);
            drawRingText(painter, centre, inner, false, task, titleMetrics,
                         2 * M_PI - projectSweep - 2 * kGap);
        }
    }
    painter.setFont(was);
    painter.setOpacity(1.0);
}

void AgentActivityView::paintSoundList(QPainter &painter)
{
    if (m_listRect.isEmpty())
        return;
    const double now = m_scene->now();
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_dark ? QColor(8, 11, 20, 170) : QColor(255, 255, 255, 200));
    painter.drawRoundedRect(m_listRect, 10, 10);

    // A short window squeezes the rows: the words shrink with them rather
    // than write over the row below.
    QFont name = painter.font();
    name.setPixelSize(qBound(8, int(m_rowHeight) - 3, 12));
    name.setBold(true);
    QFont small = painter.font();
    small.setPixelSize(qBound(7, int(m_rowHeight) - 5, 10));
    const QFontMetricsF nameMetrics(name);
    const QFontMetricsF smallMetrics(small);
    const double ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;

    for (const Motion motion : listedMotions(m_scene)) {
        const MotionStyle &style = styleFor(motion);
        const QRectF row = soundRowRect(motion);
        const double glow = soundGlow(motion);
        // Lit: the row fills with its colour, a bar on its edge, the figure
        // beside it playing, all fading as the note does.
        if (glow > 0.01) {
            QLinearGradient fill(row.topLeft(), row.topRight());
            fill.setColorAt(0.0, ink(style.hue, 0.9, 0.55, 0.55 * glow));
            fill.setColorAt(1.0, ink(style.hue, 0.9, 0.55, 0.08 * glow));
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(row, 6, 6);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink(style.hue, 0.95, 0.66, 0.35 + 0.65 * glow));
        painter.drawRoundedRect(QRectF(row.left(), row.top() + 3, 3, row.height() - 6), 1.5, 1.5);

        const double iconSize = qMin(9.0, row.height() * 0.32) * (1.0 + 0.35 * glow);
        const QPointF iconAt(row.left() + 20, row.top() + qMin(row.height() / 2, 13.0));
        if (glow > 0.05) {
            // The field's own figure for it, small, playing while it is lit.
            const double t = std::fmod((now - qMax(m_scene->lastSounded(motion),
                                                    m_scene->lastLanded(motion)))
                                           / qMax(0.4, style.seconds),
                                       1.0);
            painter.save();
            painter.setClipRect(row);
            paintFigure(painter, motion, iconAt, 5.0, qBound(0.0, t, 1.0), 0.1, glow);
            painter.restore();
        }
        // The rest of the row - its icon, its switches and its words - only
        // changes when the row does: painted once into an image and kept
        // until then, rather than laid out and rasterised again every frame.
        const bool shown = motionShown(motion);
        const bool heard = motionHeard(motion);
        const QString live = m_scene->lastText(motion);
        const double liveAge = now - m_scene->lastTextSeconds(motion);
        const int count = m_scene->landCount(motion);
        const double fresh = std::exp(-qMax(0.0, liveAge) / 2.5);
        const QString key =
            QStringList{QString::number(row.x()), QString::number(row.y()),
                        QString::number(row.width()), QString::number(row.height()),
                        QString::number(ratio), QString::number(m_rowLines),
                        QString::number(m_foreground.rgba()), QString::number(m_muted.rgba()),
                        QString::number(m_dark), QString::number(shown), QString::number(heard),
                        QString::number(qRound(glow * 40)), QString::number(qRound(fresh * 20)),
                        QString::number(count), painter.font().key(), live}
                .join(QLatin1Char('|'));
        std::pair<QString, QImage> &cached = m_soundRows[int(motion)];
        const QRectF area = row.adjusted(-2, -2, 2, 2);
        if (cached.first != key) {
            QImage image((area.size() * ratio).toSize(), QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(ratio);
            image.fill(Qt::transparent);
            QPainter p(&image);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setRenderHint(QPainter::TextAntialiasing, true);
            p.translate(-area.topLeft());
            paintIcon(p, motion, iconAt, iconSize, (0.55 + 0.45 * glow) * (shown ? 1.0 : 0.35));
            paintKindSwitch(p, kindSwitchRect(motion, false), false, shown, style.hue);
            paintKindSwitch(p, kindSwitchRect(motion, true), true, heard, style.hue);

            const double left = row.left() + 38;
            const double switches = kindSwitchRect(motion, false).left();
            const double textWidth = qMax(20.0, switches - left - 6);
            // A kind that is both hidden and silenced is out of the picture: its
            // row stays, dimmed, so it can be switched back on.
            p.setOpacity(shown || heard ? 1.0 : 0.45);
            QColor nameInk = glow > 0.05 ? m_foreground : m_muted;
            if (glow > 0.05)
                nameInk = ink(style.hue, 0.6, m_dark ? 0.92 : 0.55, 0.7 + 0.3 * glow);
            p.setFont(name);
            p.setPen(nameInk);
            const double lineHeight = nameMetrics.height();
            const double top = row.top() + (m_rowLines == 1 ? (row.height() - lineHeight) / 2 : 3);
            const QString label = QString::fromLatin1(style.label);
            p.drawText(QRectF(left, top, textWidth, lineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                       label);

            // Live: the latest thing of this kind, word for word, as it lands -
            // each tool call's one line replacing the last, bright while it is
            // new. On one line it follows the name; with room it has a line of
            // its own in place of the description.
            QColor liveInk = ink(style.hue, 0.55, m_dark ? 0.9 : 0.35, 0.5 + 0.5 * fresh);
            if (m_rowLines == 1 && !live.isEmpty()) {
                const double nameWidth =
                    qMin(textWidth * 0.45, nameMetrics.horizontalAdvance(label) + 8);
                const double room = textWidth - nameWidth;
                if (room > 30) {
                    p.setFont(small);
                    p.setPen(liveInk);
                    p.drawText(QRectF(left + nameWidth, top, room, lineHeight),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               smallMetrics.elidedText(live, Qt::ElideRight, room));
                }
            }
            if (m_rowLines >= 2) {
                p.setFont(small);
                if (count > 0) {
                    // How many have landed, beside the name.
                    const QString tally = QStringLiteral("×%1").arg(count);
                    QColor tallyInk = m_muted;
                    tallyInk.setAlphaF(float(0.6 + 0.4 * glow));
                    p.setPen(tallyInk);
                    p.drawText(QRectF(left, top, textWidth, lineHeight),
                               Qt::AlignRight | Qt::AlignVCenter, tally);
                }
                if (!live.isEmpty()) {
                    p.setPen(liveInk);
                    p.drawText(QRectF(left, top + lineHeight, textWidth, smallMetrics.height()),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               smallMetrics.elidedText(live, Qt::ElideRight, textWidth));
                } else {
                    QColor meaning = m_foreground;
                    meaning.setAlphaF(float(0.55 + 0.4 * glow));
                    p.setPen(meaning);
                    p.drawText(QRectF(left, top + lineHeight, textWidth, smallMetrics.height()),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               smallMetrics.elidedText(QString::fromLatin1(style.description),
                                                       Qt::ElideRight, textWidth));
                }
            }
            if (m_rowLines >= 3) {
                QColor sound = m_muted;
                sound.setAlphaF(float(0.75 + 0.25 * glow));
                p.setPen(sound);
                QFont italic = small;
                italic.setItalic(true);
                p.setFont(italic);
                p.drawText(QRectF(left, top + lineHeight + smallMetrics.height(), textWidth,
                                  smallMetrics.height()),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           smallMetrics.elidedText(QStringLiteral("♪ ")
                                                       + QString::fromLatin1(style.sound),
                                                   Qt::ElideRight, textWidth));
            }
            p.end();
            cached = {key, image};
        }
        painter.drawImage(area.topLeft(), cached.second);
    }
}

void AgentActivityView::paintKindSwitch(QPainter &painter, const QRectF &box, bool sound,
                                        bool on, double hue) const
{
    if (box.isEmpty())
        return;
    painter.save();
    const QColor colour = on ? ink(hue, 0.7, m_dark ? 0.82 : 0.40, 0.95) : m_muted;
    painter.setPen(Qt::NoPen);
    painter.setBrush(on ? ink(hue, 0.8, 0.5, 0.22) : QColor(0, 0, 0, 0));
    painter.drawRoundedRect(box, 3, 3);
    const QPointF c = box.center();
    const double r = box.width() / 2 - 2.5;
    QPen pen(colour, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    if (sound) {
        // A speaker: the box and the cone, and a wave when it is on.
        QPolygonF cone;
        cone << c + QPointF(-r, -r * 0.35) << c + QPointF(-r * 0.4, -r * 0.35)
             << c + QPointF(r * 0.15, -r * 0.85) << c + QPointF(r * 0.15, r * 0.85)
             << c + QPointF(-r * 0.4, r * 0.35) << c + QPointF(-r, r * 0.35);
        painter.drawPolygon(cone);
        if (on) {
            const QRectF wave(c.x() - r * 0.2, c.y() - r * 0.7, r * 1.2, r * 1.4);
            painter.drawArc(wave, -50 * 16, 100 * 16);
        }
    } else {
        // An eye: the lids and the pupil.
        QPainterPath lids;
        lids.moveTo(c + QPointF(-r, 0));
        lids.quadTo(c + QPointF(0, -r * 1.1), c + QPointF(r, 0));
        lids.quadTo(c + QPointF(0, r * 1.1), c + QPointF(-r, 0));
        painter.drawPath(lids);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        painter.drawEllipse(c, r * 0.3, r * 0.3);
        painter.setPen(pen);
    }
    if (!on)
        painter.drawLine(c + QPointF(-r, r), c + QPointF(r, -r));
    painter.restore();
}

void AgentActivityView::paintLogs(QPainter &painter)
{
    if (!m_logs || m_logRect.isEmpty())
        return;
    const double now = m_scene->now();
    QFont font = painter.font();
    font.setPixelSize(11);
    QFont head = font;
    head.setBold(true);
    QFont chip = font;
    chip.setPixelSize(9);
    chip.setBold(true);
    const QFontMetricsF metrics(font);
    const QFontMetricsF chipMetrics(chip);
    const double line = metrics.height() + 5;
    const QRectF box = m_logRect;

    painter.setPen(Qt::NoPen);
    painter.setBrush(m_dark ? QColor(255, 255, 255, 8) : QColor(16, 22, 34, 8));
    painter.drawRoundedRect(box, 8, 8);

    painter.setFont(head);
    painter.setPen(m_muted);
    const QRectF title(box.left() + 12, box.top() + 6, box.width() - 24, line);
    painter.drawText(title, Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Log"));
    if (m_onOpenLogs) {
        painter.setFont(font);
        painter.drawText(title, Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("open the Logs page ›"));
    }

    painter.setFont(font);
    double y = box.top() + 8 + line;
    const QVector<LogLine> &logs = m_scene->logs();
    int drawn = 0;
    for (int i = logs.size() - 1; i >= 0 && y + line <= box.bottom() - 4; --i) {
        const LogLine &entry = logs.at(i);
        if (m_hideOutput && entry.output)
            continue;
        ++drawn;
        const double age = now - entry.seconds;
        const double fade = qBound(0.35, 1.0 - age / 300.0, 1.0);
        QColor accent(entry.accent);
        if (!accent.isValid())
            accent = m_muted;

        // The category as the Logs page badges it, in its own colour.
        const double chipWidth = chipMetrics.horizontalAdvance(entry.badge) + 8;
        const QRectF chipBox(box.left() + 10, y + (line - chipMetrics.height() - 2) / 2,
                             chipWidth, chipMetrics.height() + 2);
        QColor chipFill = accent;
        chipFill.setAlphaF(float(0.22 * fade + (entry.sparking(now) ? 0.25 : 0.0)));
        painter.setPen(Qt::NoPen);
        painter.setBrush(chipFill);
        painter.drawRoundedRect(chipBox, 3, 3);
        QColor chipInk = accent;
        chipInk.setAlphaF(float(0.5 + 0.5 * fade));
        painter.setFont(chip);
        painter.setPen(chipInk);
        painter.drawText(chipBox, Qt::AlignCenter, entry.badge);
        painter.setFont(font);

        double textLeft = chipBox.right() + 6;
        // A line about an agent carries the agent's number in its colour, the
        // way the feed does.
        if (const Body *body = m_scene->body(entry.sessionId)) {
            const QString who = QStringLiteral("#%1").arg(entry.sessionId);
            const double whoWidth = metrics.horizontalAdvance(who) + 6;
            painter.setPen(ink(body->hue, 0.85, 0.70, fade));
            painter.drawText(QRectF(textLeft, y, whoWidth, line),
                             Qt::AlignLeft | Qt::AlignVCenter, who);
            textLeft += whoWidth;
        }
        QString text = entry.text;
        if (entry.repeats > 1)
            text += QStringLiteral("  ×%1").arg(entry.repeats);
        QColor textInk = m_foreground;
        textInk.setAlphaF(float(0.35 + 0.6 * fade));
        painter.setPen(textInk);
        const double room = box.right() - textLeft - 10;
        painter.drawText(QRectF(textLeft, y, room, line), Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(text, Qt::ElideRight, room));
        y += line;
    }
    if (drawn == 0) {
        painter.setPen(m_muted);
        painter.drawText(QRectF(box.left() + 12, y, box.width() - 24, line * 2),
                         Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                         logs.isEmpty()
                             ? QStringLiteral("Nothing logged yet. Lines appear here as "
                                              "the desktop writes them.")
                             : QStringLiteral("Only node and mirror output logged "
                                              "lately, and it is hidden."));
    }
}

void AgentActivityView::paintLogSparks(QPainter &painter)
{
    if (!m_logs)
        return;
    const double now = m_scene->now();
    const QVector<LogLine> &logs = m_scene->logs();
    // No words on the spark: the field carries none. Newest last, so the fresh spark is drawn over the one it follows. Only
    // the lines still sparking; a backlog seeded on open was born spent.
    int first = logs.size();
    while (first > 0 && logs.at(first - 1).sparking(now))
        --first;
    first = qMax(first, int(logs.size()) - 24);
    for (int i = first; i < logs.size(); ++i) {
        const LogLine &entry = logs.at(i);
        const bool seat = entry.sessionId == Scene::kDesktop
                          || m_scene->mirrorForSource(entry.sessionId) != nullptr;
        // Likewise: a mirror's spark rises off the desktop's node when the
        // seats are not drawn.
        const double shown = seat ? 1.0 : presence(entry.sessionId);
        if (shown <= 0.01)
            continue;
        const QPointF from = flowEnd(entry.sessionId);
        if (from.isNull())
            continue;
        const double t = qBound(0.0, (now - entry.seconds) / LogLine::kSparkSeconds, 1.0);
        const double eased = 1.0 - (1.0 - t) * (1.0 - t);
        // Fanned by its place in the log so a burst does not stack on itself.
        const double sway = (scatter(quint32(i * 31 + 7)) - 0.5) * 36.0;
        const bool desktop = entry.sessionId == Scene::kDesktop;
        const bool mirror = !desktop && m_network
                            && m_scene->mirrorForSource(entry.sessionId);
        const double start = desktop ? 16.0
                                     : (mirror ? 10.0 : bodyRadius(entry.sessionId) + 4.0);
        // The desktop's sparks rise further and fan wider off its node; a
        // mirror's fall away from its seat, since the field is below it.
        const double rise = desktop ? 80.0 : (mirror ? -34.0 : 46.0);
        const QPointF at = from + QPointF(sway * (desktop ? 2.0 : 1.0) * eased,
                                          -(mirror ? -start + rise * eased
                                                   : start + rise * eased));
        const double alpha = shown * (1.0 - t) * qMin(1.0, t * 8.0 + 0.2);
        QColor accent(entry.accent);
        if (!accent.isValid())
            accent = m_muted;

        QColor glow = accent;
        glow.setAlphaF(float(qBound(0.0, 0.35 * alpha, 1.0)));
        painter.setPen(Qt::NoPen);
        drawGlow(painter, at, 11, glow);
        paintMark(painter, entry.motion, at, 3.4, alpha);
    }
}

void AgentActivityView::paintRows(QPainter &painter)
{
    // Each band's name at the head of its first row, over a faint rule: the
    // reason an agent sits where it does is written on the field.
    const QVector<BandRow> &rows = m_scene->rows();
    if (rows.isEmpty())
        return;
    const QFont was = painter.font();
    QFont font = was;
    font.setPixelSize(10);
    font.setBold(true);
    font.setCapitalization(QFont::AllUppercase);
    font.setLetterSpacing(QFont::PercentageSpacing, 108);
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    for (int i = 0; i < rows.size(); ++i) {
        const BandRow &row = rows.at(i);
        if (row.count == 0)
            continue;
        // The caption sits in the band's own strip, just over its first row.
        const double rowTop = m_field.top() + (row.y - row.height / 2) * m_field.height();
        const double top = rowTop - Scene::kBandHeader * m_field.height();
        if (i > 0) {
            painter.setPen(QPen(m_guide, 1.0, Qt::DashLine));
            painter.drawLine(QPointF(m_field.left(), top), QPointF(m_field.right(), top));
        }
        const QString text = QStringLiteral("%1 · %2").arg(bandLabel(row.band)).arg(row.count);
        painter.setPen(m_muted);
        painter.drawText(QPointF(m_field.left() + 2, rowTop - metrics.descent() - 1), text);
    }
    painter.setFont(was);
}

QVector<std::pair<Motion, int>> AgentActivityView::hoverKinds() const
{
    QVector<std::pair<Motion, int>> kinds;
    if (!m_scene || !m_mouseInside)
        return kinds;
    int owner = 0;
    if (const Body *body = m_scene->body(m_hover); body && shownNow(*body)) {
        owner = body->sessionId;
    } else if (!m_logRect.contains(m_mouse) && !m_listRect.contains(m_mouse)) {
        const QPointF desktop = flowEnd(Scene::kDesktop);
        if (std::hypot(desktop.x() - m_mouse.x(), desktop.y() - m_mouse.y()) < 12) {
            owner = Scene::kDesktop;
        } else if (m_network) {
            for (const Host &host : m_scene->hosts()) {
                if (!host.mirror)
                    continue;
                const QPointF at = hostPosition(host.name);
                if (std::hypot(at.x() - m_mouse.x(), at.y() - m_mouse.y()) < 12) {
                    owner = host.source;
                    break;
                }
            }
        }
    }
    if (owner == 0)
        return kinds;
    for (const Motion motion : forkmesh::sonify::kMotions) {
        if (const int count = m_scene->kindCount(owner, motion); count > 0)
            kinds.append({motion, count});
    }
    std::stable_sort(kinds.begin(), kinds.end(),
                     [](const auto &a, const auto &b) { return a.second > b.second; });
    return kinds;
}

void AgentActivityView::paintHoverCard(QPainter &painter)
{
    const QStringList lines = hoverLines();
    if (lines.isEmpty()) {
        m_cardSubject.clear();
        return;
    }
    QFont font = painter.font();
    font.setPixelSize(11);
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const double lineHeight = metrics.height() + 3;
    double widest = 0;
    for (const QString &text : lines)
        widest = qMax(widest, qMin(420.0, metrics.horizontalAdvance(text)));
    // The key: every kind of mark it has made, its figure, what it stands
    // for and how many, most first.
    constexpr int kKeyRows = 12;
    const QVector<std::pair<Motion, int>> kinds = hoverKinds();
    const int keyRows = int(qMin<qsizetype>(kinds.size(), kKeyRows));
    const bool keyMore = kinds.size() > kKeyRows;
    int keyTotal = 0;
    for (const auto &kind : kinds)
        keyTotal += kind.second;
    const QString keyHeading =
        QStringLiteral("its marks: %1 in all, %2 kind%3")
            .arg(keyTotal)
            .arg(kinds.size())
            .arg(kinds.size() == 1 ? QString() : QStringLiteral("s"));
    const double iconRoom = 20;
    double countWidth = 0;
    double labelWidth = 0;
    for (int i = 0; i < keyRows; ++i) {
        countWidth = qMax(countWidth, metrics.horizontalAdvance(QString::number(kinds.at(i).second)));
        labelWidth = qMax(labelWidth,
                          metrics.horizontalAdvance(QString::fromLatin1(styleFor(kinds.at(i).first).label)));
    }
    if (keyRows > 0) {
        widest = qMax(widest, metrics.horizontalAdvance(keyHeading));
        for (int i = 0; i < keyRows; ++i) {
            const double row = iconRoom + countWidth + 8 + labelWidth + 10
                               + metrics.horizontalAdvance(
                                   QString::fromLatin1(styleFor(kinds.at(i).first).description));
            widest = qMax(widest, qMin(420.0, row));
        }
    }
    const int keyLines = keyRows > 0 ? keyRows + 1 + (keyMore ? 1 : 0) : 0;
    const double w = widest + 20;
    const double h = lineHeight * lines.size() + (keyLines > 0 ? lineHeight * keyLines + 6 : 0)
                     + 14;
    QPointF anchor = m_mouse;
    double gap = 16;
    double accent = 0.58;
    const Body *body = m_scene->body(m_hover);
    if (body) {
        anchor = pointFor(*body);
        gap = radiusFor(*body) + 20;
        accent = body->hue;
    }
    // A body is known by its session, anything else by the card's heading.
    const QString subject = body ? QStringLiteral("#%1").arg(body->sessionId)
                                 : QStringLiteral("~") + lines.first();
    QRectF card(anchor.x() + gap, anchor.y() - h / 2, w, h);
    if (subject == m_cardSubject) {
        card.moveTopLeft(m_cardAt);
    } else if (card.right() > width() - 8) {
        card.moveRight(anchor.x() - gap);
    }
    card.moveLeft(qBound(4.0, card.left(), qMax(4.0, width() - w - 4)));
    card.moveTop(qBound(4.0, card.top(), qMax(4.0, height() - h - 4)));
    if (subject != m_cardSubject) {
        m_cardSubject = subject;
        m_cardAt = card.topLeft();
    }

    painter.setPen(QPen(ink(accent, 0.8, 0.6, 0.6), 1));
    painter.setBrush(m_dark ? QColor(10, 14, 24, 235) : QColor(255, 255, 255, 240));
    painter.drawRoundedRect(card, 7, 7);
    double y = card.top() + 7;
    for (int i = 0; i < lines.size(); ++i) {
        painter.setPen(i == 0 ? m_foreground : m_muted);
        painter.drawText(QRectF(card.left() + 10, y, w - 20, lineHeight),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(lines.at(i), Qt::ElideMiddle, w - 20));
        y += lineHeight;
    }
    if (keyRows == 0)
        return;
    y += 3;
    painter.setPen(QPen(m_guide, 1.0));
    painter.drawLine(QPointF(card.left() + 10, y), QPointF(card.right() - 10, y));
    y += 3;
    painter.setPen(m_foreground);
    painter.drawText(QRectF(card.left() + 10, y, w - 20, lineHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, keyHeading);
    y += lineHeight;
    for (int i = 0; i < keyRows; ++i) {
        const Motion motion = kinds.at(i).first;
        const MotionStyle &style = styleFor(motion);
        double x = card.left() + 10;
        paintIcon(painter, motion, QPointF(x + 7, y + lineHeight / 2), 6.0, 1.0);
        x += iconRoom;
        painter.setPen(m_foreground);
        painter.drawText(QRectF(x, y, countWidth, lineHeight), Qt::AlignRight | Qt::AlignVCenter,
                         QString::number(kinds.at(i).second));
        x += countWidth + 8;
        painter.setPen(ink(style.hue, 0.8, m_dark ? 0.72 : 0.36));
        painter.drawText(QRectF(x, y, labelWidth, lineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         QString::fromLatin1(style.label));
        x += labelWidth + 10;
        painter.setPen(m_muted);
        const double room = card.right() - 10 - x;
        painter.drawText(QRectF(x, y, room, lineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(QString::fromLatin1(style.description),
                                            Qt::ElideRight, room));
        y += lineHeight;
    }
    if (keyMore) {
        int rest = 0;
        for (qsizetype i = kKeyRows; i < kinds.size(); ++i)
            rest += kinds.at(i).second;
        painter.setPen(m_muted);
        painter.drawText(QRectF(card.left() + 10 + iconRoom, y, w - 20 - iconRoom, lineHeight),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("and %1 more of %2 other kinds")
                             .arg(rest)
                             .arg(kinds.size() - kKeyRows));
    }
}

void AgentActivityView::paintSky(QPainter &painter)
{
    // The sky is a still picture: nothing in it stands for anything, so
    // nothing in it moves, and it is painted once.
    const double w = width();
    const double h = height();
    switch (m_backdrop) {
    case Backdrop::Theme: {
        QLinearGradient sky(0, 0, 0, h);
        sky.setColorAt(0.0, m_dark ? QColor(11, 16, 28) : QColor(252, 253, 255));
        sky.setColorAt(1.0, m_background);
        painter.fillRect(rect(), sky);
        return;
    }
    case Backdrop::Space: {
        QLinearGradient sky(0, 0, 0, h);
        sky.setColorAt(0.0, QColor(2, 3, 12));
        sky.setColorAt(0.6, QColor(7, 9, 28));
        sky.setColorAt(1.0, QColor(13, 9, 32));
        painter.fillRect(rect(), sky);
        // Nebulae: three clouds of colour.
        const struct {
            double x, y, r, hue;
        } clouds[] = {{0.22, 0.32, 0.46, 0.78}, {0.76, 0.64, 0.52, 0.53}, {0.56, 0.12, 0.30, 0.93}};
        painter.setPen(Qt::NoPen);
        for (int i = 0; i < 3; ++i) {
            const QPointF centre(clouds[i].x * w, clouds[i].y * h);
            const double radius = clouds[i].r * qMax(w, h);
            QRadialGradient cloud(centre, radius);
            cloud.setColorAt(0.0, ink(clouds[i].hue, 0.65, 0.45, 0.17));
            cloud.setColorAt(0.5, ink(clouds[i].hue, 0.6, 0.35, 0.06));
            cloud.setColorAt(1.0, ink(clouds[i].hue, 0.6, 0.3, 0.0));
            painter.setBrush(cloud);
            painter.drawEllipse(centre, radius, radius);
        }
        paintStars(painter, 1.0, 1.0);
        return;
    }
    case Backdrop::Aurora: {
        QLinearGradient sky(0, 0, 0, h);
        sky.setColorAt(0.0, QColor(2, 7, 16));
        sky.setColorAt(0.55, QColor(5, 18, 30));
        sky.setColorAt(1.0, QColor(7, 12, 24));
        painter.fillRect(rect(), sky);
        paintStars(painter, 0.5, 0.7);
        // Three curtains of light.
        painter.setPen(Qt::NoPen);
        const double hues[3] = {0.38, 0.46, 0.78};
        for (int k = 0; k < 3; ++k) {
            const double base = h * (0.16 + 0.11 * k);
            const double band = h * (0.20 + 0.04 * k);
            QPainterPath curtain;
            QVector<QPointF> top;
            for (double x = -20; x <= w + 20; x += 14) {
                const double u = x / qMax(1.0, w);
                const double y = base
                                 + h * 0.05 * std::sin(u * 8.2 + k)
                                 + h * 0.02 * std::sin(u * 19.5 + k * 2.0);
                top.append(QPointF(x, y));
            }
            curtain.moveTo(top.first());
            for (const QPointF &point : std::as_const(top))
                curtain.lineTo(point);
            for (int i = top.size() - 1; i >= 0; --i)
                curtain.lineTo(top[i] + QPointF(0, band));
            curtain.closeSubpath();
            QLinearGradient light(0, base - h * 0.07, 0, base + band + h * 0.07);
            light.setColorAt(0.0, ink(hues[k], 0.9, 0.55, 0.0));
            light.setColorAt(0.3, ink(hues[k], 0.9, 0.55, 0.20));
            light.setColorAt(1.0, ink(hues[k], 0.9, 0.45, 0.0));
            painter.setBrush(light);
            painter.drawPath(curtain);
        }
        return;
    }
    case Backdrop::Ocean: {
        QLinearGradient sea(0, 0, 0, h);
        sea.setColorAt(0.0, QColor(6, 32, 58));
        sea.setColorAt(0.5, QColor(3, 18, 38));
        sea.setColorAt(1.0, QColor(1, 8, 20));
        painter.fillRect(rect(), sea);
        // Shafts of light from the surface.
        painter.setPen(Qt::NoPen);
        for (int i = 0; i < 5; ++i) {
            const double x = w * (0.1 + 0.2 * i) + 40 * std::sin(i * 1.3);
            const double spread = w * 0.05 + 20 * std::sin(double(i));
            QPainterPath shaft;
            shaft.moveTo(x - spread * 0.4, 0);
            shaft.lineTo(x + spread * 0.4, 0);
            shaft.lineTo(x + spread * 2.2 + 60, h);
            shaft.lineTo(x - spread * 0.6 + 60, h);
            shaft.closeSubpath();
            QLinearGradient light(0, 0, 0, h);
            light.setColorAt(0.0, QColor(160, 220, 255, 26));
            light.setColorAt(1.0, QColor(160, 220, 255, 0));
            painter.setBrush(light);
            painter.drawPath(shaft);
        }
        // Motes in the water.
        for (int i = 0; i < m_stars.size(); i += 3) {
            const Star &star = m_stars.at(i);
            const double y = star.y;
            const double x = star.x;
            const double alpha = 0.10 + 0.25 * star.brightness * (1.0 - y * 0.6);
            painter.setBrush(QColor::fromRgbF(0.7f, 0.9f, 1.0f, float(alpha)));
            painter.drawEllipse(QPointF(x * w, y * h), star.size * 0.8, star.size * 0.8);
        }
        return;
    }
    }
}

void AgentActivityView::paintBackdrop(QPainter &painter)
{
    // The whole backdrop holds still, so it is painted once into m_sky for
    // this size and theme, and every frame after is one blit.
    const double ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const QString key = QStringLiteral("%1|%2x%3@%4|%5")
                            .arg(int(m_backdrop))
                            .arg(width())
                            .arg(height())
                            .arg(ratio)
                            .arg(m_background.rgba());
    if (key != m_skyKey || m_sky.isNull()) {
        // Opaque, so laying it down is a copy rather than a blend.
        m_sky = QImage((QSizeF(size()) * ratio).toSize(), QImage::Format_RGB32);
        m_sky.setDevicePixelRatio(ratio);
        QPainter sky(&m_sky);
        sky.setRenderHint(QPainter::Antialiasing, true);
        paintSky(sky);
        sky.end();
        m_skyKey = key;
    }
    painter.drawImage(QPointF(0, 0), m_sky);
}

void AgentActivityView::paintStars(QPainter &painter, double density, double brightness)
{
    const double w = width();
    const double h = height();
    const int count = int(m_stars.size() * density);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < count; ++i) {
        const Star &star = m_stars.at(i);
        const double x = star.x * w;
        const double y = star.y * h;
        const double shine = brightness * star.brightness
                             * (0.62 + 0.38 * std::sin(star.phase));
        if (star.size > 1.6) {
            drawGlow(painter, QPointF(x, y), star.size * 4,
                     QColor::fromRgbF(0.8f, 0.87f, 1.0f, float(qBound(0.0, 0.35 * shine, 1.0))));
        }
        painter.setBrush(QColor::fromRgbF(0.92f, 0.95f, 1.0f, float(qBound(0.0, shine, 1.0))));
        painter.drawEllipse(QPointF(x, y), star.size * 0.6, star.size * 0.6);
    }
}

void AgentActivityView::paintNetwork(QPainter &painter)
{
    if (!m_network || !m_scene)
        return;
    const double now = m_scene->now();
    const QList<Host> &hosts = m_scene->hosts();
    if (hosts.isEmpty())
        return;

    // The pipes: a faint line for every conversation, as thick as it is busy.
    const QVector<Flow> flows = m_scene->flows();
    QVector<Pipe> pipes;
    pipes.reserve(flows.size());
    for (const Flow &flow : flows) {
        const QPointF from = flowEnd(flow.sessionId);
        const QPointF to = hostPosition(flow.host);
        if (from.isNull() || to.isNull())
            continue;
        const double shown = flow.sessionId == Scene::kDesktop ? 1.0 : presence(flow.sessionId);
        if (shown <= 0.01)
            continue;
        const double rate = Scene::rateOf(flow.upRecent + flow.downRecent);
        const double busy = qBound(0.0, std::log10(1.0 + rate) / 6.0, 1.0);
        const double idle = qBound(0.0, 1.0 - (now - flow.lastSeconds) / Scene::kHostLinger, 1.0);
        const Host *host = m_scene->host(flow.host);
        const bool mirror = host && host->mirror;
        pipes.append({from, to,
                      ink(mirror ? kMirrorHue : 0.55, mirror ? 0.5 : 0.35, 0.75,
                          shown * ((mirror ? 0.10 : 0.04) + 0.08 * idle + 0.26 * busy)),
                      0.6 + 2.2 * busy});
    }
    paintPipes(painter, pipes);

    // The packets themselves: out towards the host warm, in towards the agent
    // cool, each a small lit bead easing along the pipe.
    painter.setPen(Qt::NoPen);
    for (const Packet &packet : m_scene->packets()) {
        if (packet.pending(now))
            continue;
        const QPointF body = flowEnd(packet.sessionId);
        const QPointF host = hostPosition(packet.host);
        if (body.isNull() || host.isNull())
            continue;
        const double shown = packet.sessionId == Scene::kDesktop ? 1.0 : presence(packet.sessionId);
        if (shown <= 0.01)
            continue;
        const double t = packet.life(now);
        const double eased = t * t * (3.0 - 2.0 * t);
        const QPointF control = flowControl(body, host);
        const QPointF at = packet.up ? onQuad(body, control, host, eased)
                                     : onQuad(body, control, host, 1.0 - eased);
        const double alpha = shown * std::sqrt(std::sin(M_PI * t));
        const double hue = packet.up ? 0.08 : 0.52;
        const double radius = 1.3 + 2.6 * packet.size;
        drawGlow(painter, at, radius * 3.2, ink(hue, 0.95, 0.66, 0.45 * alpha));
        painter.setBrush(ink(hue, 0.9, 0.80, alpha));
        painter.drawEllipse(at, radius, radius);
    }

    // The hosts along the top, brighter the more is crossing to them right
    // now, each over its name. Your mirrors, at the left, are green, there
    // even while nobody is talking to them.
    const QFont was = painter.font();
    QFont label = was;
    label.setPixelSize(9);
    const QFontMetricsF labelMetrics(label);
    for (const Host &host : hosts) {
        const QPointF at = hostPosition(host.name);
        const double idle = qBound(0.25, 1.0 - (now - host.lastSeconds) / Scene::kHostLinger, 1.0);
        const double rate = Scene::rateOf(host.upRecent + host.downRecent);
        const double busy = qBound(0.0, std::log10(1.0 + rate) / 6.0, 1.0);
        const double hue = host.mirror ? kMirrorHue : 0.58;
        painter.setPen(Qt::NoPen);
        drawGlow(painter, at, 14 + 14 * busy, ink(hue, 0.6, 0.7, 0.35 * idle + 0.3 * busy));
        const HostPlace place = hostPlace(host.name, host.mirror);
        if (host.mirror) {
            const double lit = qBound(0.0, host.energy, 1.0);
            const double r = 5.0 + 1.8 * busy + 2.4 * lit;
            if (lit > 0.01)
                drawGlow(painter, at, 10 + 16 * lit, ink(hue, 0.5, 0.85, 0.35 * lit));
            paintPlace(painter, place, at, r, hue, qBound(0.45, qMax(idle, lit) + 0.3 * busy, 1.0));
            if (busy > 0.02) {
                painter.setPen(QPen(ink(hue, 0.55, 0.88, 0.5 * idle + 0.4 * busy + 0.3 * lit),
                                    1.0));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(at, r + 3.0 + 4.0 * busy, r + 3.0 + 4.0 * busy);
            }
        } else {
            paintPlace(painter, place, at, 5.0 + 1.6 * busy, hue,
                       qBound(0.4, idle + 0.3 * busy, 1.0));
        }
        painter.setFont(label);
        painter.setPen(ink(hue, 0.25, m_dark ? 0.72 : 0.38, 0.55 + 0.4 * idle));
        painter.drawText(QRectF(at.x() - 60, at.y() + 10, 120, 13), Qt::AlignCenter,
                         labelMetrics.elidedText(host.name, Qt::ElideMiddle, 116));
    }
    painter.setFont(was);
}

void AgentActivityView::paintPipes(QPainter &painter, const QVector<Pipe> &pipes)
{
    const double ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    // The layer is kept while every pipe is within a pixel of where it was
    // drawn, and within a step of its shade and weight: the bodies' float is
    // a few pixels a second, and a pipe's fade takes seconds.
    const auto near = [](const QPointF &a, const QPointF &b) {
        return std::abs(a.x() - b.x()) <= 1.0 && std::abs(a.y() - b.y()) <= 1.0;
    };
    bool kept = !m_pipes.isNull() && m_pipes.devicePixelRatio() == ratio
                && m_pipesDrawn.size() == pipes.size();
    for (qsizetype i = 0; kept && i < pipes.size(); ++i) {
        const Pipe &now = pipes.at(i);
        const Pipe &was = m_pipesDrawn.at(i);
        kept = near(now.from, was.from) && near(now.to, was.to)
               && now.colour.rgb() == was.colour.rgb()
               && std::abs(now.colour.alpha() - was.colour.alpha()) <= 3
               && std::abs(now.width - was.width) <= 0.15;
    }
    if (!kept) {
        m_pipesDrawn = pipes;
        QRectF bounds;
        for (const Pipe &pipe : pipes) {
            const QPointF control = flowControl(pipe.from, pipe.to);
            const double pad = pipe.width + 2;
            QRectF box = QRectF(pipe.from, pipe.to).normalized();
            box |= QRectF(control, QSizeF(0, 0));
            bounds |= box.adjusted(-pad, -pad, pad, pad);
        }
        bounds &= QRectF(rect());
        if (bounds.isEmpty()) {
            m_pipes = QImage();
            return;
        }
        const QRect device = QRectF(bounds.topLeft() * ratio, bounds.size() * ratio).toAlignedRect();
        m_pipesAt = QPointF(device.topLeft()) / ratio;
        m_pipes = QImage(device.size(), QImage::Format_ARGB32_Premultiplied);
        m_pipes.setDevicePixelRatio(ratio);
        m_pipes.fill(Qt::transparent);
        QPainter layer(&m_pipes);
        layer.setRenderHint(QPainter::Antialiasing, true);
        layer.translate(-m_pipesAt);
        // Source, not SourceOver: a stroke of a translucent colour blended
        // over the field is split across Qt's thread pool span by span, which
        // costs far more than the pixels; written straight into the empty
        // layer it is not, and the layer is laid over the field in one go.
        // Where two pipes cross, the later one's shade wins.
        layer.setCompositionMode(QPainter::CompositionMode_Source);
        layer.setBrush(Qt::NoBrush);
        for (const Pipe &pipe : pipes) {
            QPainterPath path(pipe.from);
            path.quadTo(flowControl(pipe.from, pipe.to), pipe.to);
            layer.setPen(QPen(pipe.colour, pipe.width, Qt::SolidLine, Qt::RoundCap));
            layer.drawPath(path);
        }
    }
    if (!m_pipes.isNull())
        painter.drawImage(m_pipesAt, m_pipes);
}

void AgentActivityView::paintDesktopNode(QPainter &painter)
{
    const QPointF at = flowEnd(Scene::kDesktop);
    paintPlace(painter, HostPlace::Machine, at, 9.0, 0.58, 0.95);
    QFont font = painter.font();
    font.setPixelSize(10);
    painter.setFont(font);
    painter.setPen(m_muted);
    painter.drawText(QRectF(at.x() - 60, at.y() + 12, 120, 14), Qt::AlignCenter,
                     QStringLiteral("this desktop"));
}

QPointF AgentActivityView::clientPosition(const QString &id) const
{
    if (!m_scene)
        return {};
    const double scale = qMax(0.8, m_unit / 520.0);
    const QPointF node = flowEnd(Scene::kDesktop);
    // The room: over the node, clear of the ring of its background work.
    if (id.isEmpty())
        return node + QPointF(0, -46.0 * scale);
    const QList<Client> &clients = m_scene->clients();
    int index = -1;
    for (int i = 0; i < clients.size(); ++i) {
        if (clients.at(i).info.id == id) {
            index = i;
            break;
        }
    }
    return index < 0 ? QPointF() : clientSeat(index);
}

QString AgentActivityView::clientAt(const QPointF &point) const
{
    if (!m_scene || !m_network)
        return {};
    const QList<Client> &clients = m_scene->clients();
    for (int i = 0; i < clients.size(); ++i) {
        const QPointF p = clientSeat(i);
        if (!p.isNull() && std::hypot(p.x() - point.x(), p.y() - point.y()) <= 7)
            return clients.at(i).info.id;
    }
    return {};
}

QString AgentActivityView::flagFamily()
{
#if defined(Q_OS_WIN)
    // Windows' emoji font spells a flag out as its two letters.
    return {};
#else
    static const QString family = [] {
        const QStringList families = QFontDatabase::families();
        for (const char *name : {"Noto Color Emoji", "Apple Color Emoji", "Twemoji",
                                 "Twitter Color Emoji", "JoyPixels", "EmojiOne Color"}) {
            if (families.contains(QLatin1String(name)))
                return QString::fromLatin1(name);
        }
        return QString();
    }();
    return family;
#endif
}

const QPixmap &AgentActivityView::flagArt(const QString &country, double height,
                                          qreal ratio) const
{
    const int pixels = qMax(4, qRound(height));
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(country)
                            .arg(pixels)
                            .arg(ratio)
                            .arg(m_dark ? 1 : 0);
    auto it = m_flags.find(key);
    if (it != m_flags.end())
        return *it;
    // Half as wide again as it is tall, the shape most flags are.
    const QSizeF size(pixels * 1.5, pixels * 1.5);
    QPixmap art((size * ratio).toSize());
    art.setDevicePixelRatio(ratio);
    art.fill(Qt::transparent);
    QPainter p(&art);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QString flag = flagForCountry(country);
    const QString family = flagFamily();
    if (!flag.isEmpty() && !family.isEmpty()) {
        QFont font(family);
        font.setPixelSize(qRound(pixels * 1.3));
        p.setFont(font);
        p.drawText(QRectF(QPointF(0, 0), size), Qt::AlignCenter, flag);
    } else {
        // No font draws it: its code on a little pennant.
        const QRectF chip(0, (size.height() - pixels) / 2, size.width(), pixels);
        p.setPen(QPen(m_dark ? QColor(200, 210, 230, 200) : QColor(60, 70, 90, 200), 0.8));
        p.setBrush(m_dark ? QColor(22, 30, 48, 235) : QColor(245, 247, 252, 240));
        p.drawRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), 1.5, 1.5);
        QFont font = p.font();
        font.setPixelSize(qMax(5, qRound(pixels * 0.72)));
        font.setBold(true);
        p.setFont(font);
        p.setPen(m_dark ? QColor(230, 236, 248) : QColor(30, 36, 50));
        p.drawText(chip, Qt::AlignCenter, country.left(2));
    }
    p.end();
    // A handful of countries at two or three sizes: never many, but bounded.
    if (m_flags.size() > 600)
        m_flags.clear();
    return *m_flags.insert(key, art);
}

void AgentActivityView::paintThreatSign(QPainter &painter, const QPointF &at, double r,
                                        Threat threat, bool blocked, double alpha) const
{
    painter.save();
    if (blocked) {
        // A no-entry bar across it: stopped at the edge.
        QPen bar(QColor(248, 81, 73, int(230 * alpha)), qMax(1.4, r * 0.45), Qt::SolidLine,
                 Qt::RoundCap);
        painter.setPen(bar);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(at, r + 2.2, r + 2.2);
        painter.drawLine(at + QPointF(-(r + 1.4), r + 1.4) * 0.72,
                         at + QPointF(r + 1.4, -(r + 1.4)) * 0.72);
        painter.restore();
        return;
    }
    if (threat == Threat::None) {
        painter.restore();
        return;
    }
    // A warning sign over its top right: amber to watch, red for danger.
    const bool danger = threat == Threat::Danger;
    const double side = danger ? qMax(10.0, r * 3.0) : qMax(8.5, r * 2.5);
    const QPointF apex = at + QPointF(r * 1.1, -r - side * 0.9);
    const QPointF triangle[] = {apex, apex + QPointF(side * 0.58, side),
                                apex + QPointF(-side * 0.58, side)};
    const QColor fill = danger ? QColor(248, 81, 73) : QColor(240, 180, 40);
    QColor glow = fill;
    glow.setAlphaF(float(0.35 * alpha));
    painter.setPen(Qt::NoPen);
    drawGlow(painter, apex + QPointF(0, side * 0.6), side * 1.3, glow);
    QColor body = fill;
    body.setAlphaF(float(alpha));
    painter.setPen(QPen(m_dark ? QColor(10, 12, 20, int(220 * alpha))
                               : QColor(255, 255, 255, int(230 * alpha)),
                        0.8));
    painter.setBrush(body);
    painter.drawPolygon(triangle, 3);
    // The "!".
    painter.setPen(QPen(QColor(20, 16, 10, int(240 * alpha)), qMax(1.0, side * 0.14),
                        Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(apex + QPointF(0, side * 0.34), apex + QPointF(0, side * 0.64));
    painter.drawPoint(apex + QPointF(0, side * 0.84));
    painter.restore();
}

QPointF AgentActivityView::clientSeat(int index) const
{
    const double scale = qMax(0.8, m_unit / 520.0);
    const QPointF node = flowEnd(Scene::kDesktop);
    // Leftwards from the node, the nearest seat first; the network meter has
    // the other side. Wide enough apart for a flag on every seat.
    const double pitch = qMax(14.0, 17.0 * scale);
    const double right = node.x() - 48.0 * scale;
    const double left = m_field.left() + 8.0;
    if (right <= left)
        return {};
    const int perRow = qMax(1, int((right - left - pitch * 0.5) / pitch) + 1);
    const int row = index / perRow;
    if (row >= kClientRows)
        return {};
    // Every other row half a step over, so a crowd packs rather than stacks.
    return QPointF(right - (index % perRow) * pitch - (row % 2) * pitch * 0.5,
                   node.y() + 4.0 - row * pitch);
}

void AgentActivityView::paintClients(QPainter &painter)
{
    if (!m_network || !m_scene)
        return;
    const QList<Client> &clients = m_scene->clients();
    const QVector<Frame> &frames = m_scene->frames();
    if (clients.isEmpty() && frames.isEmpty())
        return;
    const double now = m_scene->now();
    const QPointF node = flowEnd(Scene::kDesktop);
    const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    // Every seat once a frame: a room of visitors has many beads in flight.
    QHash<QString, QPointF> seats;
    seats.reserve(clients.size() + 1);
    seats.insert(QString(), clientPosition(QString()));
    for (int i = 0; i < clients.size(); ++i)
        seats.insert(clients.at(i).info.id, clientSeat(i));

    // The frames in flight, as beads along the same bow a packet takes: warm
    // leaving this desktop, cool arriving at it.
    painter.setPen(Qt::NoPen);
    for (const Frame &frame : frames) {
        if (now < frame.bornSeconds)
            continue;
        const QPointF far = seats.value(frame.client);
        if (far.isNull())
            continue;
        const double t = frame.life(now);
        const double eased = t * t * (3.0 - 2.0 * t);
        const QPointF control = flowControl(node, far);
        const QPointF at = frame.up ? onQuad(node, control, far, eased)
                                    : onQuad(node, control, far, 1.0 - eased);
        const double alpha = std::sqrt(std::sin(M_PI * t));
        const double hue = frame.up ? 0.08 : 0.52;
        const double radius = 1.1 + 1.9 * frame.size;
        drawGlow(painter, at, radius * 3.0, ink(hue, 0.95, 0.66, 0.4 * alpha));
        painter.setBrush(ink(hue, 0.9, 0.80, alpha));
        painter.drawEllipse(at, radius, radius);
    }

    // The clients: a globe for a web visitor, a screen for a desktop like
    // this one, a slate for a phone. Lit as frames cross to them, ringed as
    // they arrive, fading dashed as they leave.
    int hidden = 0;
    QPointF furthest;
    for (const Client &client : clients) {
        const QPointF at = seats.value(client.info.id);
        if (at.isNull()) {
            if (client.present())
                ++hidden;
            continue;
        }
        if (furthest.isNull() || at.x() < furthest.x())
            furthest = at;
        const double hue = clientHue(client.kind);
        const double shown =
            client.present()
                ? 1.0
                : qBound(0.0, 1.0 - (now - client.leftSeconds) / Scene::kClientLinger, 1.0);
        if (shown <= 0.01)
            continue;
        const double lit = qBound(0.0, client.energy, 1.0);
        const double r = 3.0 + 1.4 * lit;
        if (lit > 0.02) {
            painter.setPen(Qt::NoPen);
            drawGlow(painter, at, 7.0 + 9.0 * lit, ink(hue, 0.6, 0.7, 0.35 * lit * shown));
        }
        const double age = now - client.bornSeconds;
        if (age >= 0 && age < Scene::kClientArrival) {
            const double k = age / Scene::kClientArrival;
            painter.setPen(QPen(ink(hue, 0.7, 0.75, (1.0 - k) * shown), 1.2));
            painter.setBrush(Qt::NoBrush);
            const double ring = r + 2.0 + 14.0 * easeOut(k);
            painter.drawEllipse(at, ring, ring);
        }
        const QColor colour = ink(hue, 0.6, 0.62 + 0.2 * lit, shown * (0.7 + 0.3 * lit));
        // Where it is: its country's flag, for a visitor in place of the
        // globe (which only says "somewhere"), for anybody else under it.
        const bool flagged = !client.info.country.isEmpty();
        const double flagHeight = qMax(7.0, r * 2.1);
        switch (client.kind) {
        case ClientKind::Visitor:
            if (flagged) {
                const QPixmap &art = flagArt(client.info.country, flagHeight, ratio);
                const QSizeF size = art.deviceIndependentSize();
                painter.save();
                painter.setOpacity(shown * (client.blocked ? 0.45 : 0.8 + 0.2 * lit));
                painter.drawPixmap(QRectF(at - QPointF(size.width(), size.height()) / 2, size),
                                   art, QRectF(QPointF(0, 0), art.size()));
                painter.restore();
            } else {
                paintCueGlyph(painter, Motion::Visitor, at, r * 1.15, colour, 0.0);
            }
            break;
        case ClientKind::Node:
            paintPlace(painter, HostPlace::Machine, at, r * 1.05, hue,
                       shown * (0.7 + 0.3 * lit));
            break;
        case ClientKind::Phone: {
            painter.setPen(QPen(colour, 1.0));
            QColor fill = colour;
            fill.setAlphaF(fill.alphaF() * 0.45f);
            painter.setBrush(fill);
            painter.drawRoundedRect(QRectF(at.x() - r * 0.6, at.y() - r, r * 1.2, r * 2.0),
                                    r * 0.3, r * 0.3);
            break;
        }
        }
        if (flagged && client.kind != ClientKind::Visitor) {
            const QPixmap &art = flagArt(client.info.country, flagHeight * 0.75, ratio);
            const QSizeF size = art.deviceIndependentSize();
            painter.save();
            painter.setOpacity(shown);
            painter.drawPixmap(QRectF(at + QPointF(-size.width() / 2, r * 0.6), size), art,
                               QRectF(QPointF(0, 0), art.size()));
            painter.restore();
        }
        if (!client.present()) {
            painter.setPen(QPen(ink(hue, 0.4, 0.7, 0.6 * shown), 0.9, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(at, r + 3.0, r + 3.0);
        }
        paintThreatSign(painter, at, r, client.threat, client.blocked, shown);
    }
    // The site as a whole under threat: a sign over the row, with why.
    if (const Threat threat = m_scene->siteThreat(); threat != Threat::None) {
        const QPointF first = clientSeat(0);
        const double pitch = qMax(14.0, 17.0 * qMax(0.8, m_unit / 520.0));
        int rows = 1;
        for (int i = 0; i < clients.size(); ++i) {
            const QPointF seat = clientSeat(i);
            if (seat.isNull())
                break;
            rows = qMax(rows, int(std::round((first.y() - seat.y()) / pitch)) + 1);
        }
        QFont font = painter.font();
        font.setPixelSize(10);
        font.setBold(true);
        painter.setFont(font);
        const QFontMetricsF metrics(font);
        const QString text = QStringLiteral("⚠ %1: %2")
                                 .arg(threatLabel(threat),
                                      m_scene->siteThreatReasons().join(QStringLiteral(" · ")));
        const double right = first.isNull() ? node.x() - 40 : first.x() + 8;
        const double w = qMin(metrics.horizontalAdvance(text) + 14, qMax(80.0, right - m_field.left()));
        const QRectF chip(right - w, first.y() - rows * pitch - 16, w, metrics.height() + 5);
        const bool danger = threat == Threat::Danger;
        painter.setPen(QPen(danger ? QColor(248, 81, 73) : QColor(240, 180, 40), 1.0));
        painter.setBrush(m_dark ? QColor(30, 12, 12, 220) : QColor(255, 244, 236, 235));
        painter.drawRoundedRect(chip, 5, 5);
        painter.setPen(danger ? QColor(255, 120, 110) : QColor(230, 160, 20));
        painter.drawText(chip.adjusted(7, 0, -7, 0), Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(text, Qt::ElideRight, chip.width() - 14));
    }
    // More than the rows hold: the rest are a number at the end of the row.
    if (hidden > 0 && !furthest.isNull()) {
        QFont font = painter.font();
        font.setPixelSize(9);
        painter.setFont(font);
        painter.setPen(m_muted);
        painter.drawText(QRectF(furthest.x() - 40, furthest.y() - 16, 38, 12),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral("+%1").arg(hidden));
    }
}

QPointF AgentActivityView::taskPosition(const Task &task) const
{
    const double scale = m_unit / 520.0;
    if (task.sessionId == Scene::kDesktop) {
        // A ring round the desktop's node at the foot of the field, well
        // out from it so the work reads apart from the node's own marks.
        const QPointF at = flowEnd(Scene::kDesktop);
        return polar(at, 30.0 * qMax(0.8, scale), task.angle * 2.0 * M_PI);
    }
    const Body *body = m_scene ? m_scene->body(task.sessionId) : nullptr;
    if (!body)
        return {};
    // Just outside its ring of history, so the two never overlay.
    return polar(pointFor(*body), radiusFor(*body) + 22.0 * qMax(0.8, scale),
                 task.angle * 2.0 * M_PI);
}

QPointF AgentActivityView::processPosition(const Process &process) const
{
    const Body *body = m_scene ? m_scene->body(process.sessionId) : nullptr;
    if (!body)
        return {};
    // Out past the ring of background work, a step further for each
    // generation, so the tree fans out from the body. Off the body's resting
    // size, not its glow, so the tree does not breathe with every note.
    const double scale = qMax(0.8, m_unit / 520.0);
    const double steady = qBound(5.0, (9.0 + 17.0 * body->size) * m_unit / 520.0, 64.0);
    const double reach = steady + 42.0 * scale + (process.depth - 1) * 15.0 * scale;
    return polar(pointFor(*body), reach, process.angle * 2.0 * M_PI);
}

void AgentActivityView::paintProcesses(QPainter &painter)
{
    if (!m_processes || !m_scene || m_scene->processes().isEmpty())
        return;
    const double now = m_scene->now();
    for (const Process &process : m_scene->processes()) {
        const double shown = presence(process.sessionId);
        if (shown <= 0.01)
            continue;
        const QPointF at = processPosition(process);
        if (at.isNull())
            continue;
        const Body *body = m_scene->body(process.sessionId);
        QPointF from;
        if (const Process *parent =
                process.parentPid ? m_scene->process(process.sessionId, process.parentPid)
                                  : nullptr)
            from = processPosition(*parent);
        if (from.isNull())
            from = polar(pointFor(*body), radiusFor(*body), process.angle * 2.0 * M_PI);
        // Its colour from its name, so a compiler reads the same under any
        // agent; how lit it is from how hard it is working.
        const double hue = std::fmod(double(qHash(process.name) % 1000) / 1000.0, 1.0);
        const double load = qBound(0.0, process.cpu, 1.0);
        const double appear = easeOut((now - process.bornSeconds) / 0.3);
        const double gone = process.running()
                                ? 0.0
                                : qBound(0.0, (now - process.endSeconds) / Scene::kProcessLinger, 1.0);
        painter.setOpacity(shown * appear * (1.0 - gone));

        // The branch back to whoever started it: brighter while it works.
        painter.setPen(QPen(ink(hue, 0.45, 0.65, 0.22 + 0.4 * load), 1.0 + load));
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(from, at);

        const double size = 2.4 + 2.4 * load + (process.depth == 1 ? 0.6 : 0.0);
        const QRectF box(at - QPointF(size, size), QSizeF(size * 2, size * 2));
        if (process.busy()) {
            // Working: a glow that swells with its load.
            const double glowRadius = size * (2.2 + 0.8 * load);
            painter.setPen(Qt::NoPen);
            drawGlow(painter, at, glowRadius, ink(hue, 0.85, 0.65, 0.25 + 0.35 * load));
            painter.setBrush(ink(hue, 0.8, 0.62, 0.95));
            painter.setPen(QPen(ink(hue, 0.9, 0.8, 0.9), 1.0));
        } else if (process.running()) {
            // Asleep: an outline, waiting.
            painter.setBrush(ink(hue, 0.3, 0.3, 0.35));
            painter.setPen(QPen(ink(hue, 0.45, 0.62, 0.7), 1.0));
        } else {
            // Ended: grey, shrinking away.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(m_muted, 1.0));
        }
        // A process is a square, where background work is a round moon.
        painter.drawRoundedRect(gone > 0 ? QRectF(at - QPointF(size, size) * (1.0 - 0.6 * gone),
                                                  QSizeF(size, size) * 2 * (1.0 - 0.6 * gone))
                                         : box,
                                1.2, 1.2);
    }
    painter.setOpacity(1.0);
    // The rest of a crowd too big to draw, counted beside its body.
    QFont font = painter.font();
    font.setPointSizeF(qMax(7.0, font.pointSizeF() * 0.8));
    painter.setFont(font);
    for (const Body &body : m_scene->bodies()) {
        const int hidden = m_scene->hiddenProcesses(body.sessionId);
        const double shown = presence(body.sessionId);
        if (hidden <= 0 || shown <= 0.01)
            continue;
        painter.setOpacity(shown);
        painter.setPen(m_muted);
        const QPointF c = pointFor(body);
        const double r = radiusFor(body);
        painter.drawText(c + QPointF(r + 4, r + 10),
                         QStringLiteral("+%1 processes").arg(hidden));
    }
    painter.setOpacity(1.0);
}

void AgentActivityView::paintTasks(QPainter &painter)
{
    if (!m_scene || m_scene->tasks().isEmpty())
        return;
    const double now = m_scene->now();
    bool desktopWorks = false;
    for (const Task &task : m_scene->tasks())
        desktopWorks = desktopWorks || task.sessionId == Scene::kDesktop;
    if (desktopWorks)
        paintDesktopNode(painter);

    for (const Task &task : m_scene->tasks()) {
        const double shown =
            task.sessionId == Scene::kDesktop ? 1.0 : presence(task.sessionId);
        if (shown <= 0.01)
            continue;
        const QPointF at = taskPosition(task);
        if (at.isNull())
            continue;
        const QPointF owner = task.sessionId == Scene::kDesktop
                                  ? flowEnd(Scene::kDesktop)
                                  : bodyPosition(task.sessionId);
        // Its own colour from what kind of work it is, so a git status reads
        // the same wherever it runs.
        const double hue = std::fmod(double(qHash(task.word) % 1000) / 1000.0, 1.0);
        const double appear = easeOut((now - task.bornSeconds) / 0.25);
        painter.setOpacity(shown * appear);

        // A thread back to whoever it works for.
        painter.setPen(QPen(ink(hue, 0.5, 0.7, 0.28), 1.0, Qt::DotLine));
        painter.drawLine(owner, at);

        if (task.running()) {
            // A small moon with a ring round it while the work runs. A
            // subagent is a bigger moon, a second worker with its own orbit;
            // a background shell a prompt-shaped chevron.
            const bool subagent = task.kind == TaskKind::Subagent;
            const double glowRadius = subagent ? 12.0 : 9.0;
            painter.setPen(Qt::NoPen);
            drawGlow(painter, at, glowRadius, ink(hue, 0.8, 0.65, 0.35));
            painter.setBrush(ink(hue, 0.75, 0.62, 0.95));
            if (task.kind == TaskKind::Shell) {
                QPen chevron(ink(hue, 0.85, 0.75, 0.95), 1.5);
                chevron.setCapStyle(Qt::RoundCap);
                chevron.setJoinStyle(Qt::RoundJoin);
                painter.setPen(chevron);
                painter.setBrush(Qt::NoBrush);
                QPolygonF prompt;
                prompt << at + QPointF(-2.5, -3) << at + QPointF(1.0, 0) << at + QPointF(-2.5, 3);
                painter.drawPolyline(prompt);
                painter.drawLine(at + QPointF(1.5, 3), at + QPointF(3.5, 3));
            } else {
                paintMark(painter, subagent ? Motion::Delegate : Motion::Task, at,
                          subagent ? 3.6 : 3.0, 1.0);
            }
            QPen arc(ink(hue, 0.9, 0.78, 0.9), 1.6);
            arc.setCapStyle(Qt::RoundCap);
            painter.setPen(arc);
            painter.setBrush(Qt::NoBrush);
            const double orbit = subagent ? 8.0 : 6.5;
            painter.drawEllipse(at, orbit, orbit);
        } else {
            // Done: a ring in the outcome's colour, opening as it fades.
            const double t = qBound(0.0, (now - task.endSeconds) / Scene::kTaskLinger, 1.0);
            const QColor done = task.failed ? QColor(248, 81, 73) : QColor(63, 185, 80);
            QColor ring = done;
            ring.setAlphaF(float(0.9 * (1.0 - t)));
            painter.setPen(QPen(ring, 1.6));
            painter.setBrush(Qt::NoBrush);
            const double r = 3.5 + 9.0 * easeOut(t);
            painter.drawEllipse(at, r, r);
            QColor dot = done;
            dot.setAlphaF(float(1.0 - t));
            painter.setPen(Qt::NoPen);
            painter.setBrush(dot);
            painter.drawEllipse(at, 2.6, 2.6);
        }
    }
    painter.setOpacity(1.0);
}

void AgentActivityView::paintNetworkMeter(QPainter &painter)
{
    if (!m_network || !m_scene)
        return;
    // In and out for the whole field, beside the desktop's node: a bar for
    // each on a log scale, in the packets' own colours - cool coming in, warm
    // going out - and the rate beside it.
    const double down = m_scene->downRate();
    const double up = m_scene->upRate();
    QFont font = painter.font();
    font.setPointSizeF(qMax(7.0, font.pointSizeF() * 0.85));
    painter.setFont(font);
    const QFontMetricsF metrics(font);
    const double lineHeight = metrics.height();
    const QPointF node = flowEnd(Scene::kDesktop);
    const double barWidth = 64.0;
    // Clear of the ring of the desktop's own background work.
    const double left = node.x() + 44.0 * qMax(0.8, m_unit / 520.0);
    const double top = node.y() - lineHeight;
    const struct {
        const char *arrow;
        double rate;
        double hue;
    } rows[] = {{"↓ in", down, 0.52}, {"↑ out", up, 0.08}};
    for (int i = 0; i < 2; ++i) {
        const double y = top + i * lineHeight;
        const double level = qBound(0.0, std::log10(1.0 + rows[i].rate) / 7.0, 1.0);
        const QRectF track(left, y + lineHeight * 0.35, barWidth, lineHeight * 0.3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink(rows[i].hue, 0.3, 0.5, 0.18));
        painter.drawRoundedRect(track, 2, 2);
        if (level > 0.0) {
            painter.setBrush(ink(rows[i].hue, 0.9, 0.66, 0.9));
            painter.drawRoundedRect(QRectF(track.topLeft(), QSizeF(barWidth * level, track.height())),
                                    2, 2);
        }
        painter.setPen(rows[i].rate >= 1 ? ink(rows[i].hue, 0.8, 0.75, 0.95) : m_muted);
        painter.drawText(QPointF(left + barWidth + 6, y + metrics.ascent()),
                         QStringLiteral("%1 %2").arg(QString::fromUtf8(rows[i].arrow),
                                                     formatRate(rows[i].rate)));
    }
}
