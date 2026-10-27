#include "WorkflowsCards.h"

#include "WorkflowsModel.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDrag>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QStyle>
#include <QTextLayout>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace forkmesh::workflows {
namespace {

constexpr char kWorkflowMime[] = "application/x-forkmesh-workflow";
constexpr char kWorkflowPinMime[] = "application/x-forkmesh-workflow-pin";
constexpr int kListControlsWidth = 262;

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

qint64 integer(const QJsonObject &object, const char *key)
{
    return qint64(object.value(QLatin1String(key)).toDouble());
}

QString rgba(const QColor &color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(QString::number(color.alphaF(), 'f', 3));
}

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(alpha);
    return color;
}

QLabel *plainLabel(const QString &value, const char *name)
{
    auto *label = new QLabel(value);
    label->setObjectName(QLatin1String(name));
    label->setTextFormat(Qt::PlainText);
    return label;
}

QLabel *dotLabel(const QColor &color, int side = 8)
{
    auto *label = new QLabel;
    label->setObjectName(QStringLiteral("workflowCardDot"));
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(side, side) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(0.5, 0.5, side - 1, side - 1));
    painter.end();
    label->setPixmap(pixmap);
    label->setFixedSize(side, side);
    return label;
}

QString elided(const QString &value, const QFont &font, int width)
{
    return QFontMetrics(font).elidedText(value, Qt::ElideRight, std::max(0, width));
}

class ElidedLabel final : public QLabel
{
public:
    ElidedLabel(const QString &value, const char *name) : m_full(value)
    {
        setObjectName(QLatin1String(name));
        setTextFormat(Qt::PlainText);
        setToolTip(value);
        setMinimumWidth(24);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        QLabel::setText(value);
    }

    QSize sizeHint() const override
    {
        return {QFontMetrics(font()).horizontalAdvance(m_full) + 2, QLabel::sizeHint().height()};
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        QLabel::setText(elided(m_full, font(), width()));
    }

private:
    QString m_full;
};

class ChipLabel final : public QLabel
{
public:
    ChipLabel(const QString &value, const QColor &fill, const QColor &ink) : m_fill(fill)
    {
        setObjectName(QStringLiteral("workflowKindChip"));
        setTextFormat(Qt::PlainText);
        setText(value);
        setContentsMargins(8, 1, 8, 1);
        QFont small = font();
        small.setPixelSize(11);
        small.setWeight(QFont::DemiBold);
        setFont(small);
        setStyleSheet(QStringLiteral("QLabel#workflowKindChip { color: %1; background: transparent; border: none; }")
                          .arg(ink.name()));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_fill);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.drawRoundedRect(box, box.height() / 2, box.height() / 2);
        painter.end();
        QLabel::paintEvent(event);
    }

private:
    QColor m_fill;
};

} // namespace

QString viewModeKey(ViewMode mode)
{
    switch (mode) {
    case ViewMode::List:
        return QStringLiteral("list");
    case ViewMode::Compact:
        return QStringLiteral("compact");
    case ViewMode::Thumbnails:
        break;
    }
    return QStringLiteral("thumbnails");
}

ViewMode viewModeFromKey(const QString &key)
{
    if (key == QLatin1String("list"))
        return ViewMode::List;
    if (key == QLatin1String("compact"))
        return ViewMode::Compact;
    return ViewMode::Thumbnails;
}

QColor mix(const QColor &a, const QColor &b, qreal amount)
{
    const qreal keep = 1.0 - amount;
    return QColor::fromRgbF(float(a.redF() * keep + b.redF() * amount), float(a.greenF() * keep + b.greenF() * amount),
                            float(a.blueF() * keep + b.blueF() * amount),
                            float(a.alphaF() * keep + b.alphaF() * amount));
}

Tones Tones::from(const QPalette &palette)
{
    Tones tones;
    tones.window = palette.color(QPalette::Window);
    tones.ink = palette.color(QPalette::WindowText);
    tones.dark = tones.window.lightnessF() < 0.5;
    tones.surface = palette.color(QPalette::Base);
    if (std::abs(tones.surface.lightnessF() - tones.window.lightnessF()) > 0.25)
        tones.surface = mix(tones.window, tones.ink, tones.dark ? 0.04 : 0.0);
    if (!tones.dark)
        tones.surface = QColor(Qt::white);
    tones.sunken = mix(tones.surface, tones.ink, tones.dark ? 0.06 : 0.035);
    tones.secondary = mix(tones.ink, tones.window, 0.22);
    tones.muted = mix(tones.ink, tones.window, 0.42);
    tones.border = mix(tones.window, tones.ink, tones.dark ? 0.16 : 0.12);
    tones.strong = mix(tones.window, tones.ink, tones.dark ? 0.3 : 0.24);
    tones.hover = mix(tones.surface, tones.ink, tones.dark ? 0.09 : 0.06);
    tones.accent = palette.color(QPalette::Highlight);
    if (tones.accent.saturationF() < 0.2)
        tones.accent = tones.dark ? QColor(QStringLiteral("#4493f8")) : QColor(QStringLiteral("#0969da"));
    tones.success = tones.dark ? QColor(QStringLiteral("#3fb950")) : QColor(QStringLiteral("#1a7f37"));
    tones.danger = tones.dark ? QColor(QStringLiteral("#f85149")) : QColor(QStringLiteral("#cf222e"));
    tones.attention = tones.dark ? QColor(QStringLiteral("#d29922")) : QColor(QStringLiteral("#9a6700"));
    tones.agent = tones.dark ? QColor(QStringLiteral("#a371f7")) : QColor(QStringLiteral("#8250df"));
    tones.account = tones.dark ? QColor(QStringLiteral("#39c5cf")) : QColor(QStringLiteral("#1b7c83"));
    return tones;
}

QColor Tones::kind(const QString &value) const
{
    if (value == QLatin1String("agent"))
        return agent;
    if (value == QLatin1String("account"))
        return account;
    return accent;
}

QColor Tones::state(const QJsonObject &entry) const
{
    if (!text(entry, "error").isEmpty())
        return attention;
    if (entry.value(QStringLiteral("running")).toBool() || entry.value(QStringLiteral("agentWorking")).toBool())
        return accent;
    if (!entry.value(QStringLiteral("enabled")).toBool())
        return muted;
    const QString status = text(entry, "lastStatus");
    if (status == QLatin1String("error"))
        return danger;
    if (status == QLatin1String("success"))
        return success;
    return muted;
}

