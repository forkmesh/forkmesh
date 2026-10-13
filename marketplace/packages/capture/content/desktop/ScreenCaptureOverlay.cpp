#include "ScreenCaptureOverlay.h"

#include <QByteArray>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QRandomGenerator>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#ifdef FORKMESH_HAVE_PORTAL
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#endif

namespace {

bool platformNeedsWaylandPortal(const QString &platformName,
                                const QByteArray &sessionType)
{
    if (platformName.startsWith(QLatin1String("wayland"),
                                Qt::CaseInsensitive))
        return true;

    // XWayland presents to Qt as xcb even though the compositor still blocks
    // direct desktop grabs. Only use the ambient session type for that backend:
    // offscreen tests inherit XDG_SESSION_TYPE from the developer's desktop and
    // must never be allowed to invoke the real screenshot portal.
    return platformName.compare(QLatin1String("xcb"), Qt::CaseInsensitive) == 0 &&
           sessionType.compare("wayland", Qt::CaseInsensitive) == 0;
}

bool runningOnWayland()
{
    return platformNeedsWaylandPortal(QGuiApplication::platformName(),
                                      qgetenv("XDG_SESSION_TYPE"));
}

// The single gate every portal entry point passes through. Taking a screenshot
// is a visible, side-effecting act on the user's real desktop — it flashes the
// screen, raises a permission dialog, and (see sweepPortalLitter) drops a PNG
// into their Pictures folder. Only the two platform plugins that drive a real
// session may ask for one: offscreen/minimal test runs and headless nodes have
// no desktop worth grabbing and must never reach the portal, no matter which
// fallback path led them here.
bool platformCanUsePortal(const QString &platformName)
{
    return platformName.startsWith(QLatin1String("wayland"), Qt::CaseInsensitive) ||
           platformName.compare(QLatin1String("xcb"), Qt::CaseInsensitive) == 0;
}

bool portalCaptureAllowed()
{
#ifdef FORKMESH_WINDOW_TESTS
    // Only the shipped app may ever ask for a real screenshot. The test
    // binaries link these same sources and get launched straight from a
    // developer's desktop session, where the platform check below would happily
    // say yes — and a suite that fires the portal flashes the screen, raises a
    // permission dialog, and drops a full-desktop PNG in the developer's
    // Pictures folder. Refuse at compile time rather than trust an env var to
    // be set on every possible way of running them.
    return false;
#else
    if (qEnvironmentVariableIntValue("FORKMESH_NO_SCREENSHOT_PORTAL") != 0)
        return false; // escape hatch for anyone scripting the app headlessly
    return platformCanUsePortal(QGuiApplication::platformName());
#endif
}

QString picturesFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
}

// True when `fileName` is a screenshot some portal backend wrote, rather than a
// picture of the user's own. Both halves are needed: GNOME tags its PNGs with a
// Software chunk, other backends only name the file.
bool looksLikePortalScreenshot(const QString &fileName, const QString &softwareTag)
{
    if (!fileName.endsWith(QLatin1String(".png"), Qt::CaseInsensitive))
        return false;
    for (const QLatin1String &writer : {QLatin1String("screenshot"),
                                        QLatin1String("gnome-shell"),
                                        QLatin1String("xdg-desktop-portal")}) {
        if (softwareTag.contains(writer, Qt::CaseInsensitive))
            return true;
    }
    return fileName.startsWith(QLatin1String("screenshot"), Qt::CaseInsensitive);
}

QSet<QString> picturesFolderContents()
{
    const QString folder = picturesFolder();
    if (folder.isEmpty())
        return {};
    const QStringList names =
        QDir(folder).entryList(QStringList{QStringLiteral("*.png")}, QDir::Files);
    return QSet<QString>(names.cbegin(), names.cend());
}

