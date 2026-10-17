#include "SynapseView.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRadialGradient>
#include <QVector2D>
#include <QWheelEvent>
#include <QtMath>

#ifdef FORKMESH_HAVE_OPENGL
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QSurfaceFormat>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace forkmesh::synapse {

namespace {

constexpr float kFov = 38.0f;
constexpr float kBow = 0.26f;
constexpr float kStarRadius = 1500.0f;
const QColor kBackground(7, 11, 22);
const QColor kCycle(255, 84, 80);
const QColor kImport(60, 200, 255);
const QColor kImporter(188, 140, 255);
const QColor kSelected(236, 246, 255);
const QColor kText(230, 237, 243);
const QColor kMuted(139, 148, 158);

QColor mix(const QColor &a, const QColor &b, float t)
{
    const float u = 1.0f - t;
    return QColor::fromRgbF(a.redF() * u + b.redF() * t, a.greenF() * u + b.greenF() * t,
                            a.blueF() * u + b.blueF() * t, a.alphaF() * u + b.alphaF() * t);
}

QColor withAlpha(QColor colour, float alpha)
{
    colour.setAlphaF(std::clamp(alpha, 0.0f, 1.0f));
    return colour;
}

float phaseOf(quint32 seed)
{
    return float(seed & 0xffffu) / 65535.0f;
}

float easeInOut(float t)
{
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
}

QPointF quadPoint(const QPointF &a, const QPointF &c, const QPointF &b, float t)
{
    const float u = 1.0f - t;
    return a * (u * u) + c * (2.0f * u * t) + b * (t * t);
}

QString shortLobeName(const QString &name)
{
    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() <= 2)
        return name;
    return parts.mid(parts.size() - 2).join(QLatin1Char('/'));
}

}

QColor lobeColour(int lobe)
{
    static const QColor palette[] = {
        QColor(0x6c, 0xb6, 0xff), QColor(0x56, 0xd4, 0xdd), QColor(0xff, 0xa6, 0x57),
        QColor(0xd2, 0xa8, 0xff), QColor(0xe3, 0xc0, 0x6d), QColor(0x79, 0xe2, 0xc0),
        QColor(0xf2, 0xa7, 0xe0), QColor(0xa5, 0xb4, 0xfc), QColor(0x9e, 0xe7, 0xff),
        QColor(0xff, 0xc6, 0x80), QColor(0xb4, 0x9c, 0xff), QColor(0x8f, 0xd3, 0xb6),
    };
    const int count = int(sizeof(palette) / sizeof(palette[0]));
    return palette[((lobe % count) + count) % count];
}

QColor languageColour(const QString &language)
{
    static const QHash<QString, QColor> known = {
        {QStringLiteral("C++"), QColor(0xf3, 0x4b, 0x7d)},
        {QStringLiteral("C"), QColor(0x8f, 0xa3, 0xb8)},
        {QStringLiteral("TypeScript"), QColor(0x4a, 0x9b, 0xf0)},
        {QStringLiteral("JavaScript"), QColor(0xf1, 0xe0, 0x5a)},
        {QStringLiteral("Python"), QColor(0x5b, 0x8f, 0xd6)},
        {QStringLiteral("Go"), QColor(0x00, 0xc0, 0xe8)},
        {QStringLiteral("Rust"), QColor(0xde, 0xa5, 0x84)},
        {QStringLiteral("Java"), QColor(0xd0, 0x8a, 0x3c)},
        {QStringLiteral("Kotlin"), QColor(0xa9, 0x7b, 0xff)},
        {QStringLiteral("Swift"), QColor(0xf0, 0x6a, 0x4a)},
        {QStringLiteral("C#"), QColor(0x9b, 0x6f, 0xd6)},
        {QStringLiteral("Ruby"), QColor(0xe0, 0x4a, 0x5f)},
        {QStringLiteral("PHP"), QColor(0x8c, 0x92, 0xd8)},
        {QStringLiteral("Dart"), QColor(0x3d, 0xc5, 0xc5)},
        {QStringLiteral("Objective-C"), QColor(0x64, 0x9c, 0xf0)},
        {QStringLiteral("Shell"), QColor(0x89, 0xe0, 0x51)},
        {QStringLiteral("QML"), QColor(0x44, 0xd0, 0x8e)},
        {QStringLiteral("Vue"), QColor(0x41, 0xd8, 0x9a)},
        {QStringLiteral("Svelte"), QColor(0xff, 0x6e, 0x40)},
        {QStringLiteral("CSS"), QColor(0x9a, 0x7f, 0xe0)},
        {QStringLiteral("SCSS"), QColor(0xd6, 0x6f, 0xa8)},
        {QStringLiteral("Lua"), QColor(0x60, 0x7c, 0xf0)},
        {QStringLiteral("Scala"), QColor(0xe0, 0x50, 0x48)},
    };
    const auto it = known.constFind(language);
    if (it != known.constEnd())
        return it.value();
    return lobeColour(int(stableHash(language) % 12u));
}

QColor healthColour(double health)
{
    if (health >= 8)
        return QColor(0x3f, 0xb9, 0x8f);
    if (health >= 4)
        return QColor(0xe3, 0xb3, 0x41);
    return QColor(0xe5, 0x53, 0x4b);
}

QColor hotspotColour(double hotspot)
{
    const float t = float(std::clamp(hotspot, 0.0, 1.0));
    const QColor cool(0x3a, 0x4d, 0x72);
    const QColor warm(0xe3, 0xa2, 0x3c);
    const QColor hot(0xff, 0x6f, 0x3c);
    if (t < 0.5f)
        return mix(cool, warm, t * 2.0f);
    return mix(warm, hot, (t - 0.5f) * 2.0f);
}

#ifdef FORKMESH_HAVE_OPENGL

namespace {

const char *kNodeVertex = R"(
attribute vec3 a_centre;
attribute vec2 a_corner;
attribute float a_radius;
attribute vec4 a_color;
attribute float a_flags;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform float u_scale;
uniform float u_fogNear;
uniform float u_fogFar;
uniform float u_fogAmount;
varying vec2 v_uv;
varying vec4 v_color;
varying float v_fog;
varying float v_flags;
void main() {
    vec4 c = u_view * vec4(a_centre, 1.0);
    c.xy += a_corner * a_radius * u_scale;
    gl_Position = u_proj * c;
    v_uv = a_corner;
    v_color = a_color;
    v_flags = a_flags;
    float depth = -c.z;
    v_fog = 1.0 - u_fogAmount * clamp((depth - u_fogNear) / max(u_fogFar - u_fogNear, 1.0), 0.0, 1.0);
}
)";

const char *kSphereFragment = R"(
varying vec2 v_uv;
varying vec4 v_color;
varying float v_fog;
varying float v_flags;
uniform float u_time;
uniform vec3 u_fogColor;
void main() {
    float r2 = dot(v_uv, v_uv);
    if (r2 > 1.0)
        discard;
    vec3 n = vec3(v_uv.x, -v_uv.y, sqrt(1.0 - r2));
    vec3 key = normalize(vec3(-0.45, 0.6, 0.66));
    vec3 fill = normalize(vec3(0.7, -0.35, 0.45));
    float diffuse = max(dot(n, key), 0.0);
    float fillLight = max(dot(n, fill), 0.0) * 0.32;
    vec3 h = normalize(key + vec3(0.0, 0.0, 1.0));
    float spec = pow(max(dot(n, h), 0.0), 36.0);
    float rim = pow(1.0 - n.z, 2.4);
    vec3 base = v_color.rgb;
    float pulse = v_flags > 0.5 ? 0.5 + 0.5 * sin(u_time * 4.0) : 0.0;
    vec3 col = base * (0.16 + 0.86 * diffuse + fillLight);
    col += vec3(1.0) * spec * 0.55;
    col += base * rim * 0.85 + vec3(0.55, 0.75, 1.0) * rim * 0.12;
    col += base * pulse * 0.55;
    col = mix(u_fogColor, col, v_fog);
    float edge = 1.0 - smoothstep(0.86, 1.0, r2);
    gl_FragColor = vec4(col, edge);
}
)";

const char *kHaloFragment = R"(
varying vec2 v_uv;
varying vec4 v_color;
varying float v_fog;
varying float v_flags;
uniform float u_time;
uniform float u_intensity;
void main() {
    float r2 = dot(v_uv, v_uv);
    if (r2 >= 1.0)
        discard;
    float g = (exp(-r2 * 4.5) - 0.011) * (1.0 - r2);
    float pulse = v_flags > 0.5 ? 0.65 + 0.6 * (0.5 + 0.5 * sin(u_time * 4.0)) : 1.0;
    float a = max(g, 0.0) * v_color.a * v_fog * pulse * u_intensity;
    gl_FragColor = vec4(v_color.rgb * a, a);
}
)";

const char *kFibreVertex = R"(
attribute vec3 a_p0;
attribute vec3 a_p2;
attribute vec4 a_params;
attribute vec4 a_color;
attribute float a_width;
uniform mat4 u_view;
uniform mat4 u_proj;
uniform vec3 u_centre;
uniform float u_bow;
uniform vec2 u_halfViewport;
uniform float u_pixelScale;
uniform float u_fogNear;
uniform float u_fogFar;
uniform float u_fogAmount;
varying vec4 v_color;
varying float v_t;
varying float v_side;
varying float v_fog;
varying float v_pulse;
varying float v_phase;
vec3 curve(float t) {
    vec3 mid = 0.5 * (a_p0 + a_p2);
    vec3 c = mid + (u_centre - mid) * u_bow;
    float u = 1.0 - t;
    return u * u * a_p0 + 2.0 * u * t * c + t * t * a_p2;
}
vec2 screenOf(vec3 p) {
    vec4 clip = u_proj * u_view * vec4(p, 1.0);
    return clip.xy / max(clip.w, 0.001) * u_halfViewport;
}
void main() {
    float t = a_params.x;
    float side = a_params.y;
    vec3 p = curve(t);
    vec4 viewPos = u_view * vec4(p, 1.0);
    vec4 clip = u_proj * viewPos;
    vec2 ahead = screenOf(curve(min(t + 0.02, 1.0)));
    vec2 behind = screenOf(curve(max(t - 0.02, 0.0)));
    vec2 dir = ahead - behind;
    float len = length(dir);
    dir = len > 0.0001 ? dir / len : vec2(1.0, 0.0);
    vec2 normal = vec2(-dir.y, dir.x);
    clip.xy += normal * side * a_width * u_pixelScale / u_halfViewport * clip.w;
    gl_Position = clip;
    v_color = a_color;
    v_t = t;
    v_side = side;
    v_pulse = a_params.z;
    v_phase = a_params.w;
    float depth = -viewPos.z;
    v_fog = 1.0 - u_fogAmount * clamp((depth - u_fogNear) / max(u_fogFar - u_fogNear, 1.0), 0.0, 1.0);
}
)";

const char *kFibreFragment = R"(
varying vec4 v_color;
varying float v_t;
varying float v_side;
varying float v_fog;
varying float v_pulse;
varying float v_phase;
uniform float u_time;
void main() {
    float soft = smoothstep(0.0, 0.75, 1.0 - abs(v_side));
    float a = v_color.a * soft * v_fog;
    vec3 col = v_color.rgb;
    if (v_pulse > 0.5) {
        float head = fract(u_time * 0.32 + v_phase);
        float d = head - v_t;
        float glow = d >= 0.0 ? exp(-d * 26.0) : exp(d * 320.0);
        col = mix(col, vec3(1.0), glow * 0.6);
        a += glow * 0.85 * soft * v_fog;
    }
    gl_FragColor = vec4(col * a, a);
}
)";