QString Tones::sheet() const
{
    QString css = QStringLiteral(
        "QFrame#workflowCard { background: @surface; border: 1px solid @border; border-radius: 12px; }"
        "QFrame#workflowCard:hover { border-color: @strong; }"
        "QFrame#workflowCard[selected=\"true\"] { border: 1px solid @accent; }"
        "QFrame#workflowCard QLabel { background: transparent; border: none; }"
        "QLabel#workflowCardName { color: @text; font-size: 13px; font-weight: 600; }"
        "QLabel#workflowCardMeta, QLabel#workflowCardTimes { color: @muted; font-size: 11px; }"
        "QLabel#workflowListColumn { color: @secondary; font-size: 12px; }"
        "QLabel#workflowListHeading { color: @muted; font-size: 11px; font-weight: 600; }"
        "QWidget#workflowCardOutput { background: @sunken; border-radius: 6px; }"
        "QToolButton#workflowCardButton { background: transparent; border: none; border-radius: 6px;"
        " min-width: 26px; max-width: 26px; min-height: 26px; max-height: 26px; padding: 0px; }"
        "QToolButton#workflowCardButton:hover { background: @hover; }"
        "QToolButton#workflowCardButton:disabled { background: transparent; }"
        "QToolButton#workflowCardRun { background: @accentSoft; color: @accent; border: none; border-radius: 6px;"
        " min-height: 26px; max-height: 26px; padding: 0px 10px 0px 7px; font-size: 12px; font-weight: 600; }"
        "QToolButton#workflowCardRun:hover { background: @accentHover; }"
        "QToolButton#workflowCardRun[stopping=\"true\"] { background: @dangerSoft; color: @danger; }"
        "QToolButton#workflowCardRun:disabled { background: @sunken; color: @muted; }"
        "QFrame#workflowsKpiTile { background: @surface; border: 1px solid @border; border-radius: 12px; }"
        "QFrame#workflowsKpiTile QLabel { background: transparent; border: none; }"
        "QLabel#workflowsKpiName { color: @muted; font-size: 11px; font-weight: 600; }"
        "QLabel#workflowsKpi { color: @text; font-size: 22px; font-weight: 650; }"
        "QLabel#workflowsKpiNote { color: @muted; font-size: 11px; }"
        "QLabel#workflowsSubtitle { color: @muted; font-size: 12px; }"
        "QToolButton#workflowsScope { background: transparent; color: @secondary; border: 1px solid @border;"
        " border-radius: 13px; min-height: 24px; max-height: 24px; padding: 0px 11px; font-size: 12px; }"
        "QToolButton#workflowsScope:hover { background: @hover; }"
        "QToolButton#workflowsScope:checked { background: @accentSoft; color: @accent; border-color: @accentEdge;"
        " font-weight: 600; }"
        "QWidget#workflowsViewSwitch { background: @sunken; border: 1px solid @border; border-radius: 8px; }"
        "QToolButton#workflowsViewButton { background: transparent; border: none; border-radius: 6px;"
        " min-width: 28px; max-width: 28px; min-height: 24px; max-height: 24px; padding: 0px; }"
        "QToolButton#workflowsViewButton:hover { background: @hover; }"
        "QToolButton#workflowsViewButton:checked { background: @surface; }"
        "QScrollArea#workflowsCollection, QWidget#workflowsCollectionBody { background: transparent; border: none; }"
        "QLabel#workflowsCollectionEmpty { color: @muted; font-size: 13px; padding: 36px; }"
        "QListWidget#workflowsSteps { background: @surface; border: 1px solid @border; border-radius: 10px;"
        " padding: 4px; outline: 0; }"
        "QListWidget#workflowsSteps::item { background: transparent; border: none; }"
        "QLabel#workflowsPaneTitle { color: @muted; font-size: 11px; font-weight: 600; }"
        "QLabel#workflowsFact { background: @sunken; color: @secondary; border: 1px solid @border;"
        " border-radius: 10px; padding: 1px 8px; font-size: 11px; }"
        "QLabel#workflowsFact[tone=\"accent\"] { background: @accentSoft; color: @accent; border-color: @accentEdge; }"
        "QLabel#workflowsFact[tone=\"success\"] { background: @successSoft; color: @success; border-color: @successEdge; }"
        "QLabel#workflowsFact[tone=\"danger\"] { background: @dangerSoft; color: @danger; border-color: @dangerEdge; }"
        "QLabel#workflowsFact[tone=\"attention\"] { background: @attentionSoft; color: @attention;"
        " border-color: @attentionEdge; }"
        "QLabel#workflowsFactFace { background: transparent; border: none; }");
    const QList<QPair<QString, QColor>> roles{
        {QStringLiteral("@accentSoft"), withAlpha(accent, dark ? 0.2 : 0.1)},
        {QStringLiteral("@accentHover"), withAlpha(accent, dark ? 0.3 : 0.17)},
        {QStringLiteral("@accentEdge"), withAlpha(accent, 0.45)},
        {QStringLiteral("@dangerSoft"), withAlpha(danger, dark ? 0.2 : 0.1)},
        {QStringLiteral("@dangerEdge"), withAlpha(danger, 0.4)},
        {QStringLiteral("@successSoft"), withAlpha(success, dark ? 0.2 : 0.08)},
        {QStringLiteral("@successEdge"), withAlpha(success, 0.4)},
        {QStringLiteral("@attentionSoft"), withAlpha(attention, dark ? 0.2 : 0.08)},
        {QStringLiteral("@attentionEdge"), withAlpha(attention, 0.4)},
        {QStringLiteral("@success"), success},
        {QStringLiteral("@attention"), attention},
        {QStringLiteral("@surface"), surface},
        {QStringLiteral("@sunken"), sunken},
        {QStringLiteral("@border"), border},
        {QStringLiteral("@strong"), strong},
        {QStringLiteral("@hover"), hover},
        {QStringLiteral("@accent"), accent},
        {QStringLiteral("@danger"), danger},
        {QStringLiteral("@secondary"), secondary},
        {QStringLiteral("@muted"), muted},
        {QStringLiteral("@text"), ink},
    };
    for (const auto &role : roles)
        css.replace(role.first, rgba(role.second));
    return css;
}