// The portal writes the screenshot to disk and hands the caller its URI — and
// GNOME writes it *before* asking whether to share it, so a denied, cancelled,
// or unanswered request leaves a full-desktop grab in ~/Pictures that no reply
// ever names. Every request snapshots the folder first and sweeps afterwards,
// whatever the outcome, so nothing we caused is left behind.
//
// Deliberately narrow: only PNGs that appeared during the request, only at the
// top level (the user's own Print-Screen shots land in Pictures/Screenshots/),
// and only ones a screenshot tool wrote. Returns how many it removed.
int sweepPortalLitter(const QString &folder, const QSet<QString> &before,
                      const QDateTime &since)
{
    if (folder.isEmpty() || !since.isValid())
        return 0;
    // Filesystem timestamps are coarser than the clock on some mounts; a file
    // stamped a moment before the snapshot could still be ours.
    const QDateTime cutoff = since.addSecs(-2);
    int removed = 0;
    const QFileInfoList entries =
        QDir(folder).entryInfoList(QStringList{QStringLiteral("*.png")},
                                   QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo &entry : entries) {
        if (before.contains(entry.fileName()))
            continue; // already the user's before we asked for anything
        if (entry.lastModified() < cutoff)
            continue;
        QImageReader reader(entry.absoluteFilePath());
        if (!looksLikePortalScreenshot(entry.fileName(),
                                       reader.text(QStringLiteral("Software"))))
            continue;
        if (QFile::remove(entry.absoluteFilePath()))
            ++removed;
    }
    return removed;
}

bool looksLikeFailedGrab(const QImage &img)
{
    if (img.isNull())
        return true;
    const int cols = qMin(img.width(), 48);
    const int rows = qMin(img.height(), 48);
    if (cols <= 0 || rows <= 0)
        return true;
    for (int iy = 0; iy < rows; ++iy) {
        const int y = (iy * (img.height() - 1)) / qMax(1, rows - 1);
        for (int ix = 0; ix < cols; ++ix) {
            const int x = (ix * (img.width() - 1)) / qMax(1, cols - 1);
            const QRgb px = img.pixel(x, y);
            if (qRed(px) != 0 || qGreen(px) != 0 || qBlue(px) != 0)
                return false;
        }
    }
    return true;
}

QCursor makeSnipCursor()
{
    constexpr int n = 32;       // logical cursor size
    constexpr int c = n / 2;    // centre / hotspot
    constexpr int gap = 3;      // clear space around the exact point
    constexpr int arm = c - 1;  // how far each arm reaches

    QPixmap pm(n, n);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, false);
    const auto drawCross = [&](const QColor &col, int w) {
        QPen pen(col);
        pen.setWidth(w);
        p.setPen(pen);
        p.drawLine(c - arm, c, c - gap, c); // left arm
        p.drawLine(c + gap, c, c + arm, c); // right arm
        p.drawLine(c, c - arm, c, c - gap); // top arm
        p.drawLine(c, c + gap, c, c + arm); // bottom arm
    };
    drawCross(QColor(0, 0, 0, 200), 3); // halo
    drawCross(QColor(255, 255, 255), 1); // core
    p.end();
    return QCursor(pm, c, c);
}

} // namespace

class PerScreenPanel : public QWidget
{
public:
    PerScreenPanel(ScreenCaptureOverlay *owner, const QRect &screenGeom,
                   const QCursor &cursor)
        : QWidget(nullptr), m_owner(owner), m_screenGeom(screenGeom)
    {
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint |
                       Qt::WindowStaysOnTopHint | Qt::BypassWindowManagerHint);
        setAttribute(Qt::WA_DeleteOnClose);
        if (owner->m_frozen.isNull())
            setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);
        setGeometry(screenGeom);
        setCursor(cursor);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        if (!m_owner->m_frozen.isNull()) {
            const QImage &frozen = m_owner->m_frozen;
            const QRect &vg = m_owner->m_virtualGeom;
            const double sx = double(frozen.width()) / vg.width();
            const double sy = double(frozen.height()) / vg.height();
            painter.drawImage(rect(), frozen,
                              QRectF((m_screenGeom.x() - vg.x()) * sx,
                                     (m_screenGeom.y() - vg.y()) * sy,
                                     m_screenGeom.width() * sx,
                                     m_screenGeom.height() * sy));
        } else {
            painter.fillRect(rect(), QColor(0, 0, 0, 1));
        }
        const QColor shade(0, 0, 0, 90);
        if (!m_owner->m_dragging) {
            painter.fillRect(rect(), shade);
            return;
        }
        const QPoint origin  = m_owner->m_originGlobal  - m_screenGeom.topLeft();
        const QPoint current = m_owner->m_currentGlobal - m_screenGeom.topLeft();
        const QRect sel = QRect(origin, current).normalized();
        if (sel.isNull()) {
            painter.fillRect(rect(), shade);
            return;
        }
        for (const QRect &r : QRegion(rect()) - QRegion(sel))
            painter.fillRect(r, shade);
        QPen pen(QColor("#2f81f7"));
        pen.setWidth(2);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(sel.adjusted(0, 0, -1, -1));
        if (m_screenGeom.contains(m_owner->m_currentGlobal)) {
            const QRect globalSel =
                QRect(m_owner->m_originGlobal, m_owner->m_currentGlobal).normalized();
            const QString label = QStringLiteral("%1 × %2")
                                      .arg(globalSel.width())
                                      .arg(globalSel.height());
            const QFontMetrics fm = painter.fontMetrics();
            QRect box = fm.boundingRect(label).adjusted(-6, -3, 6, 3);
            box.moveTopLeft(sel.topLeft() + QPoint(0, -box.height() - 6));
            if (box.top() < 0) // selection touches the top edge: drop inside
                box.moveTopLeft(sel.topLeft() + QPoint(6, 6));
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 180));
            painter.drawRoundedRect(box, 4, 4);
            painter.setPen(Qt::white);
            painter.drawText(box, Qt::AlignCenter, label);
        }
    }

    QPoint globalFromLocal(const QPointF &local) const
    {
        return m_screenGeom.topLeft() + local.toPoint();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_owner->onPress(event->button(), globalFromLocal(event->position()));
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        m_owner->onMove(globalFromLocal(event->position()));
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        m_owner->onRelease(event->button(), globalFromLocal(event->position()));
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            m_owner->onEscape();
            event->accept();
        } else {
            QWidget::keyPressEvent(event);
        }
    }

