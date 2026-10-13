#pragma once

// The picture of the fleet: AgentActivityModel's field, painted.
//
// The layout says what each agent is doing: the hosts they talk to along the
// top - your own mirrors first, green servers that hold their seats while
// everything else comes and goes - then a row of agents at work, a row
// waiting on you and a row idle or finished, and the desktop they all run on
// at the foot. Each row reads left to right as the mix does, and an agent is
// drawn in the colour of the instrument it would be played on. Its body
// swells with the tokens it has burned and glows while it is working, and it
// holds still on its seat: nothing on the field moves unless a real event or number moved
// it, and nothing is decoration for its own sake. Every note it plays is a
// figure of its own off the body - a bolt for a command, a sweep for a search,
// a comet for your prompt, brackets for stream framing, a cog for a hook - and
// a ring of its recent history circles it.
//
// Each row of agents is named for what they are doing, each agent carries its
// name under it and each host its name under it; everything else is on the
// cursor, whose card ends with a key: every kind of mark the thing under it
// has made, its figure, what it stands for and how many. The one list is the
// sounds: every kind, what it stands for and what you
// hear, each row lighting up the moment a note of its kind sounds. Each row
// has two switches, its figure and its sound, so any kind can be hidden or
// silenced on its own.
//
// Plain QWidget (no moc), callbacks are std::function, like CodeMapView and
// StatsDashboard. The view owns no state but the hover and the animation
// clock: everything it draws comes from the Scene its owner feeds. Asked to
// (setGpuRendering), it paints through an OpenGL surface laid over itself and
// keeps up with the display's refresh rate; otherwise, and wherever there is
// no OpenGL, QPainter's raster engine paints it at thirty frames a second.

#include "AgentActivityModel.h"

#include <QBasicTimer>
#include <QCache>
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QRegion>
#include <QRectF>
#include <QWidget>

#include <functional>
#include <utility>

class QPainter;

class AgentActivityView : public QWidget
{
public:
    // What the field is drawn on. Theme follows the app; the others are night
    // skies of one kind or another, and draw the field's own marks lit.
    enum class Backdrop { Theme, Space, Aurora, Ocean };
    static QStringList backdropNames();
    enum class HostPlace { Machine, Mirror, Mesh, Ai, Git, Package, Cloud, Web };
    static HostPlace hostPlace(const QString &name, bool mirror);
    static QString hostPlaceLabel(HostPlace place);

    explicit AgentActivityView(forkmesh::agentvis::Scene *scene,
                               QWidget *parent = nullptr);

    // The list of sounds down the right-hand side: every kind, what it means,
    // and a glow on its row when one sounds.
    void setShowSounds(bool show);
    bool showSounds() const { return m_sounds; }
    // Where the list is drawn, empty when it is off or the window is too
    // narrow for it. For the tests.
    QRectF soundListRect() const { return m_listRect; }
    // Where one row of it is, for the tests.
    QRectF soundRowRect(forkmesh::agentvis::Motion motion) const;
    // 0..1: how lit a row is right now. Full for a note that just sounded, a
    // fainter glow for a mark of an agent nobody is listening to.
    double soundGlow(forkmesh::agentvis::Motion motion) const;

