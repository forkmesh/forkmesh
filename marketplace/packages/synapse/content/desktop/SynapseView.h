#pragma once

#include "SynapseLayout.h"

#include <QColor>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QMatrix4x4>
#include <QPixmap>
#include <QPointF>
#include <QSet>
#include <QTimer>
#include <QVector3D>
#include <QVector4D>
#include <QWidget>

#include <functional>

namespace forkmesh::synapse {

enum class ColourBy { Module, Language, Health, Hotspot };
enum class ShowMode { Everything, Cycles, Orphans, Neighbourhood };

struct Camera {
    float yaw = 1.05f;
    float pitch = 0.32f;
    float distance = 420.0f;
    QVector3D target;
};

QColor lobeColour(int lobe);
QColor languageColour(const QString &language);
QColor healthColour(double health);
QColor hotspotColour(double hotspot);

class SynapseView : public QWidget
{
public:
    explicit SynapseView(QWidget *parent = nullptr);
    ~SynapseView() override;

    static bool gpuAvailable();
    bool gpuRendering() const;
    void setGpuRendering(bool on);

    void setGraph(const GraphPtr &graph, const QVector<QVector3D> &positions);
    void setPositions(const QVector<QVector3D> &positions, double progress);
    void clearGraph(const QString &message);
    void setMessage(const QString &message);
    GraphPtr graph() const { return m_graph; }
    const QVector<QVector3D> &positions() const { return m_positions; }
    double layoutProgress() const { return m_progress; }
    bool settled() const { return m_graph && m_progress >= 1.0; }

    void setColourBy(ColourBy mode);
    ColourBy colourBy() const { return m_colourBy; }
    void setShowMode(ShowMode mode);
    ShowMode showMode() const { return m_showMode; }
    void setDepth(int hops);
    int depth() const { return m_depth; }
    void setAutoRotate(bool on);
    bool autoRotate() const { return m_autoRotate; }
    void setPulses(bool on);
    bool pulses() const { return m_pulses; }
    void setSearchQuery(const QString &query);
    void setIsolatedCycle(int cycle);
    int isolatedCycle() const { return m_isolatedCycle; }

    void selectFile(int file, bool fly);
    int selectedFile() const { return m_selected; }
    int hoveredFile() const { return m_hovered; }
    void clearSelection();
    void fit(bool animate);
    void flyToFile(int file);
    void flyToCycle(int cycle);
    Camera camera() const { return m_camera; }
    void setCamera(const Camera &camera);
    void setClock(double seconds);
    int visibleNodeCount() const;
    int drawnEdgeCount() const { return m_drawEdges.size(); }

    int pick(const QPointF &point);
    QPointF screenPosition(int file);
    QImage renderSoftware(const QSize &size, qreal devicePixelRatio = 1.0);

    void setOnSelectionChanged(std::function<void(int)> callback);
    void setOnActivated(std::function<void(int)> callback);
    void setOnAutoRotateChanged(std::function<void(bool)> callback);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    enum NodeState : quint8 { Normal, Dim, Selected, Import, Importer, Match, Hidden };
    enum EdgeState : quint8 { EdgeNormal, EdgeDim, EdgeImport, EdgeImporter, EdgeCycle, EdgeHidden };

    struct Projected {
        QPointF screen;
        float depth = 0;
        float radius = 0;
        float fog = 1;
        bool onScreen = false;
    };

    struct Frame {
        QSize size;
        QMatrix4x4 view;
        QMatrix4x4 projection;
        QMatrix4x4 viewProjection;
        QVector3D eye;
        float focal = 1;
        float fogNear = 0;
        float fogFar = 1;
        double time = 0;
        QVector<Projected> nodes;
    };

    struct Flight {
        bool active = false;
        Camera from;
        Camera to;
        qint64 start = 0;
        int duration = 900;
    };

    class GlSurface;
    friend class GlSurface;

    void restyle();
    void computeLobeCentres();
    void buildDust();
    QColor baseColour(int node) const;
    float nodeRadius(int node) const;
    QColor nodeColour(int node) const;
    QColor edgeColour(int edge, float t) const;
    float edgeWidth(int edge) const;
    bool edgePulses(int edge) const;
    Frame makeFrame(const QSize &size) const;
    const Frame &currentFrame();
    QVector3D eyePosition(const Camera &camera) const;
    QVector3D curvePoint(int edge, float t) const;
    void paintBackground(QPainter &painter, const Frame &frame);
    void paintScene(QPainter &painter, const Frame &frame);
    void paintOverlay(QPainter &painter, const Frame &frame);
    void paintLabels(QPainter &painter, const Frame &frame);
    void paintLegend(QPainter &painter, const Frame &frame);
    const QPixmap &sphereSprite(const QColor &colour, float radius, qreal dpr);
    const QPixmap &haloSprite(const QColor &colour, float radius, qreal dpr);
    Camera fittedCamera(const QVector<int> &nodes) const;
    void flyTo(const Camera &camera);
    void requestFrame();
    void tick();
    void updateTimer();
    void pauseRotation();
    bool rotating() const;
    bool animating() const;
    double now() const;
    void buildNodeVertices(QVector<float> &out) const;
    void buildEdgeVertices(QVector<float> &out, int &segments) const;
    void setHovered(int file);

    GraphPtr m_graph;
    QVector<QVector3D> m_positions;
    QVector<QVector3D> m_lobeCentres;
    double m_progress = 0;
    QString m_message;
    ColourBy m_colourBy = ColourBy::Module;
    ShowMode m_showMode = ShowMode::Everything;
    int m_depth = 1;
    bool m_autoRotate = true;
    bool m_pulses = true;
    QString m_query;
    int m_isolatedCycle = -1;
    int m_selected = -1;
    int m_hovered = -1;
    Neighbourhood m_neighbours;

    QVector<quint8> m_nodeState;
    QVector<QColor> m_nodeColour;
    QVector<quint8> m_edgeState;
    QVector<int> m_drawEdges;
    QVector<int> m_hubs;
    int m_pulseEdges = 0;
    int m_cycleEdges = 0;
    bool m_cycleNodesVisible = false;
    int m_visibleNodes = 0;

    Camera m_camera;
    Flight m_flight;
    QElapsedTimer m_clock;
    double m_fixedTime = -1;
    qint64 m_lastTick = 0;
    qint64 m_pauseUntil = 0;
    QTimer m_timer;
    bool m_dragging = false;
    bool m_panning = false;
    bool m_moved = false;
    QPointF m_pressPos;
    QPointF m_lastPos;

    Frame m_frame;
    bool m_frameValid = false;
    QImage m_backdrop;
    QVector<QVector4D> m_stars;
    QVector<QVector3D> m_dust;
    bool m_dustDirty = true;
    QHash<quint64, QPixmap> m_sprites;

    QWidget *m_surface = nullptr;
    bool m_gpuFailed = false;
    bool m_fastFrame = false;
    bool m_nodesDirty = true;
    bool m_edgesDirty = true;

    std::function<void(int)> m_onSelectionChanged;
    std::function<void(int)> m_onActivated;
    std::function<void(bool)> m_onAutoRotateChanged;
};

}