QPixmap glyphPixmap(const QString &name, const QColor &color, int size, qreal ratio)
{
    QPixmap pixmap(QSize(size, size) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal s = size;
    QPen pen(color, std::max(1.3, s / 11.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    const auto at = [s](qreal x, qreal y) { return QPointF(x * s, y * s); };
    if (name == QLatin1String("play")) {
        QPainterPath path;
        path.moveTo(at(0.3, 0.2));
        path.lineTo(at(0.8, 0.5));
        path.lineTo(at(0.3, 0.8));
        path.closeSubpath();
        painter.setBrush(color);
        painter.drawPath(path);
    } else if (name == QLatin1String("stop")) {
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(QRectF(at(0.26, 0.26), at(0.74, 0.74)), s * 0.08, s * 0.08);
    } else if (name == QLatin1String("pause")) {
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(QRectF(at(0.28, 0.22), at(0.43, 0.78)), s * 0.05, s * 0.05);
        painter.drawRoundedRect(QRectF(at(0.57, 0.22), at(0.72, 0.78)), s * 0.05, s * 0.05);
    } else if (name == QLatin1String("edit")) {
        QPainterPath path;
        path.moveTo(at(0.2, 0.8));
        path.lineTo(at(0.24, 0.62));
        path.lineTo(at(0.66, 0.2));
        path.lineTo(at(0.8, 0.34));
        path.lineTo(at(0.38, 0.76));
        path.closeSubpath();
        painter.drawPath(path);
        painter.drawLine(at(0.58, 0.28), at(0.72, 0.42));
    } else if (name == QLatin1String("pin") || name == QLatin1String("pinned")) {
        painter.translate(s / 2, s / 2);
        painter.rotate(40);
        painter.translate(-s / 2, -s / 2);
        QPainterPath head;
        head.moveTo(at(0.36, 0.18));
        head.lineTo(at(0.64, 0.18));
        head.lineTo(at(0.6, 0.42));
        head.lineTo(at(0.72, 0.56));
        head.lineTo(at(0.28, 0.56));
        head.lineTo(at(0.4, 0.42));
        head.closeSubpath();
        if (name == QLatin1String("pinned"))
            painter.setBrush(color);
        painter.drawPath(head);
        painter.drawLine(at(0.5, 0.56), at(0.5, 0.84));
    } else if (name == QLatin1String("clock")) {
        painter.drawEllipse(QRectF(at(0.18, 0.18), at(0.82, 0.82)));
        painter.drawLine(at(0.5, 0.5), at(0.5, 0.31));
        painter.drawLine(at(0.5, 0.5), at(0.64, 0.58));
    } else if (name == QLatin1String("more")) {
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        for (qreal x : {0.24, 0.5, 0.76})
            painter.drawEllipse(at(x, 0.5), s * 0.075, s * 0.075);
    } else if (name == QLatin1String("output")) {
        painter.drawRoundedRect(QRectF(at(0.24, 0.16), at(0.76, 0.84)), s * 0.08, s * 0.08);
        for (qreal y : {0.38, 0.52, 0.66})
            painter.drawLine(at(0.36, y), at(y > 0.6 ? 0.54 : 0.64, y));
    } else if (name == QLatin1String("list")) {
        for (qreal y : {0.28, 0.5, 0.72}) {
            painter.drawPoint(at(0.22, y));
            painter.drawLine(at(0.38, y), at(0.8, y));
        }
    } else if (name == QLatin1String("thumbnails")) {
        for (qreal x : {0.18, 0.54})
            for (qreal y : {0.18, 0.54})
                painter.drawRoundedRect(QRectF(at(x, y), at(x + 0.28, y + 0.28)), s * 0.05, s * 0.05);
    } else if (name == QLatin1String("compact")) {
        painter.setBrush(color);
        painter.setPen(Qt::NoPen);
        for (qreal x : {0.17, 0.42, 0.67})
            for (qreal y : {0.17, 0.42, 0.67})
                painter.drawRoundedRect(QRectF(at(x, y), at(x + 0.16, y + 0.16)), s * 0.03, s * 0.03);
    } else if (name == QLatin1String("workflow")) {
        painter.drawRoundedRect(QRectF(at(0.14, 0.2), at(0.38, 0.4)), s * 0.05, s * 0.05);
        painter.drawRoundedRect(QRectF(at(0.62, 0.6), at(0.86, 0.8)), s * 0.05, s * 0.05);
        QPainterPath link;
        link.moveTo(at(0.38, 0.3));
        link.cubicTo(at(0.6, 0.3), at(0.5, 0.7), at(0.62, 0.7));
        painter.drawPath(link);
    } else if (name == QLatin1String("plus")) {
        painter.drawLine(at(0.5, 0.22), at(0.5, 0.78));
        painter.drawLine(at(0.22, 0.5), at(0.78, 0.5));
    }
    painter.end();
    return pixmap;
}

QIcon glyphIcon(const QString &name, const QColor &color, int size)
{
    QIcon icon;
    for (qreal ratio : {1.0, 2.0})
        icon.addPixmap(glyphPixmap(name, color, size, ratio));
    return icon;
}

QPixmap roundFace(const QPixmap &source, const QString &kind, const Tones &tones, int side, qreal ratio)
{
    QPixmap canvas(QSize(side, side) * ratio);
    canvas.setDevicePixelRatio(ratio);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF box(0, 0, side, side);
    if (source.isNull()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(tones.kind(kind), tones.dark ? 0.26 : 0.16));
        painter.drawEllipse(box);
        const int glyph = std::max(10, side * 11 / 20);
        painter.drawPixmap(QRectF((side - glyph) / 2.0, (side - glyph) / 2.0, glyph, glyph),
                           glyphPixmap(QStringLiteral("workflow"), tones.kind(kind), glyph, ratio),
                           QRectF(0, 0, glyph * ratio, glyph * ratio));
    } else {
        painter.drawPixmap(box, source, QRectF(source.rect()));
    }
    painter.end();
    return canvas;
}

ClampLabel::ClampLabel(const QString &value, int lines, QWidget *parent)
    : QWidget(parent), m_text(value.simplified()), m_lines(std::max(1, lines))
{
    setObjectName(QStringLiteral("workflowCardOutputText"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setToolTip(value.trimmed().left(1200));
    m_color = palette().color(QPalette::WindowText);
    m_placeholderColor = m_color;
    updateHeight();
}

void ClampLabel::setText(const QString &value)
{
    m_text = value.simplified();
    setToolTip(value.trimmed().left(1200));
    update();
}

void ClampLabel::setPlaceholder(const QString &placeholder)
{
    m_placeholder = placeholder;
    update();
}

void ClampLabel::setColors(const QColor &color, const QColor &placeholder)
{
    m_color = color;
    m_placeholderColor = placeholder;
    update();
}

QSize ClampLabel::sizeHint() const
{
    return {120, m_lines * fontMetrics().lineSpacing()};
}

QSize ClampLabel::minimumSizeHint() const
{
    return {24, m_lines * fontMetrics().lineSpacing()};
}

void ClampLabel::updateHeight()
{
    setFixedHeight(m_lines * fontMetrics().lineSpacing());
}

void ClampLabel::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange)
        updateHeight();
}

void ClampLabel::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const bool empty = m_text.isEmpty();
    QFont font = this->font();
    if (empty)
        font.setItalic(true);
    painter.setFont(font);
    painter.setPen(empty ? m_placeholderColor : m_color);
    const QString shown = empty ? m_placeholder : m_text;
    const QFontMetrics metrics(font);
    QTextLayout layout(shown, font);
    layout.beginLayout();
    qreal y = 0;
    int drawn = 0;
    while (drawn < m_lines) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width());
        const QString piece = shown.mid(line.textStart(), line.textLength());
        if (drawn == m_lines - 1 && line.textStart() + line.textLength() < shown.size()) {
            painter.drawText(QPointF(0, y + metrics.ascent()),
                             metrics.elidedText(shown.mid(line.textStart()), Qt::ElideRight, width()));
        } else {
            painter.drawText(QPointF(0, y + metrics.ascent()), piece.trimmed());
        }
        y += metrics.lineSpacing();
        ++drawn;
    }
    layout.endLayout();
}

