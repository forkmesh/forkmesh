#pragma once

// The repository code map (adhoc #187): every file of a commit as a zoomable
// treemap with the real source drawn inside each cell — a density minimap from
// far away, highlighted readable text up close — and a rotatable 3D view of the
// same layout in which a folder's height follows its size.
//
// Both views are painted with QPainter. The 3D one is a small software
// renderer rather than OpenGL on purpose: the scene is nothing but axis-aligned
// boxes standing on a guillotine partition, for which CodeMapModel can hand out
// an exact back-to-front order, so a painter's algorithm is correct and the
// desktop needs no extra Qt module on any platform.
//
// Plain QWidget (no moc): callbacks are std::function, like StatsDashboard and
// ComponentHealthBoard. The view never touches git; its owner answers
// onWantContent() with setContent() calls and one finishContentBatch().

#include "CodeMapModel.h"

#include <QBasicTimer>
#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QPolygonF>
#include <QSet>
#include <QVector>
#include <QVector3D>
#include <QWidget>

#include <functional>

class QPainter;

class CodeMapView : public QWidget
{
public:
    enum class Mode { Flat, City };
    struct Wording {
        QString whole = QStringLiteral("repository");
        QString folder = QStringLiteral("folder");
        QString leaf = QStringLiteral("file");
    };

    explicit CodeMapView(QWidget *parent = nullptr);
    ~CodeMapView() override;

    void setTree(forkmesh::codemap::Tree tree);
    const forkmesh::codemap::Tree &tree() const { return m_tree; }

    void setWording(const Wording &wording);
    void setCategoryColors(const QHash<QString, QColor> &colors);

    void setMode(Mode mode);
    Mode mode() const { return m_mode; }

    int focusNode() const { return m_focus; }
    void goTo(int node, bool animate = true);
    bool goToPath(const QString &path);
    // Steps out to the focus's parent. False when already at the root.
    bool zoomOut();
    void zoomBy(double factor);
    void resetView();

    // Highlights every file whose path contains `text`; returns the count.
    int setSearch(const QString &text);
    void visitMatch(bool backwards);

    // The owner's reply to onWantContent(): bytes == nullptr marks the file
    // unavailable. finishContentBatch() lets the next batch go out.
    // An image file's bytes may instead arrive already decoded (the owner can
    // run decodePreviewImage off the GUI thread); setContent decodes them
    // itself otherwise.
    void setContent(const QString &path, const QByteArray *bytes);
    void setPreview(const QString &path, const QImage &image, const QSize &naturalSize);
    void finishContentBatch();

    // Decodes picture bytes into an image at most kPreviewMaxSide on its longer
    // side; null when they do not decode. Reentrant: safe on a worker thread.
    static QImage decodePreviewImage(const QByteArray &bytes, const QString &fileName, QSize *naturalSize);

    void setOnWantContent(std::function<void(const QStringList &)> handler) { m_onWantContent = std::move(handler); }
    void setOnFocusChanged(std::function<void(int)> handler) { m_onFocusChanged = std::move(handler); }
    void setOnOpenFile(std::function<void(const QString &)> handler) { m_onOpenFile = std::move(handler); }
    void setOnCloseRequested(std::function<void()> handler) { m_onCloseRequested = std::move(handler); }
    void setOnLoadingChanged(std::function<void(int)> handler) { m_onLoadingChanged = std::move(handler); }

    QColor languageColor(const QString &language) const;
    QColor otherColor() const { return m_theme.gray; }
    QString describeSize(int node) const;