private:
    ScreenCaptureOverlay *m_owner;
    QRect m_screenGeom; // this screen's position in global coords
};

ScreenCaptureOverlay *ScreenCaptureOverlay::begin()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty())
        return nullptr;

    QRect virtualGeom;
    for (QScreen *s : screens)
        virtualGeom = virtualGeom.united(s->geometry());
    if (virtualGeom.isEmpty())
        return nullptr;

    const qreal dpr = QGuiApplication::primaryScreen()
                          ? QGuiApplication::primaryScreen()->devicePixelRatio()
                          : 1.0;

    return new ScreenCaptureOverlay(virtualGeom, dpr, screens);
}

#ifdef FORKMESH_WINDOW_TESTS
bool ScreenCaptureOverlay::testPlatformNeedsPortal(
    const QString &platformName, const QByteArray &sessionType)
{
    return platformNeedsWaylandPortal(platformName, sessionType);
}

bool ScreenCaptureOverlay::testPlatformCanUsePortal(const QString &platformName)
{
    return platformCanUsePortal(platformName);
}

bool ScreenCaptureOverlay::testPortalCaptureAllowed()
{
    return portalCaptureAllowed();
}

bool ScreenCaptureOverlay::testLooksLikePortalScreenshot(
    const QString &fileName, const QString &softwareTag)
{
    return looksLikePortalScreenshot(fileName, softwareTag);
}

int ScreenCaptureOverlay::testSweepPortalLitter(const QString &folder,
                                                const QSet<QString> &before,
                                                const QDateTime &since)
{
    return sweepPortalLitter(folder, before, since);
}
#endif

void ScreenCaptureOverlay::cancel()
{
    finish(QImage());
}

ScreenCaptureOverlay::ScreenCaptureOverlay(const QRect &virtualGeom, qreal dpr,
                                            const QList<QScreen *> &screens)
    : QObject(nullptr), m_virtualGeom(virtualGeom), m_dpr(dpr), m_screens(screens)
{
    if (!runningOnWayland()) {
        const QImage shot = compositeScreens();
        if (!looksLikeFailedGrab(shot))
            m_frozen = shot;
    }
#ifdef FORKMESH_HAVE_PORTAL
    else if (portalCaptureAllowed()) {
        m_awaitingFreezeGrab = true;
        grabViaPortal(QRect());
        return;
    }
#endif

    createPanels();
}

void ScreenCaptureOverlay::createPanels()
{
    const QCursor snip = makeSnipCursor();
    QGuiApplication::setOverrideCursor(snip);
    m_cursorPushed = true;

    for (QScreen *s : m_screens) {
        auto *panel = new PerScreenPanel(this, s->geometry(), snip);
        m_panels.append(panel);
        panel->setScreen(s);
        if (runningOnWayland())
            panel->showFullScreen();
        else
            panel->show();
        panel->raise();
    }
    if (!m_panels.isEmpty()) {
        m_panels.first()->activateWindow();
        m_panels.first()->setFocus();
    }
}

