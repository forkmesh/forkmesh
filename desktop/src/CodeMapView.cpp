#include "CodeMapView.h"
#include "Theme.h"

#include <QBuffer>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImageReader>
#include <QKeyEvent>
#include <QLineF>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QTimerEvent>
#include <QToolTip>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace forkmesh::codemap;

namespace {

constexpr double kWorldHeight = 1000;
constexpr double kFieldOfView = 40.0 * M_PI / 180.0;
constexpr int kMaxBitmapSide = 2048;
constexpr qint64 kMaxBitmapPixels = 4 * 1024 * 1024;
constexpr int kCloseupScale = 2; // the 3D close-up may use bitmaps this much larger
constexpr qint64 kBitmapBudgetPixels = 24 * 1024 * 1024;
constexpr qint64 kTextBudgetChars = 96 * 1024 * 1024;
constexpr int kBatchPaths = 60;
constexpr qint64 kBatchBytes = 3 * 1024 * 1024;
constexpr qint64 kPictureBudgetPixels = 24 * 1024 * 1024;

QString clipLabel(const QString &text, double maxWidth, double fontPixels)
{
    const int room = int(maxWidth / (fontPixels * 0.6));
    if (room < 3)
        return {};
    return text.size() <= room ? text : text.left(room - 1) + QChar(0x2026);
}

QString formatBytes(qint64 bytes)
{
    if (bytes < 1024)
        return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', bytes < 10240 ? 1 : 0);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / 1048576.0, 0, 'f', bytes < 10485760 ? 1 : 0);
    return QStringLiteral("%1 GB").arg(bytes / 1073741824.0, 0, 'f', 1);
}

double easeInOut(double t)
{
    return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2;
}

quint64 bitmapKey(int index, int revision, double level)
{
    return (quint64(quint32(index)) << 32) | (quint64(quint16(revision)) << 16)
        | quint64(quint16(std::lround(level * 8)));
}

} // namespace

CodeMapView::CodeMapView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(320, 240);
    setAccessibleName(QStringLiteral("Code map"));
    describeForAccessibility();
    applyTheme();
}

void CodeMapView::setWording(const Wording &wording)
{
    m_wording = wording;
    describeForAccessibility();
}

void CodeMapView::describeForAccessibility()
{
    const Wording &w = m_wording;
    QString folder = w.folder;
    if (!folder.isEmpty())
        folder[0] = folder[0].toUpper();
    setAccessibleDescription(m_mode == Mode::Flat
        ? QStringLiteral("Treemap of every %1 in the %2, sized by bytes. Scroll to zoom, drag to "
                         "pan, click to zoom into a %3 or %1.").arg(w.leaf, w.whole, w.folder)
        : QStringLiteral("3D view of the %1. %2 height follows %3 size. Drag to rotate, "
                         "right-drag to pan, scroll to zoom, click to fly to a %3 or %4.")
              .arg(w.whole, folder, w.folder, w.leaf));
}

CodeMapView::~CodeMapView() = default;

// Language hues are the validated eight-slot categorical order (blue, orange,
// aqua, yellow, magenta, green, violet, red), assigned to a repository's
// languages by bytes and never cycled: a ninth language is gray "Other". A
// treemap puts every pair of hues side by side, which no eight-hue order can
// keep separable for every reader, so identity never rides on colour alone —
// the legend, the folder and file labels and the hover readout all name it.
void CodeMapView::applyTheme()
{
    m_theme.dark = palette().color(QPalette::Window).lightness() < 128;
    const char *darkHues[] = {"#3987e5", "#d95926", "#199e70", "#c98500",
                              "#d55181", "#008300", "#9085e9", "#e66767"};
    const char *lightHues[] = {"#2a78d6", "#eb6834", "#1baf7a", "#eda100",
                               "#e87ba4", "#008300", "#4a3aa7", "#e34948"};
    const char *darkCode[] = {"#c9d1d9", "#ff7b72", "#a5d6ff", "#8b949e",
                              "#79c0ff", "#d2a8ff", "#8b949e"};
    const char *lightCode[] = {"#24292f", "#cf222e", "#0a3069", "#6e7781",
                               "#0550ae", "#8250df", "#6e7781"};
    for (int i = 0; i < kHueSlots; ++i)
        m_theme.hues[i] = QColor(QLatin1String(m_theme.dark ? darkHues[i] : lightHues[i]));
    for (int i = 0; i < 7; ++i)
        m_theme.code[i] = QColor(QLatin1String(m_theme.dark ? darkCode[i] : lightCode[i]));
    m_theme.background = ::Theme::remapColor(QColor(m_theme.dark ? "#010409" : "#ffffff"));
    m_theme.tile = QColor(m_theme.dark ? "#10141a" : "#ffffff");
    m_theme.foreground = ::Theme::remapColor(QColor(m_theme.dark ? "#f0f6fc" : "#1f2328"));
    m_theme.muted = QColor(m_theme.dark ? "#9198a1" : "#59636e");
    m_theme.accent = QColor(m_theme.dark ? "#58a6ff" : "#0969da");
    m_theme.gray = QColor("#8a8a8a");
    m_theme.pill = m_theme.dark ? QColor(1, 4, 9, 210) : QColor(255, 255, 255, 225);
    m_bitmaps.clear();
    m_bitmapPixels = 0;
}

void CodeMapView::setCategoryColors(const QHash<QString, QColor> &colors)
{
    m_categoryColors = colors;
    m_bitmaps.clear();
    m_bitmapPixels = 0;
    m_cards.clear();
    update();
}

QColor CodeMapView::languageColor(const QString &language) const
{
    const auto fixed = m_categoryColors.constFind(language);
    if (fixed != m_categoryColors.constEnd())
        return fixed.value();
    for (int i = 0; i < m_tree.languages.size() && i < kHueSlots; ++i) {
        if (m_tree.languages.at(i).first == language)
            return m_theme.hues[i];
    }
    return m_theme.gray;
}

QColor CodeMapView::nodeColor(const Node &node) const
{
    const auto fixed = m_categoryColors.constFind(node.language);
    if (fixed != m_categoryColors.constEnd())
        return fixed.value();
    return node.hue >= 0 && node.hue < kHueSlots ? m_theme.hues[node.hue] : m_theme.gray;
}

QColor CodeMapView::directoryFill(int depth) const
{
    const double step = std::min(depth, 8);
    return m_theme.dark ? QColor::fromHslF(215.0 / 360.0, 0.22, (5 + step * 2.2) / 100.0)
                        : QColor::fromHslF(210.0 / 360.0, 0.18, (97 - step * 2) / 100.0);
}

void CodeMapView::setTree(Tree tree)
{
    m_tree = std::move(tree);
    m_fileIndex.clear();
    for (int i = 0; i < m_tree.nodes.size(); ++i) {
        if (m_tree.nodes.at(i).type == Node::File)
            m_fileIndex.insert(m_tree.nodes.at(i).path, i);
    }
    m_bitmaps.clear();
    m_bitmapPixels = 0;
    m_queue.clear();
    m_loaded.clear();
    m_loadedChars = 0;
    m_pictures.clear();
    m_picturePixels = 0;
    m_cards.clear();
    m_pendingFiles = 0;
    m_batchInFlight = false;
    m_matches.clear();
    m_matchSet.clear();
    m_focus = 0;
    m_hover = -1;
    m_worldWidth = std::round(kWorldHeight
                              * std::clamp(double(width()) / std::max(1, height()), 1.0, 2.4));
    layoutTree(m_tree, m_worldWidth, kWorldHeight);
    elevateTree(m_tree, std::min(m_worldWidth, kWorldHeight) * 0.2);
    const double span = std::max(m_worldWidth, kWorldHeight);
    m_homeOrbit = Orbit();
    m_homeOrbit.radius = span * 1.15;
    m_orbit = m_homeOrbit;
    m_atHome = true;
    if (!m_tree.nodes.isEmpty())
        m_view = fitFor(0, 0.03);
    if (m_onFocusChanged)
        m_onFocusChanged(m_focus);
    update();
}

QVector<int> CodeMapView::chainOf(int node) const
{
    QVector<int> chain;
    for (int at = node; at >= 0 && at < m_tree.nodes.size(); at = m_tree.nodes.at(at).parent)
        chain.prepend(at);
    return chain;
}

QString CodeMapView::describeSize(int node) const
{
    if (node < 0 || node >= m_tree.nodes.size())
        return {};
    const qint64 size = m_tree.nodes.at(node).size;
    const qint64 total = m_tree.nodes.at(0).size;
    const double share = total > 0 ? double(size) / double(total) * 100.0 : 0;
    const QString percent = share >= 10 ? QString::number(share, 'f', 0)
        : share >= 0.1                  ? QString::number(share, 'f', 1)
                                        : QStringLiteral("<0.1");
    return QStringLiteral("%1 · %2%").arg(formatBytes(size), percent);
}

void CodeMapView::setFocusNode(int node)
{
    if (node == m_focus || node < 0 || node >= m_tree.nodes.size())
        return;
    m_focus = node;
    if (m_onFocusChanged)
        m_onFocusChanged(node);
}

void CodeMapView::goTo(int node, bool animate)
{
    if (node < 0 || node >= m_tree.nodes.size())
        return;
    setFocusNode(node);
    m_focusPinned = true;
    if (m_mode == Mode::City) {
        flyTo(node, animate);
        return;
    }
    m_atHome = node == 0;
    const FlatView target = fitFor(node, node == 0 ? 0.03 : 0.04);
    if (!animate) {
        m_view = target;
        update();
        return;
    }
    m_animation.fromScale = m_view.scale;
    m_animation.toScale = target.scale;
    m_animation.fromCenter = QPointF((width() / 2.0 - m_view.tx) / m_view.scale,
                                     (height() / 2.0 - m_view.ty) / m_view.scale);
    m_animation.toCenter = QPointF((width() / 2.0 - target.tx) / target.scale,
                                   (height() / 2.0 - target.ty) / target.scale);
    startAnimation();
}