const char *kBackgroundVertex = R"(
attribute vec2 a_pos;
varying vec2 v_pos;
void main() {
    v_pos = a_pos;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)";

const char *kBackgroundFragment = R"(
varying vec2 v_pos;
uniform vec2 u_glow;
uniform float u_glowRadius;
uniform float u_aspect;
void main() {
    float t = v_pos.y * 0.5 + 0.5;
    vec3 col = mix(vec3(0.010, 0.016, 0.034), vec3(0.045, 0.066, 0.130), t);
    vec2 d = (v_pos - u_glow) * vec2(u_aspect, 1.0);
    float g = exp(-dot(d, d) / max(u_glowRadius * u_glowRadius, 0.0001));
    col += vec3(0.05, 0.09, 0.21) * g;
    float vignette = 1.0 - smoothstep(0.55, 1.65, length(v_pos * vec2(1.0, 1.15)));
    col *= mix(0.42, 1.0, vignette);
    gl_FragColor = vec4(col, 1.0);
}
)";

constexpr int kNodeFloats = 11;
constexpr int kEdgeFloats = 15;
constexpr int kQuadsPerBatch = 16384;

}

class SynapseView::GlSurface : public QOpenGLWidget, protected QOpenGLFunctions
{
public:
    explicit GlSurface(SynapseView *view)
        : QOpenGLWidget(view)
        , m_view(view)
        , m_quad(QOpenGLBuffer::VertexBuffer)
        , m_nodes(QOpenGLBuffer::VertexBuffer)
        , m_edges(QOpenGLBuffer::VertexBuffer)
        , m_stars(QOpenGLBuffer::VertexBuffer)
        , m_dust(QOpenGLBuffer::VertexBuffer)
        , m_indices(QOpenGLBuffer::IndexBuffer)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        QSurfaceFormat surface = format();
        surface.setSamples(4);
        setFormat(surface);
    }

    ~GlSurface() override
    {
        if (!m_ready)
            return;
        makeCurrent();
        m_quad.destroy();
        m_nodes.destroy();
        m_edges.destroy();
        m_stars.destroy();
        m_dust.destroy();
        m_indices.destroy();
        doneCurrent();
    }

protected:
    void initializeGL() override
    {
        initializeOpenGLFunctions();
        const bool es = QOpenGLContext::currentContext()->isOpenGLES();
        const QByteArray prologue = es ? QByteArrayLiteral("#version 100\nprecision mediump float;\n")
                                       : QByteArrayLiteral("#version 120\n");
        const auto build = [&](QOpenGLShaderProgram &program, const char *vertex, const char *fragment) {
            return program.addShaderFromSourceCode(QOpenGLShader::Vertex, prologue + vertex)
                   && program.addShaderFromSourceCode(QOpenGLShader::Fragment, prologue + fragment)
                   && program.link();
        };
        const bool built = build(m_background, kBackgroundVertex, kBackgroundFragment)
                           && build(m_sphere, kNodeVertex, kSphereFragment)
                           && build(m_halo, kNodeVertex, kHaloFragment)
                           && build(m_fibre, kFibreVertex, kFibreFragment);
        if (!built) {
            QPointer<SynapseView> view(m_view);
            QTimer::singleShot(0, m_view, [view] {
                if (view) {
                    view->m_gpuFailed = true;
                    view->setGpuRendering(false);
                }
            });
            return;
        }
        const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        m_quad.create();
        m_quad.bind();
        m_quad.allocate(quad, int(sizeof(quad)));
        m_quad.release();
        QVector<quint16> indices;
        indices.reserve(kQuadsPerBatch * 6);
        for (int q = 0; q < kQuadsPerBatch; ++q) {
            const quint16 b = quint16(q * 4);
            indices << b << quint16(b + 1) << quint16(b + 2) << b << quint16(b + 2) << quint16(b + 3);
        }
        m_indices.create();
        m_indices.bind();
        m_indices.allocate(indices.constData(), int(indices.size() * sizeof(quint16)));
        m_indices.release();
        m_nodes.create();
        m_nodes.setUsagePattern(QOpenGLBuffer::DynamicDraw);
        m_edges.create();
        m_edges.setUsagePattern(QOpenGLBuffer::DynamicDraw);
        m_dust.create();
        m_view->m_dustDirty = true;
        QVector<float> stars;
        for (const QVector4D &star : m_view->m_stars) {
            const QVector3D p = star.toVector3D() * kStarRadius;
            const float size = 1.4f + 2.2f * star.w();
            const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto &corner : corners) {
                stars << p.x() << p.y() << p.z() << corner[0] << corner[1] << size
                      << 0.78f << 0.86f << 1.0f << (0.25f + 0.6f * star.w()) << 0.0f;
            }
        }
        m_starCount = m_view->m_stars.size();
        m_stars.create();
        m_stars.bind();
        m_stars.allocate(stars.constData(), int(stars.size() * sizeof(float)));
        m_stars.release();
        m_ready = true;
    }

    void paintGL() override
    {
        SynapseView &view = *m_view;
        view.m_frame = view.makeFrame(size());
        view.m_frameValid = true;
        const SynapseView::Frame &frame = view.m_frame;
        const qreal dpr = devicePixelRatioF();
        const int w = int(width() * dpr);
        const int h = int(height() * dpr);
        glViewport(0, 0, w, h);
        glDisable(GL_CULL_FACE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glClearColor(kBackground.redF(), kBackground.greenF(), kBackground.blueF(), 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (m_ready) {
            drawBackground(frame);
            if (view.m_graph && !view.m_positions.isEmpty())
                drawScene(frame, dpr);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        QPainter painter(this);
        view.paintOverlay(painter, frame);
    }

private:
    void drawBackground(const SynapseView::Frame &frame)
    {
        const QVector4D centre = frame.viewProjection * QVector4D(0, 0, 0, 1);
        QVector2D glow(0, 0);
        float radius = 0.8f;
        if (centre.w() > 0.01f) {
            glow = QVector2D(centre.x() / centre.w(), centre.y() / centre.w());
            radius = std::clamp(m_view->m_graph ? 150.0f * frame.focal / centre.w()
                                                       / float(std::max(1, frame.size.height())) * 2.0f
                                                 : 0.8f,
                                0.2f, 2.5f);
        }
        m_background.bind();
        m_background.setUniformValue("u_glow", glow);
        m_background.setUniformValue("u_glowRadius", radius);
        m_background.setUniformValue("u_aspect", float(frame.size.width()) / float(std::max(1, frame.size.height())));
        m_quad.bind();
        const int pos = m_background.attributeLocation("a_pos");
        m_background.enableAttributeArray(pos);
        m_background.setAttributeBuffer(pos, GL_FLOAT, 0, 2, 2 * sizeof(float));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        m_background.disableAttributeArray(pos);
        m_quad.release();
        m_background.release();
    }

    void bindNodeAttributes(QOpenGLShaderProgram &program, int vertexOffset)
    {
        const int stride = kNodeFloats * sizeof(float);
        const int base = vertexOffset * stride;
        const char *names[] = {"a_centre", "a_corner", "a_radius", "a_color", "a_flags"};
        const int sizes[] = {3, 2, 1, 4, 1};
        int offset = 0;
        for (int i = 0; i < 5; ++i) {
            const int location = program.attributeLocation(names[i]);
            if (location >= 0) {
                program.enableAttributeArray(location);
                program.setAttributeBuffer(location, GL_FLOAT, base + offset * int(sizeof(float)), sizes[i], stride);
            }
            offset += sizes[i];
        }
    }

    void unbindNodeAttributes(QOpenGLShaderProgram &program)
    {
        for (const char *name : {"a_centre", "a_corner", "a_radius", "a_color", "a_flags"}) {
            const int location = program.attributeLocation(name);
            if (location >= 0)
                program.disableAttributeArray(location);
        }
    }

    void drawQuads(QOpenGLShaderProgram &program, QOpenGLBuffer &buffer, int quads)
    {
        buffer.bind();
        m_indices.bind();
        for (int first = 0; first < quads; first += kQuadsPerBatch) {
            const int count = std::min(kQuadsPerBatch, quads - first);
            bindNodeAttributes(program, first * 4);
            glDrawElements(GL_TRIANGLES, count * 6, GL_UNSIGNED_SHORT, nullptr);
        }
        unbindNodeAttributes(program);
        m_indices.release();
        buffer.release();
    }

    void setCommon(QOpenGLShaderProgram &program, const SynapseView::Frame &frame, float fogAmount)
    {
        program.setUniformValue("u_view", frame.view);
        program.setUniformValue("u_proj", frame.projection);
        program.setUniformValue("u_fogNear", frame.fogNear);
        program.setUniformValue("u_fogFar", frame.fogFar);
        program.setUniformValue("u_fogAmount", fogAmount);
        program.setUniformValue("u_time", float(frame.time));
    }

    void drawScene(const SynapseView::Frame &frame, qreal dpr)
    {
        SynapseView &view = *m_view;
        if (view.m_nodesDirty) {
            QVector<float> data;
            view.buildNodeVertices(data);
            m_nodeCount = data.size() / (kNodeFloats * 4);
            m_nodes.bind();
            m_nodes.allocate(data.constData(), int(data.size() * sizeof(float)));
            m_nodes.release();
            view.m_nodesDirty = false;
        }
        if (view.m_edgesDirty) {
            QVector<float> data;
            int segments = 0;
            view.buildEdgeVertices(data, segments);
            m_edgeVertices = data.size() / kEdgeFloats;
            m_edges.bind();
            m_edges.allocate(data.constData(), int(data.size() * sizeof(float)));
            m_edges.release();
            view.m_edgesDirty = false;
        }
        if (view.m_dustDirty) {
            QVector<float> data;
            const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const QVector3D &p : view.m_dust) {
                for (const auto &corner : corners)
                    data << p.x() << p.y() << p.z() << corner[0] << corner[1] << 1.3f << 0.47f << 0.63f
                         << 0.92f << 0.5f << 0.0f;
            }
            m_dustCount = view.m_dust.size();
            m_dust.bind();
            m_dust.allocate(data.constData(), int(data.size() * sizeof(float)));
            m_dust.release();
            view.m_dustDirty = false;
        }
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        m_halo.bind();
        setCommon(m_halo, frame, 0.0f);
        m_halo.setUniformValue("u_scale", 1.0f);
        m_halo.setUniformValue("u_intensity", 1.0f);
        drawQuads(m_halo, m_stars, m_starCount);
        setCommon(m_halo, frame, 0.85f);
        drawQuads(m_halo, m_dust, m_dustCount);
        m_halo.release();

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        m_sphere.bind();
        setCommon(m_sphere, frame, 0.72f);
        m_sphere.setUniformValue("u_scale", 1.0f);
        m_sphere.setUniformValue("u_fogColor", QVector3D(kBackground.redF(), kBackground.greenF(), kBackground.blueF()));
        drawQuads(m_sphere, m_nodes, m_nodeCount);
        m_sphere.release();

        glDepthMask(GL_FALSE);
        glDisable(GL_DEPTH_TEST);
        glBlendFunc(GL_ONE, GL_ONE);
        if (m_edgeVertices > 0) {
            m_fibre.bind();
            setCommon(m_fibre, frame, 0.8f);
            m_fibre.setUniformValue("u_centre", QVector3D(0, 0, 0));
            m_fibre.setUniformValue("u_bow", kBow);
            m_fibre.setUniformValue("u_halfViewport", QVector2D(width() * dpr * 0.5f, height() * dpr * 0.5f));
            m_fibre.setUniformValue("u_pixelScale", float(dpr));
            m_edges.bind();
            const int stride = kEdgeFloats * sizeof(float);
            const char *names[] = {"a_p0", "a_p2", "a_params", "a_color", "a_width"};
            const int sizes[] = {3, 3, 4, 4, 1};
            int offset = 0;
            for (int i = 0; i < 5; ++i) {
                const int location = m_fibre.attributeLocation(names[i]);
                if (location >= 0) {
                    m_fibre.enableAttributeArray(location);
                    m_fibre.setAttributeBuffer(location, GL_FLOAT, offset * int(sizeof(float)), sizes[i], stride);
                }
                offset += sizes[i];
            }
            glDrawArrays(GL_TRIANGLE_STRIP, 0, m_edgeVertices);
            for (const char *name : names) {
                const int location = m_fibre.attributeLocation(name);
                if (location >= 0)
                    m_fibre.disableAttributeArray(location);
            }
            m_edges.release();
            m_fibre.release();
        }

        glEnable(GL_DEPTH_TEST);
        m_halo.bind();
        setCommon(m_halo, frame, 0.8f);
        m_halo.setUniformValue("u_scale", 3.0f);
        m_halo.setUniformValue("u_intensity", 0.9f);
        drawQuads(m_halo, m_nodes, m_nodeCount);
        m_halo.release();
    }

    SynapseView *m_view;
    QOpenGLShaderProgram m_background;
    QOpenGLShaderProgram m_sphere;
    QOpenGLShaderProgram m_halo;
    QOpenGLShaderProgram m_fibre;
    QOpenGLBuffer m_quad;
    QOpenGLBuffer m_nodes;
    QOpenGLBuffer m_edges;
    QOpenGLBuffer m_stars;
    QOpenGLBuffer m_dust;
    QOpenGLBuffer m_indices;
    int m_dustCount = 0;
    int m_nodeCount = 0;
    int m_edgeVertices = 0;
    int m_starCount = 0;
    bool m_ready = false;
};

#else

class SynapseView::GlSurface : public QWidget
{
};

#endif

SynapseView::SynapseView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(320, 240);
    setAccessibleName(QStringLiteral("Import graph"));
    m_clock.start();
    m_message = QStringLiteral("Open or clone a repository to see how its files connect.");
    m_stars.reserve(420);
    for (int i = 0; i < 420; ++i) {
        const quint32 a = stableHash(QStringLiteral("star-a-%1").arg(i));
        const quint32 b = stableHash(QStringLiteral("star-b-%1").arg(i));
        const quint32 c = stableHash(QStringLiteral("star-c-%1").arg(i));
        const float z = float(a & 0xffff) / 32767.5f - 1.0f;
        const float theta = float(b & 0xffff) / 65535.0f * 6.2831853f;
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float bright = std::pow(float(c & 0xffff) / 65535.0f, 3.0f);
        m_stars.append(QVector4D(r * std::cos(theta), z, r * std::sin(theta), bright));
    }
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    setGpuRendering(gpuAvailable());
}