void ScreenCaptureOverlay::popOverrideCursor()
{
    if (!m_cursorPushed)
        return;
    m_cursorPushed = false;
    QGuiApplication::restoreOverrideCursor();
}

void ScreenCaptureOverlay::onPress(Qt::MouseButton button, QPoint globalPos)
{
    if (button != Qt::LeftButton) {
        finish(QImage()); // right/middle click cancels
        return;
    }
    m_dragging = true;
    m_originGlobal = globalPos;
    m_currentGlobal = globalPos;
    for (auto *p : m_panels)
        p->update();
}

void ScreenCaptureOverlay::onMove(QPoint globalPos)
{
    if (!m_dragging)
        return;
    m_currentGlobal = globalPos;
    for (auto *p : m_panels)
        p->update();
}

void ScreenCaptureOverlay::onRelease(Qt::MouseButton button, QPoint globalPos)
{
    if (!m_dragging || button != Qt::LeftButton)
        return;
    m_dragging = false;
    m_currentGlobal = globalPos;
    const QPoint a = m_originGlobal  - m_virtualGeom.topLeft();
    const QPoint b = m_currentGlobal - m_virtualGeom.topLeft();
    const QRect sel = QRect(a, b).normalized();
    if (sel.width() < 3 || sel.height() < 3) {
        finish(QImage()); // a click without a real drag is a cancel
        return;
    }
    beginCapture(sel);
}

void ScreenCaptureOverlay::onEscape()
{
    // Escape is commonly pressed to get out of a transient tool or selection,
    // but ending Capture here throws away the whole in-progress workflow. Keep
    // the overlay alive and only reset a drag that has not been released yet.
    // Right/middle click remains the explicit way to discard the region step.
    if (!m_dragging)
        return;
    m_dragging = false;
    m_originGlobal = QPoint();
    m_currentGlobal = QPoint();
    for (auto *panel : m_panels)
        panel->update();
}

void ScreenCaptureOverlay::beginCapture(const QRect &sel)
{
    if (!m_frozen.isNull()) {
        finish(cropDesktop(m_frozen, sel));
        return;
    }

    popOverrideCursor();
    for (auto *p : m_panels)
        p->hide();

#ifdef FORKMESH_HAVE_PORTAL
    if (runningOnWayland()) {
        m_pendingSel = sel;
        QTimer::singleShot(150, this, [this] { grabViaPortal(m_pendingSel); });
        return;
    }

    const QImage desktop = compositeScreens();
    // A black grab means the compositor refused us — but only a real desktop
    // session may answer that by asking the portal. An offscreen test run grabs
    // black every time, and firing the portal there would take a screenshot of
    // the developer's actual screen and leave it in their Pictures folder.
    if (looksLikeFailedGrab(desktop) && portalCaptureAllowed()) {
        m_pendingSel = sel;
        QTimer::singleShot(150, this, [this] { grabViaPortal(m_pendingSel); });
        return;
    }
    finish(cropDesktop(desktop, sel));
#else
    finish(grabViaScreens(sel));
#endif
}

QImage ScreenCaptureOverlay::cropDesktop(const QImage &full, const QRect &sel) const
{
    if (full.isNull())
        return QImage();
    const double sx = double(full.width()) / m_virtualGeom.width();
    const double sy = double(full.height()) / m_virtualGeom.height();
    QRect dev(qRound(sel.x() * sx), qRound(sel.y() * sy),
              qRound(sel.width() * sx), qRound(sel.height() * sy));
    dev = dev.intersected(full.rect());
    if (dev.isEmpty())
        return QImage();
    return full.copy(dev);
}

QImage ScreenCaptureOverlay::compositeScreens()
{
    QGuiApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);

    QPixmap shot(m_virtualGeom.size() * m_dpr);
    shot.setDevicePixelRatio(m_dpr);
    shot.fill(Qt::black);
    {
        QPainter painter(&shot);
        for (QScreen *s : QGuiApplication::screens()) {
            QPixmap grab = s->grabWindow(0);
            const QRect g = s->geometry();
            const QSize native(qRound(g.width() * s->devicePixelRatio()),
                               qRound(g.height() * s->devicePixelRatio()));
            if (grab.width() > native.width() + 2 ||
                grab.height() > native.height() + 2) {
                const qreal rx = qreal(grab.width()) / m_virtualGeom.width();
                const qreal ry = qreal(grab.height()) / m_virtualGeom.height();
                const QRect slice(qRound((g.x() - m_virtualGeom.x()) * rx),
                                  qRound((g.y() - m_virtualGeom.y()) * ry),
                                  qRound(g.width() * rx),
                                  qRound(g.height() * ry));
                const QRect bounded = slice.intersected(grab.rect());
                if (!bounded.isEmpty())
                    grab = grab.copy(bounded);
            }
            grab.setDevicePixelRatio(s->devicePixelRatio());
            painter.drawPixmap(QPointF(g.x() - m_virtualGeom.x(),
                                       g.y() - m_virtualGeom.y()),
                               grab);
        }
    }
    return shot.toImage();
}