bool CodeMapView::goToPath(const QString &path)
{
    if (path.isEmpty()) {
        goTo(0);
        return true;
    }
    for (int i = 0; i < m_tree.nodes.size(); ++i) {
        if (m_tree.nodes.at(i).path == path) {
            goTo(i);
            return true;
        }
    }
    return false;
}

bool CodeMapView::zoomOut()
{
    if (m_focus <= 0)
        return false;
    goTo(std::max(0, m_tree.nodes.at(m_focus).parent));
    return true;
}

void CodeMapView::zoomBy(double factor)
{
    if (m_mode == Mode::Flat)
        zoomFlatAt(QPointF(width() / 2.0, height() / 2.0), factor);
    else
        dolly(1.0 / factor, nullptr);
}

void CodeMapView::resetView()
{
    if (m_mode == Mode::City) {
        m_orbit.theta = m_homeOrbit.theta;
        m_orbit.phi = m_homeOrbit.phi;
    }
    goTo(0);
}

void CodeMapView::setMode(Mode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    m_animation.active = false;
    m_animationTimer.stop();
    m_hover = -1;
    QToolTip::hideText();
    if (mode == Mode::Flat) {
        m_view = fitFor(m_focus, m_focus == 0 ? 0.03 : 0.04);
        m_atHome = m_focus == 0;
    } else if (m_focus != 0) {
        flyTo(m_focus, false);
    }
    describeForAccessibility();
    update();
}

int CodeMapView::setSearch(const QString &text)
{
    m_matches.clear();
    m_matchSet.clear();
    m_matchCursor = -1;
    const QString needle = text.trimmed();
    if (!needle.isEmpty()) {
        for (int i = 0; i < m_tree.nodes.size(); ++i) {
            const Node &node = m_tree.nodes.at(i);
            if (node.type == Node::File && node.path.contains(needle, Qt::CaseInsensitive))
                m_matches.append(i);
        }
        std::sort(m_matches.begin(), m_matches.end(), [this](int a, int b) {
            return m_tree.nodes.at(a).size > m_tree.nodes.at(b).size;
        });
        m_matchSet = QSet<int>(m_matches.begin(), m_matches.end());
    }
    update();
    return m_matches.size();
}

void CodeMapView::visitMatch(bool backwards)
{
    if (m_matches.isEmpty())
        return;
    const int count = m_matches.size();
    m_matchCursor = (m_matchCursor + (backwards ? -1 : 1) + count) % count;
    goTo(m_matches.at(m_matchCursor));
}

CodeMapView::Stats CodeMapView::stats() const
{
    Stats out = m_stats;
    out.frames = m_frame;
    out.files = m_tree.fileCount;
    out.loadedFiles = m_loaded.size();
    out.previews = m_pictures.size();
    out.scale = m_view.scale;
    return out;
}

// --- content ---------------------------------------------------------------

void CodeMapView::scheduleScan()
{
    m_scanTimer.start(140, this);
}

// View-driven loading: the wish list is whatever is large enough on screen to
// show its source right now, most important first, and it replaces the queue,
// so text nobody is looking at any more is never read. One batch at a time.
void CodeMapView::pumpContent()
{
    if (m_batchInFlight || !m_onWantContent)
        return;
    QStringList paths;
    qint64 bytes = 0;
    while (!m_queue.isEmpty() && paths.size() < kBatchPaths) {
        const int index = m_queue.first();
        Node &node = m_tree.nodes[index];
        if (node.contentState != 0) {
            m_queue.removeFirst();
            continue;
        }
        if (!paths.isEmpty() && bytes + node.size > kBatchBytes)
            break;
        m_queue.removeFirst();
        node.contentState = 1;
        paths.append(node.path);
        bytes += node.size;
    }
    if (paths.isEmpty())
        return;
    m_batchInFlight = true;
    m_pendingFiles = paths.size();
    if (m_onLoadingChanged)
        m_onLoadingChanged(m_pendingFiles);
    m_onWantContent(paths);
}

void CodeMapView::setContent(const QString &path, const QByteArray *bytes)
{
    const auto found = m_fileIndex.constFind(path);
    if (found == m_fileIndex.constEnd())
        return;
    Node &node = m_tree.nodes[found.value()];
    if (node.contentState == 2)
        return;
    if (bytes && node.preview == Preview::Image) {
        QSize natural;
        const QImage image = decodePreviewImage(*bytes, node.name, &natural);
        if (!image.isNull()) {
            storePicture(found.value(), image, natural);
            return;
        }
        // An SVG this Qt cannot render is still readable source below.
    } else if (bytes && node.preview == Preview::Font) {
        const QImage specimen = renderFontSpecimen(*bytes);
        if (specimen.isNull())
            node.contentState = 3;
        else
            storePicture(found.value(), specimen, QSize());
        return;
    }
    // A NUL in the first stretch is how git itself decides a blob is binary.
    if (!bytes || bytes->left(8000).contains('\0')) {
        node.contentState = 3;
        return;
    }
    const QString text = QString::fromUtf8(*bytes);
    node.code = prepareCode(text, node.language);
    node.contentState = 2;
    node.lastSeen = m_frame;
    ++node.revision;
    m_loaded.append(found.value());
    m_loadedChars += text.size();
    trimLoadedText();
}

void CodeMapView::setPreview(const QString &path, const QImage &image, const QSize &naturalSize)
{
    const auto found = m_fileIndex.constFind(path);
    if (found == m_fileIndex.constEnd() || m_tree.nodes.at(found.value()).contentState == 2)
        return;
    if (image.isNull())
        m_tree.nodes[found.value()].contentState = 3;
    else
        storePicture(found.value(), image, naturalSize);
}