SynapseView::~SynapseView() = default;

bool SynapseView::gpuAvailable()
{
#ifdef FORKMESH_HAVE_OPENGL
    static const bool available = [] {
        const QString setting = qEnvironmentVariable("FORKMESH_SYNAPSE_GPU");
        if (setting == QLatin1String("0"))
            return false;
        const QString platform = QGuiApplication::platformName();
        if (setting != QLatin1String("1")
            && (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal")))
            return false;
        QOpenGLContext probe;
        return probe.create() && probe.isValid();
    }();
    return available;
#else
    return false;
#endif
}

bool SynapseView::gpuRendering() const
{
    return m_surface != nullptr;
}

void SynapseView::setGpuRendering(bool on)
{
    on = on && gpuAvailable() && !m_gpuFailed;
    if (on == (m_surface != nullptr))
        return;
#ifdef FORKMESH_HAVE_OPENGL
    if (on) {
        auto *surface = new GlSurface(this);
        m_surface = surface;
        surface->setGeometry(rect());
        surface->show();
    } else
#endif
    {
        delete m_surface;
        m_surface = nullptr;
    }
    m_nodesDirty = true;
    m_edgesDirty = true;
    restyle();
    updateTimer();
}

void SynapseView::setGraph(const GraphPtr &graph, const QVector<QVector3D> &positions)
{
    const bool sameRoot = m_graph && graph && m_graph->snapshot && graph->snapshot
                          && m_graph->snapshot->root == graph->snapshot->root;
    QString selectedPath;
    if (m_graph && m_graph->snapshot && m_selected >= 0 && m_selected < m_graph->snapshot->files.size())
        selectedPath = m_graph->snapshot->files.at(m_selected).path;
    m_graph = graph;
    m_positions = positions;
    m_progress = 0;
    m_hovered = -1;
    m_selected = -1;
    m_isolatedCycle = -1;
    if (graph && graph->snapshot && !selectedPath.isEmpty() && sameRoot)
        m_selected = graph->snapshot->indexOf(selectedPath);
    m_sprites.clear();
    computeLobeCentres();
    buildDust();
    restyle();
    if (!sameRoot) {
        Camera camera;
        camera.yaw = m_camera.yaw;
        camera.pitch = 0.32f;
        m_camera = camera;
        m_camera = fittedCamera({});
        m_flight.active = false;
    }
    requestFrame();
    updateTimer();
}

void SynapseView::setPositions(const QVector<QVector3D> &positions, double progress)
{
    if (!m_graph || positions.size() != m_graph->nodes.size())
        return;
    m_positions = positions;
    m_progress = progress;
    computeLobeCentres();
    m_nodesDirty = true;
    m_edgesDirty = true;
    requestFrame();
    updateTimer();
}

void SynapseView::clearGraph(const QString &message)
{
    m_graph.reset();
    m_positions.clear();
    m_lobeCentres.clear();
    m_dust.clear();
    m_dustDirty = true;
    m_progress = 0;
    m_selected = -1;
    m_hovered = -1;
    m_isolatedCycle = -1;
    m_message = message;
    restyle();
    requestFrame();
    updateTimer();
}

void SynapseView::setMessage(const QString &message)
{
    m_message = message;
    requestFrame();
}

void SynapseView::setColourBy(ColourBy mode)
{
    if (m_colourBy == mode)
        return;
    m_colourBy = mode;
    restyle();
}

void SynapseView::setShowMode(ShowMode mode)
{
    if (m_showMode == mode && m_isolatedCycle < 0)
        return;
    m_showMode = mode;
    m_isolatedCycle = -1;
    restyle();
}

void SynapseView::setDepth(int hops)
{
    hops = std::clamp(hops, 1, 3);
    if (m_depth == hops)
        return;
    m_depth = hops;
    restyle();
}

void SynapseView::setAutoRotate(bool on)
{
    if (m_autoRotate == on)
        return;
    m_autoRotate = on;
    m_pauseUntil = 0;
    updateTimer();
}

void SynapseView::setPulses(bool on)
{
    if (m_pulses == on)
        return;
    m_pulses = on;
    restyle();
}

void SynapseView::setSearchQuery(const QString &query)
{
    const QString trimmed = query.trimmed();
    if (trimmed == m_query)
        return;
    m_query = trimmed;
    restyle();
}

void SynapseView::setIsolatedCycle(int cycle)
{
    if (!m_graph || cycle < 0 || cycle >= m_graph->cycles.size())
        cycle = -1;
    m_isolatedCycle = cycle;
    if (cycle >= 0)
        m_showMode = ShowMode::Cycles;
    restyle();
}

void SynapseView::selectFile(int file, bool fly)
{
    if (!m_graph || !m_graph->snapshot || file < 0 || file >= m_graph->snapshot->files.size())
        file = -1;
    m_selected = file;
    restyle();
    if (fly && file >= 0)
        flyToFile(file);
}

void SynapseView::clearSelection()
{
    if (m_selected < 0 && m_isolatedCycle < 0)
        return;
    m_selected = -1;
    m_isolatedCycle = -1;
    restyle();
}

int SynapseView::visibleNodeCount() const
{
    return m_visibleNodes;
}

void SynapseView::setOnSelectionChanged(std::function<void(int)> callback)
{
    m_onSelectionChanged = std::move(callback);
}

void SynapseView::setOnActivated(std::function<void(int)> callback)
{
    m_onActivated = std::move(callback);
}

void SynapseView::setOnAutoRotateChanged(std::function<void(bool)> callback)
{
    m_onAutoRotateChanged = std::move(callback);
}

void SynapseView::computeLobeCentres()
{
    if (!m_graph) {
        m_lobeCentres.clear();
        return;
    }
    m_lobeCentres = lobeCentres(*m_graph, m_positions);
}

void SynapseView::buildDust()
{
    m_dust.clear();
    m_dustDirty = true;
    if (!m_graph)
        return;
    const BrainShape &shape = m_graph->shape;
    const int count = 1800;
    m_dust.reserve(count);
    for (int i = 0; i < count; ++i) {
        const float y = 1.0f - 2.0f * (float(i) + 0.5f) / float(count);
        const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
        const float phi = float(i) * 2.39996323f;
        QVector3D direction(r * std::sin(phi), y, r * std::cos(phi));
        QVector3D p = shape.onShell(direction, shape.outerAt(direction) + 0.025f);
        if (std::abs(p.x()) < shape.fissure * 0.6f)
            continue;
        m_dust.append(p);
    }
}

QColor SynapseView::baseColour(int node) const
{
    const Node &n = m_graph->nodes.at(node);
    const insight::FileInfo &file = m_graph->snapshot->files.at(n.file);
    switch (m_colourBy) {
    case ColourBy::Module:
        return lobeColour(n.lobe);
    case ColourBy::Language:
        return languageColour(file.language);
    case ColourBy::Health:
        return healthColour(file.health);
    case ColourBy::Hotspot:
        return hotspotColour(file.hotspot);
    }
    return lobeColour(n.lobe);
}

QColor SynapseView::nodeColour(int node) const
{
    return m_nodeColour.value(node, kMuted);
}

float SynapseView::nodeRadius(int node) const
{
    const float base = std::min(m_graph->spacing * 0.15f, 3.4f) * m_graph->nodes.at(node).size;
    switch (m_nodeState.value(node, Normal)) {
    case Selected:
        return base * 1.55f;
    case Import:
    case Importer:
    case Match:
        return base * 1.15f;
    case Dim:
        return base * 0.85f;
    default:
        return base;
    }
}

float SynapseView::edgeWidth(int edge) const
{
    switch (m_edgeState.value(edge, EdgeNormal)) {
    case EdgeImport:
    case EdgeImporter:
        return 1.5f;
    case EdgeCycle:
        return m_cycleEdges > 300 ? 0.95f : 1.35f;
    case EdgeDim:
        return 0.6f;
    default:
        return 0.85f;
    }
}

bool SynapseView::edgePulses(int edge) const
{
    if (!m_pulses)
        return false;
    const quint8 state = m_edgeState.value(edge, EdgeHidden);
    if (state == EdgeImport || state == EdgeImporter || state == EdgeCycle)
        return true;
    return state == EdgeNormal && m_graph && m_graph->edges.size() <= 1500;
}

QColor SynapseView::edgeColour(int edge, float t) const
{
    const Edge &e = m_graph->edges.at(edge);
    const quint8 state = m_edgeState.value(edge, EdgeNormal);
    switch (state) {
    case EdgeImport:
        return withAlpha(mix(kImport, QColor(150, 230, 255), t * 0.5f), 0.85f);
    case EdgeImporter:
        return withAlpha(mix(QColor(220, 190, 255), kImporter, t), 0.85f);
    case EdgeCycle:
        return withAlpha(kCycle, m_cycleEdges > 300 ? 0.3f : 0.8f);
    case EdgeDim: {
        const QColor c = mix(m_nodeColour.value(e.from), m_nodeColour.value(e.to), t);
        return withAlpha(c, 0.05f);
    }
    default:
        break;
    }
    const int drawn = std::max(1, int(m_drawEdges.size()));
    const float alpha = std::clamp(0.5f / std::sqrt(std::max(1.0f, float(drawn) / 120.0f)), 0.08f, 0.42f);
    const QColor a = baseColour(e.from);
    const QColor b = baseColour(e.to);
    const QColor tone = e.cycle ? mix(mix(a, b, t), kCycle, 0.45f) : mix(a, b, t);
    return withAlpha(mix(tone, QColor(255, 255, 255), 0.18f), alpha);
}

void SynapseView::restyle()
{
    m_frameValid = false;
    m_nodesDirty = true;
    m_edgesDirty = true;
    if (!m_graph || !m_graph->snapshot) {
        m_nodeState.clear();
        m_nodeColour.clear();
        m_edgeState.clear();
        m_drawEdges.clear();
        m_hubs.clear();
        m_pulseEdges = 0;
        m_cycleNodesVisible = false;
        m_visibleNodes = 0;
        requestFrame();
        return;
    }
    const Graph &graph = *m_graph;
    const insight::Snapshot &snapshot = *graph.snapshot;
    const int n = graph.nodes.size();
    QVector<bool> visible(n, true);
    if (m_isolatedCycle >= 0 && m_isolatedCycle < graph.cycles.size()) {
        visible.fill(false);
        for (int file : graph.cycles.at(m_isolatedCycle).files) {
            const int node = graph.nodeFor(file);
            if (node >= 0)
                visible[node] = true;
        }
    } else if (m_showMode == ShowMode::Cycles) {
        for (int i = 0; i < n; ++i)
            visible[i] = graph.nodes.at(i).cycle;
    } else if (m_showMode == ShowMode::Orphans) {
        for (int i = 0; i < n; ++i)
            visible[i] = graph.nodes.at(i).fanIn == 0 && graph.nodes.at(i).fanOut == 0;
    }
    m_neighbours = m_selected >= 0 ? neighbourhood(snapshot, m_selected, m_depth, Direction::Both)
                                   : Neighbourhood();
    if (m_showMode == ShowMode::Neighbourhood && m_selected >= 0 && m_isolatedCycle < 0) {
        for (int i = 0; i < n; ++i) {
            const int file = graph.nodes.at(i).file;
            visible[i] = file == m_selected || m_neighbours.contains(file);
        }
    }
    const bool searching = !m_query.isEmpty() && m_selected < 0;
    m_nodeState.fill(Normal, n);
    m_nodeColour.resize(n);
    m_visibleNodes = 0;
    m_cycleNodesVisible = false;
    for (int i = 0; i < n; ++i) {
        const Node &node = graph.nodes.at(i);
        quint8 state = Normal;
        if (!visible.at(i)) {
            state = Hidden;
        } else if (m_selected >= 0) {
            if (node.file == m_selected)
                state = Selected;
            else if (m_neighbours.imports.contains(node.file))
                state = Import;
            else if (m_neighbours.importers.contains(node.file))
                state = Importer;
            else
                state = Dim;
        } else if (searching) {
            state = snapshot.files.at(node.file).path.contains(m_query, Qt::CaseInsensitive) ? Match : Dim;
        }
        m_nodeState[i] = state;
        if (state != Hidden)
            ++m_visibleNodes;
        if (state == Normal && node.cycle)
            m_cycleNodesVisible = true;
        const QColor base = baseColour(i);
        QColor colour = base;
        switch (state) {
        case Selected:
            colour = kSelected;
            break;
        case Import:
            colour = kImport;
            break;
        case Importer:
            colour = kImporter;
            break;
        case Match:
            colour = node.cycle ? kCycle : base.lighter(125);
            break;
        case Dim:
            colour = mix(node.cycle ? kCycle : base, kBackground, 0.62f);
            break;
        default:
            colour = node.cycle ? kCycle : base;
            break;
        }
        m_nodeColour[i] = colour;
    }
    m_edgeState.fill(EdgeNormal, graph.edges.size());
    const auto smallCycle = [&](const Edge &edge) {
        const int cycle = snapshot.files.at(graph.nodes.at(edge.from).file).cycle;
        return cycle >= 0 && cycle < snapshot.cycles.size() && snapshot.cycles.at(cycle).size() <= 40;
    };
    QVector<int> highlighted;
    QVector<int> normal;
    QVector<int> dim;
    for (int e = 0; e < graph.edges.size(); ++e) {
        const Edge &edge = graph.edges.at(e);
        const quint8 a = m_nodeState.at(edge.from);
        const quint8 b = m_nodeState.at(edge.to);
        quint8 state = EdgeNormal;
        if (a == Hidden || b == Hidden) {
            state = EdgeHidden;
        } else if (m_selected >= 0) {
            const int fa = graph.nodes.at(edge.from).file;
            const int fb = graph.nodes.at(edge.to).file;
            const int ia = fa == m_selected ? 0 : m_neighbours.imports.value(fa, -1);
            const int ib = m_neighbours.imports.value(fb, -1);
            const int ra = m_neighbours.importers.value(fa, -1);
            const int rb = fb == m_selected ? 0 : m_neighbours.importers.value(fb, -1);
            if (ia >= 0 && ib == ia + 1)
                state = EdgeImport;
            else if (rb >= 0 && ra == rb + 1)
                state = EdgeImporter;
            else
                state = EdgeDim;
        } else if (searching) {
            state = a == Match && b == Match ? EdgeNormal : EdgeDim;
        } else if (edge.cycle && (m_isolatedCycle >= 0 || smallCycle(edge))) {
            state = EdgeCycle;
        }
        m_edgeState[e] = state;
        if (state == EdgeImport || state == EdgeImporter || state == EdgeCycle)
            highlighted.append(e);
        else if (state == EdgeNormal)
            normal.append(e);
        else if (state == EdgeDim)
            dim.append(e);
    }
    const int cap = gpuRendering() ? 16000 : 5000;
    const auto bySeed = [&](int x, int y) {
        const quint32 sx = graph.edges.at(x).seed;
        const quint32 sy = graph.edges.at(y).seed;
        return sx != sy ? sx < sy : x < y;
    };
    if (highlighted.size() > cap) {
        std::sort(highlighted.begin(), highlighted.end(), bySeed);
        highlighted.resize(cap);
    }
    int room = cap - highlighted.size();
    if (normal.size() > room) {
        std::sort(normal.begin(), normal.end(), bySeed);
        normal.resize(std::max(0, room));
    }
    room -= normal.size();
    const int dimCap = gpuRendering() ? 6000 : 1200;
    if (dim.size() > std::min(room, dimCap)) {
        std::sort(dim.begin(), dim.end(), bySeed);
        dim.resize(std::max(0, std::min(room, dimCap)));
    }
    m_drawEdges = dim + normal + highlighted;
    m_cycleEdges = 0;
    for (int e : highlighted) {
        if (m_edgeState.at(e) == EdgeCycle)
            ++m_cycleEdges;
    }
    m_pulseEdges = 0;
    for (int e : m_drawEdges) {
        if (edgePulses(e))
            ++m_pulseEdges;
    }
    m_hubs.clear();
    for (int i = 0; i < n; ++i) {
        if (m_nodeState.at(i) != Hidden && m_nodeState.at(i) != Dim && graph.nodes.at(i).fanIn > 0)
            m_hubs.append(i);
    }
    std::sort(m_hubs.begin(), m_hubs.end(), [&](int x, int y) {
        const int fx = graph.nodes.at(x).fanIn;
        const int fy = graph.nodes.at(y).fanIn;
        return fx != fy ? fx > fy : x < y;
    });
    if (m_hubs.size() > 10)
        m_hubs.resize(10);
    requestFrame();
    updateTimer();
}

QVector3D SynapseView::eyePosition(const Camera &camera) const
{
    const float cp = std::cos(camera.pitch);
    return camera.target
           + QVector3D(cp * std::sin(camera.yaw), std::sin(camera.pitch), cp * std::cos(camera.yaw))
                 * camera.distance;
}

SynapseView::Frame SynapseView::makeFrame(const QSize &size) const
{
    Frame frame;
    frame.size = size.isValid() && !size.isEmpty() ? size : QSize(1, 1);
    const float aspect = float(frame.size.width()) / float(frame.size.height());
    frame.eye = eyePosition(m_camera);
    frame.view.lookAt(frame.eye, m_camera.target, QVector3D(0, 1, 0));
    const float nearPlane = std::max(1.0f, m_camera.distance * 0.05f);
    frame.projection.perspective(kFov, aspect, nearPlane, m_camera.distance + kStarRadius + 600.0f);
    frame.viewProjection = frame.projection * frame.view;
    frame.focal = float(frame.size.height()) * 0.5f / std::tan(qDegreesToRadians(kFov * 0.5f));
    const float radius = m_graph ? m_graph->shape.boundingRadius() : 120.0f;
    const float centreDepth = (frame.view.map(QVector3D(0, 0, 0))).z() * -1.0f;
    frame.fogNear = centreDepth - radius * 0.55f;
    frame.fogFar = centreDepth + radius * 1.1f;
    frame.time = now();
    if (!m_graph)
        return frame;
    const int n = m_graph->nodes.size();
    frame.nodes.resize(n);
    const float w = float(frame.size.width());
    const float h = float(frame.size.height());
    for (int i = 0; i < n && i < m_positions.size(); ++i) {
        Projected &p = frame.nodes[i];
        if (m_nodeState.value(i, Hidden) == Hidden)
            continue;
        const QVector3D v = frame.view.map(m_positions.at(i));
        const float depth = -v.z();
        if (depth < nearPlane)
            continue;
        const QVector4D clip = frame.projection * QVector4D(v, 1.0f);
        const float x = (clip.x() / clip.w() + 1.0f) * 0.5f * w;
        const float y = (1.0f - clip.y() / clip.w()) * 0.5f * h;
        p.screen = QPointF(x, y);
        p.depth = depth;
        p.radius = nodeRadius(i) * frame.focal / depth;
        p.fog = 1.0f - 0.72f * std::clamp((depth - frame.fogNear) / std::max(1.0f, frame.fogFar - frame.fogNear), 0.0f, 1.0f);
        const float margin = p.radius * 3.0f + 2.0f;
        p.onScreen = x > -margin && x < w + margin && y > -margin && y < h + margin;
    }
    return frame;
}

const SynapseView::Frame &SynapseView::currentFrame()
{
    if (!m_frameValid || m_frame.size != size()) {
        m_frame = makeFrame(size());
        m_frameValid = true;
    }
    return m_frame;
}

double SynapseView::now() const
{
    return m_fixedTime >= 0 ? m_fixedTime : double(m_clock.elapsed()) / 1000.0;
}

void SynapseView::setClock(double seconds)
{
    m_fixedTime = seconds;
    updateTimer();
    requestFrame();
}

void SynapseView::setCamera(const Camera &camera)
{
    m_camera = camera;
    m_camera.pitch = std::clamp(m_camera.pitch, -1.45f, 1.45f);
    m_camera.distance = std::clamp(m_camera.distance, 12.0f, 2400.0f);
    m_flight.active = false;
    requestFrame();
}

QVector3D SynapseView::curvePoint(int edge, float t) const
{
    const Edge &e = m_graph->edges.at(edge);
    const QVector3D a = m_positions.at(e.from);
    const QVector3D b = m_positions.at(e.to);
    const QVector3D mid = (a + b) * 0.5f;
    const QVector3D c = mid + (QVector3D(0, 0, 0) - mid) * kBow;
    const float u = 1.0f - t;
    return a * (u * u) + c * (2.0f * u * t) + b * (t * t);
}

int SynapseView::pick(const QPointF &point)
{
    if (!m_graph)
        return -1;
    const Frame &frame = currentFrame();
    int best = -1;
    float bestDepth = 0;
    int near = -1;
    float nearDistance = 9.0f;
    for (int i = 0; i < frame.nodes.size(); ++i) {
        const Projected &p = frame.nodes.at(i);
        if (!p.onScreen || m_nodeState.value(i, Hidden) == Hidden)
            continue;
        const float d = float(std::hypot(p.screen.x() - point.x(), p.screen.y() - point.y()));
        const float reach = std::max(p.radius, 3.0f) + 1.5f;
        if (d <= reach && (best < 0 || p.depth < bestDepth)) {
            best = i;
            bestDepth = p.depth;
        } else if (best < 0 && d < nearDistance) {
            near = i;
            nearDistance = d;
        }
    }
    if (best < 0)
        best = near;
    return best >= 0 ? m_graph->nodes.at(best).file : -1;
}

QPointF SynapseView::screenPosition(int file)
{
    if (!m_graph)
        return QPointF(qQNaN(), qQNaN());
    const int node = m_graph->nodeFor(file);
    const Frame &frame = currentFrame();
    if (node < 0 || node >= frame.nodes.size() || !frame.nodes.at(node).onScreen)
        return QPointF(qQNaN(), qQNaN());
    return frame.nodes.at(node).screen;
}

QImage SynapseView::renderSoftware(const QSize &size, qreal devicePixelRatio)
{
    QImage image(size * devicePixelRatio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(kBackground);
    const Frame frame = makeFrame(size);
    QPainter painter(&image);
    paintScene(painter, frame);
    paintOverlay(painter, frame);
    painter.end();
    return image;
}

const QPixmap &SynapseView::sphereSprite(const QColor &colour, float radius, qreal dpr)
{
    const int bucket = std::clamp(int(std::lround(radius * 2.0f)), 2, 220);
    const quint64 key = (quint64(colour.rgb() & 0xffffffu) << 24) | (quint64(bucket) << 4) | 1u
                        | (quint64(std::lround(dpr * 4)) << 56);
    auto it = m_sprites.find(key);
    if (it != m_sprites.end())
        return it.value();
    if (m_sprites.size() > 5000)
        m_sprites.clear();
    const float r = float(bucket) * 0.5f;
    const int side = int(std::ceil(r * 2.0f + 2.0f));
    QPixmap pixmap(QSize(side, side) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPointF centre(side * 0.5, side * 0.5);
    QRadialGradient body(centre, r, centre + QPointF(-0.38 * r, -0.42 * r));
    body.setColorAt(0.0, mix(colour, QColor(255, 255, 255), 0.55f));
    body.setColorAt(0.32, colour.lighter(112));
    body.setColorAt(0.78, colour.darker(165));
    body.setColorAt(1.0, colour.darker(260));
    painter.setPen(Qt::NoPen);
    painter.setBrush(body);
    painter.drawEllipse(centre, r, r);
    if (r >= 2.5f) {
        QRadialGradient rim(centre, r);
        rim.setColorAt(0.0, Qt::transparent);
        rim.setColorAt(0.72, Qt::transparent);
        rim.setColorAt(0.93, withAlpha(colour.lighter(150), 0.55f));
        rim.setColorAt(1.0, withAlpha(colour, 0.0f));
        painter.setBrush(rim);
        painter.drawEllipse(centre, r, r);
        QRadialGradient spec(centre + QPointF(-0.36 * r, -0.4 * r), r * 0.42);
        spec.setColorAt(0.0, QColor(255, 255, 255, 210));
        spec.setColorAt(1.0, QColor(255, 255, 255, 0));
        painter.setBrush(spec);
        painter.drawEllipse(centre + QPointF(-0.36 * r, -0.4 * r), r * 0.42, r * 0.42);
    }
    painter.end();
    return m_sprites.insert(key, pixmap).value();
}

const QPixmap &SynapseView::haloSprite(const QColor &colour, float radius, qreal dpr)
{
    const int bucket = std::clamp(int(std::lround(radius)), 2, 200);
    const quint64 key = (quint64(colour.rgba()) << 24) | (quint64(bucket) << 4) | 2u
                        | (quint64(std::lround(dpr * 4)) << 58);
    auto it = m_sprites.find(key);
    if (it != m_sprites.end())
        return it.value();
    if (m_sprites.size() > 5000)
        m_sprites.clear();
    const float r = float(bucket);
    const int side = int(std::ceil(r * 2.0f + 2.0f));
    QPixmap pixmap(QSize(side, side) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPointF centre(side * 0.5, side * 0.5);
    QRadialGradient glow(centre, r);
    glow.setColorAt(0.0, withAlpha(colour, colour.alphaF()));
    glow.setColorAt(0.25, withAlpha(colour, colour.alphaF() * 0.45f));
    glow.setColorAt(0.6, withAlpha(colour, colour.alphaF() * 0.1f));
    glow.setColorAt(1.0, withAlpha(colour, 0.0f));
    painter.setPen(Qt::NoPen);
    painter.setBrush(glow);
    painter.drawEllipse(centre, r, r);
    painter.end();
    return m_sprites.insert(key, pixmap).value();
}

void SynapseView::paintBackground(QPainter &painter, const Frame &frame)
{
    const qreal dpr = painter.device()->devicePixelRatioF();
    const QSize pixels = frame.size * dpr;
    if (m_backdrop.size() != pixels) {
        m_backdrop = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
        m_backdrop.setDevicePixelRatio(dpr);
        QPainter p(&m_backdrop);
        const QRectF r(QPointF(0, 0), QSizeF(frame.size));
        QLinearGradient sky(r.topLeft(), r.bottomLeft());
        sky.setColorAt(0.0, QColor(11, 17, 34));
        sky.setColorAt(1.0, QColor(3, 5, 10));
        p.fillRect(r, sky);
        QRadialGradient vignette(r.center(), std::hypot(r.width(), r.height()) * 0.62);
        vignette.setColorAt(0.0, QColor(0, 0, 0, 0));
        vignette.setColorAt(0.55, QColor(0, 0, 0, 0));
        vignette.setColorAt(1.0, QColor(0, 0, 0, 150));
        p.fillRect(r, vignette);
    }
    painter.drawImage(QPointF(0, 0), m_backdrop);
    painter.setRenderHint(QPainter::Antialiasing);
    const QVector4D centre = frame.viewProjection * QVector4D(0, 0, 0, 1);
    if (m_graph && centre.w() > 1.0f) {
        const QPointF c((centre.x() / centre.w() + 1.0f) * 0.5f * frame.size.width(),
                        (1.0f - centre.y() / centre.w()) * 0.5f * frame.size.height());
        const float radius = m_graph->shape.boundingRadius() * 1.25f * frame.focal / centre.w();
        QRadialGradient aura(c, radius);
        aura.setColorAt(0.0, QColor(40, 72, 160, 70));
        aura.setColorAt(0.5, QColor(30, 50, 120, 30));
        aura.setColorAt(1.0, QColor(20, 30, 80, 0));
        painter.setCompositionMode(QPainter::CompositionMode_Plus);
        painter.setPen(Qt::NoPen);
        painter.setBrush(aura);
        painter.drawEllipse(c, radius, radius);
    }
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setPen(Qt::NoPen);
    const float w = float(frame.size.width());
    const float h = float(frame.size.height());
    for (const QVector4D &star : m_stars) {
        const QVector4D clip = frame.viewProjection * QVector4D(star.toVector3D() * kStarRadius, 1.0f);
        if (clip.w() <= 1.0f)
            continue;
        const float x = (clip.x() / clip.w() + 1.0f) * 0.5f * w;
        const float y = (1.0f - clip.y() / clip.w()) * 0.5f * h;
        if (x < 0 || y < 0 || x > w || y > h)
            continue;
        const float size = 0.5f + 1.1f * star.w();
        painter.setBrush(QColor(170, 196, 255, int(40 + 150 * star.w())));
        painter.drawEllipse(QPointF(x, y), size, size);
    }
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
}

void SynapseView::paintScene(QPainter &painter, const Frame &frame)
{
    paintBackground(painter, frame);
    if (!m_graph || m_positions.size() != m_graph->nodes.size() || frame.nodes.isEmpty())
        return;
    const qreal dpr = painter.device()->devicePixelRatioF();
    const Graph &graph = *m_graph;
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setBrush(Qt::NoBrush);
    const auto project = [&](const QVector3D &world, QPointF &out, float &depth) {
        const QVector4D clip = frame.viewProjection * QVector4D(world, 1.0f);
        if (clip.w() <= 0.5f)
            return false;
        out = QPointF((clip.x() / clip.w() + 1.0f) * 0.5f * frame.size.width(),
                      (1.0f - clip.y() / clip.w()) * 0.5f * frame.size.height());
        depth = clip.w();
        return true;
    };
    painter.setPen(Qt::NoPen);
    for (const QVector3D &point : m_dust) {
        const QVector4D clip = frame.viewProjection * QVector4D(point, 1.0f);
        if (clip.w() <= 1.0f)
            continue;
        const QPointF at((clip.x() / clip.w() + 1.0f) * 0.5f * frame.size.width(),
                         (1.0f - clip.y() / clip.w()) * 0.5f * frame.size.height());
        const float fog = 1.0f - 0.8f * std::clamp((clip.w() - frame.fogNear) / std::max(1.0f, frame.fogFar - frame.fogNear), 0.0f, 1.0f);
        painter.setBrush(QColor(120, 160, 235, int(90 * fog)));
        painter.drawEllipse(at, 0.9, 0.9);
    }
    painter.setBrush(Qt::NoBrush);
    const QRectF bounds(QPointF(-40, -40), QSizeF(frame.size) + QSizeF(80, 80));
    QVector<std::pair<int, std::array<QPointF, 3>>> pulses;
    int plain = 0;
    for (int e : m_drawEdges) {
        const quint8 state = m_edgeState.at(e);
        if (state == EdgeNormal || state == EdgeDim)
            ++plain;
    }
    const int stride = m_fastFrame ? std::max(1, plain / 1200) : 1;
    int plainIndex = 0;
    for (int e : m_drawEdges) {
        const Edge &edge = graph.edges.at(e);
        if (m_fastFrame) {
            const quint8 kind = m_edgeState.at(e);
            if (kind == EdgeDim && plain > 1200)
                continue;
            if ((kind == EdgeNormal || kind == EdgeDim) && (plainIndex++ % stride) != 0)
                continue;
        }
        const Projected &pa = frame.nodes.at(edge.from);
        const Projected &pb = frame.nodes.at(edge.to);
        if (pa.depth <= 0 || pb.depth <= 0)
            continue;
        const QVector3D a = m_positions.at(edge.from);
        const QVector3D b = m_positions.at(edge.to);
        const QVector3D mid = (a + b) * 0.5f;
        QPointF control;
        float controlDepth = 0;
        if (!project(mid - mid * kBow, control, controlDepth))
            continue;
        QRectF box = QRectF(pa.screen, pb.screen).normalized();
        box = box.united(QRectF(control, QSizeF(1, 1)));
        if (!box.intersects(bounds))
            continue;
        const float fog = 0.5f * (pa.fog + pb.fog);
        const quint8 state = m_edgeState.at(e);
        QColor colour = edgeColour(e, 0.5f);
        colour.setAlphaF(colour.alphaF() * fog);
        QPainterPath path(pa.screen);
        path.quadTo(control, pb.screen);
        if (state == EdgeImport || state == EdgeImporter || state == EdgeCycle) {
            QLinearGradient gradient(pa.screen, pb.screen);
            QColor start = edgeColour(e, 0.0f);
            QColor end = edgeColour(e, 1.0f);
            start.setAlphaF(start.alphaF() * fog);
            end.setAlphaF(end.alphaF() * fog);
            gradient.setColorAt(0.0, start);
            gradient.setColorAt(1.0, end);
            QColor wide = colour;
            wide.setAlphaF(colour.alphaF() * 0.18f);
            painter.setPen(QPen(wide, edgeWidth(e) * 4.0f, Qt::SolidLine, Qt::RoundCap));
            painter.drawPath(path);
            painter.setPen(QPen(QBrush(gradient), edgeWidth(e) * 1.4f, Qt::SolidLine, Qt::RoundCap));
        } else {
            painter.setPen(QPen(colour, edgeWidth(e) * 1.1f, Qt::SolidLine, Qt::RoundCap));
        }
        painter.drawPath(path);
        if (edgePulses(e) && pulses.size() < 900)
            pulses.append({e, {pa.screen, control, pb.screen}});
    }
    for (const auto &pulse : pulses) {
        const Edge &edge = graph.edges.at(pulse.first);
        const float head = float(std::fmod(frame.time * 0.32 + phaseOf(edge.seed), 1.0));
        const QColor colour = mix(edgeColour(pulse.first, head), QColor(255, 255, 255), 0.55f);
        const float fog = 0.5f * (frame.nodes.at(edge.from).fog + frame.nodes.at(edge.to).fog);
        for (int k = 0; k < 4; ++k) {
            const float t = head - float(k) * 0.022f;
            if (t < 0)
                break;
            const QPointF at = quadPoint(pulse.second[0], pulse.second[1], pulse.second[2], t);
            const float size = (3.6f - float(k) * 0.7f);
            QColor c = colour;
            c.setAlphaF(std::clamp((0.9f - 0.2f * float(k)) * fog, 0.0f, 1.0f));
            const QPixmap &sprite = haloSprite(c, size, dpr);
            const QSizeF half = QSizeF(sprite.size()) / dpr / 2.0;
            painter.drawPixmap(at - QPointF(half.width(), half.height()), sprite);
        }
    }

    QVector<int> order;
    order.reserve(frame.nodes.size());
    for (int i = 0; i < frame.nodes.size(); ++i) {
        if (frame.nodes.at(i).onScreen && m_nodeState.at(i) != Hidden)
            order.append(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        const float da = frame.nodes.at(a).depth;
        const float db = frame.nodes.at(b).depth;
        return da != db ? da > db : a < b;
    });
    const bool many = order.size() > (m_fastFrame ? 1500 : 4000);
    const float pulse = 0.5f + 0.5f * float(std::sin(frame.time * 4.0));
    for (int i : order) {
        const Projected &p = frame.nodes.at(i);
        const quint8 state = m_nodeState.at(i);
        const QColor colour = m_nodeColour.at(i);
        const float r = std::max(0.9f, p.radius);
        const bool cycle = graph.nodes.at(i).cycle && state == Normal;
        if (state != Dim && r >= 1.2f && (!many || state != Normal || cycle)) {
            float intensity = state == Normal ? 0.32f : 0.62f;
            if (state == Selected)
                intensity = 0.9f;
            if (cycle)
                intensity = 0.4f + (m_pulses ? 0.45f * pulse : 0.2f);
            painter.setCompositionMode(QPainter::CompositionMode_Plus);
            painter.setOpacity(std::clamp(p.fog, 0.0f, 1.0f));
            const QPixmap &halo = haloSprite(withAlpha(colour, intensity), r * 3.0f, dpr);
            const QSizeF half = QSizeF(halo.size()) / dpr / 2.0;
            painter.drawPixmap(p.screen - QPointF(half.width(), half.height()), halo);
        }
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setOpacity(std::clamp(state == Dim ? p.fog * 0.85f : 0.25f + 0.75f * p.fog, 0.0f, 1.0f));
        QColor body = colour;
        if (cycle && m_pulses)
            body = mix(colour, QColor(255, 190, 180), 0.25f * pulse);
        if (r < 1.6f) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(body);
            painter.drawEllipse(p.screen, r, r);
            continue;
        }
        const QPixmap &sprite = sphereSprite(body, r, dpr);
        const QSizeF half = QSizeF(sprite.size()) / dpr / 2.0;
        painter.drawPixmap(p.screen - QPointF(half.width(), half.height()), sprite);
        if (state == Selected) {
            painter.setOpacity(1.0);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(255, 255, 255, 200), 1.4));
            painter.drawEllipse(p.screen, r + 4.0, r + 4.0);
            painter.setPen(QPen(QColor(120, 200, 255, 90), 1.0));
            painter.drawEllipse(p.screen, r + 8.0, r + 8.0);
        }
    }
    painter.setOpacity(1.0);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
}

void SynapseView::paintLabels(QPainter &painter, const Frame &frame)
{
    if (!m_graph || frame.nodes.isEmpty())
        return;
    const Graph &graph = *m_graph;
    const insight::Snapshot &snapshot = *graph.snapshot;
    QVector<QRectF> taken;
    QFont base = font();
    base.setPixelSize(12);
    QFont bold = base;
    bold.setWeight(QFont::DemiBold);
    bold.setPixelSize(13);
    QFont small = base;
    small.setPixelSize(10);
    small.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    small.setWeight(QFont::DemiBold);
    const QRectF screen(QPointF(0, 0), QSizeF(frame.size));
    const auto place = [&](const QPointF &anchor, float offset, const QString &text, const QFont &f,
                           const QColor &colour, const QColor &border, bool pill) {
        const QFontMetricsF metrics(f);
        const qreal w = metrics.horizontalAdvance(text) + (pill ? 14 : 0);
        const qreal h = metrics.height() + (pill ? 6 : 0);
        QRectF rect(anchor.x() + offset, anchor.y() - h / 2, w, h);
        if (rect.right() > screen.right() - 6)
            rect.moveRight(anchor.x() - offset);
        if (!screen.adjusted(4, 4, -4, -4).contains(rect))
            return false;
        for (const QRectF &other : taken) {
            if (other.adjusted(-3, -2, 3, 2).intersects(rect))
                return false;
        }
        taken.append(rect);
        painter.setFont(f);
        if (pill) {
            painter.setPen(QPen(border, 1.0));
            painter.setBrush(QColor(10, 14, 26, 205));
            painter.drawRoundedRect(rect, h / 2, h / 2);
            painter.setPen(colour);
            painter.drawText(rect, Qt::AlignCenter, text);
        } else {
            painter.setPen(QColor(0, 0, 0, 150));
            painter.drawText(rect.translated(0, 1), Qt::AlignLeft | Qt::AlignVCenter, text);
            painter.setPen(colour);
            painter.drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, text);
        }
        return true;
    };
    const auto nodeLabel = [&](int node, const QColor &border, bool strong) {
        const Projected &p = frame.nodes.at(node);
        if (!p.onScreen)
            return;
        const QString name = fileName(snapshot.files.at(graph.nodes.at(node).file).path);
        place(p.screen, std::max(4.0f, p.radius) + 7.0f, name, strong ? bold : base, kText, border, true);
    };
    if (m_hovered >= 0) {
        const int node = graph.nodeFor(m_hovered);
        if (node >= 0 && m_nodeState.at(node) != Hidden)
            nodeLabel(node, QColor(255, 255, 255, 120), true);
    }
    if (m_selected >= 0) {
        const int node = graph.nodeFor(m_selected);
        if (node >= 0 && m_nodeState.at(node) != Hidden)
            nodeLabel(node, QColor(120, 200, 255, 220), true);
        QVector<int> neighbours;
        for (int i = 0; i < graph.nodes.size(); ++i) {
            const quint8 state = m_nodeState.at(i);
            if (state == Import || state == Importer)
                neighbours.append(i);
        }
        std::sort(neighbours.begin(), neighbours.end(), [&](int a, int b) {
            const int da = std::max(m_neighbours.imports.value(graph.nodes.at(a).file, 9),
                                    0) + m_neighbours.importers.value(graph.nodes.at(a).file, 0);
            const int db = std::max(m_neighbours.imports.value(graph.nodes.at(b).file, 9),
                                    0) + m_neighbours.importers.value(graph.nodes.at(b).file, 0);
            if (da != db)
                return da < db;
            return graph.nodes.at(a).fanIn > graph.nodes.at(b).fanIn;
        });
        int shown = 0;
        for (int i : neighbours) {
            if (shown >= 28)
                break;
            const QColor border = m_nodeState.at(i) == Import ? withAlpha(kImport, 0.6f)
                                                              : withAlpha(kImporter, 0.6f);
            nodeLabel(i, border, false);
            ++shown;
        }
    } else {
        if (m_isolatedCycle >= 0) {
            for (int file : graph.cycles.at(m_isolatedCycle).files) {
                const int node = graph.nodeFor(file);
                if (node >= 0)
                    nodeLabel(node, withAlpha(kCycle, 0.7f), false);
            }
        }
        for (int hub : m_hubs)
            nodeLabel(hub, QColor(255, 255, 255, 50), false);
        if (m_showMode == ShowMode::Everything && m_query.isEmpty() && m_isolatedCycle < 0) {
            QVector<int> lobes(graph.lobes.size());
            std::iota(lobes.begin(), lobes.end(), 0);
            std::sort(lobes.begin(), lobes.end(), [&](int a, int b) {
                return graph.lobes.at(a).nodes.size() > graph.lobes.at(b).nodes.size();
            });
            int shown = 0;
            for (int l : lobes) {
                if (shown >= 18 || l >= m_lobeCentres.size() || graph.lobes.at(l).nodes.size() < 2)
                    break;
                const QVector4D clip = frame.viewProjection * QVector4D(m_lobeCentres.at(l), 1.0f);
                if (clip.w() <= 1.0f)
                    continue;
                const QPointF at((clip.x() / clip.w() + 1.0f) * 0.5f * frame.size.width(),
                                 (1.0f - clip.y() / clip.w()) * 0.5f * frame.size.height());
                const QColor colour = m_colourBy == ColourBy::Module
                                          ? withAlpha(lobeColour(l).lighter(115), 0.9f)
                                          : QColor(200, 210, 230, 190);
                const QString text = shortLobeName(graph.lobes.at(l).name).toUpper();
                const QFontMetricsF metrics(small);
                if (place(at - QPointF(metrics.horizontalAdvance(text) / 2, 0), 0, text, small, colour,
                          Qt::transparent, false))
                    ++shown;
            }
        }
    }
}

void SynapseView::paintLegend(QPainter &painter, const Frame &frame)
{
    if (!m_graph)
        return;
    QList<QPair<QColor, QString>> rows;
    const Graph &graph = *m_graph;
    switch (m_colourBy) {
    case ColourBy::Module: {
        QVector<int> lobes(graph.lobes.size());
        std::iota(lobes.begin(), lobes.end(), 0);
        std::sort(lobes.begin(), lobes.end(), [&](int a, int b) {
            return graph.lobes.at(a).nodes.size() > graph.lobes.at(b).nodes.size();
        });
        for (int i = 0; i < lobes.size() && i < 6; ++i)
            rows.append({lobeColour(lobes.at(i)), shortLobeName(graph.lobes.at(lobes.at(i)).name)});
        break;
    }
    case ColourBy::Language: {
        QHash<QString, int> counts;
        for (const Node &node : graph.nodes)
            counts[graph.snapshot->files.at(node.file).language] += 1;
        QList<QPair<int, QString>> sorted;
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
            sorted.append({-it.value(), it.key()});
        std::sort(sorted.begin(), sorted.end());
        for (int i = 0; i < sorted.size() && i < 6; ++i)
            rows.append({languageColour(sorted.at(i).second),
                         sorted.at(i).second.isEmpty() ? QStringLiteral("Other") : sorted.at(i).second});
        break;
    }
    case ColourBy::Health:
        rows.append({healthColour(10), QStringLiteral("Healthy")});
        rows.append({healthColour(5), QStringLiteral("Problematic")});
        rows.append({healthColour(1), QStringLiteral("Unhealthy")});
        break;
    case ColourBy::Hotspot:
        rows.append({hotspotColour(1.0), QStringLiteral("Changes often")});
        rows.append({hotspotColour(0.5), QStringLiteral("Sometimes")});
        rows.append({hotspotColour(0.0), QStringLiteral("Rarely")});
        break;
    }
    if (m_selected >= 0) {
        rows.append({kImport, QStringLiteral("Imports")});
        rows.append({kImporter, QStringLiteral("Imported by")});
    }
    if (!graph.cycles.isEmpty())
        rows.append({kCycle, QStringLiteral("Circular dependency")});
    QFont f = font();
    f.setPixelSize(11);
    painter.setFont(f);
    const QFontMetricsF metrics(f);
    const qreal rowHeight = 18;
    qreal y = frame.size.height() - 16 - rows.size() * rowHeight;
    for (const auto &row : rows) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(row.first);
        painter.drawEllipse(QPointF(22, y + rowHeight / 2), 4.0, 4.0);
        painter.setPen(QColor(200, 210, 225, 200));
        painter.drawText(QRectF(34, y, 260, rowHeight), Qt::AlignLeft | Qt::AlignVCenter,
                         metrics.elidedText(row.second, Qt::ElideMiddle, 240));
        y += rowHeight;
    }
    const QString hint = QStringLiteral("Drag to orbit · Right-drag to pan · Scroll to zoom · Double-click to fly in");
    painter.setPen(QColor(139, 148, 158, 170));
    painter.drawText(QRectF(0, frame.size.height() - 30, frame.size.width() - 18, 20),
                     Qt::AlignRight | Qt::AlignVCenter, hint);
}