QImage ScreenCaptureOverlay::grabViaScreens(const QRect &sel)
{
    return cropDesktop(compositeScreens(), sel);
}

void ScreenCaptureOverlay::grabViaPortal(const QRect &sel)
{
#ifdef FORKMESH_HAVE_PORTAL
    // Last line of defence. Every caller checks too, but this is the one place
    // a real screenshot can be taken, so it refuses outright rather than trust
    // that some future fallback path remembered to ask.
    if (!portalCaptureAllowed()) {
        if (m_awaitingFreezeGrab) {
            m_awaitingFreezeGrab = false;
            createPanels(); // no frozen desktop; drag over the live one
            return;
        }
        finish(grabViaScreens(sel));
        return;
    }

    // Never two requests at once. The 8s overlay timeout hands control back to
    // the user while the freeze grab is still outstanding (a permission dialog
    // they haven't answered yet), and the selection they then drag would ask
    // again. Two live requests share one before-snapshot and one reply slot, so
    // the second would redefine "what was already in Pictures" to include the
    // first one's screenshot — stranding it there permanently. Wait the first
    // one out instead; a degraded capture is worth more than a leaked file.
    if (m_portalInFlight) {
        finish(grabViaScreens(sel));
        return;
    }

    QDBusConnection bus = QDBusConnection::sessionBus();

    const QString token =
        QStringLiteral("forkmesh_%1").arg(QRandomGenerator::global()->generate());
    QString sender = bus.baseService(); // ":1.xx"
    if (sender.startsWith(QLatin1Char(':')))
        sender.remove(0, 1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString reqPath =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
            .arg(sender, token);

    // Record the folder the portal will drop the PNG into *before* asking, so
    // settlePortalRequest can tell its litter from the user's own pictures.
    m_portalInFlight = true;
    const int generation = ++m_portalGeneration;
    m_portalStarted = QDateTime::currentDateTime();
    m_picturesBefore = picturesFolderContents();
    subscribeToPortalRequest(reqPath);

    QVariantMap opts;
    opts.insert(QStringLiteral("handle_token"), token);
    opts.insert(QStringLiteral("interactive"), false); // grab the whole desktop
    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Screenshot"),
        QStringLiteral("Screenshot"));
    call << QString() << opts; // parent_window (none) + options
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher *w) {
                w->deleteLater();
                if (m_portalGeneration != generation || !m_portalInFlight)
                    return; // this request is already closed out
                const QDBusMessage reply = w->reply();
                if (reply.type() == QDBusMessage::ErrorMessage) {
                    settlePortalRequest(); // no portal, so nothing was written
                    return;
                }
                // The handle we guessed from the token is what the spec tells
                // callers to subscribe to, but the reply names the real one.
                // Subscribe to that too, or a backend that hands back a
                // different path would never reach onPortalResponse and its
                // screenshot would sit in the user's Pictures folder forever.
                const QString handle =
                    reply.arguments().value(0).value<QDBusObjectPath>().path();
                if (!handle.isEmpty())
                    subscribeToPortalRequest(handle);
            });

    // Safety net: never leave the overlay alive forever if the portal (or a
    // stuck permission dialog) never answers. For the pre-capture freeze grab
    // the user hasn't selected anything yet, so fall back to the live overlay
    // instead of silently cancelling the tool.
    const bool freezePhase = m_awaitingFreezeGrab;
    QTimer::singleShot(8000, this, [this, freezePhase] {
        if (m_done)
            return;
        if (freezePhase) {
            if (m_awaitingFreezeGrab) {
                m_awaitingFreezeGrab = false;
                createPanels();
            }
            return; // the freeze grab already resolved; a selection is underway
        }
        finish(QImage());
    });

    // Giving up on the *overlay* after 8s must not mean giving up on the file:
    // a permission dialog the user takes a minute to answer still writes one.
    // Stay subscribed well past the visible timeout, then sweep regardless.
    QTimer::singleShot(120000, this, [this, generation] {
        if (m_portalGeneration == generation)
            settlePortalRequest();
    });