WorkflowSwitch::WorkflowSwitch(const Tones &tones, QWidget *parent) : QAbstractButton(parent), m_tones(tones)
{
    setObjectName(QStringLiteral("workflowCardSwitch"));
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
}

QSize WorkflowSwitch::sizeHint() const
{
    return {32, 26};
}

void WorkflowSwitch::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF track(2, (height() - 16) / 2.0, 28, 16);
    QColor fill = isChecked() ? m_tones.accent : m_tones.strong;
    if (!isEnabled())
        fill = withAlpha(fill, 0.45);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(track, 8, 8);
    const qreal knob = 12;
    const qreal x = isChecked() ? track.right() - knob - 2 : track.left() + 2;
    painter.setBrush(QColor(Qt::white));
    painter.drawEllipse(QRectF(x, track.top() + 2, knob, knob));
    if (hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(withAlpha(m_tones.accent, 0.6), 1.5));
        painter.drawRoundedRect(track.adjusted(-1.5, -1.5, 1.5, 1.5), 9, 9);
    }
}

WorkflowThumbnail::WorkflowThumbnail(const QJsonObject &entry, const QPixmap &face, const Tones &tones,
                                     QWidget *parent)
    : QWidget(parent), m_entry(entry), m_face(face), m_tones(tones)
{
    setObjectName(QStringLiteral("workflowThumbnail"));
    setFixedHeight(112);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QSize WorkflowThumbnail::sizeHint() const
{
    return {260, 112};
}

void WorkflowThumbnail::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(rect()).adjusted(1, 1, -1, 0);
    const qreal radius = 11;
    QPainterPath clip;
    clip.moveTo(box.left(), box.bottom());
    clip.lineTo(box.left(), box.top() + radius);
    clip.quadTo(box.topLeft(), QPointF(box.left() + radius, box.top()));
    clip.lineTo(box.right() - radius, box.top());
    clip.quadTo(box.topRight(), QPointF(box.right(), box.top() + radius));
    clip.lineTo(box.right(), box.bottom());
    clip.closeSubpath();
    painter.setClipPath(clip);

    const QColor tone = m_tones.kind(text(m_entry, "kind"));
    QLinearGradient wash(box.topLeft(), box.bottomRight());
    wash.setColorAt(0, mix(m_tones.surface, tone, m_tones.dark ? 0.3 : 0.17));
    wash.setColorAt(1, mix(m_tones.surface, tone, m_tones.dark ? 0.1 : 0.05));
    painter.fillRect(box, wash);
    painter.setPen(Qt::NoPen);
    painter.setBrush(withAlpha(tone, m_tones.dark ? 0.22 : 0.16));
    for (qreal x = box.left() + 10; x < box.right(); x += 14)
        for (qreal y = box.top() + 10; y < box.bottom(); y += 14)
            painter.drawEllipse(QPointF(x, y), 0.9, 0.9);

    const qreal faceSide = 46;
    const QPointF faceAt(box.left() + 18, box.center().y() - faceSide / 2 + 10);
    painter.setBrush(m_tones.surface);
    painter.setPen(QPen(withAlpha(tone, 0.35), 1));
    painter.drawEllipse(QRectF(faceAt, QSizeF(faceSide, faceSide)).adjusted(-3, -3, 3, 3));
    if (!m_face.isNull())
        painter.drawPixmap(QRectF(faceAt, QSizeF(faceSide, faceSide)), m_face, QRectF(m_face.rect()));

    const int steps = std::clamp(m_entry.value(QStringLiteral("steps")).toInt(), 1, 6);
    const qreal left = faceAt.x() + faceSide + 22;
    const qreal right = box.right() - 18;
    const qreal mid = faceAt.y() + faceSide / 2;
    const qreal nodeWidth = 22;
    const qreal nodeHeight = 15;
    const qreal gap = steps > 1 ? std::min<qreal>(40, (right - left - nodeWidth) / (steps - 1)) : 0;
    QList<QPointF> centres;
    for (int index = 0; index < steps; ++index) {
        const qreal wave = (index % 2 == 0 ? -1 : 1) * (steps > 2 ? 7 : 0);
        centres.append(QPointF(left + nodeWidth / 2 + index * gap, mid + wave));
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(withAlpha(tone, 0.55), 1.6, Qt::SolidLine, Qt::RoundCap));
    for (int index = 1; index < centres.size(); ++index) {
        QPainterPath link;
        const QPointF from = centres.at(index - 1) + QPointF(nodeWidth / 2, 0);
        const QPointF to = centres.at(index) - QPointF(nodeWidth / 2, 0);
        const qreal bend = (to.x() - from.x()) / 2;
        link.moveTo(from);
        link.cubicTo(from + QPointF(bend, 0), to - QPointF(bend, 0), to);
        painter.drawPath(link);
    }
    const bool running = m_entry.value(QStringLiteral("running")).toBool() ||
                         m_entry.value(QStringLiteral("agentWorking")).toBool();
    for (int index = 0; index < centres.size(); ++index) {
        const QRectF node(centres.at(index) - QPointF(nodeWidth / 2, nodeHeight / 2), QSizeF(nodeWidth, nodeHeight));
        if (index == 0) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(tone);
            painter.drawRoundedRect(node, nodeHeight / 2, nodeHeight / 2);
            continue;
        }
        painter.setPen(QPen(running && index == 1 ? m_tones.accent : withAlpha(tone, 0.75), 1.3));
        painter.setBrush(m_tones.surface);
        painter.drawRoundedRect(node, 4.5, 4.5);
        painter.setPen(QPen(withAlpha(tone, 0.5), 1.2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(node.left() + 5, node.center().y()), QPointF(node.right() - 5, node.center().y()));
    }

    const QString state = workflowStateText(m_entry).section(QStringLiteral(" · "), 0, 0);
    QFont pillFont = font();
    pillFont.setPixelSize(11);
    pillFont.setWeight(QFont::DemiBold);
    painter.setFont(pillFont);
    const QFontMetrics metrics(pillFont);
    const QColor stateColor = m_tones.state(m_entry);
    const qreal pillWidth = metrics.horizontalAdvance(state) + 26;
    const QRectF pill(box.right() - pillWidth - 10, box.top() + 10, pillWidth, 20);
    painter.setPen(Qt::NoPen);
    painter.setBrush(withAlpha(m_tones.surface, 0.92));
    painter.drawRoundedRect(pill, 10, 10);
    painter.setBrush(stateColor);
    painter.drawEllipse(QPointF(pill.left() + 11, pill.center().y()), 3.5, 3.5);
    painter.setPen(m_tones.dark ? m_tones.ink : mix(stateColor, m_tones.ink, 0.35));
    painter.drawText(pill.adjusted(19, 0, -7, 0), Qt::AlignVCenter | Qt::AlignLeft, state);

    const QString rail = text(m_entry, "pinned");
    if (!rail.isEmpty()) {
        const QString label = QStringLiteral("Pinned · %1").arg(rail);
        const qreal width = metrics.horizontalAdvance(label) + 28;
        const QRectF badge(box.left() + 10, box.top() + 10, width, 20);
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(m_tones.surface, 0.92));
        painter.drawRoundedRect(badge, 10, 10);
        painter.drawPixmap(QRectF(badge.left() + 6, badge.top() + 3, 14, 14),
                           glyphPixmap(QStringLiteral("pinned"), m_tones.accent, 14, devicePixelRatioF()),
                           QRectF(0, 0, 14 * devicePixelRatioF(), 14 * devicePixelRatioF()));
        painter.setPen(m_tones.secondary);
        painter.drawText(badge.adjusted(22, 0, -7, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
    }
}

WorkflowCard::WorkflowCard(const QJsonObject &entry, const QPixmap &face, const Tones &tones,
                           const Options &options, Action action, QWidget *parent)
    : QFrame(parent), m_entry(entry), m_tones(tones), m_options(options), m_action(std::move(action))
{
    setObjectName(QStringLiteral("workflowCard"));
    setProperty("workflowKey", key());
    setProperty("workflowName", text(entry, "name"));
    setProperty("selected", options.selected);
    setAccessibleName(text(entry, "name"));
    setAccessibleDescription(QStringLiteral("%1 · %2").arg(text(entry, "kindLabel"), workflowStateText(entry)));
    setToolTip(text(entry, "description").isEmpty()
                   ? QString()
                   : text(entry, "description"));
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::ClickFocus);
    const qreal ratio = qApp ? qApp->devicePixelRatio() : 1.0;
    const QString name = text(entry, "name").isEmpty() ? QStringLiteral("Untitled workflow") : text(entry, "name");
    const QString output = text(entry, "lastOutput");

    if (options.mode == ViewMode::Thumbnails) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(new WorkflowThumbnail(entry, roundFace(face, text(entry, "kind"), tones, 46, ratio), tones));
        auto *body = new QVBoxLayout;
        body->setContentsMargins(14, 11, 12, 10);
        body->setSpacing(6);
        auto *top = new QHBoxLayout;
        top->setSpacing(8);
        top->addWidget(new ElidedLabel(name, "workflowCardName"), 1);
        top->addWidget(kindChip());
        body->addLayout(top);
        body->addWidget(new ElidedLabel(QStringLiteral("%1 · %2").arg(text(entry, "place"), text(entry, "trigger")),
                                        "workflowCardMeta"));
        auto *box = new QWidget;
        box->setObjectName(QStringLiteral("workflowCardOutput"));
        box->setAttribute(Qt::WA_StyledBackground);
        auto *boxLayout = new QHBoxLayout(box);
        boxLayout->setContentsMargins(8, 6, 8, 6);
        boxLayout->setSpacing(7);
        auto *bar = new QWidget;
        bar->setFixedWidth(2);
        bar->setAttribute(Qt::WA_StyledBackground);
        bar->setStyleSheet(QStringLiteral("background: %1; border-radius: 1px;").arg(rgba(tones.state(entry))));
        boxLayout->addWidget(bar);
        auto *latest = new ClampLabel(output, 2);
        QFont small = latest->font();
        small.setPixelSize(12);
        latest->setFont(small);
        latest->setPlaceholder(QStringLiteral("No output yet. Run it to see what it makes."));
        latest->setColors(tones.secondary, tones.muted);
        latest->setAccessibleName(QStringLiteral("Most recent output"));
        boxLayout->addWidget(latest, 1);
        body->addWidget(box);
        body->addWidget(plainLabel(timesText(), "workflowCardTimes"));
        body->addWidget(buildControls(false));
        layout->addLayout(body);
        return;
    }

    if (options.mode == ViewMode::Compact) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(10, 10, 8, 6);
        layout->setSpacing(6);
        auto *top = new QHBoxLayout;
        top->setSpacing(9);
        auto *icon = new QLabel;
        icon->setPixmap(roundFace(face, text(entry, "kind"), tones, 30, ratio));
        icon->setFixedSize(30, 30);
        top->addWidget(icon, 0, Qt::AlignTop);
        auto *words = new QVBoxLayout;
        words->setSpacing(1);
        words->addWidget(new ElidedLabel(name, "workflowCardName"));
        words->addWidget(new ElidedLabel(QStringLiteral("%1 · %2").arg(text(entry, "kindLabel"),
                                                                       relativeTime(integer(entry, "lastRunAt"),
                                                                                    QDateTime::currentMSecsSinceEpoch())),
                                         "workflowCardMeta"));
        top->addLayout(words, 1);
        auto *dot = dotLabel(tones.state(entry));
        dot->setToolTip(workflowStateText(entry));
        top->addWidget(dot, 0, Qt::AlignTop);
        layout->addLayout(top);
        auto *latest = new ClampLabel(output, 1);
        QFont small = latest->font();
        small.setPixelSize(11);
        latest->setFont(small);
        latest->setPlaceholder(QStringLiteral("No output yet"));
        latest->setColors(tones.secondary, tones.muted);
        latest->setAccessibleName(QStringLiteral("Most recent output"));
        layout->addWidget(latest);
        layout->addWidget(buildControls(true));
        return;
    }

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(12, 9, 10, 9);
    row->setSpacing(12);
    auto *icon = new QLabel;
    icon->setPixmap(roundFace(face, text(entry, "kind"), tones, 36, ratio));
    icon->setFixedSize(36, 36);
    row->addWidget(icon);
    auto *words = new QVBoxLayout;
    words->setSpacing(3);
    auto *title = new QHBoxLayout;
    title->setSpacing(8);
    title->addWidget(new ElidedLabel(name, "workflowCardName"));
    title->addWidget(kindChip());
    title->addStretch(1);
    words->addLayout(title);
    auto *latest = new ClampLabel(output, 1);
    QFont small = latest->font();
    small.setPixelSize(12);
    latest->setFont(small);
    latest->setPlaceholder(QStringLiteral("No output yet"));
    latest->setColors(tones.secondary, tones.muted);
    latest->setAccessibleName(QStringLiteral("Most recent output"));
    words->addWidget(latest);
    row->addLayout(words, 1);
    auto *trigger = new ElidedLabel(text(entry, "trigger"), "workflowListColumn");
    trigger->setFixedWidth(150);
    trigger->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    row->addWidget(trigger);
    auto *last = new QWidget;
    last->setFixedWidth(130);
    auto *lastLayout = new QHBoxLayout(last);
    lastLayout->setContentsMargins(0, 0, 0, 0);
    lastLayout->setSpacing(6);
    lastLayout->addWidget(dotLabel(tones.state(entry)));
    lastLayout->addWidget(new ElidedLabel(workflowStateText(entry), "workflowListColumn"), 1);
    last->setToolTip(QStringLiteral("Last run %1").arg(relativeTime(integer(entry, "lastRunAt"),
                                                                    QDateTime::currentMSecsSinceEpoch())
                                                           .toLower()));
    row->addWidget(last);
    const qint64 next = integer(entry, "nextRunAt");
    auto *nextLabel = new ElidedLabel(next > 0 ? relativeTime(next, QDateTime::currentMSecsSinceEpoch())
                                               : relativeTime(integer(entry, "lastRunAt"),
                                                              QDateTime::currentMSecsSinceEpoch()),
                                      "workflowListColumn");
    nextLabel->setToolTip(next > 0 ? QStringLiteral("Next run") : QStringLiteral("Last run"));
    nextLabel->setFixedWidth(96);
    nextLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    row->addWidget(nextLabel);
    QWidget *controls = buildControls(false);
    controls->setFixedWidth(kListControlsWidth);
    row->addWidget(controls);
}