QImage CodeMapView::decodePreviewImage(const QByteArray &bytes, const QString &fileName, QSize *naturalSize)
{
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, extensionOf(fileName).toLatin1());
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    QSize natural = reader.size();
    // Vectors are drawn at the full preview size, however small they say they are.
    const bool vector = extensionOf(fileName) == QLatin1String("svg");
    if (natural.isValid() && (vector || std::max(natural.width(), natural.height()) > kPreviewMaxSide))
        reader.setScaledSize(natural.scaled(kPreviewMaxSide, kPreviewMaxSide, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull())
        return {};
    if (!natural.isValid())
        natural = image.size();
    if (std::max(image.width(), image.height()) > kPreviewMaxSide)
        image = image.scaled(kPreviewMaxSide, kPreviewMaxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (naturalSize)
        *naturalSize = natural;
    return image;
}

// A type specimen: the font's own "Aa" large, then a line of words and digits.
QImage CodeMapView::renderFontSpecimen(const QByteArray &bytes) const
{
    const int id = QFontDatabase::addApplicationFontFromData(bytes);
    if (id < 0)
        return {};
    const QStringList families = QFontDatabase::applicationFontFamilies(id);
    QImage image;
    if (!families.isEmpty()) {
        image = QImage(640, 400, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::TextAntialiasing);
        QFont font(families.first());
        font.setPixelSize(200);
        painter.setFont(font);
        painter.setPen(m_theme.foreground);
        painter.drawText(QPointF(32, 220), QStringLiteral("Aa"));
        font.setPixelSize(44);
        painter.setFont(font);
        painter.drawText(QRectF(32, 256, 576, 60), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("The quick brown fox"));
        painter.setPen(m_theme.muted);
        painter.drawText(QRectF(32, 320, 576, 60), Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("0123456789 &?!"));
    }
    QFontDatabase::removeApplicationFont(id);
    return image;
}

void CodeMapView::storePicture(int index, const QImage &image, const QSize &natural)
{
    Node &node = m_tree.nodes[index];
    Picture picture;
    picture.pixmap = QPixmap::fromImage(image);
    picture.natural = natural;
    picture.cost = qint64(image.width()) * image.height();
    m_picturePixels += picture.cost;
    m_pictures.insert(index, picture);
    node.contentState = 2;
    node.lastSeen = m_frame;
    ++node.revision;
    trimPictures();
}

// Least recently seen pictures go first; they reload if looked at again.
void CodeMapView::trimPictures()
{
    if (m_picturePixels <= kPictureBudgetPixels)
        return;
    QVector<int> held = m_pictures.keys().toVector();
    std::sort(held.begin(), held.end(), [this](int a, int b) {
        return m_tree.nodes.at(a).lastSeen < m_tree.nodes.at(b).lastSeen;
    });
    for (int index : std::as_const(held)) {
        if (m_picturePixels <= kPictureBudgetPixels * 8 / 10 || m_pictures.size() <= 1)
            break;
        m_picturePixels -= m_pictures.take(index).cost;
        Node &old = m_tree.nodes[index];
        old.contentState = 0;
        ++old.revision;
    }
}

// The picture a file cell shows, if any: its decoded image or specimen, or
// (setting *card) the type card of a binary nothing can picture.
const QPixmap *CodeMapView::pictureFor(int index, bool *card)
{
    *card = false;
    Node &node = m_tree.nodes[index];
    if (node.type != Node::File)
        return nullptr;
    if (node.contentState == 2) {
        const auto found = m_pictures.constFind(index);
        if (found == m_pictures.constEnd())
            return nullptr;
        node.lastSeen = m_frame;
        return &found->pixmap;
    }
    if (node.contentState != 3)
        return nullptr;
    const QString ext = extensionOf(node.name);
    const QString key = ext + QLatin1Char('|') + nodeColor(node).name();
    auto found = m_cards.find(key);
    if (found == m_cards.end())
        found = m_cards.insert(key, cardFor(node));
    *card = true;
    return &found.value();
}

// A page outline in the file's hue, its extension, and what kind of file it is.
QPixmap CodeMapView::cardFor(const Node &node)
{
    QPixmap card(240, 300);
    card.fill(Qt::transparent);
    QPainter painter(&card);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor hue = nodeColor(node);
    const double fold = 56;
    QPolygonF page({{24, 12}, {216 - fold, 12}, {216, 12 + fold}, {216, 288}, {24, 288}});
    QColor wash = hue;
    wash.setAlphaF(0.1);
    QColor ink = hue;
    ink.setAlphaF(0.85);
    painter.setBrush(wash);
    painter.setPen(QPen(ink, 6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolygon(page);
    painter.drawPolyline(QPolygonF({{216 - fold, 12}, {216 - fold, 12 + fold}, {216, 12 + fold}}));
    QString label = extensionOf(node.name).left(5).toUpper();
    if (label.isEmpty())
        label = QStringLiteral("BIN");
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setBold(true);
    font.setPixelSize(label.size() > 3 ? 44 : 56);
    painter.setFont(font);
    painter.setPen(m_theme.foreground);
    painter.drawText(QRectF(32, 130, 176, 76), Qt::AlignCenter, label);
    font.setBold(false);
    font.setPixelSize(26);
    painter.setFont(font);
    painter.setPen(m_theme.muted);
    painter.drawText(QRectF(32, 214, 176, 44), Qt::AlignCenter, fileCategoryOf(node.name));
    return card;
}

void CodeMapView::paintFlatPicture(QPainter &painter, const Node &node, const QPixmap &pixmap,
                                   const QRectF &cell)
{
    const QRectF area = previewArea(node, double(pixmap.width()) / std::max(1, pixmap.height()));
    const QRectF target(cell.x() + area.x() * m_view.scale, cell.y() + area.y() * m_view.scale,
                        area.width() * m_view.scale, area.height() * m_view.scale);
    painter.save();
    // Icons blown up far past their pixels stay crisp instead of smeared.
    painter.setRenderHint(QPainter::SmoothPixmapTransform, target.width() < pixmap.width() * 4);
    painter.drawPixmap(target, pixmap, QRectF(pixmap.rect()));
    painter.restore();
}

void CodeMapView::finishContentBatch()
{
    // Anything the owner could not answer must not stay "requested" forever.
    for (Node &node : m_tree.nodes) {
        if (node.contentState == 1)
            node.contentState = 3;
    }
    m_batchInFlight = false;
    m_pendingFiles = 0;
    if (m_onLoadingChanged)
        m_onLoadingChanged(0);
    update();
    pumpContent();
}

void CodeMapView::trimLoadedText()
{
    if (m_loadedChars <= kTextBudgetChars)
        return;
    std::sort(m_loaded.begin(), m_loaded.end(), [this](int a, int b) {
        return m_tree.nodes.at(a).lastSeen < m_tree.nodes.at(b).lastSeen;
    });
    while (m_loadedChars > kTextBudgetChars * 8 / 10 && m_loaded.size() > 1) {
        Node &old = m_tree.nodes[m_loaded.takeFirst()];
        if (old.code) {
            for (const QString &line : std::as_const(old.code->lines))
                m_loadedChars -= line.size() + 1;
        }
        old.code.reset();
        old.contentState = 0;
        ++old.revision;
    }
}

// --- code bitmaps ----------------------------------------------------------

// A monospace font whose glyph advance is exactly one character cell, so
// tokens placed by column stay flush whatever the platform's fixed font is.
QFont CodeMapView::monoFont(double characterWidth, double lineHeight) const
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (m_monoAdvance <= 0) {
        QFont probe = font;
        probe.setPixelSize(100);
        m_monoAdvance = QFontMetricsF(probe).horizontalAdvance(QLatin1Char('M')) / 100.0;
        if (!(m_monoAdvance > 0))
            m_monoAdvance = 0.6;
    }
    const int pixels = std::max(1, int(std::floor(std::min(characterWidth / m_monoAdvance,
                                                           lineHeight * 0.95))));
    font.setPixelSize(pixels);
    font.setLetterSpacing(QFont::AbsoluteSpacing, characterWidth - pixels * m_monoAdvance);
    font.setKerning(false);
    return font;
}

// Bitmaps are built inside the paint, within a small time budget — but always
// at least one per frame, or a frame that spends its budget on geometry alone
// (a wide 3D view) would never show any source at all.
bool CodeMapView::mayBuildBitmap() const
{
    return m_bitmapsBuilt == 0 || m_frameClock.elapsed() < m_bitmapDeadline;
}

QSize CodeMapView::bitmapSize(const Node &node, double level) const
{
    const Code &code = *node.code;
    if (!(code.lineHeight > 0))
        return {};
    const double scale = level / code.lineHeight;
    const int w = int(std::ceil(code.area.width() * scale));
    const int h = int(std::ceil(code.area.height() * scale));
    if (w < 1 || h < 1)
        return {};
    // The 3D view cannot draw text live the way the flat one does, so its text
    // levels may use larger bitmaps; only a handful of tiles are ever near
    // enough to want one, and assignCityLevels() keeps them inside the budget.
    const int roomy = level > levels().last() || (m_mode == Mode::City && level >= 8)
        ? kCloseupScale : 1;
    if (w > kMaxBitmapSide * roomy || h > kMaxBitmapSide * roomy
        || qint64(w) * h > kMaxBitmapPixels * roomy * 3 / 2)
        return {};
    return QSize(w, h);
}

double CodeMapView::closeupLevel(const Node &node, double want) const
{
    const Code &code = *node.code;
    const double side = kMaxBitmapSide * kCloseupScale;
    const double fit = std::min({
        side * code.lineHeight / std::max(code.area.width(), 1e-9),
        side * code.lineHeight / std::max(code.area.height(), 1e-9),
        std::sqrt(double(kMaxBitmapPixels) * kCloseupScale * 1.5 * code.lineHeight
                  * code.lineHeight / std::max(code.area.width() * code.area.height(), 1e-9)),
    });
    const double level = std::floor(std::min({want, fit, 48.0}) / 8.0) * 8.0;
    return level > levels().last() ? level : levels().last();
}

QPixmap CodeMapView::anyBitmapFor(int index) const
{
    const Node &node = m_tree.nodes.at(index);
    for (int i = levels().size() - 1; i >= 0; --i) {
        const auto found = m_bitmaps.constFind(bitmapKey(index, node.revision, levels().at(i)));
        if (found != m_bitmaps.constEnd())
            return found->pixmap;
    }
    return {};
}

// Returns the cell's source drawn at `level` device pixels per line. A null
// pixmap with *pending set means "not built yet, out of frame budget"; a null
// pixmap without it means this level cannot be a bitmap (too large).
QPixmap CodeMapView::bitmapFor(int index, double level, bool build, bool *pending)
{
    Node &node = m_tree.nodes[index];
    const quint64 key = bitmapKey(index, node.revision, level);
    const auto hit = m_bitmaps.find(key);
    if (hit != m_bitmaps.end()) {
        if (hit->used != m_frame)
            m_framePixels += hit->cost;
        hit->used = m_frame;
        return hit->pixmap;
    }
    const QSize size = bitmapSize(node, level);
    if (size.isEmpty())
        return {};
    if (!build) {
        *pending = true;
        return {};
    }
    ++m_bitmapsBuilt;
    Code &code = *node.code;
    QPixmap pixmap(size);
    pixmap.fill(m_theme.tile);
    QPainter painter(&pixmap);
    const double charWidth = level * kCharAspect;
    const double columnWidth = code.columnWidth * (level / code.lineHeight);
    const int rows = code.rows;
    const int maxLength = code.maxLength;
    if (level <= 2) {
        painter.setRenderHint(QPainter::Antialiasing, level < 1);
        const bool far = level < 1;
        const double bar = far ? level * 0.75 : std::max(1.0, level * 0.7);
        QColor plain = m_theme.code[0];
        QColor comment = m_theme.code[3];
        plain.setAlphaF(far ? 0.9 : 0.62);
        comment.setAlphaF(far ? 0.9 : 0.5);
        for (int i = 0; i < code.lines.size(); ++i) {
            const QString &line = code.lines.at(i);
            int lead = 0;
            while (lead < line.size() && line.at(lead).isSpace())
                ++lead;
            int end = std::min<int>(line.size(), maxLength);
            while (end > lead && line.at(end - 1).isSpace())
                --end;
            if (end <= lead)
                continue;
            const QChar first = line.at(lead);
            const bool isComment = code.startsInBlock.at(i) || first == QLatin1Char('#')
                || (first == QLatin1Char('/') && lead + 1 < line.size()
                    && line.at(lead + 1) == QLatin1Char('/'));
            const double w = (end - lead) * charWidth;
            painter.fillRect(QRectF((i / rows) * columnWidth + lead * charWidth,
                                    (i % rows) * level, far ? w : std::max(1.0, w), bar),
                             isComment ? comment : plain);
        }
    } else {
        const bool text = level >= 8;
        if (text) {
            painter.setFont(monoFont(charWidth, level));
            painter.setRenderHint(QPainter::TextAntialiasing, true);
        }
        const QFontMetricsF metrics(painter.font());
        const double baseline = (level + metrics.ascent() - metrics.descent()) / 2.0;
        for (int i = 0; i < code.lines.size(); ++i) {
            const double x0 = (i / rows) * columnWidth;
            const double y0 = (i % rows) * level;
            const QVector<Token> &tokens = tokenizeLine(code, i);
            for (const Token &token : tokens) {
                if (token.column >= maxLength)
                    break;
                const int length = std::min(token.length, maxLength - token.column);
                QColor color = m_theme.code[token.kind];
                if (text) {
                    painter.setPen(color);
                    painter.drawText(QPointF(x0 + token.column * charWidth, y0 + baseline),
                                     code.lines.at(i).mid(token.column, length));
                } else {
                    color.setAlphaF(0.8);
                    painter.fillRect(QRectF(x0 + token.column * charWidth, y0 + level * 0.2,
                                            std::max(1.0, length * charWidth - 0.5), level * 0.6),
                                     color);
                }
            }
        }
    }
    painter.end();

    CachedBitmap entry;
    entry.pixmap = pixmap;
    entry.cost = qint64(size.width()) * size.height();
    entry.used = m_frame;
    m_bitmaps.insert(key, entry);
    m_bitmapPixels += entry.cost;
    m_framePixels += entry.cost;
    // Least recently used goes first, but never a bitmap this very frame is
    // showing: evicting those only rebuilds them next frame, forever.
    while (m_bitmapPixels > kBitmapBudgetPixels && m_bitmaps.size() > 1) {
        auto oldest = m_bitmaps.end();
        for (auto it = m_bitmaps.begin(); it != m_bitmaps.end(); ++it) {
            if (it->used != m_frame && (oldest == m_bitmaps.end() || it->used < oldest->used))
                oldest = it;
        }
        if (oldest == m_bitmaps.end())
            break;
        m_bitmapPixels -= oldest->cost;
        m_bitmaps.erase(oldest);
    }
    return pixmap;
}

// --- flat view ---------------------------------------------------------------

CodeMapView::FlatView CodeMapView::fitFor(int index, double margin) const
{
    const QRectF rect = m_tree.nodes.at(index).rect;
    FlatView out;
    out.scale = std::min(width() / std::max(rect.width(), 1e-6),
                         height() / std::max(rect.height(), 1e-6)) * (1 - margin * 2);
    out.tx = width() / 2.0 - rect.center().x() * out.scale;
    out.ty = height() / 2.0 - rect.center().y() * out.scale;
    return out;
}

void CodeMapView::clampFlatView()
{
    if (m_tree.nodes.isEmpty())
        return;
    const double home = fitFor(0, 0.03).scale;
    m_view.scale = std::clamp(m_view.scale, home * 0.6, home * 8000);
    m_view.tx = std::min(width() * 0.8, std::max(width() * 0.2 - m_worldWidth * m_view.scale, m_view.tx));
    m_view.ty = std::min(height() * 0.8, std::max(height() * 0.2 - kWorldHeight * m_view.scale, m_view.ty));
}

void CodeMapView::zoomFlatAt(const QPointF &point, double factor)
{
    m_animation.active = false;
    const double before = m_view.scale;
    m_view.scale *= factor;
    clampFlatView();
    const double ratio = m_view.scale / before;
    m_view.tx = point.x() - (point.x() - m_view.tx) * ratio;
    m_view.ty = point.y() - (point.y() - m_view.ty) * ratio;
    clampFlatView();
    m_atHome = false;
    m_focusPinned = false;
    update();
}

QRectF CodeMapView::screenRect(int index) const
{
    if (index < 0 || index >= m_tree.nodes.size())
        return {};
    const QRectF rect = m_tree.nodes.at(index).rect;
    return QRectF(rect.x() * m_view.scale + m_view.tx, rect.y() * m_view.scale + m_view.ty,
                  rect.width() * m_view.scale, rect.height() * m_view.scale);
}

int CodeMapView::flatNodeAt(const QPointF &point, double minimumPixels) const
{
    if (m_tree.nodes.isEmpty())
        return -1;
    const QPointF world((point.x() - m_view.tx) / m_view.scale, (point.y() - m_view.ty) / m_view.scale);
    int at = 0;
    for (;;) {
        const Node &node = m_tree.nodes.at(at);
        int next = -1;
        for (int child : node.children) {
            if (m_tree.nodes.at(child).rect.contains(world)) {
                next = child;
                break;
            }
        }
        if (next < 0)
            return at;
        const QRectF rect = m_tree.nodes.at(next).rect;
        if (std::min(rect.width(), rect.height()) * m_view.scale < minimumPixels)
            return at;
        at = next;
    }
}

// code-health: allow(deep-nested-loop) columns x rows x tokens, but only of the lines inside the viewport at a readable size: a few thousand tokens at most
void CodeMapView::paintLiveCode(QPainter &painter, Code &code, const QPointF &origin,
                                double lineHeight, const QRectF &clip)
{
    // The cell is too large for a bitmap at this zoom, so only the lines inside
    // the viewport are drawn, straight onto the widget.
    const double columnWidth = code.columnWidth * m_view.scale;
    const double charWidth = lineHeight * kCharAspect;
    const int c0 = std::max(0, int(std::floor(-origin.x() / columnWidth)));
    const int c1 = std::min(code.columns - 1, int(std::floor((width() - origin.x()) / columnWidth)));
    const int r0 = std::max(0, int(std::floor(-origin.y() / lineHeight)));
    const int r1 = std::min(code.rows - 1, int(std::floor((height() - origin.y()) / lineHeight)));
    const bool text = lineHeight >= 6;
    const bool tokens = lineHeight >= 3;
    painter.save();
    painter.setClipRect(clip);
    double baseline = 0;
    if (text) {
        painter.setFont(monoFont(charWidth, lineHeight));
        const QFontMetricsF metrics(painter.font());
        baseline = (lineHeight + metrics.ascent() - metrics.descent()) / 2.0;
    }
    for (int c = c0; c <= c1; ++c) {
        for (int r = r0; r <= r1; ++r) {
            const int index = c * code.rows + r;
            if (index >= code.lines.size())
                break;
            const double x = origin.x() + c * columnWidth;
            const double y = origin.y() + r * lineHeight;
            const QString &line = code.lines.at(index);
            if (!tokens) {
                int lead = 0;
                while (lead < line.size() && line.at(lead).isSpace())
                    ++lead;
                const int length = std::min<int>(line.size(), code.maxLength) - lead;
                if (length <= 0)
                    continue;
                QColor color = m_theme.code[code.startsInBlock.at(index) ? 3 : 0];
                color.setAlphaF(0.6);
                painter.fillRect(QRectF(x + lead * charWidth, y, length * charWidth,
                                        std::max(1.0, lineHeight * 0.7)), color);
                continue;
            }
            for (const Token &token : tokenizeLine(code, index)) {
                if (token.column >= code.maxLength)
                    break;
                const int length = std::min(token.length, code.maxLength - token.column);
                QColor color = m_theme.code[token.kind];
                if (text) {
                    painter.setPen(color);
                    painter.drawText(QPointF(x + token.column * charWidth, y + baseline),
                                     line.mid(token.column, length));
                } else {
                    color.setAlphaF(0.8);
                    painter.fillRect(QRectF(x + token.column * charWidth, y + lineHeight * 0.2,
                                            length * charWidth - 0.5, lineHeight * 0.6), color);
                }
            }
        }
    }
    painter.restore();
}

void CodeMapView::paintFlatCode(QPainter &painter, int index, const QRectF &cell)
{
    Node &node = m_tree.nodes[index];
    Code &code = *node.code;
    if (fitCode(code, node))
        ++node.revision;
    const double lineHeight = code.lineHeight * m_view.scale;
    if (!(lineHeight > 0))
        return;
    const double dpr = devicePixelRatioF();
    const double level = pickLevel(lineHeight * dpr);
    const QRectF target(cell.x() + code.area.x() * m_view.scale, cell.y() + code.area.y() * m_view.scale,
                        code.area.width() * m_view.scale, code.area.height() * m_view.scale);
    QPixmap bitmap;
    if (level > 0) {
        bool pending = false;
        bitmap = bitmapFor(index, level, mayBuildBitmap(), &pending);
        if (pending) {
            m_bitmapsPending = true;
            bitmap = anyBitmapFor(index);
            if (bitmap.isNull())
                return;
        }
    }
    if (!bitmap.isNull()) {
        painter.drawPixmap(target, bitmap, QRectF(bitmap.rect()));
        return;
    }
    if (lineHeight < 1.5) {
        // Too many lines for even the coarsest bitmap: an even wash stands in
        // for the text until the cell is zoomed far enough to draw it.
        QColor wash = m_theme.code[0];
        wash.setAlphaF(0.14);
        painter.fillRect(target, wash);
        return;
    }
    paintLiveCode(painter, code, target.topLeft(), lineHeight, target);
}

void CodeMapView::paintFlatNode(QPainter &painter, int index)
{
    Node &node = m_tree.nodes[index];
    const QRectF cell = screenRect(index);
    if (cell.left() > width() || cell.top() > height() || cell.right() < 0 || cell.bottom() < 0
        || cell.width() < 0.7 || cell.height() < 0.7)
        return;
    QColor hue = nodeColor(node);
    if (node.type == Node::Directory && !node.children.isEmpty()) {
        if (cell.width() < 5 || cell.height() < 5) {
            hue.setAlphaF(0.45);
            painter.fillRect(cell, hue);
            return;
        }
        painter.fillRect(cell, directoryFill(node.depth));
        hue.setAlphaF(0.55);
        painter.setPen(QPen(hue, 1));
        painter.drawRect(cell.adjusted(0.5, 0.5, -0.5, -0.5));
        const QVector<int> children = node.children;
        for (int child : children)
            paintFlatNode(painter, child);
        if (m_tree.nodes.at(index).depth > 0 && cell.width() >= 40 && cell.height() >= 16)
            m_flatLabels.append(index);
        return;
    }
    if (cell.width() < 3.5 || cell.height() < 3.5) {
        hue.setAlphaF(0.45);
        painter.fillRect(cell, hue);
        return;
    }
    painter.fillRect(cell, m_theme.tile);
    ++m_stats.boxesDrawn;
    bool card = false;
    const QPixmap *picture = pictureFor(index, &card);
    const bool pictured = picture && (card ? cell.width() >= 18 && cell.height() >= 18
                                           : cell.width() >= 6 && cell.height() >= 6);
    if (picture) {
        if (pictured) {
            paintFlatPicture(painter, node, *picture, cell);
            ++m_stats.texturesDrawn;
        }
    } else if (node.contentState == 2 && node.code) {
        node.lastSeen = m_frame;
        if (cell.width() >= 10 && cell.height() >= 10) {
            paintFlatCode(painter, index, cell);
            ++m_stats.texturesDrawn;
        }
    } else if (node.contentState == 0 && cell.width() * cell.height() >= 320
               && std::min(cell.width(), cell.height()) >= 12) {
        m_wish.append({cell.width() * cell.height(), index});
    }
    hue.setAlphaF(0.8);
    painter.setPen(QPen(hue, 1));
    painter.drawRect(cell.adjusted(0.5, 0.5, -0.5, -0.5));
    const double band = node.band * m_view.scale;
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (band >= 9 && cell.width() >= 30) {
        const int pixels = std::max(7, int(std::min(12.0, band * 0.72)));
        font.setPixelSize(pixels);
        painter.setFont(font);
        painter.setPen(m_theme.foreground);
        const double left = std::max(cell.left(), 0.0) + 4;
        const QString text = clipLabel(node.name, std::min(cell.right(), double(width())) - left - 4, pixels);
        painter.drawText(QRectF(left, cell.top() + 1, cell.width(), band), Qt::AlignVCenter | Qt::AlignLeft, text);
    } else if ((node.contentState == 3 || picture) && cell.width() >= 60 && cell.height() >= 24) {
        const QString text = clipLabel(node.name, cell.width() - 8, 10);
        font.setPixelSize(10);
        painter.setFont(font);
        if (pictured) {
            painter.fillRect(QRectF(cell.left() + 2, cell.top() + 3, text.size() * 6.1 + 6, 14), m_theme.pill);
            painter.setPen(m_theme.foreground);
        } else {
            painter.setPen(m_theme.muted);
        }
        painter.drawText(QPointF(cell.left() + 4, cell.top() + 13), text);
    }
}

void CodeMapView::paintFlatLabels(QPainter &painter)
{
    QVector<QRectF> placed;
    auto claim = [&placed](const QRectF &rect) {
        for (const QRectF &other : std::as_const(placed)) {
            if (rect.intersects(other))
                return false;
        }
        placed.append(rect);
        return true;
    };
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    // Parents come first in the list, so they win a contested spot.
    for (int i = 0; i < m_flatLabels.size() && placed.size() < 400; ++i) {
        const Node &node = m_tree.nodes.at(m_flatLabels.at(i));
        const QRectF cell = screenRect(m_flatLabels.at(i));
        const double band = node.band * m_view.scale;
        const double left = std::max(cell.left(), 0.0) + 4;
        const double room = std::min(cell.right(), double(width())) - left - 4;
        if (band >= 11 && cell.top() > -band * 0.4) {
            const int pixels = std::max(8, int(std::min(13.0, band * 0.7)));
            const QString text = clipLabel(node.name, room, pixels);
            if (text.isEmpty() || !claim(QRectF(left, cell.top(), text.size() * pixels * 0.6, band)))
                continue;
            font.setPixelSize(pixels);
            font.setBold(true);
            painter.setFont(font);
            painter.setPen(m_theme.foreground);
            painter.drawText(QRectF(left, cell.top(), room, band), Qt::AlignVCenter | Qt::AlignLeft, text);
            ++m_stats.labelsDrawn;
        } else if (cell.width() >= 64 && cell.height() >= 28) {
            const QString text = clipLabel(node.name, std::min(room, 150.0), 10);
            const double top = std::max(cell.top(), 0.0) + 2;
            const QRectF pill(left - 2, top, text.size() * 6.1 + 8, 14);
            if (text.isEmpty() || top + 14 > cell.bottom() || !claim(pill))
                continue;
            painter.fillRect(pill, m_theme.pill);
            font.setPixelSize(10);
            font.setBold(false);
            painter.setFont(font);
            painter.setPen(m_theme.foreground);
            painter.drawText(pill.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
            ++m_stats.labelsDrawn;
        }
    }
}

void CodeMapView::outlineFlat(QPainter &painter, int index, const QColor &color, double penWidth)
{
    const QRectF cell = screenRect(index);
    if (cell.left() > width() || cell.top() > height() || cell.right() < 0 || cell.bottom() < 0)
        return;
    if (cell.width() < 4 || cell.height() < 4) {
        painter.fillRect(QRectF(cell.left() - 1.5, cell.top() - 1.5, std::max(4.0, cell.width() + 3),
                                std::max(4.0, cell.height() + 3)), color);
        return;
    }
    painter.setPen(QPen(color, penWidth));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(cell);
}

void CodeMapView::paintFlat(QPainter &painter)
{
    painter.fillRect(rect(), m_theme.background);
    if (m_tree.nodes.isEmpty())
        return;
    m_flatLabels.clear();
    m_wish.clear();
    m_bitmapsPending = false;
    m_bitmapsBuilt = 0;
    m_framePixels = 0;
    m_bitmapDeadline = 7;
    m_frameClock.start();
    painter.setBrush(Qt::NoBrush);
    paintFlatNode(painter, 0);
    paintFlatLabels(painter);
    for (int i = 0; i < m_matches.size() && i < 3000; ++i)
        outlineFlat(painter, m_matches.at(i), m_theme.accent, 2);
    if (m_hover > 0)
        outlineFlat(painter, m_hover, m_theme.foreground, 1.5);
    if (m_bitmapsPending)
        update();
    scheduleScan();
}

// A click zooms one meaningful step toward what is under the pointer: the first
// cell on the way down from the focus that does not already fill the view.
void CodeMapView::clickFlat(const QPointF &point)
{
    const int target = flatNodeAt(point, 26);
    if (target < 0)
        return;
    const QVector<int> chain = chainOf(target);
    const int start = std::max<int>(1, std::min<int>(chain.size() - 1, chain.indexOf(m_focus) + 1));
    int pick = target;
    for (int i = start; i < chain.size(); ++i) {
        const QRectF cell = screenRect(chain.at(i));
        if (cell.width() * cell.height() < width() * height() * 0.5) {
            pick = chain.at(i);
            break;
        }
    }
    if (pick > 0)
        goTo(pick);
}

// After free navigation the breadcrumb follows whatever fills the viewport.
void CodeMapView::deriveFlatFocus()
{
    if (m_focusPinned || m_tree.nodes.isEmpty())
        return;
    if (m_atHome) { // a folder filling the home view is not a zoom
        setFocusNode(0);
        return;
    }
    const QPointF center((width() / 2.0 - m_view.tx) / m_view.scale, (height() / 2.0 - m_view.ty) / m_view.scale);
    int at = 0;
    for (;;) {
        int next = -1;
        for (int child : m_tree.nodes.at(at).children) {
            if (m_tree.nodes.at(child).rect.contains(center)) {
                next = child;
                break;
            }
        }
        if (next < 0)
            break;
        const QRectF cell = screenRect(next);
        if (cell.width() * cell.height() < width() * height() * 0.4)
            break;
        at = next;
    }
    setFocusNode(at);
}

// --- city view ---------------------------------------------------------------
//
// Layout point (x, y) stands at world (x - W/2, elevation, y - H/2): the map
// lies on the ground with its top edge away from the default camera, so the
// 3D view reads the same way round as the flat one.

CodeMapView::Camera CodeMapView::camera() const
{
    Camera out;
    const double sinPhi = std::sin(m_orbit.phi);
    out.position = m_orbit.target
        + QVector3D(float(m_orbit.radius * sinPhi * std::sin(m_orbit.theta)),
                    float(m_orbit.radius * std::cos(m_orbit.phi)),
                    float(m_orbit.radius * sinPhi * std::cos(m_orbit.theta)));
    out.forward = (m_orbit.target - out.position).normalized();
    out.right = QVector3D::crossProduct(out.forward, QVector3D(0, 1, 0)).normalized();
    out.up = QVector3D::crossProduct(out.right, out.forward);
    out.focal = height() / (2.0 * std::tan(kFieldOfView / 2.0));
    out.nearPlane = std::max(0.01, m_orbit.radius * 0.02);
    return out;
}

void CodeMapView::clampOrbit()
{
    const double span = std::max(m_worldWidth, kWorldHeight);
    m_orbit.phi = std::clamp(m_orbit.phi, 0.04, 1.5);
    m_orbit.radius = std::clamp(m_orbit.radius, 0.3, span * 4);
    const float limit = float(span * 0.75);
    m_orbit.target.setX(std::clamp(m_orbit.target.x(), -limit, limit));
    m_orbit.target.setZ(std::clamp(m_orbit.target.z(), -limit, limit));
}

// Clips a quad against the near plane in view space and projects what is left.
bool CodeMapView::projectFace(const Camera &camera, const QVector3D corners[4], QPolygonF *out,
                              bool *clipped) const
{
    QVector3D view[8];
    int count = 0;
    QVector3D previous;
    bool previousInside = false;
    QVector3D points[4];
    for (int i = 0; i < 4; ++i) {
        const QVector3D d = corners[i] - camera.position;
        points[i] = QVector3D(QVector3D::dotProduct(d, camera.right),
                              QVector3D::dotProduct(d, camera.up),
                              QVector3D::dotProduct(d, camera.forward));
    }
    const float nearPlane = float(camera.nearPlane);
    previous = points[3];
    previousInside = previous.z() >= nearPlane;
    *clipped = false;
    for (int i = 0; i < 4; ++i) {
        const QVector3D current = points[i];
        const bool inside = current.z() >= nearPlane;
        if (inside != previousInside) {
            const float t = (nearPlane - previous.z()) / (current.z() - previous.z());
            view[count++] = previous + (current - previous) * t;
            *clipped = true;
        }
        if (inside)
            view[count++] = current;
        previous = current;
        previousInside = inside;
    }
    if (count < 3)
        return false;
    out->clear();
    out->reserve(count);
    for (int i = 0; i < count; ++i) {
        const double scale = camera.focal / view[i].z();
        out->append(QPointF(width() / 2.0 + view[i].x() * scale, height() / 2.0 - view[i].y() * scale));
    }
    return true;
}

double CodeMapView::idealCityLevel(const Node &node, double want) const
{
    double level = pickLevel(want);
    if (level <= 0)
        level = closeupLevel(node, want);
    if (level > levels().last() && bitmapSize(node, level).isEmpty())
        level = levels().last();
    while (level > levels().first() && bitmapSize(node, level).isEmpty())
        level /= 2;
    return level;
}

// Shares the bitmap budget between the tiles the last frame textured, the one
// covering most of the screen first. Returns true when any level changed.
bool CodeMapView::assignCityLevels()
{
    std::sort(m_cityRequests.begin(), m_cityRequests.end(),
              [](const CityRequest &a, const CityRequest &b) { return a.visibleArea > b.visibleArea; });
    QHash<int, double> assigned;
    qint64 pixels = 0;
    for (const CityRequest &request : std::as_const(m_cityRequests)) {
        const Node &node = m_tree.nodes.at(request.node);
        if (!node.code)
            continue;
        double level = idealCityLevel(node, request.want);
        for (;;) {
            const QSize size = bitmapSize(node, level);
            const qint64 cost = qint64(size.width()) * size.height();
            if (pixels + cost <= kBitmapBudgetPixels * 9 / 10 || level <= levels().first()) {
                pixels += cost;
                break;
            }
            level = level > levels().last() ? levels().last() : level / 2;
        }
        assigned.insert(request.node, level);
    }
    const bool changed = assigned != m_cityLevels;
    m_cityLevels = assigned;
    return changed;
}

// Draws the file's source on its tile's top face. A planar patch maps onto the
// screen with one projective transform, which is exact — but only while the
// whole patch is in front of the camera. Up close a tile reaches behind the
// near plane, so the patch is quartered until the pieces are either fully in
// front and of a sane size on screen (drawn) or fully behind (dropped).
void CodeMapView::paintCityTexture(QPainter &painter, int index, const Camera &camera,
                                   const QRectF &ground, double elevation, double pixelsPerUnit,
                                   double visibleArea)
{
    Node &node = m_tree.nodes[index];
    Code &code = *node.code;
    if (!(code.lineHeight > 0))
        return;
    const double want = code.lineHeight * pixelsPerUnit * devicePixelRatioF();
    m_cityRequests.append({index, want, visibleArea});
    // Tiles paint far to near, so handing out bitmap pixels in paint order
    // would spend them on the background. Levels are instead assigned at the
    // end of each frame, most visible tile first, and used by the next one; a
    // tile seen for the first time starts coarse.
    double level = m_cityLevels.value(index, -1);
    if (level < 0 || bitmapSize(node, level).isEmpty())
        level = std::min(1.0, idealCityLevel(node, want));
    bool pending = false;
    QPixmap bitmap = bitmapFor(index, level, mayBuildBitmap(), &pending);
    if (pending) {
        m_bitmapsPending = true;
        bitmap = anyBitmapFor(index);
    }
    if (bitmap.isNull())
        return;
    paintCityQuad(painter, bitmap, camera, ground, elevation);
}

void CodeMapView::paintCityQuad(QPainter &painter, const QPixmap &bitmap, const Camera &camera,
                                const QRectF &ground, double elevation)
{
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, !m_interacting);
    const QTransform device = painter.transform();
    const QRectF viewport(rect());
    std::function<void(const QRectF &, int)> patch = [&](const QRectF &part, int depth) {
        // `part` is in 0..1 of the ground rect (and therefore of the bitmap).
        const float x0 = float(ground.left() + part.left() * ground.width());
        const float x1 = float(ground.left() + part.right() * ground.width());
        const float z0 = float(ground.top() + part.top() * ground.height());
        const float z1 = float(ground.top() + part.bottom() * ground.height());
        const QVector3D corners[4] = {{x0, float(elevation), z0}, {x1, float(elevation), z0},
                                      {x1, float(elevation), z1}, {x0, float(elevation), z1}};
        QPolygonF quad;
        int ahead = 0;
        for (const QVector3D &corner : corners) {
            const QVector3D d = corner - camera.position;
            const double vz = QVector3D::dotProduct(d, camera.forward);
            if (vz < camera.nearPlane)
                continue;
            ++ahead;
            const double scale = camera.focal / vz;
            quad.append(QPointF(width() / 2.0 + QVector3D::dotProduct(d, camera.right) * scale,
                                height() / 2.0 - QVector3D::dotProduct(d, camera.up) * scale));
        }
        if (ahead == 0)
            return;
        const QRectF bounds = ahead == 4 ? quad.boundingRect() : QRectF();
        if (ahead == 4 && !bounds.intersects(viewport))
            return;
        // Pieces reaching behind the camera cannot be mapped at all, and ones
        // projected far larger than the widget overflow the raster engine's
        // perspective path, so both are quartered until they are neither.
        const bool oversized = ahead == 4
            && (bounds.width() > viewport.width() * 2 || bounds.height() > viewport.height() * 2);
        if (ahead < 4 || oversized) {
            if (depth >= 7) {
                if (ahead < 4)
                    return;
            } else {
                const QPointF mid = part.center();
                patch(QRectF(part.topLeft(), mid), depth + 1);
                patch(QRectF(QPointF(mid.x(), part.top()), QPointF(part.right(), mid.y())), depth + 1);
                patch(QRectF(QPointF(part.left(), mid.y()), QPointF(mid.x(), part.bottom())), depth + 1);
                patch(QRectF(mid, part.bottomRight()), depth + 1);
                return;
            }
        }
        const QRectF source(part.left() * bitmap.width(), part.top() * bitmap.height(),
                            part.width() * bitmap.width(), part.height() * bitmap.height());
        const QPolygonF from({source.topLeft(), source.topRight(), source.bottomRight(), source.bottomLeft()});
        QTransform transform;
        if (!QTransform::quadToQuad(from, quad, transform))
            return;
        painter.setTransform(transform * device, false);
        painter.drawPixmap(source, bitmap, source);
    };
    patch(QRectF(0, 0, 1, 1), 0);
    painter.restore();
    ++m_stats.texturesDrawn;
}

void CodeMapView::paintCityNode(QPainter &painter, const Camera &camera, int index)
{
    Node &node = m_tree.nodes[index];
    const bool solid = node.type == Node::Directory && !node.children.isEmpty();
    const double halfW = m_worldWidth / 2.0, halfH = kWorldHeight / 2.0;
    const QRectF rect = node.rect;
    if (!(rect.width() > 0) || !(rect.height() > 0))
        return;

    // Cull by the whole subtree's bounds, projected.
    const float bx0 = float(rect.left() - halfW), bx1 = float(rect.right() - halfW);
    const float bz0 = float(rect.top() - halfH), bz1 = float(rect.bottom() - halfH);
    const float by0 = float(node.base), by1 = float(std::max(node.subtreeTop, node.top));
    double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
    bool anyBehind = false, anyAhead = false;
    double centerDepth = 0;
    for (int i = 0; i < 8; ++i) {
        const QVector3D d = QVector3D(i & 1 ? bx1 : bx0, i & 2 ? by1 : by0, i & 4 ? bz1 : bz0) - camera.position;
        const double vz = QVector3D::dotProduct(d, camera.forward);
        centerDepth += vz / 8.0;
        if (vz < camera.nearPlane) {
            anyBehind = true;
            continue;
        }
        anyAhead = true;
        const double scale = camera.focal / vz;
        const double sx = width() / 2.0 + QVector3D::dotProduct(d, camera.right) * scale;
        const double sy = height() / 2.0 - QVector3D::dotProduct(d, camera.up) * scale;
        minX = std::min(minX, sx); maxX = std::max(maxX, sx);
        minY = std::min(minY, sy); maxY = std::max(maxY, sy);
    }
    if (!anyAhead)
        return;
    double extent = 1e9;
    if (!anyBehind) {
        if (maxX < 0 || maxY < 0 || minX > width() || minY > height())
            return;
        extent = std::max(maxX - minX, maxY - minY);
        // While the camera moves, specks are skipped to keep the frame rate up.
        if (extent < (m_interacting && !solid ? 3.0 : 1.2))
            return;
    }

    const double gap = solid ? 0 : std::min(rect.width(), rect.height()) * 0.045;
    const float x0 = float(rect.left() + gap - halfW), x1 = float(rect.right() - gap - halfW);
    const float z0 = float(rect.top() + gap - halfH), z1 = float(rect.bottom() - gap - halfH);
    const float y0 = float(node.base), y1 = float(node.top);

    QColor color;
    if (index == 0)
        color = ::Theme::remapColor(QColor(m_theme.dark ? "#0d1117" : "#f6f8fa"));
    else if (solid) {
        const QColor hue = nodeColor(node);
        const double mix = 0.1 + (node.depth % 2) * 0.16;
        color = QColor::fromRgbF(hue.redF() + (m_theme.background.redF() - hue.redF()) * mix,
                                 hue.greenF() + (m_theme.background.greenF() - hue.greenF()) * mix,
                                 hue.blueF() + (m_theme.background.blueF() - hue.blueF()) * mix);
    } else {
        const QColor hue = nodeColor(node);
        color = QColor::fromRgbF(m_theme.tile.redF() + (hue.redF() - m_theme.tile.redF()) * 0.12,
                                 m_theme.tile.greenF() + (hue.greenF() - m_theme.tile.greenF()) * 0.12,
                                 m_theme.tile.blueF() + (hue.blueF() - m_theme.tile.blueF()) * 0.12);
    }
    auto shade = [&color](double factor) {
        return QColor::fromRgbF(color.redF() * factor, color.greenF() * factor, color.blueF() * factor);
    };
    painter.setPen(solid && extent >= 24 ? QPen(shade(0.55), 1) : Qt::NoPen);

    QPolygonF polygon;
    bool clipped = false;
    auto drawFace = [&](const QVector3D corners[4], const QColor &fill) -> bool {
        if (!projectFace(camera, corners, &polygon, &clipped))
            return false;
        painter.setBrush(fill);
        painter.drawConvexPolygon(polygon);
        m_faces.append({polygon, polygon.boundingRect(), index});
        return true;
    };

    const double thicknessPixels = (y1 - y0) * camera.focal / std::max(centerDepth, camera.nearPlane);
    if (thicknessPixels >= 0.6) {
        if (camera.position.x() < x0) {
            const QVector3D face[4] = {{x0, y1, z0}, {x0, y1, z1}, {x0, y0, z1}, {x0, y0, z0}};
            drawFace(face, shade(0.78));
        } else if (camera.position.x() > x1) {
            const QVector3D face[4] = {{x1, y1, z1}, {x1, y1, z0}, {x1, y0, z0}, {x1, y0, z1}};
            drawFace(face, shade(0.78));
        }
        if (camera.position.z() > z1) {
            const QVector3D face[4] = {{x0, y1, z1}, {x1, y1, z1}, {x1, y0, z1}, {x0, y0, z1}};
            drawFace(face, shade(0.62));
        } else if (camera.position.z() < z0) {
            const QVector3D face[4] = {{x1, y1, z0}, {x0, y1, z0}, {x0, y0, z0}, {x1, y0, z0}};
            drawFace(face, shade(0.62));
        }
    }
    if (camera.position.y() > y1) {
        const QVector3D top[4] = {{x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}};
        if (drawFace(top, color)) {
            ++m_stats.boxesDrawn;
            const double pixelsPerUnit = camera.focal / std::max(centerDepth, camera.nearPlane);
            const double area = rect.width() * rect.height() * pixelsPerUnit * pixelsPerUnit;
            bool card = false;
            const QPixmap *picture = node.type == Node::File && area >= 500 ? pictureFor(index, &card) : nullptr;
            if (picture) {
                const QRectF ground = previewArea(node, double(picture->width()) / std::max(1, picture->height()))
                                          .translated(rect.topLeft())
                                          .translated(-halfW, -halfH);
                paintCityQuad(painter, *picture, camera, ground,
                              y1 + std::max(0.002, double(y1 - y0) * 0.02));
            } else if (node.type == Node::File && area >= 500) {
                if (node.contentState == 0)
                    m_wish.append({area, index});
                else if (node.contentState == 2 && node.code) {
                    node.lastSeen = m_frame;
                    if (fitCode(*node.code, node))
                        ++node.revision;
                    const QRectF codeArea = node.code->area.translated(rect.topLeft())
                                                .translated(-halfW, -halfH);
                    const double visible = m_faces.last().bounds.intersected(QRectF(this->rect())).width()
                        * m_faces.last().bounds.intersected(QRectF(this->rect())).height();
                    paintCityTexture(painter, index, camera, codeArea,
                                     y1 + std::max(0.002, double(y1 - y0) * 0.02), pixelsPerUnit,
                                     visible);
                }
            }
            // Label anchor: the far-left corner of the top face.
            const bool wantsLabel = index != 0 && !clipped
                && (solid ? rect.width() * pixelsPerUnit >= 70 && rect.height() * pixelsPerUnit >= 26
                          : rect.width() * pixelsPerUnit >= 120);
            if (wantsLabel) {
                const QVector3D anchor[4] = {
                    {x0 + float(rect.width() * 0.03), y1, z0 + float(rect.height() * 0.03)},
                    {x0, y1, z0}, {x0, y1, z0}, {x0, y1, z0}};
                const QVector3D d = anchor[0] - camera.position;
                const double vz = QVector3D::dotProduct(d, camera.forward);
                if (vz > camera.nearPlane) {
                    const double scale = camera.focal / vz;
                    m_cityLabels.append({QPointF(width() / 2.0 + QVector3D::dotProduct(d, camera.right) * scale,
                                                 height() / 2.0 - QVector3D::dotProduct(d, camera.up) * scale),
                                         clipLabel(node.name, 170, 10), double(node.size)});
                }
            }
        }
    }

    if (!solid || extent < 4)
        return;
    const QVector<int> order = backToFront(m_tree, index, camera.position.x() + halfW,
                                           camera.position.z() + halfH);
    for (int child : order)
        paintCityNode(painter, camera, child);
}

void CodeMapView::paintCityLabels(QPainter &painter)
{
    std::sort(m_cityLabels.begin(), m_cityLabels.end(),
              [](const Label &a, const Label &b) { return a.weight > b.weight; });
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(10);
    painter.setFont(font);
    QVector<QRectF> placed;
    for (const Label &label : std::as_const(m_cityLabels)) {
        if (placed.size() >= 170)
            break;
        if (label.text.isEmpty())
            continue;
        const QRectF pill(label.at.x(), label.at.y(), label.text.size() * 6.1 + 10, 15);
        if (!QRectF(rect()).intersects(pill))
            continue;
        bool free = true;
        for (const QRectF &other : std::as_const(placed)) {
            if (pill.intersects(other)) {
                free = false;
                break;
            }
        }
        if (!free)
            continue;
        placed.append(pill);
        painter.fillRect(pill, m_theme.pill);
        painter.setPen(m_theme.foreground);
        painter.drawText(pill.adjusted(5, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, label.text);
        ++m_stats.labelsDrawn;
    }
}

void CodeMapView::paintCity(QPainter &painter)
{
    painter.fillRect(rect(), m_theme.background);
    if (m_tree.nodes.isEmpty())
        return;
    m_faces.clear();
    m_cityLabels.clear();
    m_cityRequests.clear();
    m_wish.clear();
    m_bitmapsPending = false;
    m_bitmapsBuilt = 0;
    m_framePixels = 0;
    m_bitmapDeadline = 10;
    m_frameClock.start();
    const Camera cam = camera();
    painter.setRenderHint(QPainter::Antialiasing, false);
    paintCityNode(painter, cam, 0);
    if (m_hover > 0) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(m_theme.foreground, 1.5));
        for (const Face &face : std::as_const(m_faces)) {
            if (face.node == m_hover)
                painter.drawPolygon(face.polygon);
        }
    }
    paintCityLabels(painter);
    if (assignCityLevels() || m_bitmapsPending)
        update();
    scheduleScan();
}

int CodeMapView::cityNodeAt(const QPointF &point) const
{
    // Faces are stored in paint order, so the last one containing the point is
    // the one the user sees there.
    for (int i = m_faces.size() - 1; i >= 0; --i) {
        const Face &face = m_faces.at(i);
        if (face.bounds.contains(point) && face.polygon.containsPoint(point, Qt::OddEvenFill))
            return face.node;
    }
    return -1;
}

bool CodeMapView::groundPoint(const Camera &cam, const QPointF &screen, QVector3D *out) const
{
    const QVector3D direction = cam.forward * float(cam.focal)
        + cam.right * float(screen.x() - width() / 2.0)
        - cam.up * float(screen.y() - height() / 2.0);
    if (direction.y() >= -1e-6f)
        return false;
    const float t = (m_orbit.target.y() - cam.position.y()) / direction.y();
    *out = cam.position + direction * t;
    return true;
}

void CodeMapView::dolly(double factor, const QPointF *toward)
{
    m_animation.active = false;
    const Camera before = camera();
    const double old = m_orbit.radius;
    m_orbit.radius *= factor;
    clampOrbit();
    QVector3D ground;
    if (toward && m_orbit.radius < old && groundPoint(before, *toward, &ground)) {
        const float share = float(1.0 - m_orbit.radius / old);
        m_orbit.target += (ground - m_orbit.target) * share;
        clampOrbit();
    }
    m_focusPinned = false;
    markInteracting();
    update();
}

void CodeMapView::flyTo(int index, bool animate)
{
    const Node &node = m_tree.nodes.at(index);
    QVector3D target = m_homeOrbit.target;
    double radius = m_homeOrbit.radius;
    if (index != 0) {
        target = QVector3D(float(node.rect.center().x() - m_worldWidth / 2.0), float(node.top),
                           float(node.rect.center().y() - kWorldHeight / 2.0));
        radius = std::max(0.5, std::max(node.rect.width(), node.rect.height())
                                   * (node.type == Node::File ? 1.5 : 1.7));
    }
    if (!animate) {
        m_orbit.target = target;
        m_orbit.radius = radius;
        clampOrbit();
        update();
        return;
    }
    m_animation.fromTarget = m_orbit.target;
    m_animation.toTarget = target;
    m_animation.fromRadius = m_orbit.radius;
    m_animation.toRadius = radius;
    startAnimation();
}

// --- shared plumbing -----------------------------------------------------------

void CodeMapView::paintEvent(QPaintEvent *)
{
    ++m_frame;
    m_stats.boxesDrawn = 0;
    m_stats.texturesDrawn = 0;
    m_stats.labelsDrawn = 0;
    QPainter painter(this);
    if (m_tree.isEmpty()) {
        painter.fillRect(rect(), m_theme.background);
        painter.setPen(m_theme.muted);
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("No files to map at this ref."));
        return;
    }
    if (m_mode == Mode::Flat)
        paintFlat(painter);
    else
        paintCity(painter);
}