    // Read-only numbers for tests and diagnostics.
    struct Stats {
        int files = 0;
        int loadedFiles = 0;
        int previews = 0;       // images and font specimens held
        double scale = 0;
        int boxesDrawn = 0;
        int texturesDrawn = 0;
        int labelsDrawn = 0;
        quint64 frames = 0; // paints so far; an idle view must stop counting
    };
    Stats stats() const;
    // The node under a widget position in the current mode, or -1.
    int nodeAt(const QPoint &position) const;
    // Where a node's cell sits on screen in the flat view.
    QRectF screenRect(int node) const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void timerEvent(QTimerEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    using Node = forkmesh::codemap::Node;
    using Code = forkmesh::codemap::Code;

    struct Theme {
        bool dark = true;
        QColor background, tile, foreground, muted, accent, gray, pill;
        QColor hues[forkmesh::codemap::kHueSlots];
        QColor code[7];
    };
    struct FlatView {
        double scale = 1;
        double tx = 0;
        double ty = 0;
    };
    struct Orbit {
        QVector3D target;
        double radius = 1;
        double theta = -0.38;
        double phi = 0.9;
    };
    struct Camera {
        QVector3D position, right, up, forward;
        double focal = 1;
        double nearPlane = 0.1;
    };
    struct Face {
        QPolygonF polygon;
        QRectF bounds;
        int node = -1;
    };
    struct Label {
        QPointF at;
        QString text;
        double weight = 0;
    };
    struct CityRequest {
        int node = -1;
        double want = 0;
        double visibleArea = 0;
    };
    struct CachedBitmap {
        QPixmap pixmap;
        qint64 cost = 0;
        quint64 used = 0;
    };
    struct Picture {
        QPixmap pixmap;
        QSize natural; // empty for a font specimen
        qint64 cost = 0;
    };

    void applyTheme();
    void describeForAccessibility();
    QColor nodeColor(const Node &node) const;
    QColor directoryFill(int depth) const;

    // Flat view.
    FlatView fitFor(int node, double margin) const;
    void clampFlatView();
    void zoomFlatAt(const QPointF &point, double factor);
    void paintFlat(QPainter &painter);
    void paintFlatNode(QPainter &painter, int index);
    void paintFlatCode(QPainter &painter, int index, const QRectF &cell);
    void paintLiveCode(QPainter &painter, Code &code, const QPointF &origin, double lineHeight,
                       const QRectF &clip);
    void paintFlatLabels(QPainter &painter);
    void outlineFlat(QPainter &painter, int index, const QColor &color, double width);
    int flatNodeAt(const QPointF &point, double minimumPixels) const;
    void clickFlat(const QPointF &point);
    void deriveFlatFocus();

    // City view.
    Camera camera() const;
    void clampOrbit();
    void paintCity(QPainter &painter);
    void paintCityNode(QPainter &painter, const Camera &camera, int index);
    bool projectFace(const Camera &camera, const QVector3D corners[4], QPolygonF *out,
                     bool *clipped) const;
    void paintCityTexture(QPainter &painter, int index, const Camera &camera, const QRectF &ground,
                          double elevation, double pixelsPerUnit, double visibleArea);
    void paintCityQuad(QPainter &painter, const QPixmap &bitmap, const Camera &camera,
                       const QRectF &ground, double elevation);
    double idealCityLevel(const Node &node, double want) const;
    bool assignCityLevels();
    void paintCityLabels(QPainter &painter);
    int cityNodeAt(const QPointF &point) const;
    bool groundPoint(const Camera &camera, const QPointF &screen, QVector3D *out) const;
    void dolly(double factor, const QPointF *toward);
    void flyTo(int node, bool animate);

    // Code bitmaps.
    QPixmap bitmapFor(int index, double level, bool build, bool *pending);
    QPixmap anyBitmapFor(int index) const;
    QSize bitmapSize(const Node &node, double level) const;
    bool mayBuildBitmap() const;
    double closeupLevel(const Node &node, double want) const;
    QFont monoFont(double characterWidth, double lineHeight) const;

    // Pictures: decoded images, font specimens and the shared type cards.
    const QPixmap *pictureFor(int index, bool *card);
    QPixmap cardFor(const Node &node);
    QImage renderFontSpecimen(const QByteArray &bytes) const;
    void storePicture(int index, const QImage &image, const QSize &natural);
    void trimPictures();
    void paintFlatPicture(QPainter &painter, const Node &node, const QPixmap &pixmap, const QRectF &cell);

    // Content.
    void scheduleScan();
    void pumpContent();
    void trimLoadedText();

    void setFocusNode(int node);
    void startAnimation();
    void markInteracting();
    void showTip(int node, const QPoint &globalPosition);
    QVector<int> chainOf(int node) const;

    forkmesh::codemap::Tree m_tree;
    QHash<QString, int> m_fileIndex;
    Theme m_theme;
    Wording m_wording;
    QHash<QString, QColor> m_categoryColors;
    Mode m_mode = Mode::Flat;
    double m_worldWidth = 1600;
    int m_focus = 0;
    int m_hover = -1;
    bool m_atHome = true;
    bool m_focusPinned = false;
    quint64 m_frame = 0;

    FlatView m_view;
    Orbit m_orbit;
    Orbit m_homeOrbit;

    // One animation at a time, for whichever mode is showing.
    struct {
        bool active = false;
        QElapsedTimer clock;
        int duration = 420;
        double fromScale = 1, toScale = 1;
        QPointF fromCenter, toCenter;
        QVector3D fromTarget, toTarget;
        double fromRadius = 1, toRadius = 1;
    } m_animation;
    QBasicTimer m_animationTimer;
    QBasicTimer m_scanTimer;
    QBasicTimer m_settleTimer;
    bool m_interacting = false;

    // Pointer state.
    bool m_pressed = false;
    bool m_dragged = false;
    bool m_panGesture = false;
    QPointF m_pressAt;
    QPointF m_lastAt;

    // Per-frame scratch.
    QVector<int> m_flatLabels;
    QVector<QPair<double, int>> m_wish;
    QVector<Face> m_faces;
    QVector<Label> m_cityLabels;
    QVector<CityRequest> m_cityRequests;
    QHash<int, double> m_cityLevels; // bitmap level per textured tile, for the next frame
    QElapsedTimer m_frameClock;
    qint64 m_bitmapDeadline = 0;
    bool m_bitmapsPending = false;
    int m_bitmapsBuilt = 0;
    qint64 m_framePixels = 0; // bitmap pixels the current frame is showing
    mutable Stats m_stats;

    QHash<quint64, CachedBitmap> m_bitmaps;
    qint64 m_bitmapPixels = 0;
    mutable double m_monoAdvance = 0;

    QVector<int> m_matches;
    QSet<int> m_matchSet;
    int m_matchCursor = -1;

    QVector<int> m_queue;
    bool m_batchInFlight = false;
    int m_pendingFiles = 0;
    QVector<int> m_loaded;
    qint64 m_loadedChars = 0;
    QHash<int, Picture> m_pictures;
    qint64 m_picturePixels = 0;
    QHash<QString, QPixmap> m_cards; // extension|hue

    std::function<void(const QStringList &)> m_onWantContent;
    std::function<void(int)> m_onFocusChanged;
    std::function<void(const QString &)> m_onOpenFile;
    std::function<void()> m_onCloseRequested;
    std::function<void(int)> m_onLoadingChanged;
};