#else
    finish(grabViaScreens(sel));
#endif
}

#ifdef FORKMESH_HAVE_PORTAL
void ScreenCaptureOverlay::subscribeToPortalRequest(const QString &requestPath)
{
    if (requestPath.isEmpty() || m_portalHandles.contains(requestPath))
        return;
    m_portalHandles.append(requestPath);
    QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.portal.Desktop"), requestPath,
        QStringLiteral("org.freedesktop.portal.Request"),
        QStringLiteral("Response"), this,
        SLOT(onPortalResponse(uint, QVariantMap)));
}
#endif

// Closes out a portal request: stops listening, then removes anything the
// portal left in the user's Pictures folder. Called on every outcome — reply,
// D-Bus error, or the long deadline — because a screenshot we caused is our
// litter whether or not we were ever handed it.
void ScreenCaptureOverlay::settlePortalRequest()
{
    if (!m_portalInFlight)
        return;
    m_portalInFlight = false;
#ifdef FORKMESH_HAVE_PORTAL
    for (const QString &requestPath : m_portalHandles) {
        QDBusConnection::sessionBus().disconnect(
            QStringLiteral("org.freedesktop.portal.Desktop"), requestPath,
            QStringLiteral("org.freedesktop.portal.Request"),
            QStringLiteral("Response"), this,
            SLOT(onPortalResponse(uint, QVariantMap)));
    }
#endif
    m_portalHandles.clear();
    sweepPortalLitter(picturesFolder(), m_picturesBefore, m_portalStarted);
    m_picturesBefore.clear();
    maybeSelfDestruct();
}

void ScreenCaptureOverlay::onPortalResponse(uint response, const QVariantMap &results)
{
    QImage full;
    // Delete the file on *every* outcome, not just success: the backend writes
    // it before it asks the user whether to share it, so a refusal (response 1)
    // still leaves one on disk with our name on it.
    const QString path =
        QUrl(results.value(QStringLiteral("uri")).toString()).toLocalFile();
    if (!path.isEmpty()) {
        // Decode it only if someone is still waiting for it. A cancelled
        // request (1) or one that ended some other way (2), and an overlay that
        // already gave up, all want the same thing: the file gone, not a
        // full-desktop image decoded to throw away.
        if (response == 0 && !m_done)
            full = QImage(path);
        QFile::remove(path); // the portal hands the file over; we own it now
    }
    settlePortalRequest();
    if (m_done)
        return; // the overlay already gave up; the file was all we came back for

    if (m_awaitingFreezeGrab) {
        m_awaitingFreezeGrab = false;
        if (response == 1) {
            finish(QImage());
            return;
        }
        if (!looksLikeFailedGrab(full))
            m_frozen = full;
        createPanels();
        return;
    }

    if (response != 0) {
        finish(QImage());
        return;
    }
    finish(cropDesktop(full, m_pendingSel));
}

void ScreenCaptureOverlay::finish(const QImage &image)
{
    if (m_done)
        return;
    m_done = true;
    popOverrideCursor();
    const auto panels = m_panels;
    m_panels.clear();
    for (auto *p : panels)
        p->close(); // WA_DeleteOnClose frees each panel
    if (image.isNull())
        emit cancelled();
    else
        emit captured(image);
    // Cleared only after the emit, which may be holding a reference to it. An
    // overlay waiting out a portal request can outlive its answer by a couple
    // of minutes, and a frozen desktop is a big image to sit on meanwhile.
    m_frozen = QImage();
    maybeSelfDestruct();
}

// The overlay is the only thing still listening for the portal's reply, and
// that reply is how we learn which file to delete. A right-click or the 8s
// timeout therefore hides the panels and hands the caller its answer,
// but keeps the coordinator alive (invisible and inert) until the request it
// started has been settled — otherwise a late screenshot outlives us in the
// user's Pictures folder with nobody left to clean it up.
void ScreenCaptureOverlay::maybeSelfDestruct()
{
    if (!m_done || m_portalInFlight)
        return;
    deleteLater(); // free the coordinator after the current call stack unwinds
}