QString WorkflowCard::key() const
{
    return text(m_entry, "key");
}

void WorkflowCard::setSelected(bool selected)
{
    if (property("selected").toBool() == selected)
        return;
    setProperty("selected", selected);
    style()->unpolish(this);
    style()->polish(this);
}

QString WorkflowCard::timesText() const
{
    const qint64 current = QDateTime::currentMSecsSinceEpoch();
    QStringList parts;
    const qint64 last = integer(m_entry, "lastRunAt");
    parts << (last > 0 ? QStringLiteral("Ran %1").arg(relativeTime(last, current).toLower())
                       : QStringLiteral("Never run"));
    const qint64 next = integer(m_entry, "nextRunAt");
    if (next > 0)
        parts << QStringLiteral("next %1").arg(relativeTime(next, current).toLower());
    const int runs = m_entry.value(QStringLiteral("runs")).toInt();
    if (runs > 0) {
        const int failed = m_entry.value(QStringLiteral("failedRuns")).toInt();
        parts << (failed ? QStringLiteral("%1 runs, %2 failed").arg(runs).arg(failed) : QStringLiteral("%1 runs").arg(runs));
    }
    return parts.join(QStringLiteral(" · "));
}

QWidget *WorkflowCard::kindChip() const
{
    const QColor tone = m_tones.kind(text(m_entry, "kind"));
    return new ChipLabel(text(m_entry, "kindLabel"), withAlpha(tone, m_tones.dark ? 0.22 : 0.12), tone);
}