void CodeMapView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_tree.nodes.isEmpty() || !event->oldSize().isValid())
        return;
    if (m_atHome) {
        m_view = fitFor(0, 0.03);
    } else {
        const QSize old = event->oldSize();
        const QPointF center((old.width() / 2.0 - m_view.tx) / m_view.scale,
                             (old.height() / 2.0 - m_view.ty) / m_view.scale);
        m_view.tx = width() / 2.0 - center.x() * m_view.scale;
        m_view.ty = height() / 2.0 - center.y() * m_view.scale;
        clampFlatView();
    }
    update();
}

void CodeMapView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        applyTheme();
        update();
    }
}

int CodeMapView::nodeAt(const QPoint &position) const
{
    return m_mode == Mode::Flat ? flatNodeAt(position, 5) : cityNodeAt(position);
}

void CodeMapView::showTip(int index, const QPoint &globalPosition)
{
    if (index <= 0) {
        QToolTip::hideText();
        return;
    }
    const Node &node = m_tree.nodes.at(index);
    const QString share = describeSize(index) + QStringLiteral(" of ") + m_wording.whole
        + QStringLiteral(" · ");
    QString detail = share
        + (node.type == Node::File ? (node.language.isEmpty() ? m_wording.leaf : node.language)
                                   : m_wording.folder);
    if (node.preview == Preview::Card)
        detail = share + fileCategoryOf(node.name);
    if (node.contentState == 2 && node.code)
        detail += QStringLiteral(" · %1 lines").arg(QLocale().toString(node.code->totalLines));
    const auto picture = m_pictures.constFind(index);
    if (picture != m_pictures.constEnd() && picture->natural.isValid())
        detail += QStringLiteral(" · %1×%2").arg(picture->natural.width()).arg(picture->natural.height());
    QToolTip::showText(globalPosition, node.path.toHtmlEscaped() + QStringLiteral("<br><small>")
                                           + detail.toHtmlEscaped() + QStringLiteral("</small>"),
                       this);
}