    // Each kind's figure can be hidden and its sound silenced on its own. The
    // view draws (or skips) the figures itself; the sound switch is only shown
    // here - the owner hears about the click through setOnToggleKind and
    // mutes the sonifier.
    void setMotionShown(forkmesh::agentvis::Motion motion, bool shown);
    bool motionShown(forkmesh::agentvis::Motion motion) const;
    void setMotionHeard(forkmesh::agentvis::Motion motion, bool heard);
    bool motionHeard(forkmesh::agentvis::Motion motion) const;
    // The two switches on a row of the list: the figure's, or (sound) the
    // note's. Empty when the list is not drawn.
    QRectF kindSwitchRect(forkmesh::agentvis::Motion motion, bool sound) const;
    // Told after a switch on the list was clicked, with its new state.
    void setOnToggleKind(std::function<void(forkmesh::agentvis::Motion, bool sound, bool on)> handler)
    {
        m_onToggleKind = std::move(handler);
    }
    // The links drawn between two agents on the same file.
    void setShowLinks(bool show);
    bool showLinks() const { return m_links; }
    // The ring of each agent's recent marks round its body.
    void setShowRings(bool show);
    bool showRings() const { return m_rings; }
    // Only the agents that are working - running, or active in the last half
    // minute. The rest fade out rather than vanish, and fade back in the
    // moment they do something.
    void setWorkingOnly(bool on);
    bool workingOnly() const { return m_workingOnly; }
    // 0..1: how present a body is on the screen right now, mid-fade included.
    double presence(int sessionId) const;
    void setBackdrop(Backdrop backdrop);
    Backdrop backdrop() const { return m_backdrop; }
    // Packets between agents and the hosts they talk to, in real time.
    void setShowNetwork(bool show);
    bool showNetwork() const { return m_network; }
    // Where a host sits along the top of the field, for the tests.
    QPointF hostPosition(const QString &host) const;
    QString hostAt(const QPointF &point) const;
    struct FlowHit {
        int sessionId = forkmesh::agentvis::Scene::kAnySession;
        QString host;
    };
    FlowHit flowAt(const QPointF &point) const;
    void setOnConnectionClicked(std::function<void(int sessionId, const QString &host)> handler)
    {
        m_onConnectionClicked = std::move(handler);
    }
    // The end of a flow, and where a source's own marks and sparks are
    // anchored: an agent's body, a mirror's seat, or the desktop's node.
    QPointF flowEnd(int sessionId) const;
    // Where a background task is drawn, round its agent or the desktop's
    // node. Null when it is not on screen.
    QPointF taskPosition(const forkmesh::agentvis::Task &task) const;
    // Where a relay client sits in the row left of the desktop's node; null
    // for one past the rows there is room for (counted, not drawn). An empty
    // id is the room itself, where frames nobody in the row sent come from.
    QPointF clientPosition(const QString &id) const;
    // The relay client or site visitor seated at this point, or empty.
    QString clientAt(const QPointF &point) const;
    // Told when a client in the row is clicked, with where on the screen: a
    // site visitor's click is the way to block it.
    void setOnClientClicked(std::function<void(const QString &, const QPoint &)> handler)
    {
        m_onClientClicked = std::move(handler);
    }
    // The font family that draws a pair of regional indicators as a flag,
    // or empty where none does (and a country's code is drawn instead).
    static QString flagFamily();
    // Every agent's process tree, fanning out from its body past its
    // background work: each process hangs off its parent, lit while it burns
    // CPU.
    void setShowProcesses(bool show);
    bool showProcesses() const { return m_processes; }
    // Where a process is drawn. Null when its agent is not on screen.
    QPointF processPosition(const forkmesh::agentvis::Process &process) const;

    // The desktop's own log, in the field with the agents: a column down the
    // left, newest first in each category's colour, and a spark off the agent
    // a line names (or off the desktop at the bottom). Clicking the column
    // asks for the full Logs page. Off until asked for: the one place the
    // window carries words without the cursor on them.
    void setShowLogs(bool show);
    bool showLogs() const { return m_logs; }
    void setHideOutput(bool hide);
    bool hideOutput() const { return m_hideOutput; }
    QRectF logRect() const { return m_logRect; }
    void setOnOpenLogs(std::function<void()> handler) { m_onOpenLogs = std::move(handler); }

    // A single click picks an agent out; a double click asks for it to be
    // heard as well as seen.
    void setOnSelect(std::function<void(int)> handler) { m_onSelect = std::move(handler); }
    void setOnToggleListen(std::function<void(int)> handler)
    {
        m_onToggleListen = std::move(handler);
    }

    // The fleet in one line: how many agents, how many of them are working
    // this second, and what the cursor is over. Not painted - the field has
    // no words - but it is the widget's accessible description.
    QString headline() const;
    // What the hover card says for whatever is under the cursor: an agent, a
    // row or switch in the list of sounds, a host, a link. Empty over bare
    // field.
    QStringList hoverLines() const;
    // The card's key: the kinds of mark the agent (or the desktop's node)
    // under the cursor has made, with how many of each, most first.
    QVector<std::pair<forkmesh::agentvis::Motion, int>> hoverKinds() const;