QWidget *WorkflowCard::buildControls(bool compact)
{
    auto *bar = new QWidget;
    bar->setObjectName(QStringLiteral("workflowCardControls"));
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    const bool local = text(m_entry, "source") == QLatin1String("local");
    const bool running = m_entry.value(QStringLiteral("running")).toBool() ||
                         m_entry.value(QStringLiteral("agentWorking")).toBool();
    const bool enabled = m_entry.value(QStringLiteral("enabled")).toBool();
    const auto tool = [this, layout](const QString &glyph, const QString &label, const QString &action,
                                     const QString &tip, const QColor &color) {
        auto *button = new QToolButton;
        button->setObjectName(QStringLiteral("workflowCardButton"));
        button->setIcon(glyphIcon(glyph, color, 16));
        button->setIconSize(QSize(16, 16));
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setAccessibleName(label);
        button->setToolTip(tip.isEmpty() ? label : tip);
        connect(button, &QToolButton::clicked, this, [this, action, button] {
            if (m_action)
                m_action(action, button);
        });
        layout->addWidget(button);
        return button;
    };

    auto *run = new QToolButton;
    run->setObjectName(QStringLiteral("workflowCardRun"));
    run->setCursor(Qt::PointingHandCursor);
    run->setToolButtonStyle(compact ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
    run->setText(running ? QStringLiteral("Stop") : QStringLiteral("Run"));
    run->setAccessibleName(run->text());
    run->setProperty("stopping", running);
    run->setIcon(glyphIcon(running ? QStringLiteral("stop") : QStringLiteral("play"),
                           running ? m_tones.danger : m_tones.accent, 14));
    run->setIconSize(QSize(14, 14));
    run->setToolTip(running ? QStringLiteral("Stop this run")
                            : enabled ? QStringLiteral("Run it now")
                                      : QStringLiteral("Turn it on to run it"));
    run->setEnabled(running || (enabled && !(m_options.busy && !local)));
    connect(run, &QToolButton::clicked, this, [this, run] {
        if (m_action)
            m_action(QStringLiteral("run"), run);
    });
    layout->addWidget(run);
    layout->addSpacing(4);

    tool(QStringLiteral("edit"), QStringLiteral("Edit"), QStringLiteral("edit"), QStringLiteral("Edit its steps"),
         m_tones.muted);
    if (!compact) {
        if (m_options.canPin) {
            const QString rail = text(m_entry, "pinned");
            tool(rail.isEmpty() ? QStringLiteral("pin") : QStringLiteral("pinned"), QStringLiteral("Pin to rail"),
                 QStringLiteral("pin"),
                 rail.isEmpty() ? QStringLiteral("Pin a run button to a rail")
                                : QStringLiteral("Pinned to the %1 rail").arg(rail),
                 rail.isEmpty() ? m_tones.muted : m_tones.accent);
        }
        if (local && m_entry.value(QStringLiteral("hasSchedule")).toBool()) {
            const bool active = m_entry.value(QStringLiteral("scheduleActive")).toBool();
            tool(active ? QStringLiteral("pause") : QStringLiteral("clock"),
                 active ? QStringLiteral("Pause schedule") : QStringLiteral("Start schedule"),
                 QStringLiteral("schedule"),
                 active ? QStringLiteral("Pause its schedule: %1").arg(text(m_entry, "trigger"))
                        : QStringLiteral("Start its schedule: %1").arg(text(m_entry, "trigger")),
                 active ? m_tones.muted : m_tones.accent);
        }
        auto *output = tool(QStringLiteral("output"), QStringLiteral("Last output"), QStringLiteral("output"),
                            QStringLiteral("Show the most recent output"), m_tones.muted);
        output->setEnabled(!text(m_entry, "lastRunId").isEmpty() || !text(m_entry, "lastOutputFile").isEmpty());
    }
    layout->addStretch(1);
    auto *toggle = new WorkflowSwitch(m_tones);
    toggle->setChecked(enabled);
    toggle->setAccessibleName(enabled ? QStringLiteral("Turn off") : QStringLiteral("Turn on"));
    toggle->setToolTip(enabled ? QStringLiteral("On: it runs by hand and on its schedule. Click to turn it off.")
                               : QStringLiteral("Off: it never runs. Click to turn it on."));
    toggle->setEnabled(!(m_options.busy && !local));
    connect(toggle, &QAbstractButton::clicked, this, [this, toggle] {
        toggle->setChecked(!toggle->isChecked());
        if (m_action)
            m_action(QStringLiteral("toggle"), toggle);
    });
    layout->addWidget(toggle);
    tool(QStringLiteral("more"), QStringLiteral("More actions"), QStringLiteral("menu"), QString(), m_tones.muted);
    return bar;
}

void WorkflowCard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressAt = event->position().toPoint();
        m_pressed = true;
        if (m_action)
            m_action(QStringLiteral("select"), this);
    }
    QFrame::mousePressEvent(event);
}