void SynapseView::paintOverlay(QPainter &painter, const Frame &frame)
{
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setOpacity(1.0);
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    if (!m_graph || m_graph->nodes.isEmpty()) {
        QFont f = font();
        f.setPixelSize(15);
        painter.setFont(f);
        painter.setPen(QColor(200, 210, 225));
        const QString text = m_graph && m_graph->nodes.isEmpty()
                                 ? QStringLiteral("No file imports another file of this repository yet.")
                                 : m_message;
        painter.drawText(QRectF(QPointF(0, 0), QSizeF(frame.size)).adjusted(40, 0, -40, 0),
                         Qt::AlignCenter | Qt::TextWordWrap, text);
        return;
    }
    paintLabels(painter, frame);
    paintLegend(painter, frame);
    if (m_progress < 1.0) {
        QFont f = font();
        f.setPixelSize(12);
        painter.setFont(f);
        painter.setPen(QColor(200, 214, 235, 220));
        painter.drawText(QRectF(20, 14, 300, 18), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("Growing the brain · %1%").arg(int(m_progress * 100)));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 30));
        painter.drawRoundedRect(QRectF(20, 36, 160, 3), 1.5, 1.5);
        painter.setBrush(QColor(88, 166, 255));
        painter.drawRoundedRect(QRectF(20, 36, 160 * m_progress, 3), 1.5, 1.5);
    }
    if (m_hovered >= 0 && !m_dragging && m_graph->snapshot && m_hovered < m_graph->snapshot->files.size()) {
        const insight::FileInfo &file = m_graph->snapshot->files.at(m_hovered);
        const int node = m_graph->nodeFor(m_hovered);
        if (node >= 0 && node < frame.nodes.size() && frame.nodes.at(node).onScreen) {
            QStringList lines;
            lines << file.path;
            lines << QStringLiteral("Imports %1 · Imported by %2 · %3 lines")
                         .arg(file.dependsOn.size())
                         .arg(file.dependedOnBy.size())
                         .arg(file.codeLines);
            if (file.cycle >= 0)
                lines << QStringLiteral("Part of a circular dependency");
            QFont f = font();
            f.setPixelSize(11);
            const QFontMetricsF metrics(f);
            qreal w = 0;
            for (const QString &line : lines)
                w = std::max(w, metrics.horizontalAdvance(line));
            w = std::min<qreal>(w + 20, frame.size.width() * 0.6);
            const qreal h = lines.size() * (metrics.height() + 2) + 12;
            const Projected &p = frame.nodes.at(node);
            QRectF card(p.screen.x() + p.radius + 12, p.screen.y() + 14, w, h);
            if (card.right() > frame.size.width() - 8)
                card.moveRight(p.screen.x() - p.radius - 12);
            if (card.bottom() > frame.size.height() - 8)
                card.moveBottom(p.screen.y() - 14);
            painter.setPen(QPen(QColor(255, 255, 255, 40), 1));
            painter.setBrush(QColor(13, 18, 32, 235));
            painter.drawRoundedRect(card, 8, 8);
            painter.setFont(f);
            qreal y = card.top() + 6;
            for (int i = 0; i < lines.size(); ++i) {
                painter.setPen(i == 0 ? kText : (i == 2 ? kCycle : kMuted));
                painter.drawText(QRectF(card.left() + 10, y, card.width() - 20, metrics.height()),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 metrics.elidedText(lines.at(i), Qt::ElideMiddle, card.width() - 20));
                y += metrics.height() + 2;
            }
        }
    }
}

