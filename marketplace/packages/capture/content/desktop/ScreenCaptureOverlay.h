#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

class QScreen;
class PerScreenPanel;

class ScreenCaptureOverlay : public QObject
{
    Q_OBJECT
public:
    static ScreenCaptureOverlay *begin();
    void cancel();

#ifdef FORKMESH_WINDOW_TESTS
    static bool testPlatformNeedsPortal(const QString &platformName,
                                        const QByteArray &sessionType);
    static bool testPlatformCanUsePortal(const QString &platformName);
    static bool testPortalCaptureAllowed();
    static bool testLooksLikePortalScreenshot(const QString &fileName,
                                              const QString &softwareTag);
    static int testSweepPortalLitter(const QString &folder,
                                     const QSet<QString> &before,
                                     const QDateTime &since);
    void testStartDrag() { m_dragging = true; }
    void testPressEscape() { onEscape(); }
    bool testIsDragging() const { return m_dragging; }
    bool testIsDone() const { return m_done; }
#endif

signals:
    void captured(const QImage &image); // a non-empty region was selected
    void cancelled();                   // right-click or an empty drag

private slots:
    void onPortalResponse(uint response, const QVariantMap &results);

private:
    ScreenCaptureOverlay(const QRect &virtualGeom, qreal dpr,
                         const QList<QScreen *> &screens);

    void createPanels();

    void onPress(Qt::MouseButton button, QPoint globalPos);
    void onMove(QPoint globalPos);
    void onRelease(Qt::MouseButton button, QPoint globalPos);
    void onEscape();

    void beginCapture(const QRect &sel); // sel in virtual-desktop-relative coords
    QImage compositeScreens();           // X11/macOS: live composite of all screens
    QImage grabViaScreens(const QRect &sel);
    void grabViaPortal(const QRect &sel); // Wayland: XDG portal (async)
#ifdef FORKMESH_HAVE_PORTAL
    void subscribeToPortalRequest(const QString &requestPath);
#endif
    void settlePortalRequest(); // stop listening + delete what the portal wrote
    QImage cropDesktop(const QImage &full, const QRect &sel) const;
    void popOverrideCursor();
    void finish(const QImage &image);
    void maybeSelfDestruct();

    QRect m_virtualGeom;      // union of all screen geometries, in logical coords
    qreal m_dpr = 1.0;        // device-pixel ratio to grab/crop the snapshot at
    QImage m_frozen;          // desktop grabbed before the panels appeared; the
    QPoint m_originGlobal;    // drag start in global screen coords
    QPoint m_currentGlobal;   // latest drag point in global screen coords
    QRect m_pendingSel;       // selection awaiting an async (portal) capture
    bool m_dragging = false;
    bool m_cursorPushed = false;
    bool m_done = false;      // guards against emitting twice
    bool m_awaitingFreezeGrab = false; // portal pre-grab (Wayland) still pending
    bool m_portalInFlight = false;     // a portal request may still write a file
    int m_portalGeneration = 0;        // so a stale deadline can't close a new one
    QStringList m_portalHandles;       // Request paths we listen for a reply on
    QSet<QString> m_picturesBefore;    // Pictures folder before the portal ran
    QDateTime m_portalStarted;         // when the portal request was issued
    QList<QScreen *> m_screens; // screens to cover, captured at begin() time
    QVector<PerScreenPanel *> m_panels; // one per connected screen

    friend class PerScreenPanel;
};