void WorkflowCard::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_pressed || !(event->buttons() & Qt::LeftButton) ||
        (event->position().toPoint() - m_pressAt).manhattanLength() < QApplication::startDragDistance()) {
        QFrame::mouseMoveEvent(event);
        return;
    }
    m_pressed = false;
    const QString repo = text(m_entry, "repo");
    const QString id = text(m_entry, "id");
    if (repo.isEmpty() || id.isEmpty() || !m_options.canPin)
        return;
    auto *mime = new QMimeData;
    mime->setData(QLatin1String(kWorkflowMime),
                  QJsonDocument(QJsonObject{{QStringLiteral("repo"), repo}, {QStringLiteral("id"), id}})
                      .toJson(QJsonDocument::Compact));
    mime->setData(QLatin1String(kWorkflowPinMime), QStringLiteral("%1\n%2").arg(repo, id).toUtf8());
    mime->setText(text(m_entry, "name"));
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    const QPixmap shot = grab();
    drag->setPixmap(shot.scaledToWidth(std::min(shot.width(), 220), Qt::SmoothTransformation));
    drag->exec(Qt::CopyAction | Qt::MoveAction);
}

void WorkflowCard::mouseDoubleClickEvent(QMouseEvent *event)
{
    m_pressed = false;
    if (event->button() == Qt::LeftButton && m_action)
        m_action(QStringLiteral("edit"), this);
}

void WorkflowCard::contextMenuEvent(QContextMenuEvent *event)
{
    if (m_action) {
        m_action(QStringLiteral("select"), this);
        m_action(QStringLiteral("menu-at"), this);
    }
    event->accept();
}

CardGrid::CardGrid(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("workflowsCollectionBody"));
    m_layout = new QGridLayout(this);
    m_layout->setContentsMargins(0, 2, 2, 6);
    m_layout->setHorizontalSpacing(12);
    m_layout->setVerticalSpacing(12);
}

void CardGrid::setCards(const QList<QWidget *> &cards, ViewMode mode, QWidget *header)
{
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    for (int row = 0; row < m_layout->rowCount(); ++row)
        m_layout->setRowStretch(row, 0);
    for (int column = 0; column < m_layout->columnCount(); ++column)
        m_layout->setColumnStretch(column, 0);
    m_cards.clear();
    for (QWidget *card : cards)
        m_cards.append(card);
    m_header = header;
    m_mode = mode;
    m_layout->setVerticalSpacing(mode == ViewMode::List ? 6 : 12);
    relayout(true);
}

QList<WorkflowCard *> CardGrid::cards() const
{
    QList<WorkflowCard *> list;
    for (const QPointer<QWidget> &card : m_cards)
        if (auto *found = dynamic_cast<WorkflowCard *>(card.data()))
            list.append(found);
    return list;
}

int CardGrid::columnsFor(int width) const
{
    if (m_mode == ViewMode::List)
        return 1;
    const int least = m_mode == ViewMode::Compact ? 210 : 270;
    return std::max(1, (width + m_layout->horizontalSpacing()) / (least + m_layout->horizontalSpacing()));
}