    int hoveredSession() const { return m_hover; }
    // Where a body landed, for the tests and for the window's own hit checks.
    QPointF bodyPosition(int sessionId) const;
    double bodyRadius(int sessionId) const;
    // The agent nearest this point, within reach of its body, or -1.
    int sessionAt(const QPointF &point) const;

    // An agent's face - the medallion the agent list shows for it - drawn as
    // its body, its number on top. `key` names what the icon is of, so an
    // unchanged face is not drawn again; an empty icon puts the plain disc
    // back.
    void setFace(int sessionId, const QString &key, const QIcon &face);
    bool hasFace(int sessionId) const { return m_faces.contains(sessionId); }

    // Paint on the GPU: an OpenGL surface covers the view and the field is
    // painted onto it, at the display's refresh rate rather than thirty
    // frames a second. Stays off where OpenGL is not built in or will not
    // start (the offscreen platform, a box with no driver, or
    // FORKMESH_AGENT_ACTIVITY_GPU=0); gpuRendering says which one it got.
    void setGpuRendering(bool on);
    bool gpuRendering() const { return m_surface != nullptr; }
    static bool gpuAvailable();
    // A small readout in the corner of the field: what it is painted on, the
    // frames painted in the last second and how long the last ones took.
    void setShowFrameRate(bool show);
    bool showFrameRate() const { return m_showFrameRate; }
    QString frameRateText() const;
    // Frames painted in the last second.
    int framesPerSecond() const;
    // How often the field is repainted while anything on it moves.
    int busyIntervalMs() const;