void CodeMapView::markInteracting()
{
    m_interacting = true;
    m_settleTimer.start(160, this);
}

void CodeMapView::startAnimation()
{
    m_animation.duration = m_mode == Mode::City ? 600 : 420;
    m_animation.active = true;
    m_animation.clock.start();
    m_interacting = true;
    m_animationTimer.start(16, this);
}

void CodeMapView::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_animationTimer.timerId()) {
        const double t = std::min(1.0, m_animation.clock.elapsed() / double(m_animation.duration));
        const double e = easeInOut(t);
        if (!m_animation.active) {
            m_animationTimer.stop();
        } else if (m_mode == Mode::Flat) {
            const double scale = m_animation.fromScale * std::pow(m_animation.toScale / m_animation.fromScale, e);
            const QPointF center = m_animation.fromCenter + (m_animation.toCenter - m_animation.fromCenter) * e;
            m_view.scale = scale;
            m_view.tx = width() / 2.0 - center.x() * scale;
            m_view.ty = height() / 2.0 - center.y() * scale;
        } else {
            m_orbit.target = m_animation.fromTarget + (m_animation.toTarget - m_animation.fromTarget) * float(e);
            m_orbit.radius = m_animation.fromRadius * std::pow(m_animation.toRadius / m_animation.fromRadius, e);
        }
        if (t >= 1.0) {
            m_animation.active = false;
            m_animationTimer.stop();
            m_interacting = false;
        }
        update();
        return;
    }
    if (event->timerId() == m_settleTimer.timerId()) {
        m_settleTimer.stop();
        if (!m_animation.active && !m_pressed) {
            m_interacting = false;
            update(); // one full-quality frame once the camera rests
        }
        return;
    }
    if (event->timerId() == m_scanTimer.timerId()) {
        m_scanTimer.stop();
        std::sort(m_wish.begin(), m_wish.end(),
                  [](const QPair<double, int> &a, const QPair<double, int> &b) { return a.first > b.first; });
        m_queue.clear();
        for (int i = 0; i < m_wish.size() && i < 400; ++i) {
            Node &node = m_tree.nodes[m_wish.at(i).second];
            if (node.contentState != 0)
                continue;
            const bool pictured = node.preview == Preview::Image || node.preview == Preview::Font;
            if (node.size == 0 || node.preview == Preview::Card
                || (pictured && node.size > kMaxPreviewBytes)) {
                node.contentState = 3;
                continue;
            }
            m_queue.append(m_wish.at(i).second);
        }
        pumpContent();
        if (m_mode == Mode::Flat)
            deriveFlatFocus();
        return;
    }
    QWidget::timerEvent(event);
}