void CardGrid::relayout(bool force)
{
    const int columns = columnsFor(width() > 0 ? width() : 1000);
    if (!force && columns == m_columns)
        return;
    m_columns = columns;
    for (const QPointer<QWidget> &card : std::as_const(m_cards))
        if (card)
            m_layout->removeWidget(card);
    if (m_header)
        m_layout->removeWidget(m_header);
    for (int row = 0; row < m_layout->rowCount(); ++row)
        m_layout->setRowStretch(row, 0);
    for (int column = 0; column < m_layout->columnCount(); ++column)
        m_layout->setColumnStretch(column, 0);
    int offset = 0;
    if (m_header) {
        m_layout->addWidget(m_header, 0, 0, 1, columns);
        offset = 1;
    }
    int index = 0;
    for (const QPointer<QWidget> &card : std::as_const(m_cards)) {
        if (!card)
            continue;
        m_layout->addWidget(card, offset + index / columns, index % columns, Qt::AlignTop);
        ++index;
    }
    for (int column = 0; column < columns; ++column)
        m_layout->setColumnStretch(column, 1);
    m_layout->setRowStretch(offset + (index + columns - 1) / columns, 1);
}

void CardGrid::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout(false);
}

namespace {

constexpr int kStepPad = 8;
constexpr int kStepBadge = 22;

QFont stepSmallFont(QFont font)
{
    font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1.0));
    return font;
}

QFont stepNameFont(QFont font)
{
    font.setWeight(QFont::DemiBold);
    return font;
}

} // namespace

StepDelegate::StepDelegate(const Tones *tones, QObject *parent) : QStyledItemDelegate(parent), m_tones(tones) {}

QSize StepDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const QFontMetrics name(stepNameFont(option.font));
    const QFontMetrics small(stepSmallFont(option.font));
    int height = kStepPad * 2 + name.height() + 1 + small.height();
    if (!index.data(StepDetailRole).toString().isEmpty())
        height += small.height();
    return {200, std::max(height, kStepBadge + kStepPad * 2)};
}

void StepDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const Tones &tones = *m_tones;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    const QRect rect = option.rect;
    const QRectF card = QRectF(rect).adjusted(1.5, 1.5, -1.5, -1.5);
    const bool selected = option.state.testFlag(QStyle::State_Selected);
    const bool hovered = option.state.testFlag(QStyle::State_MouseOver);
    if (selected) {
        painter->setPen(QPen(withAlpha(tones.accent, 0.45), 1));
        painter->setBrush(withAlpha(tones.accent, tones.dark ? 0.2 : 0.09));
        painter->drawRoundedRect(card, 8, 8);
    } else if (hovered) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(tones.hover);
        painter->drawRoundedRect(card, 8, 8);
    }

    const QString kind = index.data(StepKindRole).toString();
    const bool off = index.data(StepOffRole).toBool();
    QColor tone = kind == QLatin1String("trigger") ? tones.accent
                  : kind == QLatin1String("agent") ? tones.agent
                  : kind == QLatin1String("desktop") ? tones.account
                                                     : tones.secondary;
    if (off)
        tone = tones.muted;
    const QRectF badge(rect.left() + kStepPad + 2, rect.top() + kStepPad, kStepBadge, kStepBadge);
    const qreal spine = badge.center().x();
    painter->setPen(QPen(tones.border, 1.5));
    if (index.row() > 0)
        painter->drawLine(QPointF(spine, rect.top()), QPointF(spine, badge.top() - 3));
    if (index.row() + 1 < index.model()->rowCount(index.parent()))
        painter->drawLine(QPointF(spine, badge.bottom() + 3), QPointF(spine, rect.bottom() + 1));
    painter->setPen(off ? QPen(tones.border, 1, Qt::DashLine) : QPen(withAlpha(tone, 0.5), 1));
    painter->setBrush(withAlpha(tone, tones.dark ? 0.22 : 0.12));
    painter->drawEllipse(badge);
    QFont badgeFont = stepSmallFont(option.font);
    badgeFont.setWeight(QFont::Bold);
    painter->setFont(badgeFont);
    painter->setPen(tone);
    painter->drawText(badge, Qt::AlignCenter, QString::number(index.row() + 1));

    const QString status = index.data(StepStatusRole).toString();
    const int textLeft = int(badge.right()) + 10;
    const int textRight = rect.right() - kStepPad - (status.isEmpty() ? 0 : 14);
    const int width = std::max(10, textRight - textLeft);
    const QFont nameFont = stepNameFont(option.font);
    const QFont smallFont = stepSmallFont(option.font);
    const QFontMetrics nameMetrics(nameFont);
    const QFontMetrics smallMetrics(smallFont);
    int y = rect.top() + kStepPad;

    painter->setFont(nameFont);
    painter->setPen(off ? tones.muted : tones.ink);
    const QString name = index.data(StepNameRole).toString();
    painter->drawText(QRect(textLeft, y, width, nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                      nameMetrics.elidedText(name, Qt::ElideRight, width));
    if (!status.isEmpty()) {
        const QColor dot = status == QLatin1String("success")   ? tones.success
                           : status == QLatin1String("error")   ? tones.danger
                           : status == QLatin1String("running") ? tones.accent
                                                                : tones.strong;
        painter->setPen(Qt::NoPen);
        painter->setBrush(dot);
        painter->drawEllipse(QPointF(rect.right() - kStepPad - 4, y + nameMetrics.height() / 2.0), 3.5, 3.5);
    }
    y += nameMetrics.height() + 1;

    painter->setFont(smallFont);
    painter->setPen(off ? tones.muted : tones.secondary);
    QString type = index.data(StepTypeRole).toString();
    if (off)
        type = QStringLiteral("Off · ") + type;
    painter->drawText(QRect(textLeft, y, width, smallMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                      smallMetrics.elidedText(type, Qt::ElideRight, width));
    y += smallMetrics.height();

    const QString detail = index.data(StepDetailRole).toString();
    if (!detail.isEmpty()) {
        painter->setPen(index.data(StepWarnRole).toBool() ? tones.attention : tones.muted);
        painter->drawText(QRect(textLeft, y, width, smallMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          smallMetrics.elidedText(detail, Qt::ElideRight, width));
    }
    painter->restore();
}

} // namespace forkmesh::workflows