void SynapseView::paintEvent(QPaintEvent *)
{
    if (m_surface)
        return;
    m_frame = makeFrame(size());
    m_frameValid = true;
    m_fastFrame = m_dragging || m_timer.isActive();
    QPainter painter(this);
    paintScene(painter, m_frame);
    paintOverlay(painter, m_frame);
    m_fastFrame = false;
}

void SynapseView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_surface)
        m_surface->setGeometry(rect());
    m_frameValid = false;
    m_backdrop = QImage();
}

void SynapseView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_lastTick = m_clock.elapsed();
    updateTimer();
}

void SynapseView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_timer.stop();
}

void SynapseView::buildNodeVertices(QVector<float> &out) const
{
    out.clear();
    if (!m_graph || m_positions.size() != m_graph->nodes.size())
        return;
    out.reserve(m_visibleNodes * 4 * 11);
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < m_graph->nodes.size(); ++i) {
        const quint8 state = m_nodeState.value(i, Hidden);
        if (state == Hidden)
            continue;
        const QVector3D p = m_positions.at(i);
        const float radius = nodeRadius(i);
        const QColor colour = m_nodeColour.at(i);
        float glow = 0.38f;
        if (state == Dim)
            glow = 0.0f;
        else if (state == Selected)
            glow = 1.0f;
        else if (state != Normal)
            glow = 0.7f;
        const bool cycle = m_graph->nodes.at(i).cycle && state == Normal && m_pulses;
        for (const auto &corner : corners) {
            out << p.x() << p.y() << p.z() << corner[0] << corner[1] << radius
                << float(colour.redF()) << float(colour.greenF()) << float(colour.blueF()) << glow
                << (cycle ? 1.0f : 0.0f);
        }
    }
}