void CodeMapView::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    m_pressed = true;
    m_dragged = false;
    m_panGesture = event->button() != Qt::LeftButton || (event->modifiers() & Qt::ShiftModifier);
    m_pressAt = event->position();
    m_lastAt = m_pressAt;
    m_animation.active = false;
    setCursor(Qt::ClosedHandCursor);
}

void CodeMapView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF at = event->position();
    if (!m_pressed) {
        const int found = nodeAt(at.toPoint());
        if (found != m_hover) {
            m_hover = found;
            update();
        }
        showTip(found, event->globalPosition().toPoint());
        return;
    }
    if (!m_dragged && QLineF(at, m_pressAt).length() < 4)
        return;
    m_dragged = true;
    QToolTip::hideText();
    const QPointF delta = at - m_lastAt;
    m_lastAt = at;
    if (m_mode == Mode::Flat) {
        m_view.tx += delta.x();
        m_view.ty += delta.y();
        clampFlatView();
        m_atHome = false;
    } else if (m_panGesture) {
        // Slide the target along the ground, under the pointer.
        const double unit = m_orbit.radius / camera().focal;
        const double s = std::sin(m_orbit.theta), c = std::cos(m_orbit.theta);
        m_orbit.target += QVector3D(float((-delta.x() * c - delta.y() * s) * unit), 0,
                                    float((delta.x() * s - delta.y() * c) * unit));
        clampOrbit();
    } else {
        m_orbit.theta -= delta.x() * 0.006;
        m_orbit.phi -= delta.y() * 0.006;
        clampOrbit();
    }
    m_focusPinned = false;
    markInteracting();
    update();
}