    QSize sizeHint() const override { return QSize(880, 560); }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void timerEvent(QTimerEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    class GpuSurface;
    // Everything paintEvent draws, onto whichever surface is painting: the
    // view itself, or the OpenGL one over it.
    void paintField(QPainter &painter);
    void paintFrameRate(QPainter &painter);
    // update(), sent to the OpenGL surface when that is what paints.
    void redraw();
    void applyTheme();
    void layoutField();
    // A quiet field holds still: nothing is painted while nothing moves
    // (Scene::moving), so a window left open all day costs nothing.
    void retimeAnimation();
    // Whether anything on screen is moving this frame.
    bool animating() const;
    QColor ink(double hue, double saturation, double lightness, double alpha = 1.0) const;
    QPointF pointFor(const forkmesh::agentvis::Body &body) const;
    double radiusFor(const forkmesh::agentvis::Body &body) const;
    // Each band's name over its rows.
    void paintRows(QPainter &painter);
    void paintJoins(QPainter &painter);
    void paintPulses(QPainter &painter);
    void paintBodies(QPainter &painter);
    // The face cut round at this radius, cached per face and pixel size.
    QPixmap faceArt(int sessionId, double radius, qreal ratio) const;
    void paintHistoryRing(QPainter &painter, const forkmesh::agentvis::Body &body,
                          const QPointF &centre, double radius);
    // What an agent last said to you, on a dot-matrix display under it. A
    // new reply holds a moment on its first words, then goes round and round
    // until the next one replaces it; a line that fits holds still.
    struct Led {
        QString source; // the reply it was drawn from, shared with the body's
        QString text;
        qreal ratio = 1;
        QPixmap strip; // every lit dot of the whole line
        double width = 0; // in pixels
    };
    const Led &ledFor(const forkmesh::agentvis::Body &body, qreal ratio) const;
    QRectF ledRect(const forkmesh::agentvis::Body &body, const QPointF &centre,
                   double radius) const;
    // How far the line has scrolled, in pixels: 0 at rest. `window` is the
    // width the display shows of it.
    double ledScroll(const forkmesh::agentvis::Body &body, double width, double window) const;
    void paintLed(QPainter &painter, const forkmesh::agentvis::Body &body,
                  const QPointF &centre, double radius);
    // Every display, over everything but the frame rate and the hover card.
    void paintLeds(QPainter &painter);
    // Whether a display is scrolling, or has a new line still to draw.
    bool ledsMoving() const;
    // Whether the displays are all that moves on the field.
    bool onlyLedsMoving() const;
    // A frame where only the displays move: the rest of the field comes off
    // m_still and only the displays are painted over it (and, painting in
    // software, only their boxes reach the screen). Skipped when no display
    // has moved a dot since the last one.
    void redrawLeds();
    // Everything under the displays: the whole field bar them, the frame
    // rate and the hover card.
    void paintLayers(QPainter &painter);
    // How often the displays are stepped: once a refresh of the screen.
    int ledIntervalMs() const;
    QRectF frameRateRect(const QFont &font, const QString &text) const;
    void paintSoundList(QPainter &painter);
    void paintLogs(QPainter &painter);
    void paintLogSparks(QPainter &painter);
    void paintNetwork(QPainter &painter);
    // Background work round whoever it runs for, and the whole field's
    // network in and out beside the desktop's node.
    void paintTasks(QPainter &painter);
    void paintProcesses(QPainter &painter);
    void paintNetworkMeter(QPainter &painter);
    void paintDesktopNode(QPainter &painter);
    // The relay room beside the desktop's node: web visitors, other desktops
    // and phones in a row, and the websocket's frames as beads between each
    // of them and the node.
    void paintClients(QPainter &painter);
    // The seat of the index'th client in the row, or null past the rows.
    QPointF clientSeat(int index) const;
    // A country's flag `height` pixels tall, cached per country and size: a
    // colour emoji rasterised every frame for a room of visitors is the
    // costliest glyph on the field.
    const QPixmap &flagArt(const QString &country, double height, qreal ratio) const;
    // A warning sign over a flagged visitor, a bar across a blocked one.
    void paintThreatSign(QPainter &painter, const QPointF &at, double r,
                         forkmesh::agentvis::Threat threat, bool blocked, double alpha) const;
    void paintBackdrop(QPainter &painter);
    // The whole backdrop, which holds still: painted once into m_sky and
    // blitted every frame after.
    void paintSky(QPainter &painter);
    void paintStars(QPainter &painter, double density, double brightness);
    // One note's figure off its body, `t` of the way through its life: the
    // same kind always draws the same, sized by the event's strength.
    void paintFigure(QPainter &painter, forkmesh::agentvis::Motion motion,
                     const QPointF &centre, double radius, double t, double angle,
                     double strength) const;
    // The same kind of note as a small still mark, for the list.
    void paintIcon(QPainter &painter, forkmesh::agentvis::Motion motion,
                   const QPointF &centre, double size, double alpha, double hue = -1.0) const;
    void paintPlace(QPainter &painter, HostPlace place, const QPointF &centre, double size,
                    double hue, double alpha) const;
    const QPixmap &markIcon(forkmesh::agentvis::Motion motion, double size, qreal ratio) const;
    void paintMark(QPainter &painter, forkmesh::agentvis::Motion motion, const QPointF &centre,
                   double size, double alpha) const;
    struct Pipe {
        QPointF from;
        QPointF to;
        QColor colour;
        double width = 1;
    };
    // The pipes, off a layer of their own that is only repainted when one
    // of them moves or changes: long faint strokes are the costliest thing
    // on the field to rasterise.
    void paintPipes(QPainter &painter, const QVector<Pipe> &pipes);
    QPointF flowControl(const QPointF &from, const QPointF &to) const;
    QPointF joinControl(const QPointF &a, const QPointF &b) const;
    void paintKindSwitch(QPainter &painter, const QRectF &box, bool sound, bool on,
                         double hue) const;
    void paintHoverCard(QPainter &painter);
    // A soft round glow: `colour` at the centre fading to nothing at
    // `radius`, what a two-stop radial gradient draws. It comes off a sprite
    // cached per size and colour and drawn at the colour's alpha: a gradient
    // works out every pixel it covers and rebuilds its colour table for every
    // colour it has not seen, and a glow that fades is a new colour every
    // frame.
    void drawGlow(QPainter &painter, const QPointF &centre, double radius,
                  const QColor &colour) const;
    // Eases every body's presence towards shown or hidden. True while any is
    // still on its way, which keeps the frame rate up for the fade.
    bool easePresence();
    // Whether the filter shows this body at all. The fade is only how it
    // looks; clicking and hovering follow this, so a newcomer is clickable at
    // once and a filtered-out one is not, mid-fade or not.
    bool shownNow(const forkmesh::agentvis::Body &body) const;

    forkmesh::agentvis::Scene *m_scene = nullptr;
    QBasicTimer m_timer;
    QWidget *m_surface = nullptr; // the OpenGL surface, while it paints
    bool m_showFrameRate = false;
    QElapsedTimer m_frameClock;
    QVector<qint64> m_frameTimes; // when each frame of the last second began, ns
    double m_paintMs = 0; // how long a frame takes to paint, smoothed
    int m_intervalMs = 0;
    bool m_paintedMoving = true;
    QRectF m_field;
    double m_unit = 560; // the smaller side of the field: every size scales off it
    int m_hover = -1;
    QPointF m_mouse;
    bool m_mouseInside = false;
    // Where the hover card was put when its subject was first hovered: it
    // stays there while the same thing is hovered, however the body drifts
    // or the lines change.
    QString m_cardSubject;
    QPointF m_cardAt;
    bool m_workingOnly = false;
    bool m_sounds = true;
    bool m_network = true;
    bool m_processes = true;
    bool m_logs = false;
    bool m_hideOutput = false;
    QRectF m_logRect;
    Backdrop m_backdrop = Backdrop::Theme;
    struct Star {
        double x, y, size, brightness, phase;
    };
    QVector<Star> m_stars; // made once, in 0..1 of the widget
    // drawGlow's sprites, by size and opaque colour, costed in KiB: a few
    // megabytes hold every glow a busy field draws.
    mutable QCache<quint64, QImage> m_glows{24 * 1024};
    QImage m_sky;
    QImage m_pipes;
    // The list's rows, each painted for the key it was painted for.
    QHash<int, std::pair<QString, QImage>> m_soundRows;
    QPointF m_pipesAt;
    QVector<Pipe> m_pipesDrawn;
    QString m_skyKey; // what m_sky was painted for: backdrop, size, theme
    QRectF m_listRect;
    bool m_links = true;
    bool m_rings = true;
    bool m_hidden[forkmesh::sonify::kMotionCount] = {};
    bool m_silenced[forkmesh::sonify::kMotionCount] = {};
    double m_rowHeight = 0;
    int m_rowLines = 1; // how much of each row there is room to write
    QHash<int, double> m_presence;
    QHash<int, std::pair<QString, QIcon>> m_faces;
    // Cut faces by "key|pixels", costed in KiB. The size is rounded, so a body
    // swelling with every event reuses a handful of them.
    mutable QCache<QString, QPixmap> m_faceArt{4 * 1024};
    mutable QHash<int, Led> m_leds; // by session: rebuilt when its words change
    mutable QHash<QString, QPixmap> m_flags; // by "country|pixels|ratio|theme"
    mutable QHash<QString, QPixmap> m_markIcons;
    mutable QPixmap m_ledDark;      // the unlit dots behind every display
    // The field bar its displays, while only they move: painted once and
    // blitted under every display frame after. Stale after any full frame,
    // and after kStillSeconds, so what changed without moving still shows.
    QImage m_still;
    bool m_stillStale = true;
    double m_stillSeconds = 0;
    bool m_ledFrame = false; // the next paint is a display frame
    QRegion m_ledRegion;     // what a display frame repaints, in software
    size_t m_ledSeen = 0;    // where every display was when last painted
    double m_presenceClock = -1;
    bool m_fading = false;
    bool m_dark = true;
    QColor m_background;
    QColor m_foreground;
    QColor m_muted;
    QColor m_guide;
    std::function<void(int)> m_onSelect;
    std::function<void(int)> m_onToggleListen;
    std::function<void()> m_onOpenLogs;
    std::function<void(forkmesh::agentvis::Motion, bool, bool)> m_onToggleKind;
    std::function<void(const QString &, const QPoint &)> m_onClientClicked;
    std::function<void(int, const QString &)> m_onConnectionClicked;
};