void SynapseView::buildEdgeVertices(QVector<float> &out, int &segments) const
{
    out.clear();
    segments = 0;
    if (!m_graph || m_positions.size() != m_graph->nodes.size() || m_drawEdges.isEmpty())
        return;
    const int count = m_drawEdges.size();
    segments = count <= 2000 ? 16 : (count <= 8000 ? 10 : 6);
    out.reserve(count * ((segments + 1) * 2 + 2) * 15);
    bool first = true;
    for (int e : m_drawEdges) {
        const Edge &edge = m_graph->edges.at(e);
        const QVector3D a = m_positions.at(edge.from);
        const QVector3D b = m_positions.at(edge.to);
        const float pulse = edgePulses(e) ? 1.0f : 0.0f;
        const float phase = phaseOf(edge.seed);
        const float width = edgeWidth(e);
        const auto pushVertex = [&](float t, float side) {
            const QColor c = edgeColour(e, t);
            out << a.x() << a.y() << a.z() << b.x() << b.y() << b.z() << t << side << pulse << phase
                << float(c.redF()) << float(c.greenF()) << float(c.blueF()) << float(c.alphaF()) << width;
        };
        if (!first)
            pushVertex(0.0f, -1.0f);
        for (int s = 0; s <= segments; ++s) {
            const float t = float(s) / float(segments);
            pushVertex(t, -1.0f);
            pushVertex(t, 1.0f);
        }
        pushVertex(1.0f, 1.0f);
        first = false;
    }
}

