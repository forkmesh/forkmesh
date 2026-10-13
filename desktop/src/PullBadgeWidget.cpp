#include "PullBadgeWidget.h"

#include <QEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QToolTip>

#include <algorithm>

namespace {
constexpr int kMargin = 18;
constexpr int kHeaderH = 92;
constexpr int kTile = 44;
constexpr int kTilePitch = kTile + 12;
constexpr int kBarH = 5;
constexpr int kRowPitch = kTile + 46; // tile + bar + line + label + gap

const QColor kGreen(0x3f, 0xb9, 0x50);
const QColor kRed(0xf8, 0x51, 0x49);
const QColor kPurple(0xa3, 0x71, 0xf7);
const QColor kMuted(0x8b, 0x94, 0x9e);

QString dirBadgeLabel(const QString &dir)
{
    QString name = dir == QStringLiteral("/") ? dir : dir.section('/', -1);
    if (name.length() > 8)
        name = name.left(8) + QChar(0x2026);
    return name;
}
} // namespace

PullBadgeWidget::PullBadgeWidget(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
}

void PullBadgeWidget::setPull(const QString &title, int number,
                              const QString &author, int additions,
                              int deletions, const QList<FileEntry> &files)
{
    m_title = title;
    m_number = number;
    m_author = author;
    m_additions = additions;
    m_deletions = deletions;
    m_files = files;
    std::sort(m_files.begin(), m_files.end(),
              [](const FileEntry &a, const FileEntry &b) {
                  return a.path.toLower() < b.path.toLower();
              });
    relayout();
    update();
}

void PullBadgeWidget::clearPull()
{
    setPull(QString(), 0, QString(), 0, 0, {});
}

void PullBadgeWidget::relayout()
{
    m_tiles.clear();
    m_segments.clear();
    const int w = qMax(width(), 320);
    const int cols =
        qMax(1, (w - 2 * kMargin + (kTilePitch - kTile)) / kTilePitch);
    int row = 0, col = 0;
    int index = 0;
    while (index < m_files.size()) {
        const QString path = m_files.at(index).path;
        const QString dir =
            path.contains('/') ? path.section('/', 0, -2) : QStringLiteral("/");
        int count = 0;
        while (index + count < m_files.size()) {
            const QString p = m_files.at(index + count).path;
            const QString d =
                p.contains('/') ? p.section('/', 0, -2) : QStringLiteral("/");
            if (d != dir)
                break;
            ++count;
        }
        if (col && col + count > cols && count <= cols) {
            ++row;
            col = 0;
        }
        int segStartCol = col, segRow = row;
        const auto flushSegment = [&](int lastCol, bool isLast) {
            const int x1 = kMargin + segStartCol * kTilePitch;
            const int x2 = kMargin + lastCol * kTilePitch + kTile;
            const int lineY = kHeaderH + segRow * kRowPitch + kTile + 16;
            GroupSegment seg;
            seg.lineRect = QRect(x1, lineY, x2 - x1, 1);
            seg.labeled = isLast;
            if (isLast)
                seg.label = QStringLiteral("%1  (%2 file%3)")
                                .arg(dirBadgeLabel(dir))
                                .arg(count)
                                .arg(count == 1 ? QString()
                                                : QStringLiteral("s"));
            m_segments.append(seg);
        };
        for (int i = 0; i < count; ++i) {
            if (col >= cols) {
                flushSegment(cols - 1, false);
                ++row;
                col = 0;
                segStartCol = 0;
                segRow = row;
            }
            Tile tile;
            tile.fileIndex = index + i;
            tile.rect = QRect(kMargin + col * kTilePitch,
                              kHeaderH + row * kRowPitch, kTile, kTile);
            m_tiles.append(tile);
            ++col;
        }
        flushSegment(col - 1, true);
        index += count;
    }
    m_contentHeight =
        m_files.isEmpty() ? kHeaderH : kHeaderH + (row + 1) * kRowPitch + kMargin;
    setMinimumHeight(m_contentHeight);
}

void PullBadgeWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (event->oldSize().width() != width())
        relayout();
}

void PullBadgeWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor fg = palette().color(QPalette::WindowText);
    QColor line = fg;
    line.setAlpha(60);
    QColor tileBg = fg;
    tileBg.setAlpha(16);

    if (m_files.isEmpty()) {
        p.setPen(kMuted);
        p.drawText(rect().adjusted(kMargin, 0, -kMargin, 0), Qt::AlignCenter,
                   tr("No changed files to fingerprint."));
        return;
    }

    QFont titleFont = font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(font().pointSizeF() + 3);
    QFont statFont = titleFont;
    statFont.setPointSizeF(font().pointSizeF() + 2);
    QFont smallFont = font();
    smallFont.setPointSizeF(qMax(7.0, font().pointSizeF() - 1.5));

    const QFontMetrics statFm(statFont);
    const QFontMetrics smallFm(smallFont);
    const int colW =
        std::max({statFm.horizontalAdvance(
                      QStringLiteral("+%1").arg(m_additions)),
                  smallFm.horizontalAdvance(tr("Files changed")), 70}) +
        18;
    struct Stat {
        QString value;
        QString caption;
        QColor color;
    };
    const Stat stats[] = {
        {QStringLiteral("+%1").arg(m_additions), tr("Additions"), kGreen},
        {QStringLiteral("-%1").arg(m_deletions), tr("Deletions"), kRed},
        {QString::number(m_files.size()), tr("Files changed"), fg}};
    int statX = width() - kMargin - 3 * colW;
    for (const Stat &s : stats) {
        p.setFont(statFont);
        p.setPen(s.color);
        p.drawText(QRect(statX, 14, colW - 12, statFm.height()),
                   Qt::AlignRight | Qt::AlignVCenter, s.value);
        p.setFont(smallFont);
        p.setPen(kMuted);
        p.drawText(QRect(statX, 16 + statFm.height(), colW - 12,
                         smallFm.height()),
                   Qt::AlignRight | Qt::AlignVCenter, s.caption);
        statX += colW;
    }

    const int titleW = width() - 2 * kMargin - 3 * colW - 12;
    p.setFont(titleFont);
    p.setPen(fg);
    const QFontMetrics titleFm(titleFont);
    p.drawText(QRect(kMargin, 14, titleW, titleFm.height()),
               Qt::AlignLeft | Qt::AlignVCenter,
               titleFm.elidedText(m_title, Qt::ElideRight, titleW));
    p.setFont(font());
    p.setPen(kPurple);
    const QString numberLabel = m_number
        ? QStringLiteral("#%1").arg(m_number) : tr("pull request");
    const QString byline = m_author.isEmpty()
        ? numberLabel
        : QStringLiteral("%1 · %2 %3").arg(numberLabel, tr("by"), m_author);
    p.drawText(QRect(kMargin, 18 + titleFm.height(), titleW,
                     QFontMetrics(font()).height()),
               Qt::AlignLeft | Qt::AlignVCenter, byline);

    for (const Tile &tile : std::as_const(m_tiles)) {
        const FileEntry &file = m_files.at(tile.fileIndex);
        QPainterPath rr;
        rr.addRoundedRect(tile.rect, 7, 7);
        p.fillPath(rr, tileBg);
        p.setPen(QPen(line, 1));
        p.drawPath(rr);
        file.icon.paint(&p, tile.rect.adjusted(9, 9, -9, -9));

        const QRect barRect(tile.rect.left(), tile.rect.bottom() + 4,
                            tile.rect.width(), kBarH);
        const int total = file.adds + file.dels;
        QPainterPath barPath;
        barPath.addRoundedRect(barRect, kBarH / 2.0, kBarH / 2.0);
        p.setClipPath(barPath);
        if (total > 0) {
            const int greenW =
                qRound(barRect.width() * double(file.adds) / total);
            if (greenW > 0)
                p.fillRect(QRect(barRect.left(), barRect.top(), greenW,
                                 kBarH), kGreen);
            if (greenW < barRect.width())
                p.fillRect(QRect(barRect.left() + greenW, barRect.top(),
                                 barRect.width() - greenW, kBarH), kRed);
        } else {
            p.fillRect(barRect, line); // binary / no counted lines
        }
        p.setClipping(false);
    }

    p.setFont(smallFont);
    for (const GroupSegment &seg : std::as_const(m_segments)) {
        p.setPen(QPen(line, 1));
        p.drawLine(seg.lineRect.topLeft(), seg.lineRect.topRight());
        p.setPen(Qt::NoPen);
        p.setBrush(line);
        p.drawEllipse(seg.lineRect.topLeft(), 2, 2);
        p.drawEllipse(seg.lineRect.topRight(), 2, 2);
        p.setBrush(Qt::NoBrush);
        if (!seg.labeled)
            continue;
        p.setPen(kMuted);
        const QRect labelRect(seg.lineRect.left() - 26,
                              seg.lineRect.top() + 4,
                              seg.lineRect.width() + 52, smallFm.height() + 2);
        p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignVCenter,
                   smallFm.elidedText(seg.label, Qt::ElideMiddle,
                                      labelRect.width()));
    }
}

bool PullBadgeWidget::event(QEvent *e)
{
    if (e->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(e);
        for (const Tile &tile : std::as_const(m_tiles)) {
            if (tile.rect.adjusted(0, 0, 0, kBarH + 4).contains(help->pos())) {
                const FileEntry &file = m_files.at(tile.fileIndex);
                QToolTip::showText(
                    help->globalPos(),
                    QStringLiteral("%1  +%2 −%3")
                        .arg(file.path)
                        .arg(file.adds)
                        .arg(file.dels),
                    this);
                return true;
            }
        }
        QToolTip::hideText();
    }
    return QWidget::event(e);
}