void CodeMapView::mouseReleaseEvent(QMouseEvent *event)
{
    const bool click = m_pressed && !m_dragged && event->button() == Qt::LeftButton;
    m_pressed = false;
    setCursor(Qt::OpenHandCursor);
    if (!click) {
        markInteracting();
        return;
    }
    if (m_mode == Mode::Flat) {
        clickFlat(event->position());
        return;
    }
    const int found = cityNodeAt(event->position());
    if (found > 0)
        goTo(found);
}

void CodeMapView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int found = m_mode == Mode::Flat ? flatNodeAt(event->position(), 8)
                                           : cityNodeAt(event->position());
    if (found > 0 && m_tree.nodes.at(found).type == Node::File && m_onOpenFile)
        m_onOpenFile(m_tree.nodes.at(found).path);
}

void CodeMapView::wheelEvent(QWheelEvent *event)
{
    const double delta = event->angleDelta().y() != 0 ? event->angleDelta().y()
                                                      : event->pixelDelta().y() * 2.0;
    if (delta == 0)
        return;
    const QPointF at = event->position();
    if (m_mode == Mode::Flat)
        zoomFlatAt(at, std::exp(delta * 0.0016));
    else
        dolly(std::exp(-delta * 0.0016), &at);
    event->accept();
}

void CodeMapView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        if (!zoomOut() && m_onCloseRequested)
            m_onCloseRequested();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomBy(1.4);
        return;
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
        zoomBy(1 / 1.4);
        return;
    case Qt::Key_0:
        resetView();
        return;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const int dx = event->key() == Qt::Key_Left ? 1 : event->key() == Qt::Key_Right ? -1 : 0;
        const int dy = event->key() == Qt::Key_Up ? 1 : event->key() == Qt::Key_Down ? -1 : 0;
        if (m_mode == Mode::Flat) {
            m_view.tx += dx * 80;
            m_view.ty += dy * 80;
            clampFlatView();
            m_atHome = false;
        } else {
            m_orbit.theta += dx * 0.12;
            m_orbit.phi += dy * 0.08;
            clampOrbit();
        }
        m_focusPinned = false;
        markInteracting();
        update();
        return;
    }
    default:
        QWidget::keyPressEvent(event);
    }
}

void CodeMapView::leaveEvent(QEvent *)
{
    QToolTip::hideText();
    if (m_hover != -1) {
        m_hover = -1;
        update();
    }
}