Camera SynapseView::fittedCamera(const QVector<int> &nodes) const
{
    Camera camera = m_camera;
    QVector<QVector3D> points;
    if (m_graph && m_positions.size() == m_graph->nodes.size()) {
        if (nodes.isEmpty()) {
            for (int i = 0; i < m_graph->nodes.size(); ++i) {
                if (m_nodeState.value(i, Hidden) != Hidden)
                    points.append(m_positions.at(i));
            }
        } else {
            for (int node : nodes) {
                if (node >= 0 && node < m_positions.size())
                    points.append(m_positions.at(node));
            }
        }
    }
    const bool whole = nodes.isEmpty() && (points.size() == (m_graph ? m_graph->nodes.size() : 0)
                                           || m_progress < 1.0);
    if (whole || points.isEmpty()) {
        const BrainShape shape = m_graph ? m_graph->shape : BrainShape();
        points.clear();
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sz = -1; sz <= 1; sz += 2) {
                points.append(QVector3D(sx * shape.halfWidth, shape.halfHeightTop, sz * shape.halfLength) * 0.8f);
                points.append(QVector3D(sx * shape.halfWidth, -shape.halfHeightBottom, sz * shape.halfLength) * 0.8f);
            }
        points.append(QVector3D(0, 0, shape.halfLength));
        points.append(QVector3D(0, 0, -shape.halfLength));
        points.append(QVector3D(shape.halfWidth, 0, 0));
        points.append(QVector3D(-shape.halfWidth, 0, 0));
        points.append(QVector3D(0, shape.halfHeightTop, 0));
        points.append(QVector3D(0, -shape.halfHeightBottom, 0));
    }
    QVector3D lo = points.first();
    QVector3D hi = points.first();
    for (const QVector3D &p : points) {
        lo = QVector3D(std::min(lo.x(), p.x()), std::min(lo.y(), p.y()), std::min(lo.z(), p.z()));
        hi = QVector3D(std::max(hi.x(), p.x()), std::max(hi.y(), p.y()), std::max(hi.z(), p.z()));
    }
    camera.target = (lo + hi) * 0.5f;
    const float cp = std::cos(camera.pitch);
    const QVector3D back(cp * std::sin(camera.yaw), std::sin(camera.pitch), cp * std::cos(camera.yaw));
    const QVector3D forward = -back;
    const QVector3D right = QVector3D::crossProduct(forward, QVector3D(0, 1, 0)).normalized();
    const QVector3D up = QVector3D::crossProduct(right, forward);
    const float aspect = float(std::max(1, width())) / float(std::max(1, height()));
    const float tanV = std::tan(qDegreesToRadians(kFov * 0.5f)) * 0.86f;
    const float tanH = tanV * std::max(0.4f, aspect);
    float distance = 0;
    for (const QVector3D &p : points) {
        const QVector3D q = p - camera.target;
        const float x = m_autoRotate ? std::hypot(q.x(), q.z()) : std::abs(QVector3D::dotProduct(q, right));
        const float y = std::abs(QVector3D::dotProduct(q, up));
        const float z = QVector3D::dotProduct(q, back);
        distance = std::max({distance, z + x / tanH, z + y / tanV});
    }
    const float minimum = m_graph ? m_graph->spacing * 4.0f : 40.0f;
    camera.distance = std::clamp(distance, std::max(30.0f, minimum), 2400.0f);
    return camera;
}

void SynapseView::fit(bool animate)
{
    QVector<int> nodes;
    if (m_isolatedCycle >= 0 && m_graph) {
        for (int file : m_graph->cycles.at(m_isolatedCycle).files)
            nodes.append(m_graph->nodeFor(file));
    }
    const Camera camera = fittedCamera(nodes);
    if (animate)
        flyTo(camera);
    else
        setCamera(camera);
}

void SynapseView::flyToFile(int file)
{
    if (!m_graph)
        return;
    const int node = m_graph->nodeFor(file);
    if (node < 0 || node >= m_positions.size())
        return;
    QVector<int> nodes{node};
    for (int e : m_graph->outEdges.at(node))
        nodes.append(m_graph->edges.at(e).to);
    for (int e : m_graph->inEdges.at(node))
        nodes.append(m_graph->edges.at(e).from);
    Camera camera = fittedCamera(nodes);
    camera.target = m_positions.at(node) * 0.7f + camera.target * 0.3f;
    camera.distance = std::clamp(camera.distance * 1.1f, m_graph->spacing * 6.0f, m_camera.distance);
    flyTo(camera);
}

void SynapseView::flyToCycle(int cycle)
{
    if (!m_graph || cycle < 0 || cycle >= m_graph->cycles.size())
        return;
    QVector<int> nodes;
    for (int file : m_graph->cycles.at(cycle).files)
        nodes.append(m_graph->nodeFor(file));
    Camera camera = fittedCamera(nodes);
    camera.distance = std::max(camera.distance * 1.15f, m_graph->spacing * 6.0f);
    flyTo(camera);
}

void SynapseView::flyTo(const Camera &camera)
{
    m_flight.active = true;
    m_flight.from = m_camera;
    m_flight.to = camera;
    m_flight.to.yaw = m_camera.yaw;
    m_flight.start = m_clock.elapsed();
    m_flight.duration = 900;
    if (m_fixedTime >= 0 || !isVisible()) {
        m_flight.active = false;
        m_camera = m_flight.to;
        requestFrame();
        return;
    }
    pauseRotation();
    updateTimer();
}

void SynapseView::requestFrame()
{
    m_frameValid = false;
    if (m_surface)
        m_surface->update();
    else
        update();
}

bool SynapseView::rotating() const
{
    return m_autoRotate && !m_dragging && !m_flight.active && m_fixedTime < 0 && m_graph
           && m_clock.elapsed() >= m_pauseUntil;
}

bool SynapseView::animating() const
{
    if (!m_graph || m_fixedTime >= 0)
        return false;
    if (m_flight.active || (m_autoRotate && !m_dragging))
        return true;
    return m_pulses && (m_pulseEdges > 0 || m_cycleNodesVisible);
}

void SynapseView::pauseRotation()
{
    m_pauseUntil = m_clock.elapsed() + 2500;
}

void SynapseView::updateTimer()
{
    const bool need = isVisible() && animating();
    if (need) {
        const int interval = m_surface ? 16 : 33;
        if (!m_timer.isActive() || m_timer.interval() != interval) {
            m_lastTick = m_clock.elapsed();
            m_timer.start(interval);
        }
    } else {
        m_timer.stop();
    }
}

void SynapseView::tick()
{
    const qint64 nowMs = m_clock.elapsed();
    const float dt = std::clamp(float(nowMs - m_lastTick) / 1000.0f, 0.0f, 0.1f);
    m_lastTick = nowMs;
    if (m_flight.active) {
        const float u = std::clamp(float(nowMs - m_flight.start) / float(m_flight.duration), 0.0f, 1.0f);
        const float e = easeInOut(u);
        m_camera.target = m_flight.from.target + (m_flight.to.target - m_flight.from.target) * e;
        m_camera.distance = m_flight.from.distance + (m_flight.to.distance - m_flight.from.distance) * e;
        m_camera.pitch = m_flight.from.pitch + (m_flight.to.pitch - m_flight.from.pitch) * e;
        if (u >= 1.0f) {
            m_flight.active = false;
            pauseRotation();
        }
    }
    if (rotating())
        m_camera.yaw += dt * 0.12f;
    if (!animating() || !isVisible()) {
        m_timer.stop();
        update();
    }
    requestFrame();
}

void SynapseView::setHovered(int file)
{
    if (file == m_hovered)
        return;
    m_hovered = file;
    setCursor(file >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    requestFrame();
}

void SynapseView::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    m_pressPos = event->position();
    m_lastPos = event->position();
    m_moved = false;
    m_dragging = true;
    m_panning = event->button() == Qt::RightButton || event->button() == Qt::MiddleButton
                || (event->modifiers() & Qt::ShiftModifier);
    m_flight.active = false;
    pauseRotation();
    event->accept();
}

void SynapseView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    if (m_dragging && event->buttons() != Qt::NoButton) {
        const QPointF delta = pos - m_lastPos;
        m_lastPos = pos;
        if ((pos - m_pressPos).manhattanLength() > 3)
            m_moved = true;
        if (!m_moved)
            return;
        if (m_panning) {
            const float cp = std::cos(m_camera.pitch);
            const QVector3D back(cp * std::sin(m_camera.yaw), std::sin(m_camera.pitch), cp * std::cos(m_camera.yaw));
            const QVector3D right = QVector3D::crossProduct(-back, QVector3D(0, 1, 0)).normalized();
            const QVector3D up = QVector3D::crossProduct(right, -back);
            const float scale = 2.0f * m_camera.distance * std::tan(qDegreesToRadians(kFov * 0.5f))
                                / float(std::max(1, height()));
            m_camera.target -= right * float(delta.x()) * scale;
            m_camera.target += up * float(delta.y()) * scale;
        } else {
            m_camera.yaw -= float(delta.x()) * 0.008f;
            m_camera.pitch = std::clamp(m_camera.pitch + float(delta.y()) * 0.008f, -1.45f, 1.45f);
        }
        setHovered(-1);
        pauseRotation();
        requestFrame();
        return;
    }
    setHovered(pick(pos));
}

void SynapseView::mouseReleaseEvent(QMouseEvent *event)
{
    const bool click = !m_moved && event->button() == Qt::LeftButton;
    m_dragging = false;
    m_panning = false;
    pauseRotation();
    if (click) {
        const int file = pick(event->position());
        if (file >= 0) {
            selectFile(file, false);
            if (m_onSelectionChanged)
                m_onSelectionChanged(file);
        } else if (m_selected >= 0) {
            clearSelection();
            if (m_onSelectionChanged)
                m_onSelectionChanged(-1);
        }
    }
    updateTimer();
    requestFrame();
}

void SynapseView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int file = pick(event->position());
    if (file >= 0) {
        const bool changed = file != m_selected;
        selectFile(file, true);
        if (changed && m_onSelectionChanged)
            m_onSelectionChanged(file);
        if (m_onActivated)
            m_onActivated(file);
    } else {
        fit(true);
    }
}

void SynapseView::wheelEvent(QWheelEvent *event)
{
    const float steps = float(event->angleDelta().y()) / 120.0f;
    if (steps == 0.0f)
        return;
    const float factor = std::pow(0.87f, steps);
    const float newDistance = std::clamp(m_camera.distance * factor, 12.0f, 2400.0f);
    const float applied = newDistance / m_camera.distance;
    const QPointF pos = event->position();
    const float ndcX = float(pos.x() / std::max(1, width())) * 2.0f - 1.0f;
    const float ndcY = 1.0f - float(pos.y() / std::max(1, height())) * 2.0f;
    const float cp = std::cos(m_camera.pitch);
    const QVector3D back(cp * std::sin(m_camera.yaw), std::sin(m_camera.pitch), cp * std::cos(m_camera.yaw));
    const QVector3D forward = -back;
    const QVector3D right = QVector3D::crossProduct(forward, QVector3D(0, 1, 0)).normalized();
    const QVector3D up = QVector3D::crossProduct(right, forward);
    const float tanV = std::tan(qDegreesToRadians(kFov * 0.5f));
    const float tanH = tanV * float(std::max(1, width())) / float(std::max(1, height()));
    const QVector3D under = m_camera.target + (right * ndcX * tanH + up * ndcY * tanV) * m_camera.distance;
    m_camera.target += (under - m_camera.target) * (1.0f - applied);
    m_camera.distance = newDistance;
    m_flight.active = false;
    pauseRotation();
    requestFrame();
    event->accept();
}

void SynapseView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        if (m_selected >= 0 || m_isolatedCycle >= 0) {
            clearSelection();
            if (m_onSelectionChanged)
                m_onSelectionChanged(-1);
        }
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_F && event->modifiers() == Qt::NoModifier) {
        fit(true);
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Space) {
        setAutoRotate(!m_autoRotate);
        if (m_onAutoRotateChanged)
            m_onAutoRotateChanged(m_autoRotate);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SynapseView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    setHovered(-1);
}

}
