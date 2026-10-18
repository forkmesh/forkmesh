#include "RssReaderFeature.h"
#include "RssReaderFetch.h"

#include "AppPaths.h"

#include <QAbstractListModel>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonDocument>
#include <QListWidget>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStringDecoder>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace forkmesh::rssreader {
namespace {

constexpr int KindRole = Qt::UserRole + 1;
constexpr int IdRole = Qt::UserRole + 2;
constexpr int DepthRole = Qt::UserRole + 3;
enum ItemKind { SectionItem = 1, SmartItem, FolderItem, FeedItem };

constexpr qint64 kMaxImageBytes = qint64(5) * 1024 * 1024;
constexpr qint64 kMaxIconBytes = qint64(1) * 1024 * 1024;
constexpr int kIconSide = 64;
constexpr int kMinIconSide = 8;
constexpr int kMaxCachedImages = 300;
constexpr int kThumbnailSide = 56;
const QColor kStarColor(0xd4, 0xa7, 0x2c);
const QColor kWarningColor(0xd2, 0x99, 0x22);

qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QLocale english()
{
    return QLocale(QLocale::English, QLocale::UnitedStates);
}

QString articleDateText(qint64 ms)
{
    return english()
        .toString(QDateTime::fromMSecsSinceEpoch(ms), QStringLiteral("MMM d, yyyy 'at' h:mm AP"))
        .toUpper();
}

QString timelineDateText(qint64 ms, qint64 reference)
{
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(ms);
    const QDateTime current = QDateTime::fromMSecsSinceEpoch(reference);
    if (when.date() == current.date())
        return english().toString(when.time(), QStringLiteral("h:mm AP"));
    if (when.date().year() == current.date().year())
        return english().toString(when.date(), QStringLiteral("MMM d"));
    return english().toString(when.date(), QStringLiteral("MMM d, yyyy"));
}

QString whenText(qint64 ms)
{
    if (ms <= 0)
        return QStringLiteral("Never");
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(ms), QLocale::ShortFormat);
}

QPixmap tintedPixmap(const QString &name, const QColor &color, int side, qreal ratio)
{
    QString path = QStringLiteral(":/icons/octicons/%1.svg").arg(name);
    if (!QFile::exists(path))
        path = QStringLiteral(":/icons/codicons/%1.svg").arg(name);
    if (!QFile::exists(path))
        return QPixmap();
    const QIcon source(path);
    QPixmap pixmap = source.pixmap(QSize(side, side), ratio);
    if (pixmap.isNull())
        return pixmap;
    QPainter painter(&pixmap);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(QRect(QPoint(0, 0), pixmap.size()), color);
    return pixmap;
}

QIcon tintedIcon(const QString &name, const QColor &color, int side = 16)
{
    QIcon icon;
    for (const qreal ratio : {1.0, 2.0}) {
        const QPixmap pixmap = tintedPixmap(name, color, side, ratio);
        if (!pixmap.isNull())
            icon.addPixmap(pixmap);
    }
    return icon;
}

void drawChevron(QPainter *painter, const QPointF &center, bool open, const QColor &color)
{
    QPainterPath path;
    if (open) {
        path.moveTo(center + QPointF(-3.5, -1.5));
        path.lineTo(center + QPointF(0, 2));
        path.lineTo(center + QPointF(3.5, -1.5));
    } else {
        path.moveTo(center + QPointF(-1.5, -3.5));
        path.lineTo(center + QPointF(2, 0));
        path.lineTo(center + QPointF(-1.5, 3.5));
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPath(path);
    painter->restore();
}

void drawStar(QPainter *painter, const QPointF &center, qreal radius, const QColor &color)
{
    QPolygonF star;
    for (int i = 0; i < 10; ++i) {
        const qreal r = i % 2 == 0 ? radius : radius * 0.45;
        const qreal angle = -M_PI / 2 + i * M_PI / 5;
        star << center + QPointF(r * std::cos(angle), r * std::sin(angle));
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color);
    painter->drawPolygon(star);
    painter->restore();
}

QFont pixelFont(const QFont &base, int pixels, bool bold = false)
{
    QFont font = base;
    font.setPixelSize(pixels);
    font.setBold(bold);
    return font;
}

QString decodeHtml(const QByteArray &body, const QString &contentType)
{
    static const QRegularExpression headerCharset(QStringLiteral("charset=\"?([A-Za-z0-9_.:-]+)"),
                                                  QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression metaCharset(
        QStringLiteral("<meta[^>]+charset=[\"']?([A-Za-z0-9_.:-]+)"),
        QRegularExpression::CaseInsensitiveOption);
    QString charset = headerCharset.match(contentType).captured(1);
    if (charset.isEmpty())
        charset = metaCharset.match(QString::fromLatin1(body.left(4096))).captured(1);
    const QString lower = charset.toLower();
    if (lower == QLatin1String("windows-1252") || lower == QLatin1String("iso-8859-1") ||
        lower == QLatin1String("latin1"))
        return QString::fromLatin1(body);
    if (!charset.isEmpty() && lower != QLatin1String("utf-8")) {
        QStringDecoder decoder(charset.toLatin1().constData());
        if (decoder.isValid())
            return decoder.decode(body);
    }
    return QString::fromUtf8(body);
}

QString attributeIn(const QString &tag, const QString &name)
{
    const QRegularExpression pattern(QStringLiteral("\\b%1=\"([^\"]*)\"").arg(name));
    return pattern.match(tag).captured(1);
}

QString originOf(const QUrl &url)
{
    if (!url.isValid() || url.host().isEmpty())
        return {};
    QUrl origin;
    origin.setScheme(url.scheme());
    origin.setHost(url.host());
    origin.setPort(url.port());
    return origin.toString();
}

bool svgIconsSupported()
{
    static const bool supported = QImageReader::supportedImageFormats().contains(QByteArrayLiteral("svg"));
    return supported;
}

int iconFramePenalty(const QImage &image)
{
    const int side = qMax(image.width(), image.height());
    return side >= kIconSide ? side - kIconSide : (kIconSide - side) * 2;
}

QImage decodeIcon(const QByteArray &bytes)
{
    QBuffer buffer;
    buffer.setData(bytes);
    if (!buffer.open(QIODevice::ReadOnly))
        return {};
    QImageReader reader(&buffer);
    reader.setDecideFormatFromContent(true);
    const QByteArray format = reader.format();
    if (format.isEmpty())
        return {};
    if (format.startsWith("svg")) {
        const QSize size = reader.size();
        if (size.isValid() && !size.isEmpty())
            reader.setScaledSize(size.scaled(QSize(kIconSide, kIconSide), Qt::KeepAspectRatio));
        else
            reader.setScaledSize(QSize(kIconSide, kIconSide));
    }
    QImage best;
    const int count = qBound(1, reader.imageCount(), 16);
    for (int index = 0; index < count; ++index) {
        if (index > 0 && !reader.jumpToImage(index))
            break;
        const QImage frame = reader.read();
        if (!frame.isNull() && (best.isNull() || iconFramePenalty(frame) < iconFramePenalty(best)))
            best = frame;
    }
    return best;
}

bool visibleIcon(const QImage &image)
{
    if (image.isNull() || image.width() < kMinIconSide || image.height() < kMinIconSide)
        return false;
    if (!image.hasAlphaChannel())
        return true;
    const QImage sample = image.scaled(16, 16, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                              .convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < sample.height(); ++y) {
        for (int x = 0; x < sample.width(); ++x) {
            if (qAlpha(sample.pixel(x, y)) > 16)
                return true;
        }
    }
    return false;
}

bool squareEnough(const QImage &image)
{
    const int longer = qMax(image.width(), image.height());
    const int shorter = qMin(image.width(), image.height());
    return shorter > 0 && longer <= shorter * 2;
}

QString sizeLabel(qint64 bytes)
{
    return english().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
}

Source smartSource(const QString &id)
{
    if (id == QLatin1String("today"))
        return {SourceKind::Today, QString()};
    if (id == QLatin1String("starred"))
        return {SourceKind::Starred, QString()};
    if (id == QLatin1String("readlater"))
        return {SourceKind::ReadLater, QString()};
    if (id.startsWith(QStringLiteral("smart:")))
        return {SourceKind::Smart, id.mid(6)};
    return {SourceKind::AllUnread, QString()};
}

QString smartItemId(const Source &source)
{
    switch (source.kind) {
    case SourceKind::Today:
        return QStringLiteral("today");
    case SourceKind::AllUnread:
        return QStringLiteral("unread");
    case SourceKind::Starred:
        return QStringLiteral("starred");
    case SourceKind::ReadLater:
        return QStringLiteral("readlater");
    case SourceKind::Smart:
        return QStringLiteral("smart:") + source.id;
    default:
        return QString();
    }
}

QString filterFieldLabel(const QString &field)
{
    if (field == QLatin1String("titleOrText"))
        return QStringLiteral("Title or Text");
    if (field == QLatin1String("author"))
        return QStringLiteral("Author");
    return QStringLiteral("Title");
}

QString filterActionLabel(const QString &action)
{
    return action == QLatin1String("markRead") ? QStringLiteral("Mark as Read") : QStringLiteral("Hide");
}

QString timeText(qint64 ms)
{
    return english().toString(QDateTime::fromMSecsSinceEpoch(ms).time(), QStringLiteral("h:mm AP"));
}

}

class ArticleBrowser final : public QTextBrowser
{
public:
    using QTextBrowser::QTextBrowser;
    std::function<QVariant(int, const QUrl &)> provider;

    QVariant loadResource(int type, const QUrl &name) override
    {
        return provider ? provider(type, name) : QVariant();
    }
};

struct TimelineRow {
    bool header = false;
    QString feedId;
    ArticleRef ref;
};

class TimelineModel final : public QAbstractListModel
{
public:
    using QAbstractListModel::QAbstractListModel;
    const Store *store = nullptr;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_rows.size());
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size() || !store)
            return {};
        if (role != Qt::DisplayRole && role != Qt::AccessibleTextRole && role != Qt::ToolTipRole)
            return {};
        const TimelineRow &row = m_rows.at(index.row());
        if (row.header) {
            const Feed *owner = store->feed(row.feedId);
            return owner ? owner->displayName() : QString();
        }
        const Article *item = store->article(row.ref);
        if (!item || role == Qt::ToolTipRole)
            return {};
        return item->title.isEmpty() ? item->summary.left(120) : item->title;
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (!index.isValid() || index.row() >= m_rows.size())
            return Qt::NoItemFlags;
        return m_rows.at(index.row()).header ? Qt::ItemIsEnabled
                                             : Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    }

    void setRows(QList<TimelineRow> rows)
    {
        beginResetModel();
        m_rows = std::move(rows);
        endResetModel();
    }

    const TimelineRow *rowAt(int row) const
    {
        return row >= 0 && row < m_rows.size() ? &m_rows.at(row) : nullptr;
    }

    int indexOf(const ArticleRef &ref) const
    {
        if (ref.isNull())
            return -1;
        for (int i = 0; i < m_rows.size(); ++i) {
            if (!m_rows.at(i).header && m_rows.at(i).ref == ref)
                return i;
        }
        return -1;
    }

    void removeAt(int row)
    {
        if (row < 0 || row >= m_rows.size())
            return;
        beginRemoveRows(QModelIndex(), row, row);
        m_rows.removeAt(row);
        endRemoveRows();
    }

    void touch(int row)
    {
        if (row >= 0 && row < m_rows.size())
            emit dataChanged(index(row), index(row));
    }

    void touchAll()
    {
        if (!m_rows.isEmpty())
            emit dataChanged(index(0), index(int(m_rows.size()) - 1));
    }

    const QList<TimelineRow> &rows() const { return m_rows; }

private:
    QList<TimelineRow> m_rows;
};

class SourceDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    std::function<int(const QModelIndex &)> count;
    std::function<QPixmap(const QModelIndex &, int)> icon;
    std::function<QString(const QModelIndex &)> warning;
    std::function<bool(const QModelIndex &)> expanded;
    std::function<bool(const QModelIndex &)> paused;
    std::function<QPixmap(int)> warningIcon;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        Q_UNUSED(index);
        return QSize(option.rect.width(), 28);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        const QPalette &palette = option.palette;
        const QColor text = palette.color(QPalette::Text);
        const QColor muted = palette.color(QPalette::PlaceholderText);
        const QRect rect = option.rect;
        const int kind = index.data(KindRole).toInt();
        const bool isExpanded = expanded && expanded(index);
        if (kind == SectionItem) {
            painter->setFont(pixelFont(option.font, 11, true));
            painter->setPen(muted);
            painter->drawText(rect.adjusted(10, 4, -28, 0), Qt::AlignVCenter | Qt::AlignLeft,
                              index.data().toString());
            drawChevron(painter, QPointF(rect.right() - 14, rect.center().y() + 2), isExpanded, muted);
            painter->restore();
            return;
        }
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            QColor fill = palette.color(QPalette::Highlight);
            if (!selected)
                fill.setAlphaF(0.45);
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(QRectF(rect.adjusted(4, 1, -4, -1)), 6, 6);
        }
        int x = rect.left() + 10 + index.data(DepthRole).toInt() * 18;
        if (kind == FolderItem) {
            drawChevron(painter, QPointF(x + 4, rect.center().y()), isExpanded, muted);
            x += 14;
        }
        if (icon) {
            const QPixmap pixmap = icon(index, 16);
            if (!pixmap.isNull()) {
                const QSizeF size = pixmap.deviceIndependentSize();
                painter->drawPixmap(QPointF(x + (16 - size.width()) / 2,
                                            rect.center().y() - size.height() / 2 + 1),
                                    pixmap);
            }
        }
        x += 24;
        int right = rect.right() - 12;
        const int unread = count ? count(index) : 0;
        if (unread > 0) {
            const QFont countFont = pixelFont(option.font, 12);
            painter->setFont(countFont);
            painter->setPen(muted);
            const QString label = QLocale().toString(unread);
            const int width = QFontMetrics(countFont).horizontalAdvance(label);
            painter->drawText(QRect(right - width, rect.top(), width, rect.height()),
                              Qt::AlignVCenter | Qt::AlignRight, label);
            right -= width + 8;
        }
        if (warning && !warning(index).isEmpty() && warningIcon) {
            const QPixmap alert = warningIcon(14);
            painter->drawPixmap(QPointF(right - 14, rect.center().y() - 6), alert);
            right -= 20;
        }
        const bool isPaused = paused && paused(index);
        if (isPaused) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(muted);
            const qreal top = rect.center().y() - 5;
            painter->drawRoundedRect(QRectF(right - 9, top, 3, 10), 1, 1);
            painter->drawRoundedRect(QRectF(right - 4, top, 3, 10), 1, 1);
            painter->restore();
            right -= 16;
        }
        const QFont nameFont = pixelFont(option.font, 13);
        painter->setFont(nameFont);
        painter->setPen(isPaused ? muted : text);
        const QString name = QFontMetrics(nameFont).elidedText(index.data().toString(),
                                                                Qt::ElideRight, qMax(0, right - x));
        painter->drawText(QRect(x, rect.top(), qMax(0, right - x), rect.height()),
                          Qt::AlignVCenter | Qt::AlignLeft, name);
        painter->restore();
    }
};

class TimelineDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    const Store *store = nullptr;
    TimelineModel *model = nullptr;
    std::function<QPixmap(const QString &, int)> icon;
    std::function<QPixmap(const QString &)> thumbnail;

    int articleHeight(const QFont &base) const
    {
        const int lines = store ? store->settings().summaryLines : 2;
        const int title = QFontMetrics(pixelFont(base, 13, true)).height();
        const int summary = QFontMetrics(pixelFont(base, 12)).height();
        const int meta = QFontMetrics(pixelFont(base, 11)).height();
        return 10 + title + 3 + lines * summary + (lines > 0 ? 4 : 0) + meta + 10;
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const TimelineRow *row = model ? model->rowAt(index.row()) : nullptr;
        if (row && row->header)
            return QSize(200, 30);
        return QSize(200, articleHeight(option.font));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const TimelineRow *row = model ? model->rowAt(index.row()) : nullptr;
        if (!row || !store)
            return;
        painter->save();
        const QPalette &palette = option.palette;
        const QColor text = palette.color(QPalette::Text);
        const QColor muted = palette.color(QPalette::PlaceholderText);
        const QRect rect = option.rect;
        const bool showIcons = store->settings().showFeedIcons;
        if (row->header) {
            const Feed *owner = store->feed(row->feedId);
            int x = rect.left() + 12;
            if (showIcons && icon) {
                const QPixmap pixmap = icon(row->feedId, 16);
                painter->drawPixmap(QPointF(x, rect.center().y() - 7), pixmap);
                x += 22;
            }
            const QFont font = pixelFont(option.font, 12, true);
            painter->setFont(font);
            painter->setPen(muted);
            painter->drawText(QRect(x, rect.top(), rect.right() - x - 8, rect.height()),
                              Qt::AlignVCenter | Qt::AlignLeft,
                              QFontMetrics(font).elidedText(owner ? owner->displayName() : QString(),
                                                            Qt::ElideRight, rect.right() - x - 8));
            QColor line = muted;
            line.setAlphaF(0.25);
            painter->setPen(line);
            painter->drawLine(rect.left() + 8, rect.bottom(), rect.right() - 8, rect.bottom());
            painter->restore();
            return;
        }
        const Article *item = store->article(row->ref);
        if (!item) {
            painter->restore();
            return;
        }
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            QColor fill = palette.color(QPalette::Highlight);
            if (!selected)
                fill.setAlphaF(0.4);
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(fill);
            painter->drawRoundedRect(QRectF(rect.adjusted(4, 1, -4, -1)), 6, 6);
        } else {
            QColor line = muted;
            line.setAlphaF(0.18);
            painter->setPen(line);
            painter->drawLine(rect.left() + 30, rect.bottom(), rect.right() - 10, rect.bottom());
        }
        const QFont titleFont = pixelFont(option.font, 13, !item->read);
        const QFont summaryFont = pixelFont(option.font, 12);
        const QFont metaFont = pixelFont(option.font, 11);
        const QFontMetrics titleMetrics(titleFont);
        const QFontMetrics summaryMetrics(summaryFont);
        const QFontMetrics metaMetrics(metaFont);
        const int top = rect.top() + 10;
        const qreal dotX = rect.left() + 17;
        const qreal titleMid = top + titleMetrics.height() / 2.0;
        if (!item->read) {
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(palette.color(QPalette::Link));
            painter->drawEllipse(QPointF(dotX, titleMid), 4, 4);
        }
        if (item->starred)
            drawStar(painter, QPointF(dotX, item->read ? titleMid : titleMid + 15), 5.5, kStarColor);
        int x = rect.left() + 30;
        if (showIcons && icon) {
            const QPixmap pixmap = icon(item->feedId, 20);
            painter->drawPixmap(QPointF(x, top), pixmap);
            x += 28;
        }
        int trailing = rect.right() - 12;
        const Settings &settings = store->settings();
        if (settings.showThumbnails && settings.loadRemoteImages && !item->image.isEmpty() && thumbnail) {
            const QPixmap pixmap = thumbnail(item->image);
            if (!pixmap.isNull()) {
                const int side = qMax(16, qMin(kThumbnailSide, rect.height() - 16));
                const QRectF target(rect.right() - 10 - side, rect.top() + (rect.height() - side) / 2.0, side, side);
                QPainterPath clip;
                clip.addRoundedRect(target, 6, 6);
                painter->save();
                painter->setRenderHint(QPainter::Antialiasing);
                painter->setRenderHint(QPainter::SmoothPixmapTransform);
                painter->setClipPath(clip);
                painter->drawPixmap(target, pixmap, QRectF(QPointF(0, 0), QSizeF(pixmap.size())));
                painter->restore();
                trailing = int(target.left()) - 10;
            }
        }
        const int width = qMax(0, trailing - x);
        QString title = item->title;
        if (title.isEmpty())
            title = item->summary.isEmpty() ? QStringLiteral("Untitled") : item->summary.left(140);
        painter->setFont(titleFont);
        painter->setPen(text);
        painter->drawText(QRect(x, top, width, titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          titleMetrics.elidedText(title, Qt::ElideRight, width));
        int y = top + titleMetrics.height() + 3;
        const int lines = store->settings().summaryLines;
        if (lines > 0 && !item->summary.isEmpty() && !item->title.isEmpty()) {
            painter->setFont(summaryFont);
            painter->setPen(muted);
            QTextLayout layout(item->summary, summaryFont);
            layout.beginLayout();
            for (int line = 0; line < lines; ++line) {
                QTextLine textLine = layout.createLine();
                if (!textLine.isValid())
                    break;
                textLine.setLineWidth(width);
                const QString part = item->summary.mid(textLine.textStart(),
                                                       line == lines - 1 ? -1 : textLine.textLength());
                const QString shown = summaryMetrics.elidedText(part.trimmed(), Qt::ElideRight, width);
                painter->drawText(QRect(x, y, width, summaryMetrics.height()),
                                  Qt::AlignLeft | Qt::AlignVCenter, shown);
                y += summaryMetrics.height();
            }
            layout.endLayout();
        }
        const Feed *owner = store->feed(item->feedId);
        QString meta = owner ? owner->displayName() : QString();
        if (!item->author.isEmpty())
            meta += QStringLiteral(" · ") + item->author;
        const QString date = timelineDateText(item->publishedMs, now());
        painter->setFont(metaFont);
        painter->setPen(muted);
        const int metaTop = rect.bottom() - 9 - metaMetrics.height();
        const int dateWidth = metaMetrics.horizontalAdvance(date);
        painter->drawText(QRect(trailing - dateWidth, metaTop, dateWidth, metaMetrics.height()),
                          Qt::AlignRight | Qt::AlignVCenter, date);
        const int metaWidth = qMax(0, width - dateWidth - 10);
        painter->drawText(QRect(x, metaTop, metaWidth, metaMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          metaMetrics.elidedText(meta, Qt::ElideRight, metaWidth));
        painter->restore();
    }
};

RssReaderFeature::RssReaderFeature(const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_host(host)
{
    setObjectName(QStringLiteral("rssReaderSection"));
    m_disk = std::make_unique<DiskStore>(dataDirectory());
    m_feedQueue = std::make_unique<FetchQueue>(host.network, kMaxConcurrentFetches);
    m_assetQueue = std::make_unique<FetchQueue>(host.network, 4);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    m_splitter = new QSplitter(Qt::Horizontal);
    m_splitter->setObjectName(QStringLiteral("rssReaderSplitter"));
    m_splitter->setChildrenCollapsible(false);
    m_splitter->addWidget(buildSourcesPane());
    m_splitter->addWidget(buildTimelinePane());
    m_splitter->addWidget(buildArticlePane());
    m_splitter->setStretchFactor(0, 0);
    m_splitter->setStretchFactor(1, 0);
    m_splitter->setStretchFactor(2, 1);
    m_splitter->setSizes({240, 380, 620});
    outer->addWidget(m_splitter);

    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(800);
    connect(m_saveTimer, &QTimer::timeout, this, [this] { saveNow(); });
    m_renderTimer = new QTimer(this);
    m_renderTimer->setSingleShot(true);
    m_renderTimer->setInterval(80);
    connect(m_renderTimer, &QTimer::timeout, this, [this] { showArticle(); });
    m_countsTimer = new QTimer(this);
    m_countsTimer->setSingleShot(true);
    m_countsTimer->setInterval(250);
    connect(m_countsTimer, &QTimer::timeout, this, [this] { updateCounts(); });

    refreshIcons();
    updateStatus();
    updateTimelineHeader();
}

RssReaderFeature::~RssReaderFeature()
{
    saveNow();
    m_feedQueue.reset();
    m_assetQueue.reset();
}

QString RssReaderFeature::dataDirectory() const
{
    return QDir(forkmesh::paths::localDataRoot()).filePath(QStringLiteral("rss-reader"));
}

QToolButton *RssReaderFeature::toolButton(const QString &icon, const QString &tooltip, bool checkable)
{
    auto *button = new QToolButton;
    button->setObjectName(QStringLiteral("iconButton"));
    button->setProperty("rssIcon", icon);
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    button->setAutoRaise(true);
    button->setCheckable(checkable);
    button->setIconSize(QSize(16, 16));
    button->setFixedSize(28, 28);
    button->setFocusPolicy(Qt::NoFocus);
    if (checkable)
        connect(button, &QToolButton::toggled, this, [this] { refreshIcons(); });
    return button;
}

QWidget *RssReaderFeature::buildSourcesPane()
{
    auto *pane = new QWidget;
    pane->setObjectName(QStringLiteral("rssSourcesPane"));
    pane->setMinimumWidth(180);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(10, 14, 4, 8);
    layout->setSpacing(8);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(6, 0, 4, 0);
    header->setSpacing(2);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(1);
    auto *title = new QLabel(QStringLiteral("Feeds"));
    title->setObjectName(QStringLiteral("rssPaneTitle"));
    title->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600; background: transparent;"));
    titles->addWidget(title);
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("statusLine"));
    m_status->setStyleSheet(QStringLiteral("font-size: 11px; background: transparent;"));
    titles->addWidget(m_status);
    header->addLayout(titles, 1);
    m_refreshButton = toolButton(QStringLiteral("sync"), QStringLiteral("Refresh (Shift+R)"));
    m_unreadFeedsButton = toolButton(QStringLiteral("filter"), QStringLiteral("Show only feeds with unread articles"), true);
    m_settingsButton = toolButton(QStringLiteral("gear"), QStringLiteral("Settings"));
    m_addButton = toolButton(QStringLiteral("plus"), QStringLiteral("Add a feed"));
    m_sourcesMenuButton = toolButton(QStringLiteral("kebab-horizontal"), QStringLiteral("More"));
    for (QToolButton *button : {m_refreshButton, m_unreadFeedsButton, m_settingsButton, m_addButton, m_sourcesMenuButton})
        header->addWidget(button, 0, Qt::AlignTop);
    layout->addLayout(header);

    m_feedSearchField = new QLineEdit;
    m_feedSearchField->setObjectName(QStringLiteral("rssFeedSearch"));
    m_feedSearchField->setPlaceholderText(QStringLiteral("Find a Feed"));
    m_feedSearchField->setAccessibleName(QStringLiteral("Find a Feed"));
    m_feedSearchField->setClearButtonEnabled(true);
    m_feedSearchField->setFixedHeight(28);
    auto *feedSearchRow = new QHBoxLayout;
    feedSearchRow->setContentsMargins(4, 0, 6, 0);
    feedSearchRow->addWidget(m_feedSearchField);
    layout->addLayout(feedSearchRow);

    m_sources = new QTreeWidget;
    m_sources->setObjectName(QStringLiteral("rssSources"));
    m_sources->setHeaderHidden(true);
    m_sources->setColumnCount(1);
    m_sources->setIndentation(0);
    m_sources->setRootIsDecorated(false);
    m_sources->setUniformRowHeights(true);
    m_sources->setExpandsOnDoubleClick(false);
    m_sources->setFrameShape(QFrame::NoFrame);
    m_sources->setMouseTracking(true);
    m_sources->setContextMenuPolicy(Qt::CustomContextMenu);
    m_sources->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sources->setStyleSheet(QStringLiteral(
        "QTreeWidget#rssSources { background: transparent; border: none; outline: none; }"
        "QTreeWidget#rssSources::item, QTreeWidget#rssSources::item:selected,"
        "QTreeWidget#rssSources::item:hover, QTreeWidget#rssSources::branch"
        " { background: transparent; border: none; }"));
    m_sourceDelegate = new SourceDelegate(m_sources);
    m_sourceDelegate->count = [this](const QModelIndex &index) {
        const int kind = index.data(KindRole).toInt();
        const QString id = index.data(IdRole).toString();
        if (kind == SmartItem)
            return m_counts.value(id);
        if (kind == FolderItem)
            return m_counts.value(QStringLiteral("d:") + id);
        if (kind == FeedItem)
            return m_counts.value(QStringLiteral("f:") + id);
        return 0;
    };
    m_sourceDelegate->icon = [this](const QModelIndex &index, int side) {
        const int kind = index.data(KindRole).toInt();
        const QString id = index.data(IdRole).toString();
        const QColor muted = palette().color(QPalette::PlaceholderText);
        if (kind == FeedItem)
            return feedIcon(id, side);
        if (kind == FolderItem)
            return tintedPixmap(QStringLiteral("file-directory"), muted, side, devicePixelRatioF());
        const QString name = id == QLatin1String("today")             ? QStringLiteral("sun")
                             : id == QLatin1String("starred")         ? QStringLiteral("star")
                             : id == QLatin1String("readlater")       ? QStringLiteral("clock")
                             : id.startsWith(QStringLiteral("smart:")) ? QStringLiteral("filter")
                                                                       : QStringLiteral("issue-opened");
        return tintedPixmap(name, muted, side, devicePixelRatioF());
    };
    m_sourceDelegate->warning = [this](const QModelIndex &index) {
        if (index.data(KindRole).toInt() != FeedItem)
            return QString();
        const Feed *owner = m_store.feed(index.data(IdRole).toString());
        return owner && owner->failures > 0 ? owner->lastError : QString();
    };
    m_sourceDelegate->warningIcon = [this](int side) {
        return tintedPixmap(QStringLiteral("alert"), kWarningColor, side, devicePixelRatioF());
    };
    m_sourceDelegate->expanded = [this](const QModelIndex &index) { return m_sources->isExpanded(index); };
    m_sourceDelegate->paused = [this](const QModelIndex &index) {
        if (index.data(KindRole).toInt() != FeedItem)
            return false;
        const Feed *owner = m_store.feed(index.data(IdRole).toString());
        return owner && owner->paused;
    };
    m_sources->setItemDelegate(m_sourceDelegate);
    m_sources->viewport()->installEventFilter(this);
    m_sources->installEventFilter(this);
    layout->addWidget(m_sources, 1);

    connect(m_refreshButton, &QToolButton::clicked, this, [this] { checkFeeds(true); });
    connect(m_unreadFeedsButton, &QToolButton::toggled, this, [this](bool on) {
        if (m_store.settings().unreadOnlyFeeds == on)
            return;
        m_store.settings().unreadOnlyFeeds = on;
        settingsChanged(false, false);
        rebuildSources();
        updateCounts();
    });
    connect(m_settingsButton, &QToolButton::clicked, this, [this] { showSettings(); });
    connect(m_addButton, &QToolButton::clicked, this, [this] { showAddFeed(); });
    connect(m_sourcesMenuButton, &QToolButton::clicked, this, [this] { showSourcesMenu(); });
    connect(m_feedSearchField, &QLineEdit::textChanged, this, [this](const QString &text) { setFeedSearch(text); });
    connect(m_sources, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (m_rebuilding || !item)
            return;
        const int kind = item->data(0, KindRole).toInt();
        const QString id = item->data(0, IdRole).toString();
        if (kind == SmartItem) {
            selectSource(smartSource(id));
        } else if (kind == FolderItem) {
            selectSource({SourceKind::Folder, id});
        } else if (kind == FeedItem) {
            selectSource({SourceKind::Feed, id});
        }
    });
    connect(m_sources, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
        if (item && item->data(0, KindRole).toInt() == SectionItem)
            item->setExpanded(!item->isExpanded());
    });
    connect(m_sources, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item) {
        if (item && item->data(0, KindRole).toInt() == FolderItem)
            item->setExpanded(!item->isExpanded());
    });
    auto expansion = [this](QTreeWidgetItem *item, bool open) {
        if (m_rebuilding || !item)
            return;
        const int kind = item->data(0, KindRole).toInt();
        const QString id = item->data(0, IdRole).toString();
        if (kind == SectionItem) {
            if (open)
                m_collapsedSections.remove(id);
            else
                m_collapsedSections.insert(id);
        } else if (kind == FolderItem) {
            m_store.setFolderCollapsed(id, !open);
            scheduleSave();
        }
    };
    connect(m_sources, &QTreeWidget::itemExpanded, this, [expansion](QTreeWidgetItem *item) { expansion(item, true); });
    connect(m_sources, &QTreeWidget::itemCollapsed, this, [expansion](QTreeWidgetItem *item) { expansion(item, false); });
    connect(m_sources, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint &position) { sourceContextMenu(position); });
    return pane;
}

QWidget *RssReaderFeature::buildTimelinePane()
{
    auto *pane = new QWidget;
    pane->setObjectName(QStringLiteral("rssTimelinePane"));
    pane->setMinimumWidth(260);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(6, 14, 4, 8);
    layout->setSpacing(8);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(8, 0, 4, 0);
    header->setSpacing(2);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(1);
    m_timelineTitle = new QLabel;
    m_timelineTitle->setObjectName(QStringLiteral("rssPaneTitle"));
    m_timelineTitle->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 600; background: transparent;"));
    m_timelineTitle->setTextFormat(Qt::PlainText);
    titles->addWidget(m_timelineTitle);
    m_timelineCount = new QLabel;
    m_timelineCount->setObjectName(QStringLiteral("statusLine"));
    m_timelineCount->setStyleSheet(QStringLiteral("font-size: 11px; background: transparent;"));
    titles->addWidget(m_timelineCount);
    header->addLayout(titles, 1);
    m_markAllButton = toolButton(QStringLiteral("check-circle"), QStringLiteral("Mark All as Read (Shift+A)"));
    m_unreadTimelineButton = toolButton(QStringLiteral("eye"), QStringLiteral("Hide Read Articles"), true);
    m_catchUpButton = toolButton(QStringLiteral("history"), QStringLiteral("Mark Older as Read"));
    header->addWidget(m_markAllButton, 0, Qt::AlignTop);
    header->addWidget(m_catchUpButton, 0, Qt::AlignTop);
    header->addWidget(m_unreadTimelineButton, 0, Qt::AlignTop);
    layout->addLayout(header);

    m_searchField = new QLineEdit;
    m_searchField->setObjectName(QStringLiteral("rssSearch"));
    m_searchField->setPlaceholderText(QStringLiteral("Search articles"));
    m_searchField->setClearButtonEnabled(true);
    m_searchField->setFixedHeight(28);
    auto *searchRow = new QHBoxLayout;
    searchRow->setContentsMargins(6, 0, 6, 0);
    searchRow->addWidget(m_searchField);
    layout->addLayout(searchRow);

    m_timelineModel = new TimelineModel(this);
    m_timelineModel->store = &m_store;
    m_timeline = new QListView;
    m_timeline->setObjectName(QStringLiteral("rssTimeline"));
    m_timeline->setModel(m_timelineModel);
    m_timelineDelegate = new TimelineDelegate(m_timeline);
    m_timelineDelegate->store = &m_store;
    m_timelineDelegate->model = m_timelineModel;
    m_timelineDelegate->icon = [this](const QString &feedId, int side) { return feedIcon(feedId, side); };
    m_timelineDelegate->thumbnail = [this](const QString &url) { return thumbnail(url); };
    m_timeline->setItemDelegate(m_timelineDelegate);
    m_timeline->setSelectionMode(QAbstractItemView::SingleSelection);
    m_timeline->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_timeline->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_timeline->setFrameShape(QFrame::NoFrame);
    m_timeline->setMouseTracking(true);
    m_timeline->setContextMenuPolicy(Qt::CustomContextMenu);
    m_timeline->setStyleSheet(QStringLiteral(
        "QListView#rssTimeline { background: transparent; border: none; outline: none; }"
        "QListView#rssTimeline::item, QListView#rssTimeline::item:selected,"
        "QListView#rssTimeline::item:hover { background: transparent; border: none; }"));
    m_timeline->installEventFilter(this);

    auto *stack = new QStackedWidget;
    stack->setObjectName(QStringLiteral("rssTimelineStack"));
    auto *empty = new QLabel;
    empty->setObjectName(QStringLiteral("emptyStateBody"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setWordWrap(true);
    empty->setMargin(24);
    stack->addWidget(m_timeline);
    stack->addWidget(empty);
    layout->addWidget(stack, 1);

    connect(m_timelineModel, &QAbstractItemModel::modelReset, this, [this, stack, empty] {
        if (m_timelineModel->rowCount() > 0) {
            stack->setCurrentWidget(m_timeline);
            return;
        }
        if (m_store.feeds().isEmpty())
            empty->setText(QStringLiteral("No feeds yet. Press + to add a feed or a site address, or add "
                                          "the ForkMesh blog feed from Settings."));
        else if (!m_search.trimmed().isEmpty())
            empty->setText(QStringLiteral("No articles match “%1”.").arg(m_search.trimmed()));
        else if (m_source.kind == SourceKind::Starred)
            empty->setText(QStringLiteral("No starred articles. Press S on an article to star it."));
        else if (m_source.kind == SourceKind::ReadLater)
            empty->setText(QStringLiteral("No articles to read later."));
        else if (readHidden(m_source))
            empty->setText(QStringLiteral("No unread articles."));
        else
            empty->setText(QStringLiteral("No articles."));
        stack->setCurrentWidget(empty);
    });
    connect(m_timeline->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex &current) {
        if (m_rebuilding || !current.isValid())
            return;
        const TimelineRow *row = m_timelineModel->rowAt(current.row());
        if (row && !row->header)
            selectArticle(row->ref);
    });
    connect(m_timeline, &QListView::customContextMenuRequested, this,
            [this](const QPoint &position) { timelineContextMenu(position); });
    m_searchTimer = new QTimer(this);
    m_searchTimer->setSingleShot(true);
    m_searchTimer->setInterval(150);
    connect(m_searchTimer, &QTimer::timeout, this, [this] {
        m_search = m_searchField->text();
        rebuildTimeline();
    });
    connect(m_searchField, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (text.isEmpty() || !m_loaded) {
            m_searchTimer->stop();
            m_search = text;
            rebuildTimeline();
            return;
        }
        m_searchTimer->start();
    });
    connect(m_markAllButton, &QToolButton::clicked, this, [this] { markAllRead(true); });
    connect(m_catchUpButton, &QToolButton::clicked, this, [this] {
        QMenu menu(this);
        addCatchUpMenu(&menu, false);
        menu.exec(m_catchUpButton->mapToGlobal(QPoint(0, m_catchUpButton->height())));
    });
    connect(m_unreadTimelineButton, &QToolButton::toggled, this, [this](bool on) {
        if (m_store.settings().unreadOnlyTimeline == on)
            return;
        m_store.settings().unreadOnlyTimeline = on;
        m_keptRead.clear();
        if (!m_article.isNull())
            m_keptRead.insert(keptKey(m_article));
        settingsChanged(true, false);
    });
    return pane;
}

QWidget *RssReaderFeature::buildArticlePane()
{
    auto *pane = new QWidget;
    pane->setObjectName(QStringLiteral("rssArticlePane"));
    pane->setMinimumWidth(320);
    auto *layout = new QVBoxLayout(pane);
    layout->setContentsMargins(4, 10, 10, 8);
    layout->setSpacing(6);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(4, 0, 4, 0);
    toolbar->setSpacing(2);
    m_readButton = toolButton(QStringLiteral("check-circle"), QStringLiteral("Mark as Read (R)"));
    m_starButton = toolButton(QStringLiteral("star"), QStringLiteral("Star (S)"));
    m_readLaterButton = toolButton(QStringLiteral("clock"), QStringLiteral("Read Later"));
    m_nextUnreadButton = toolButton(QStringLiteral("chevron-down"), QStringLiteral("Next Unread (N)"));
    m_readerButton = toolButton(QStringLiteral("book"), QStringLiteral("Reader View (V)"), true);
    m_openButton = toolButton(QStringLiteral("link-external"), QStringLiteral("Open in Browser (O)"));
    m_copyButton = toolButton(QStringLiteral("copy"), QStringLiteral("Copy Link"));
    m_shareButton = toolButton(QStringLiteral("paper-airplane"), QStringLiteral("Share to Chat…"));
    if (!m_host.chatTargets || !m_host.sendChatMessage)
        m_shareButton->hide();
    toolbar->addWidget(m_readButton);
    toolbar->addWidget(m_starButton);
    toolbar->addWidget(m_readLaterButton);
    toolbar->addWidget(m_nextUnreadButton);
    toolbar->addStretch();
    toolbar->addWidget(m_readerButton);
    toolbar->addWidget(m_openButton);
    toolbar->addWidget(m_copyButton);
    toolbar->addWidget(m_shareButton);
    layout->addLayout(toolbar);

    m_articleStack = new QStackedWidget;
    m_articleStack->setObjectName(QStringLiteral("rssArticleStack"));
    m_articleEmpty = new QLabel(QStringLiteral("No article selected"));
    m_articleEmpty->setObjectName(QStringLiteral("emptyStateBody"));
    m_articleEmpty->setAlignment(Qt::AlignCenter);
    m_browser = new ArticleBrowser;
    m_browser->setObjectName(QStringLiteral("rssArticle"));
    m_browser->setOpenLinks(false);
    m_browser->setOpenExternalLinks(false);
    m_browser->setFrameShape(QFrame::NoFrame);
    m_browser->setUndoRedoEnabled(false);
    m_browser->setSearchPaths({});
    m_browser->document()->setDocumentMargin(28);
    m_browser->provider = [this](int type, const QUrl &url) { return articleResource(type, url); };
    m_browser->installEventFilter(this);
    m_articleStack->addWidget(m_articleEmpty);
    m_articleStack->addWidget(m_browser);
    layout->addWidget(m_articleStack, 1);

    connect(m_browser, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https") ||
            scheme == QLatin1String("mailto"))
            QDesktopServices::openUrl(url);
    });
    connect(m_readButton, &QToolButton::clicked, this, [this] { toggleRead(); });
    connect(m_starButton, &QToolButton::clicked, this, [this] { toggleStar(); });
    connect(m_readLaterButton, &QToolButton::clicked, this, [this] { toggleReadLater(); });
    connect(m_nextUnreadButton, &QToolButton::clicked, this, [this] { nextUnread(); });
    connect(m_readerButton, &QToolButton::clicked, this, [this] { showReaderView(); });
    connect(m_openButton, &QToolButton::clicked, this, [this] { openInBrowser(); });
    connect(m_copyButton, &QToolButton::clicked, this, [this] { copyLink(); });
    connect(m_shareButton, &QToolButton::clicked, this, [this] { showShareDialog(m_article); });
    updateArticleToolbar();
    return pane;
}

void RssReaderFeature::refreshIcons()
{
    const QColor muted = palette().color(QPalette::PlaceholderText);
    const QColor accent = palette().color(QPalette::Link);
    const QList<QToolButton *> buttons = findChildren<QToolButton *>(QStringLiteral("iconButton"));
    for (QToolButton *button : buttons) {
        const QString name = button->property("rssIcon").toString();
        if (name.isEmpty())
            continue;
        QColor color = button->isCheckable() && button->isChecked() ? accent : muted;
        if (button == m_starButton) {
            const Article *item = m_store.article(m_article);
            if (item && item->starred)
                color = kStarColor;
        }
        if (button == m_readButton) {
            const Article *item = m_store.article(m_article);
            if (item && !item->read)
                color = accent;
        }
        if (button == m_readLaterButton) {
            const Article *item = m_store.article(m_article);
            if (item && item->readLater)
                color = accent;
        }
        const QString key = name + QLatin1Char('|') + color.name();
        auto cached = m_iconCache.constFind(key);
        if (cached == m_iconCache.constEnd())
            cached = m_iconCache.insert(key, tintedIcon(name, color));
        if (button->property("rssIconKey").toString() != key) {
            button->setIcon(cached.value());
            button->setProperty("rssIconKey", key);
        }
    }
}

void RssReaderFeature::changeEvent(QEvent *event)
{
    forkmesh::market::NativePackageWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        m_tiles.clear();
        m_scaledIcons.clear();
        m_iconCache.clear();
        for (QToolButton *button : findChildren<QToolButton *>(QStringLiteral("iconButton")))
            button->setProperty("rssIconKey", QVariant());
        refreshIcons();
        if (m_loaded) {
            applyArticleStyle();
            m_renderTimer->start();
        }
    }
}

void RssReaderFeature::loadStore()
{
    if (m_loaded)
        return;
    m_loaded = true;
    const qint64 at = now();
    QSettings settings;
    const QString prefix = dataPrefix();
    const QString storeId = settings.value(prefix + QStringLiteral("store")).toString();
    if (!m_disk->load(m_store, storeId)) {
        m_store.clear();
        const QString opml = settings.value(prefix + QStringLiteral("subscriptions")).toString();
        const QJsonObject saved =
            QJsonDocument::fromJson(settings.value(prefix + QStringLiteral("settings")).toString().toUtf8()).object();
        const QJsonArray starred =
            QJsonDocument::fromJson(settings.value(prefix + QStringLiteral("starred")).toString().toUtf8()).array();
        if (!saved.isEmpty())
            m_store.settings() = Settings::fromJson(saved);
        if (!opml.isEmpty()) {
            const OpmlImportResult imported = m_store.importOpml(parseOpml(opml.toUtf8()), at, true);
            if (imported.added > 0)
                m_store.logActivity(at, QStringLiteral("Restored %1 feeds from the saved copy").arg(imported.added));
        }
        if (!starred.isEmpty())
            m_store.restoreStarred(starred, at);
        m_store.setStoreId(storeId.isEmpty() ? newId() : storeId);
        m_store.markExportDirty();
    }
    m_unreadFeedsButton->setChecked(m_store.settings().unreadOnlyFeeds);
    m_unreadTimelineButton->setChecked(m_store.settings().unreadOnlyTimeline);
    if (m_store.feeds().isEmpty())
        m_source = {SourceKind::AllUnread, QString()};
    applyArticleStyle();
    rebuildSources();
    updateCounts();
    rebuildTimeline();
    updateStatus();
    scheduleSave();
}

QString RssReaderFeature::dataPrefix() const
{
    const QString id = m_host.packageId.isEmpty() ? QStringLiteral("rss-reader") : m_host.packageId;
    return QStringLiteral("market/packages/%1/data/").arg(id);
}

void RssReaderFeature::persistExportKeys()
{
    QSettings settings;
    const QString prefix = dataPrefix();
    const QJsonArray starred = m_store.starredJson();
    const bool defaults = m_store.settings().toJson() == Settings().toJson();
    if (!m_store.hasData() && starred.isEmpty() && defaults) {
        for (const char *key : {"store", "subscriptions", "settings", "starred"})
            settings.remove(prefix + QLatin1String(key));
        return;
    }
    settings.setValue(prefix + QStringLiteral("store"), m_store.storeId());
    settings.setValue(prefix + QStringLiteral("subscriptions"),
                      QString::fromUtf8(writeOpml(m_store.feeds(), m_store.folders(), now())));
    settings.setValue(prefix + QStringLiteral("settings"),
                      QString::fromUtf8(QJsonDocument(m_store.settings().toJson()).toJson(QJsonDocument::Compact)));
    settings.setValue(prefix + QStringLiteral("starred"),
                      QString::fromUtf8(QJsonDocument(starred).toJson(QJsonDocument::Compact)));
}

void RssReaderFeature::scheduleSave()
{
    if (m_saveTimer)
        m_saveTimer->start();
}

void RssReaderFeature::saveNow()
{
    if (!m_loaded)
        return;
    if (m_saveTimer)
        m_saveTimer->stop();
    QString error;
    for (const QString &feedId : m_store.takeDirtyArticles()) {
        if (!m_disk->saveArticles(m_store, feedId, &error) && !error.isEmpty()) {
            m_store.logError(now(), QStringLiteral("Could not save articles: %1").arg(error));
            error.clear();
        }
    }
    const bool exportDirty = m_store.takeExportDirty();
    if (m_store.takeStateDirty() || exportDirty) {
        if (!m_disk->saveState(m_store, &error))
            m_store.logError(now(), QStringLiteral("Could not save the reader state: %1").arg(error));
    }
    if (exportDirty)
        persistExportKeys();
}

void RssReaderFeature::refresh()
{
    loadStore();
    checkFeeds(false);
}

void RssReaderFeature::prepareSearch()
{
    loadStore();
}

void RssReaderFeature::prepareNavigationBadge()
{
    if (!m_loaded) {
        const QString storeId = QSettings().value(dataPrefix() + QStringLiteral("store")).toString();
        m_badgeHint = qMax(0, m_disk->storedUnread(storeId));
    }
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
}

int RssReaderFeature::navigationBadge() const
{
    return m_loaded ? m_store.totalUnread() : m_badgeHint;
}

void RssReaderFeature::announceChanges()
{
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
    if (m_host.searchIndexChanged)
        m_host.searchIndexChanged();
}

bool RssReaderFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    loadStore();
    saveNow();
    const QJsonObject bundle = m_store.exportBundle(now());
    const bool hasData = m_store.hasData() || !bundle.value(QStringLiteral("starred")).toArray().isEmpty();
    done(true,
         QJsonObject{{QStringLiteral("hasData"), hasData},
                     {QStringLiteral("filename"), QStringLiteral("forkmesh-rss-reader.json")},
                     {QStringLiteral("export"), bundle}},
         QString());
    return true;
}

void RssReaderFeature::checkFeeds(bool force)
{
    loadStore();
    if (!m_feedQueue->available()) {
        m_status->setText(QStringLiteral("The desktop network is not available."));
        return;
    }
    QStringList due;
    for (const QString &feedId : m_store.dueFeeds(now(), force)) {
        if (!m_inFlight.contains(feedId))
            due.append(feedId);
    }
    if (due.isEmpty()) {
        updateStatus();
        return;
    }
    m_refreshTotal += int(due.size());
    for (const QString &feedId : std::as_const(due)) {
        m_inFlight.insert(feedId);
        fetchFeed(feedId);
    }
    updateStatus();
}

void RssReaderFeature::fetchFeed(const QString &feedId, bool unconditional,
                                 std::function<void(const QString &)> done)
{
    const Feed *owner = m_store.feed(feedId);
    if (!owner) {
        m_inFlight.remove(feedId);
        if (done)
            done(QStringLiteral("This feed is no longer followed."));
        return;
    }
    FetchRequest request;
    request.url = QUrl(owner->url);
    request.allowFile = true;
    if (!unconditional) {
        request.etag = owner->etag;
        request.lastModified = owner->lastModified;
    }
    m_feedQueue->get(request, [this, feedId, done](const FetchResult &result) {
        const QString line = feedFetched(feedId, result);
        if (done)
            done(line);
    });
}

QString RssReaderFeature::feedFetched(const QString &feedId, const FetchResult &result)
{
    m_inFlight.remove(feedId);
    const Feed *owner = m_store.feed(feedId);
    const qint64 at = now();
    QString line = result.error;
    if (owner) {
        const QUrl moved = result.permanentRedirect ? result.finalUrl : QUrl();
        if (!result.ok()) {
            m_store.recordFailure(feedId, result.error, at);
            ++m_refreshFailed;
        } else if (result.notModified) {
            const QString etag = result.etag.isEmpty() ? owner->etag : result.etag;
            const QString modified = result.lastModified.isEmpty() ? owner->lastModified : result.lastModified;
            m_store.recordSuccess(feedId, at, etag, modified, moved);
            line = QStringLiteral("Not modified · checked %1").arg(timeText(at));
        } else {
            const ParsedFeed parsed = parseFeed(result.body, result.finalUrl);
            if (!parsed.ok) {
                m_store.recordFailure(feedId, parsed.error, at);
                ++m_refreshFailed;
                line = parsed.error;
            } else {
                m_refreshNew += m_store.merge(feedId, parsed, at);
                m_store.recordSuccess(feedId, at, result.etag, result.lastModified, moved);
                const int count = int(parsed.items.size());
                line = QStringLiteral("%1 · %2 %3 · checked %4")
                           .arg(parsed.format, QLocale().toString(count),
                                count == 1 ? QStringLiteral("item") : QStringLiteral("items"), timeText(at));
            }
        }
        if (!m_countsTimer->isActive())
            m_countsTimer->start();
        if (m_timelineModel->rowCount() == 0 && m_store.feedIdsIn(m_source).contains(feedId))
            rebuildTimeline();
    }
    scheduleSave();
    if (m_inFlight.isEmpty())
        finishRefresh();
    else
        updateStatus();
    return line;
}

void RssReaderFeature::checkNow(const QString &feedId, std::function<void(const QString &)> done)
{
    loadStore();
    if (!m_store.feed(feedId)) {
        if (done)
            done(QStringLiteral("This feed is no longer followed."));
        return;
    }
    if (m_inFlight.contains(feedId)) {
        if (done)
            done(QStringLiteral("This feed is being checked."));
        return;
    }
    m_inFlight.insert(feedId);
    ++m_refreshTotal;
    updateStatus();
    fetchFeed(feedId, true, std::move(done));
}

QString RssReaderFeature::changeFeedAddress(const QString &feedId, const QString &address)
{
    loadStore();
    const Feed *owner = m_store.feed(feedId);
    if (!owner)
        return QStringLiteral("This feed is no longer followed.");
    const QString before = owner->url;
    QString error;
    if (!m_store.changeFeedAddress(feedId, address, now(), &error))
        return error;
    if (m_store.feed(feedId)->url == before)
        return QString();
    m_icons.remove(feedId);
    m_iconAttempts.remove(feedId);
    rebuildSources();
    updateCounts();
    scheduleSave();
    checkNow(feedId, {});
    return QString();
}

void RssReaderFeature::finishRefresh()
{
    const qint64 at = now();
    m_lastRefreshMs = at;
    if (m_refreshTotal > 0) {
        QString line = QStringLiteral("Checked %1 %2: %3 new %4")
                           .arg(m_refreshTotal)
                           .arg(m_refreshTotal == 1 ? QStringLiteral("feed") : QStringLiteral("feeds"))
                           .arg(m_refreshNew)
                           .arg(m_refreshNew == 1 ? QStringLiteral("article") : QStringLiteral("articles"));
        if (m_refreshFailed > 0)
            line += QStringLiteral(", %1 failed").arg(m_refreshFailed);
        m_store.logActivity(at, line);
    }
    m_refreshTotal = 0;
    m_refreshNew = 0;
    m_refreshFailed = 0;
    if (m_store.settings().refreshClearsRead) {
        m_keptRead.clear();
        if (!m_article.isNull())
            m_keptRead.insert(keptKey(m_article));
    }
    rebuildSources();
    updateCounts();
    rebuildTimeline();
    updateStatus();
    scheduleSave();
    announceChanges();
    fetchMissingIcons();
}

QString RssReaderFeature::statusText() const
{
    return m_status ? m_status->text() : QString();
}

void RssReaderFeature::updateStatus()
{
    if (!m_status)
        return;
    m_refreshButton->setEnabled(m_inFlight.isEmpty());
    if (!m_inFlight.isEmpty()) {
        m_status->setText(QStringLiteral("Updating…"));
        m_status->setToolTip(QStringLiteral("Checking %1 of %2 feeds")
                                 .arg(m_refreshTotal - int(m_inFlight.size()) + 1)
                                 .arg(m_refreshTotal));
        return;
    }
    m_status->setToolTip(QString());
    qint64 last = m_lastRefreshMs;
    for (const Feed &owner : m_store.feeds())
        last = qMax(last, owner.lastCheckedMs);
    if (last > 0)
        m_status->setText(QStringLiteral("Updated %1").arg(
            english().toString(QDateTime::fromMSecsSinceEpoch(last).time(), QStringLiteral("h:mm AP"))));
    else if (m_loaded && m_store.feeds().isEmpty())
        m_status->setText(QStringLiteral("No feeds yet"));
    else
        m_status->setText(QString());
}

void RssReaderFeature::updateCounts()
{
    m_countsTimer->stop();
    const qint64 at = now();
    m_counts.clear();
    m_counts.insert(QStringLiteral("today"), m_store.unreadCount({SourceKind::Today, QString()}, at));
    m_counts.insert(QStringLiteral("unread"), m_store.totalUnread());
    m_counts.insert(QStringLiteral("starred"), m_store.unreadCount({SourceKind::Starred, QString()}, at));
    m_counts.insert(QStringLiteral("readlater"), m_store.unreadCount({SourceKind::ReadLater, QString()}, at));
    for (const SmartFeed &smart : m_store.settings().smartFeeds)
        m_counts.insert(QStringLiteral("smart:") + smart.id, m_store.unreadCount({SourceKind::Smart, smart.id}, at));
    for (const Feed &owner : m_store.feeds()) {
        const int unread = m_store.feedUnread(owner.id);
        m_counts.insert(QStringLiteral("f:") + owner.id, unread);
        if (!owner.folder.isEmpty() && !owner.excludeFromUnread)
            m_counts[QStringLiteral("d:") + owner.folder] += unread;
    }
    bool readLaterShown = false;
    for (int i = 0; i < m_sources->topLevelItemCount() && !readLaterShown; ++i) {
        QTreeWidgetItem *section = m_sources->topLevelItem(i);
        for (int j = 0; j < section->childCount(); ++j)
            readLaterShown = readLaterShown || section->child(j)->data(0, IdRole).toString() == QLatin1String("readlater");
    }
    if (readLaterShown != (m_store.readLaterCount() > 0 || m_source.kind == SourceKind::ReadLater) &&
        m_feedSearch.trimmed().isEmpty())
        rebuildSources();
    if (m_store.settings().unreadOnlyFeeds) {
        for (int i = 0; i < m_sources->topLevelItemCount(); ++i) {
            QTreeWidgetItem *section = m_sources->topLevelItem(i);
            if (section->data(0, IdRole).toString() != QLatin1String("local"))
                continue;
            for (int j = 0; j < section->childCount(); ++j) {
                QTreeWidgetItem *item = section->child(j);
                bool anyVisible = false;
                for (int k = 0; k < item->childCount(); ++k) {
                    QTreeWidgetItem *child = item->child(k);
                    const QString id = child->data(0, IdRole).toString();
                    const bool visible = m_counts.value(QStringLiteral("f:") + id) > 0 ||
                                         (m_source.kind == SourceKind::Feed && m_source.id == id);
                    child->setHidden(!visible);
                    anyVisible = anyVisible || visible;
                }
                const int kind = item->data(0, KindRole).toInt();
                const QString id = item->data(0, IdRole).toString();
                const bool current = (kind == FolderItem && m_source.kind == SourceKind::Folder && m_source.id == id) ||
                                     (kind == FeedItem && m_source.kind == SourceKind::Feed && m_source.id == id);
                const QString key = (kind == FolderItem ? QStringLiteral("d:") : QStringLiteral("f:")) + id;
                item->setHidden(!(current || anyVisible || m_counts.value(key) > 0));
            }
        }
    }
    m_sources->viewport()->update();
    m_timeline->viewport()->update();
    updateTimelineHeader();
    refreshIcons();
    if (m_host.navigationBadgeChanged)
        m_host.navigationBadgeChanged();
}

QString RssReaderFeature::sourceTitle(const Source &source) const
{
    switch (source.kind) {
    case SourceKind::Today:
        return QStringLiteral("Today");
    case SourceKind::AllUnread:
        return QStringLiteral("All Unread");
    case SourceKind::Starred:
        return QStringLiteral("Starred");
    case SourceKind::ReadLater:
        return QStringLiteral("Read Later");
    case SourceKind::Smart: {
        const SmartFeed *smart = m_store.smartFeed(source.id);
        return smart ? smart->name : QString();
    }
    case SourceKind::Folder:
        return source.id;
    case SourceKind::Feed: {
        const Feed *owner = m_store.feed(source.id);
        return owner ? owner->displayName() : QString();
    }
    case SourceKind::None:
        break;
    }
    return QString();
}

void RssReaderFeature::rebuildSources()
{
    m_rebuilding = true;
    m_sources->clear();
    const qint64 at = now();
    const QString needle = m_feedSearch.trimmed();
    const bool searching = !needle.isEmpty();
    const bool byUnread = m_store.settings().sortFeedsByUnread;
    auto section = [this](const QString &title, const QString &id) {
        auto *item = new QTreeWidgetItem(m_sources, {title});
        item->setData(0, KindRole, SectionItem);
        item->setData(0, IdRole, id);
        item->setFlags(Qt::ItemIsEnabled);
        return item;
    };
    auto add = [](QTreeWidgetItem *parent, const QString &title, int kind, const QString &id, int depth) {
        auto *item = new QTreeWidgetItem(parent, {title});
        item->setData(0, KindRole, kind);
        item->setData(0, IdRole, id);
        item->setData(0, DepthRole, depth);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setToolTip(0, title);
        return item;
    };
    auto describeFeed = [this](QTreeWidgetItem *item, const QString &feedId) {
        const Feed *owner = m_store.feed(feedId);
        if (!owner)
            return;
        QStringList notes;
        if (owner->failures > 0)
            notes << QStringLiteral("Last check failed: %1").arg(owner->lastError);
        if (owner->paused)
            notes << QStringLiteral("Updates paused");
        if (!notes.isEmpty()) {
            item->setToolTip(0, notes.join(QLatin1Char('\n')));
            item->setData(0, Qt::AccessibleDescriptionRole, notes.join(QStringLiteral(". ")));
        }
    };
    QTreeWidgetItem *current = nullptr;
    const QString currentSmart = smartItemId(m_source);
    QTreeWidgetItem *smart = nullptr;
    if (!searching) {
        smart = section(QStringLiteral("Smart Feeds"), QStringLiteral("smart"));
        QList<std::pair<QString, QString>> smartFeeds{{QStringLiteral("today"), QStringLiteral("Today")},
                                                      {QStringLiteral("unread"), QStringLiteral("All Unread")},
                                                      {QStringLiteral("starred"), QStringLiteral("Starred")}};
        if (m_store.readLaterCount() > 0 || m_source.kind == SourceKind::ReadLater)
            smartFeeds.append({QStringLiteral("readlater"), QStringLiteral("Read Later")});
        QList<SmartFeed> custom = m_store.settings().smartFeeds;
        std::stable_sort(custom.begin(), custom.end(), [](const SmartFeed &a, const SmartFeed &b) {
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
        for (const SmartFeed &entry : std::as_const(custom))
            smartFeeds.append({QStringLiteral("smart:") + entry.id, entry.name});
        for (const auto &entry : std::as_const(smartFeeds)) {
            QTreeWidgetItem *item = add(smart, entry.second, SmartItem, entry.first, 0);
            if (!currentSmart.isEmpty() && currentSmart == entry.first)
                current = item;
        }
    }
    QTreeWidgetItem *local = section(QStringLiteral("On This Computer"), QStringLiteral("local"));
    QList<SourceEntry> entries;
    for (const Folder &folder : m_store.folders()) {
        bool any = !searching;
        for (const Feed &owner : m_store.feeds())
            any = any || (owner.folder == folder.name && feedMatchesSearch(owner, needle));
        if (any)
            entries.append({folder.name, folder.name, true, m_store.unreadCount({SourceKind::Folder, folder.name}, at)});
    }
    for (const Feed &owner : m_store.feeds()) {
        if ((owner.folder.isEmpty() || !m_store.folder(owner.folder)) && feedMatchesSearch(owner, needle))
            entries.append({owner.displayName(), owner.id, false, m_store.feedUnread(owner.id)});
    }
    sortSourceEntries(entries, byUnread);
    QList<QTreeWidgetItem *> folderItems;
    for (const SourceEntry &entry : std::as_const(entries)) {
        if (!entry.folder) {
            QTreeWidgetItem *item = add(local, entry.name, FeedItem, entry.id, 0);
            describeFeed(item, entry.id);
            if (m_source.kind == SourceKind::Feed && m_source.id == entry.id)
                current = item;
            continue;
        }
        QTreeWidgetItem *folderItem = add(local, entry.name, FolderItem, entry.id, 0);
        if (m_source.kind == SourceKind::Folder && m_source.id == entry.id)
            current = folderItem;
        QList<SourceEntry> children;
        for (const Feed &owner : m_store.feeds()) {
            if (owner.folder == entry.id && feedMatchesSearch(owner, needle))
                children.append({owner.displayName(), owner.id, false, m_store.feedUnread(owner.id)});
        }
        sortSourceEntries(children, byUnread);
        for (const SourceEntry &child : std::as_const(children)) {
            QTreeWidgetItem *item = add(folderItem, child.name, FeedItem, child.id, 1);
            describeFeed(item, child.id);
            if (m_source.kind == SourceKind::Feed && m_source.id == child.id)
                current = item;
        }
        folderItems.append(folderItem);
    }
    if (smart)
        smart->setExpanded(!m_collapsedSections.contains(QStringLiteral("smart")));
    local->setExpanded(searching || !m_collapsedSections.contains(QStringLiteral("local")));
    for (QTreeWidgetItem *folderItem : std::as_const(folderItems)) {
        const Folder *folder = m_store.folder(folderItem->data(0, IdRole).toString());
        folderItem->setExpanded(searching || !folder || !folder->collapsed);
    }
    if (current)
        m_sources->setCurrentItem(current);
    m_rebuilding = false;
}

QStringList RssReaderFeature::visibleSourceIds() const
{
    QStringList ids;
    QTreeWidgetItemIterator it(m_sources);
    while (*it) {
        if (!(*it)->isHidden())
            ids.append((*it)->data(0, IdRole).toString());
        ++it;
    }
    return ids;
}

void RssReaderFeature::setFeedSearch(const QString &text)
{
    loadStore();
    if (m_feedSearchField && m_feedSearchField->text() != text) {
        const QSignalBlocker blocker(m_feedSearchField);
        m_feedSearchField->setText(text);
    }
    if (m_feedSearch == text)
        return;
    m_feedSearch = text;
    rebuildSources();
    updateCounts();
}

QStringList RssReaderFeature::feedOrder() const
{
    QStringList order;
    for (int i = 0; i < m_sources->topLevelItemCount(); ++i) {
        QTreeWidgetItem *section = m_sources->topLevelItem(i);
        if (section->data(0, IdRole).toString() != QLatin1String("local"))
            continue;
        for (int j = 0; j < section->childCount(); ++j) {
            QTreeWidgetItem *item = section->child(j);
            if (item->data(0, KindRole).toInt() == FeedItem)
                order.append(item->data(0, IdRole).toString());
            for (int k = 0; k < item->childCount(); ++k)
                order.append(item->child(k)->data(0, IdRole).toString());
        }
    }
    return order;
}

QString RssReaderFeature::keptKey(const ArticleRef &ref) const
{
    return articleKey(ref);
}

bool RssReaderFeature::readHidden(const Source &source) const
{
    if (source.kind == SourceKind::Today || source.kind == SourceKind::AllUnread)
        return true;
    if (source.kind == SourceKind::Smart) {
        const SmartFeed *smart = m_store.smartFeed(source.id);
        if (smart && smart->unreadOnly)
            return true;
    }
    return m_store.settings().unreadOnlyTimeline && hideReadApplies(source);
}

void RssReaderFeature::rebuildTimeline()
{
    const bool unreadOnly = m_store.settings().unreadOnlyTimeline && hideReadApplies(m_source);
    QSet<QString> kept = m_keptRead;
    if (!m_article.isNull())
        kept.insert(keptKey(m_article));
    const QList<ArticleRef> refs = m_store.query(m_source, m_search, unreadOnly, now(), kept);
    QList<TimelineRow> rows;
    rows.reserve(refs.size());
    QString lastFeed;
    const bool group = m_store.settings().groupByFeed;
    for (const ArticleRef &ref : refs) {
        if (group && ref.feedId != lastFeed) {
            rows.append(TimelineRow{true, ref.feedId, {}});
            lastFeed = ref.feedId;
        }
        rows.append(TimelineRow{false, ref.feedId, ref});
    }
    m_rebuilding = true;
    m_timeline->setUniformItemSizes(!group);
    m_timelineModel->setRows(rows);
    const int selected = m_timelineModel->indexOf(m_article);
    if (selected >= 0)
        m_timeline->setCurrentIndex(m_timelineModel->index(selected));
    else
        m_timeline->clearSelection();
    m_rebuilding = false;
    updateTimelineHeader();
}

int RssReaderFeature::timelineRows() const
{
    return m_timelineModel ? m_timelineModel->rowCount() : 0;
}

void RssReaderFeature::updateTimelineHeader()
{
    if (!m_timelineTitle)
        return;
    const QString title = sourceTitle(m_source);
    m_timelineTitle->setText(title);
    const int unread = m_loaded ? m_store.unreadCount(m_source, now()) : 0;
    m_timelineCount->setText(unread == 0 ? QStringLiteral("No unread articles")
                                         : QStringLiteral("%1 unread").arg(QLocale().toString(unread)));
    m_markAllButton->setEnabled(unread > 0);
}

void RssReaderFeature::selectSource(const Source &source)
{
    loadStore();
    const bool changed = source != m_source;
    const bool leftReadLater = changed && m_source.kind == SourceKind::ReadLater;
    m_source = source;
    if (changed) {
        m_keptRead.clear();
        m_article = {};
        m_readerMode = false;
        showArticle();
    }
    if (!m_rebuilding) {
        m_rebuilding = true;
        QTreeWidgetItemIterator it(m_sources);
        const QString smartId = smartItemId(source);
        while (*it) {
            const int kind = (*it)->data(0, KindRole).toInt();
            const QString id = (*it)->data(0, IdRole).toString();
            const bool match =
                (kind == FeedItem && source.kind == SourceKind::Feed && id == source.id) ||
                (kind == FolderItem && source.kind == SourceKind::Folder && id == source.id) ||
                (kind == SmartItem && !smartId.isEmpty() && id == smartId);
            if (match) {
                if ((*it)->isHidden())
                    (*it)->setHidden(false);
                if ((*it)->parent() && !(*it)->parent()->isExpanded())
                    (*it)->parent()->setExpanded(true);
                m_sources->setCurrentItem(*it);
                m_sources->scrollToItem(*it);
                break;
            }
            ++it;
        }
        m_rebuilding = false;
    }
    rebuildTimeline();
    if (leftReadLater)
        updateCounts();
}

void RssReaderFeature::markSelectedRead(const ArticleRef &ref)
{
    if (m_store.setRead(ref, true)) {
        m_keptRead.insert(keptKey(ref));
        m_timelineModel->touch(m_timelineModel->indexOf(ref));
        updateCounts();
        scheduleSave();
        if (m_host.searchIndexChanged)
            m_host.searchIndexChanged();
    }
}

void RssReaderFeature::selectArticle(const ArticleRef &ref)
{
    loadStore();
    if (!m_store.article(ref))
        return;
    const bool changed = !(m_article == ref);
    m_article = ref;
    if (changed) {
        m_readerMode = false;
        m_readerError.clear();
    }
    markSelectedRead(ref);
    const int row = m_timelineModel->indexOf(ref);
    if (row >= 0 && m_timeline->currentIndex().row() != row) {
        m_rebuilding = true;
        m_timeline->setCurrentIndex(m_timelineModel->index(row));
        m_rebuilding = false;
    }
    if (row >= 0)
        m_timeline->scrollTo(m_timelineModel->index(row));
    if (changed)
        m_shownKey.clear();
    showArticle();
}

ArticleColors RssReaderFeature::articleColors() const
{
    const QString theme = m_store.settings().theme;
    if (theme == QLatin1String("sepia"))
        return {QStringLiteral("#f6efe0"), QStringLiteral("#43362a"), QStringLiteral("#8a7862"),
                QStringLiteral("#a3541c")};
    if (theme == QLatin1String("paper"))
        return {QStringLiteral("#fdfcf8"), QStringLiteral("#222222"), QStringLiteral("#767676"),
                QStringLiteral("#1f5fbf")};
    const QPalette colors = palette();
    const bool dark = colors.color(QPalette::Base).lightness() < 128;
    if (theme == QLatin1String("contrast"))
        return dark ? ArticleColors{QStringLiteral("#000000"), QStringLiteral("#ffffff"),
                                    QStringLiteral("#d6d6d6"), QStringLiteral("#8cc4ff")}
                    : ArticleColors{QStringLiteral("#ffffff"), QStringLiteral("#000000"),
                                    QStringLiteral("#2b2b2b"), QStringLiteral("#0033cc")};
    return {colors.color(QPalette::Base).name(), colors.color(QPalette::Text).name(),
            colors.color(QPalette::PlaceholderText).name(), colors.color(QPalette::Link).name()};
}

void RssReaderFeature::applyArticleStyle()
{
    const ArticleColors colors = articleColors();
    const Settings &settings = m_store.settings();
    m_browser->setStyleSheet(QStringLiteral("QTextBrowser#rssArticle { background: %1; color: %2; border: none; "
                                            "border-radius: 8px; }")
                                 .arg(colors.background, colors.text));
    QFont font = settings.fontFamily.isEmpty() ? QApplication::font() : QFont(settings.fontFamily);
    font.setPixelSize(settings.textSize);
    m_browser->document()->setDefaultFont(font);
    const int size = settings.textSize;
    m_browser->document()->setDefaultStyleSheet(
        QStringLiteral("a { color: %1; text-decoration: none; }"
                       ".byline { color: %1; font-size: %5px; }"
                       ".title { font-size: %6px; font-weight: 700; margin-top: 12px; margin-bottom: 4px; }"
                       "a.titlelink { color: %2; }"
                       ".date { color: %3; font-size: %7px; margin-bottom: 18px; }"
                       ".muted { color: %3; }"
                       ".notice { color: %3; font-size: %5px; }"
                       ".enclosure { font-size: %5px; }"
                       ".caption { color: %3; font-size: %5px; }"
                       "blockquote { color: %3; margin-left: 18px; }"
                       "pre, code { font-family: monospace; }"
                       "pre { white-space: pre; }"
                       "h1 { font-size: %8px; } h2 { font-size: %9px; } h3 { font-size: %4px; }"
                       "body { color: %2; }")
            .arg(colors.link, colors.text, colors.muted)
            .arg(size + 2)
            .arg(qMax(11, size - 3))
            .arg(int(size * 1.75))
            .arg(qMax(10, size - 5))
            .arg(int(size * 1.45))
            .arg(int(size * 1.25)));
}

QString RssReaderFeature::prepareBody(const QString &html) const
{
    static const QRegularExpression image(QStringLiteral("<img\\b[^>]*>"));
    const bool remote = m_store.settings().loadRemoteImages;
    QString out;
    qsizetype last = 0;
    auto it = image.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += html.mid(last, match.capturedStart() - last);
        last = match.capturedEnd();
        const QString tag = match.captured(0);
        const QString source = attributeIn(tag, QStringLiteral("src"));
        const QString alt = attributeIn(tag, QStringLiteral("alt"));
        if (remote && !source.isEmpty())
            out += QStringLiteral("<img src=\"%1\" alt=\"%2\">").arg(source, alt);
        else if (!source.isEmpty())
            out += QStringLiteral("<a class=\"muted\" href=\"%1\">[%2]</a>")
                       .arg(source, alt.trimmed().isEmpty() ? QStringLiteral("Image") : alt);
        else if (!alt.trimmed().isEmpty())
            out += QStringLiteral("<span class=\"muted\">[%1]</span>").arg(alt);
    }
    out += html.mid(last);
    return out;
}

QString RssReaderFeature::composeArticleHtml(const Article &item) const
{
    const Feed *owner = m_store.feed(item.feedId);
    const QString feedName = owner ? owner->displayName() : QString();
    QString home = owner ? owner->homePage : QString();
    if (home.isEmpty() && owner)
        home = originOf(QUrl(owner->url));
    QString html = QStringLiteral("<html><body>");
    html += QStringLiteral("<table width=\"100%\" cellspacing=\"0\" cellpadding=\"0\"><tr><td valign=\"middle\">");
    if (home.isEmpty())
        html += QStringLiteral("<span class=\"byline\">%1</span>").arg(escapeHtml(feedName));
    else
        html += QStringLiteral("<a class=\"byline\" href=\"%1\">%2</a>").arg(escapeHtml(home), escapeHtml(feedName));
    if (!item.author.isEmpty())
        html += QStringLiteral("<br><span class=\"byline\">%1</span>").arg(escapeHtml(item.author));
    html += QStringLiteral("</td><td width=\"56\" align=\"right\" valign=\"middle\">"
                           "<img src=\"rssicon:%1\" width=\"40\" height=\"40\"></td></tr></table><hr>")
                .arg(escapeHtml(item.feedId));
    const QString language = !item.language.isEmpty() ? item.language : owner ? owner->language : QString();
    if (language.isEmpty())
        html += QStringLiteral("<div dir=\"%1\">").arg(textDirection(language));
    else
        html += QStringLiteral("<div lang=\"%1\" dir=\"%2\">").arg(escapeHtml(language), textDirection(language));
    const QString title = item.title.isEmpty() ? QStringLiteral("Untitled") : item.title;
    const QUrl link(item.link);
    if (link.isValid() && !link.host().isEmpty())
        html += QStringLiteral("<p class=\"title\"><a class=\"titlelink\" href=\"%1\">%2</a></p>")
                    .arg(escapeHtml(item.link), escapeHtml(title));
    else
        html += QStringLiteral("<p class=\"title\">%1</p>").arg(escapeHtml(title));
    QString body = item.html;
    bool readerBody = false;
    QString notice;
    if (m_readerMode) {
        if (!m_readerError.isEmpty())
            notice = QStringLiteral("<p class=\"notice\">Reader view is not available: %1</p>").arg(escapeHtml(m_readerError));
        else if (item.readerHtml.isEmpty())
            notice = QStringLiteral("<p class=\"notice\">Loading reader view…</p>");
        else
            body = item.readerHtml;
        readerBody = !item.readerHtml.isEmpty() && m_readerError.isEmpty();
    }
    const int minutes = readingMinutes(plainText(body, 1000000));
    QString date = escapeHtml(articleDateText(item.publishedMs));
    if (minutes > 0)
        date += QStringLiteral(" · %1 min read").arg(minutes);
    html += QStringLiteral("<p class=\"date\">%1</p>").arg(date);
    html += notice;
    if (!readerBody)
        body = leadImagesHtml(item.images, body) + body;
    if (body.trimmed().isEmpty())
        body = item.summary.isEmpty()
                   ? QStringLiteral("<p class=\"muted\">This article has no text. Open it in your browser to read it.</p>")
                   : QStringLiteral("<p>%1</p>").arg(escapeHtml(item.summary));
    if (m_store.settings().loadRemoteImages)
        body = addAltCaptions(body);
    html += QStringLiteral("<div>") + prepareBody(body) + QStringLiteral("</div></div>");
    for (const Enclosure &enclosure : item.enclosures) {
        if (enclosure.type.trimmed().toLower().startsWith(QStringLiteral("image/")) ||
            item.images.contains(enclosure.url))
            continue;
        QString name = QUrl(enclosure.url).fileName();
        if (name.isEmpty())
            name = enclosure.url;
        QStringList details;
        if (!enclosure.type.isEmpty())
            details << enclosure.type;
        if (enclosure.length > 0)
            details << sizeLabel(enclosure.length);
        html += QStringLiteral("<p class=\"enclosure\">Attachment: <a href=\"%1\">%2</a>%3</p>")
                    .arg(escapeHtml(enclosure.url), escapeHtml(name),
                         details.isEmpty() ? QString()
                                           : QStringLiteral(" <span class=\"muted\">(%1)</span>")
                                                 .arg(escapeHtml(details.join(QStringLiteral(", ")))));
    }
    if (!item.comments.isEmpty() && !htmlLinksTo(body, item.comments, QUrl(item.link)))
        html += QStringLiteral("<p><a href=\"%1\">Comments</a></p>").arg(escapeHtml(item.comments));
    html += QStringLiteral("</body></html>");
    return html;
}

void RssReaderFeature::showArticle()
{
    const Article *item = m_store.article(m_article);
    if (!item) {
        m_articleStack->setCurrentWidget(m_articleEmpty);
        m_renderedHtml.clear();
        m_shownKey.clear();
        updateArticleToolbar();
        return;
    }
    const QString key = keptKey(m_article);
    const int scroll = key == m_shownKey ? m_browser->verticalScrollBar()->value() : 0;
    const Feed *owner = m_store.feed(item->feedId);
    const QString language = !item->language.isEmpty() ? item->language : owner ? owner->language : QString();
    QTextOption option = m_browser->document()->defaultTextOption();
    option.setTextDirection(textDirection(language) == QLatin1String("rtl") ? Qt::RightToLeft
                                                                            : Qt::LayoutDirectionAuto);
    m_browser->document()->setDefaultTextOption(option);
    m_renderedHtml = composeArticleHtml(*item);
    m_browser->setHtml(m_renderedHtml);
    m_browser->verticalScrollBar()->setValue(scroll);
    m_shownKey = key;
    m_articleStack->setCurrentWidget(m_browser);
    updateArticleToolbar();
}

void RssReaderFeature::updateArticleToolbar()
{
    const Article *item = m_store.article(m_article);
    const bool has = item != nullptr;
    for (QToolButton *button : {m_readButton, m_starButton, m_readLaterButton, m_readerButton, m_openButton, m_copyButton, m_shareButton})
        button->setEnabled(has);
    if (item) {
        m_readButton->setToolTip(item->read ? QStringLiteral("Mark as Unread (R)") : QStringLiteral("Mark as Read (R)"));
        m_starButton->setToolTip(item->starred ? QStringLiteral("Unstar (S)") : QStringLiteral("Star (S)"));
        const QString readLater = item->readLater ? QStringLiteral("Remove from Read Later") : QStringLiteral("Read Later");
        m_readLaterButton->setToolTip(readLater);
        m_readLaterButton->setAccessibleName(readLater);
        const bool hasLink = QUrl(item->link).isValid() && !QUrl(item->link).host().isEmpty();
        m_openButton->setEnabled(hasLink);
        m_copyButton->setEnabled(hasLink);
        m_shareButton->setEnabled(hasLink);
        m_readerButton->setEnabled(hasLink || !item->readerHtml.isEmpty());
    }
    m_nextUnreadButton->setEnabled(m_loaded && m_store.totalUnread() > 0);
    const QSignalBlocker blocker(m_readerButton);
    m_readerButton->setChecked(has && m_readerMode);
    refreshIcons();
}

QVariant RssReaderFeature::articleResource(int type, const QUrl &url)
{
    if (url.scheme() == QLatin1String("rssicon"))
        return feedIcon(url.path(), 80).toImage();
    if (type != QTextDocument::ImageResource)
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return {};
    if (!m_store.settings().loadRemoteImages)
        return {};
    const QString key = url.toString();
    const auto found = m_images.constFind(key);
    if (found != m_images.constEnd())
        return found.value();
    fetchImage(key);
    return {};
}

QPixmap RssReaderFeature::thumbnail(const QString &url)
{
    const auto found = m_thumbs.constFind(url);
    if (found != m_thumbs.constEnd())
        return found.value();
    if (!m_thumbFailed.contains(url))
        fetchThumbnail(url);
    return QPixmap();
}

void RssReaderFeature::fetchThumbnail(const QString &url)
{
    const QString scheme = QUrl(url).scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        m_thumbFailed.insert(url);
        return;
    }
    if (m_thumbPending.contains(url) || m_thumbs.contains(url))
        return;
    m_thumbPending.insert(url);
    FetchRequest request;
    request.url = QUrl(url);
    request.maxBytes = kMaxImageBytes;
    request.accept = QByteArrayLiteral("image/avif,image/webp,image/png,image/jpeg,image/gif,image/*;q=0.8");
    m_assetQueue->get(request, [this, url](const FetchResult &result) {
        m_thumbPending.remove(url);
        QImage image;
        if (result.ok() && !result.notModified)
            image.loadFromData(result.body);
        if (image.isNull() || image.width() < 2 || image.height() < 2) {
            m_thumbFailed.insert(url);
            return;
        }
        const int side = qMin(image.width(), image.height());
        image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side);
        const qreal ratio = devicePixelRatioF();
        const int pixels = qMax(1, int(kThumbnailSide * ratio));
        QPixmap pixmap = QPixmap::fromImage(image.scaled(pixels, pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
        pixmap.setDevicePixelRatio(ratio);
        if (m_thumbs.size() >= kMaxCachedImages)
            m_thumbs.clear();
        m_thumbs.insert(url, pixmap);
        if (m_timeline)
            m_timeline->viewport()->update();
    });
}

void RssReaderFeature::fetchImage(const QString &url)
{
    if (m_imagePending.contains(url) || m_images.contains(url))
        return;
    m_imagePending.insert(url);
    FetchRequest request;
    request.url = QUrl(url);
    request.maxBytes = kMaxImageBytes;
    request.accept = QByteArrayLiteral("image/avif,image/webp,image/png,image/jpeg,image/gif,image/*;q=0.8");
    m_assetQueue->get(request, [this, url](const FetchResult &result) {
        m_imagePending.remove(url);
        QImage image;
        if (result.ok() && !result.notModified)
            image.loadFromData(result.body);
        if (image.isNull())
            image = QImage(1, 1, QImage::Format_ARGB32_Premultiplied);
        if (image.width() == 1 && image.height() == 1)
            image.fill(Qt::transparent);
        const int maxWidth = qMax(200, m_browser->viewport()->width() - 64);
        if (image.width() > maxWidth)
            image = image.scaledToWidth(maxWidth, Qt::SmoothTransformation);
        if (m_images.size() >= kMaxCachedImages)
            m_images.clear();
        m_images.insert(url, image);
        m_renderTimer->start();
    });
}

void RssReaderFeature::toggleRead()
{
    const Article *item = m_store.article(m_article);
    if (!item)
        return;
    m_store.setRead(m_article, !item->read);
    m_keptRead.insert(keptKey(m_article));
    m_timelineModel->touch(m_timelineModel->indexOf(m_article));
    updateCounts();
    updateArticleToolbar();
    scheduleSave();
}

void RssReaderFeature::toggleStar()
{
    const Article *item = m_store.article(m_article);
    if (!item)
        return;
    m_store.setStarred(m_article, !item->starred);
    m_timelineModel->touch(m_timelineModel->indexOf(m_article));
    updateCounts();
    updateArticleToolbar();
    scheduleSave();
}

void RssReaderFeature::toggleReadLater()
{
    const Article *item = m_store.article(m_article);
    if (!item)
        return;
    m_store.setReadLater(m_article, !item->readLater);
    m_timelineModel->touch(m_timelineModel->indexOf(m_article));
    updateCounts();
    updateArticleToolbar();
    scheduleSave();
}

void RssReaderFeature::markAllRead(bool ask)
{
    const bool keepStarred = m_store.settings().markAllKeepsStarred;
    QList<ArticleRef> refs;
    for (const TimelineRow &row : m_timelineModel->rows()) {
        if (row.header)
            continue;
        const Article *item = m_store.article(row.ref);
        if (item && !item->read && !(keepStarred && item->starred))
            refs.append(row.ref);
    }
    if (refs.isEmpty())
        return;
    if (ask && m_store.settings().confirmMarkAllRead) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Mark All as Read"),
            QStringLiteral("Mark %1 %2 in “%3” as read?")
                .arg(QLocale().toString(int(refs.size())),
                     refs.size() == 1 ? QStringLiteral("article") : QStringLiteral("articles"),
                     sourceTitle(m_source)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            return;
    }
    m_store.markRead(refs);
    if (readHidden(m_source)) {
        for (const ArticleRef &ref : std::as_const(refs)) {
            m_keptRead.remove(keptKey(ref));
            if (ref == m_article) {
                m_article = {};
                m_readerMode = false;
                showArticle();
            }
        }
    }
    m_store.logActivity(now(), QStringLiteral("Marked %1 articles in %2 as read").arg(refs.size()).arg(sourceTitle(m_source)));
    rebuildTimeline();
    updateCounts();
    updateArticleToolbar();
    scheduleSave();
    if (m_host.searchIndexChanged)
        m_host.searchIndexChanged();
}

void RssReaderFeature::catchUp(int days, bool everywhere, bool ask)
{
    loadStore();
    const qint64 at = now();
    const qint64 cutoff = at - qint64(days) * kDayMs;
    const bool keepStarred = m_store.settings().markAllKeepsStarred;
    const QList<ArticleRef> refs = everywhere ? m_store.olderUnreadEverywhere(cutoff, keepStarred)
                                              : m_store.olderUnread(m_source, cutoff, keepStarred, at);
    const QString scope = everywhere ? QStringLiteral("every feed") : QStringLiteral("“%1”").arg(sourceTitle(m_source));
    const QString age = catchUpLabel(days).toLower();
    if (refs.isEmpty()) {
        if (ask)
            QMessageBox::information(this, QStringLiteral("Mark Older as Read"),
                                     QStringLiteral("No unread articles %1 in %2.").arg(age, scope));
        return;
    }
    if (ask && m_store.settings().confirmMarkAllRead) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Mark Older as Read"),
            QStringLiteral("Mark %1 %2 %3 in %4 as read?")
                .arg(QLocale().toString(int(refs.size())),
                     refs.size() == 1 ? QStringLiteral("article") : QStringLiteral("articles"), age, scope),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (answer != QMessageBox::Yes)
            return;
    }
    m_store.markRead(refs);
    for (const ArticleRef &ref : refs) {
        m_keptRead.remove(keptKey(ref));
        if (ref == m_article && readHidden(m_source)) {
            m_article = {};
            m_readerMode = false;
            showArticle();
        }
    }
    m_store.logActivity(at, QStringLiteral("Marked %1 articles %2 in %3 as read").arg(refs.size()).arg(age, scope));
    rebuildTimeline();
    updateCounts();
    updateArticleToolbar();
    scheduleSave();
    if (m_host.searchIndexChanged)
        m_host.searchIndexChanged();
}

void RssReaderFeature::addCatchUpMenu(QMenu *menu, bool everywhere)
{
    QMenu *older = menu->addMenu(QStringLiteral("Mark Older as Read"));
    for (const int days : catchUpDays())
        older->addAction(catchUpLabel(days), this, [this, days, everywhere] { catchUp(days, everywhere, true); });
}

void RssReaderFeature::showSourcesMenu()
{
    loadStore();
    QMenu menu(this);
    menu.addAction(QStringLiteral("Add Feed…"), this, [this] { showAddFeed(); });
    menu.addAction(QStringLiteral("New Folder…"), this, [this] { newFolder(this); });
    menu.addAction(QStringLiteral("New Smart Feed…"), this, [this] { showSmartFeedEditor(QString()); });
    menu.addSeparator();
    addCatchUpMenu(&menu, true);
    menu.addSeparator();
    QAction *sort = menu.addAction(QStringLiteral("Sort by Unread Count"));
    sort->setCheckable(true);
    sort->setChecked(m_store.settings().sortFeedsByUnread);
    connect(sort, &QAction::toggled, this, [this](bool on) {
        updateSettings([on](Settings &settings) { settings.sortFeedsByUnread = on; }, false, false);
    });
    menu.exec(m_sourcesMenuButton->mapToGlobal(QPoint(0, m_sourcesMenuButton->height())));
}

void RssReaderFeature::updateSettings(const std::function<void(Settings &)> &change, bool timeline, bool article)
{
    loadStore();
    const Settings before = m_store.settings();
    change(m_store.settings());
    const Settings &after = m_store.settings();
    if (after.unreadOnlyTimeline != before.unreadOnlyTimeline && m_unreadTimelineButton->isChecked() != after.unreadOnlyTimeline) {
        const QSignalBlocker blocker(m_unreadTimelineButton);
        m_unreadTimelineButton->setChecked(after.unreadOnlyTimeline);
        m_keptRead.clear();
        refreshIcons();
    }
    if (after.unreadOnlyFeeds != before.unreadOnlyFeeds && m_unreadFeedsButton->isChecked() != after.unreadOnlyFeeds) {
        const QSignalBlocker blocker(m_unreadFeedsButton);
        m_unreadFeedsButton->setChecked(after.unreadOnlyFeeds);
        refreshIcons();
    }
    if (after.filters != before.filters)
        m_store.filtersChanged();
    settingsChanged(timeline, article);
    if (after.sortFeedsByUnread != before.sortFeedsByUnread || after.smartFeeds != before.smartFeeds ||
        after.filters != before.filters || after.unreadOnlyFeeds != before.unreadOnlyFeeds) {
        rebuildSources();
        updateCounts();
        if (!timeline)
            rebuildTimeline();
    }
}

void RssReaderFeature::moveSelection(int delta)
{
    const int count = m_timelineModel->rowCount();
    if (count == 0)
        return;
    int row = m_timelineModel->indexOf(m_article);
    if (row < 0)
        row = delta > 0 ? -1 : count;
    for (int next = row + delta; next >= 0 && next < count; next += delta) {
        const TimelineRow *candidate = m_timelineModel->rowAt(next);
        if (candidate && !candidate->header) {
            selectArticle(candidate->ref);
            return;
        }
    }
}

void RssReaderFeature::nextUnread()
{
    loadStore();
    auto firstUnread = [this](int from, int to) {
        for (int i = from; i < to; ++i) {
            const TimelineRow *row = m_timelineModel->rowAt(i);
            if (!row || row->header)
                continue;
            const Article *item = m_store.article(row->ref);
            if (item && !item->read) {
                selectArticle(row->ref);
                return true;
            }
        }
        return false;
    };
    const int current = m_timelineModel->indexOf(m_article);
    const int count = m_timelineModel->rowCount();
    if (firstUnread(current + 1, count))
        return;
    auto otherFeed = [this]() {
        const QStringList order = feedOrder();
        const int position = m_source.kind == SourceKind::Feed ? int(order.indexOf(m_source.id)) : -1;
        for (int step = 1; step <= order.size(); ++step) {
            const QString id = order.at((position + step + order.size()) % order.size());
            if (m_source.kind == SourceKind::Feed && id == m_source.id)
                continue;
            if (m_store.feedUnread(id) > 0) {
                selectSource({SourceKind::Feed, id});
                for (int i = 0; i < m_timelineModel->rowCount(); ++i) {
                    const TimelineRow *row = m_timelineModel->rowAt(i);
                    const Article *item = row && !row->header ? m_store.article(row->ref) : nullptr;
                    if (item && !item->read) {
                        selectArticle(row->ref);
                        return true;
                    }
                }
                return true;
            }
        }
        return false;
    };
    if (m_source.kind == SourceKind::Feed) {
        if (otherFeed())
            return;
        firstUnread(0, qMax(0, current));
        return;
    }
    if (firstUnread(0, qMax(0, current)))
        return;
    otherFeed();
}

void RssReaderFeature::showReaderView()
{
    const Article *item = m_store.article(m_article);
    if (!item)
        return;
    if (m_readerMode) {
        m_readerMode = false;
        showArticle();
        return;
    }
    m_readerMode = true;
    m_readerError.clear();
    if (!item->readerHtml.isEmpty()) {
        showArticle();
        return;
    }
    const QUrl link(item->link);
    const QString scheme = link.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        m_readerError = QStringLiteral("this article has no web page");
        showArticle();
        return;
    }
    const ArticleRef ref = m_article;
    const QString key = keptKey(ref);
    if (!m_readerPending.contains(key)) {
        m_readerPending.insert(key);
        FetchRequest request;
        request.url = link;
        request.accept = QByteArrayLiteral("text/html,application/xhtml+xml;q=0.9,*/*;q=0.5");
        m_assetQueue->get(request, [this, ref, key](const FetchResult &result) {
            m_readerPending.remove(key);
            QString error;
            if (!result.ok() || result.notModified) {
                error = result.error.isEmpty() ? QStringLiteral("the page could not be loaded") : result.error;
            } else {
                const Readable readable = extractReadable(decodeHtml(result.body, result.contentType), result.finalUrl);
                if (plainText(readable.html, 200).isEmpty()) {
                    error = QStringLiteral("no article text was found on the page");
                } else {
                    m_store.setReaderHtml(ref, readable.html);
                    scheduleSave();
                }
            }
            if (!error.isEmpty())
                m_store.logError(now(), QStringLiteral("Reader view for %1: %2").arg(result.requestedUrl.toString(), error));
            if (ref == m_article && m_readerMode) {
                m_readerError = error;
                showArticle();
            }
        });
    }
    showArticle();
}

void RssReaderFeature::openInBrowser()
{
    const Article *item = m_store.article(m_article);
    if (!item)
        return;
    const QUrl link(item->link);
    const QString scheme = link.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
        QDesktopServices::openUrl(link);
}

void RssReaderFeature::copyLink()
{
    const Article *item = m_store.article(m_article);
    if (item && !item->link.isEmpty())
        QGuiApplication::clipboard()->setText(item->link);
}

QList<forkmesh::market::NativePackageChatTarget> RssReaderFeature::shareTargets() const
{
    return m_host.chatTargets ? m_host.chatTargets() : QList<forkmesh::market::NativePackageChatTarget>();
}

QString RssReaderFeature::shareArticle(const ArticleRef &ref, const QString &target)
{
    const Article *item = m_store.article(ref);
    if (!item)
        return QStringLiteral("This article is no longer in the reader.");
    if (!m_host.sendChatMessage)
        return QStringLiteral("Chat is not available in this window.");
    const QString text = shareMessage(item->title, item->link);
    if (text.isEmpty())
        return QStringLiteral("This article has no title or link to share.");
    if (target.isEmpty() || !m_host.sendChatMessage(target, text))
        return QStringLiteral("The message was not sent. Check that chat is connected and that you can post there.");
    return QString();
}

void RssReaderFeature::showShareDialog(const ArticleRef &ref)
{
    if (!m_store.article(ref) || !m_host.chatTargets || !m_host.sendChatMessage)
        return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("rssShareDialog"));
    dialog.setWindowTitle(QStringLiteral("Share to Chat"));
    dialog.setMinimumWidth(380);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);
    auto *filter = new QLineEdit;
    filter->setObjectName(QStringLiteral("rssShareFilter"));
    filter->setPlaceholderText(QStringLiteral("Find a channel or person"));
    filter->setClearButtonEnabled(true);
    layout->addWidget(filter);
    auto *list = new QListWidget;
    list->setObjectName(QStringLiteral("rssShareTargets"));
    list->setMinimumHeight(240);
    for (const auto &target : shareTargets()) {
        if (target.id.isEmpty())
            continue;
        auto *row = new QListWidgetItem(target.label.isEmpty() ? target.id : target.label, list);
        row->setData(Qt::UserRole, target.id);
        row->setToolTip(target.person ? QStringLiteral("Direct message") : QStringLiteral("Channel"));
    }
    layout->addWidget(list, 1);
    auto *status = new QLabel;
    status->setObjectName(QStringLiteral("fieldHint"));
    status->setWordWrap(true);
    status->setText(list->count() > 0
                        ? QStringLiteral("Sends the article's title and link as a message from you.")
                        : QStringLiteral("There is no channel or conversation to share to. Sign in to chat, or start a "
                                         "conversation first."));
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *send = buttons->addButton(QStringLiteral("Send"), QDialogButtonBox::AcceptRole);
    send->setObjectName(QStringLiteral("primaryButton"));
    send->setDefault(true);
    send->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(filter, &QLineEdit::textChanged, list, [list](const QString &text) {
        const QString needle = text.trimmed();
        QListWidgetItem *firstVisible = nullptr;
        for (int index = 0; index < list->count(); ++index) {
            QListWidgetItem *row = list->item(index);
            const bool hidden = !needle.isEmpty() && !row->text().contains(needle, Qt::CaseInsensitive);
            row->setHidden(hidden);
            if (!hidden && !firstVisible)
                firstVisible = row;
        }
        if (list->currentItem() && list->currentItem()->isHidden())
            list->setCurrentItem(firstVisible);
    });
    connect(list, &QListWidget::currentItemChanged, send,
            [send](QListWidgetItem *current) { send->setEnabled(current && !current->isHidden()); });
    auto submit = [this, &dialog, list, status, ref] {
        QListWidgetItem *current = list->currentItem();
        if (!current || current->isHidden())
            return;
        const QString error = shareArticle(ref, current->data(Qt::UserRole).toString());
        if (error.isEmpty())
            dialog.accept();
        else
            status->setText(error);
    };
    connect(send, &QPushButton::clicked, &dialog, submit);
    connect(list, &QListWidget::itemActivated, &dialog, submit);
    filter->setFocus();
    dialog.exec();
}

bool RssReaderFeature::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress &&
        (watched == m_sources || watched == m_timeline || watched == m_browser)) {
        if (handleKey(static_cast<QKeyEvent *>(event)))
            return true;
    }
    if (event->type() == QEvent::MouseButtonPress && m_sources && watched == m_sources->viewport()) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        QTreeWidgetItem *item = m_sources->itemAt(mouse->position().toPoint());
        if (item && item->data(0, KindRole).toInt() == FolderItem) {
            const QRect rect = m_sources->visualItemRect(item);
            if (mouse->position().x() < rect.left() + 26) {
                item->setExpanded(!item->isExpanded());
                return true;
            }
        }
    }
    return forkmesh::market::NativePackageWidget::eventFilter(watched, event);
}

bool RssReaderFeature::handleKey(QKeyEvent *event)
{
    const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
    if (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
        return false;
    const bool shift = modifiers & Qt::ShiftModifier;
    switch (event->key()) {
    case Qt::Key_J:
        moveSelection(1);
        return true;
    case Qt::Key_K:
        moveSelection(-1);
        return true;
    case Qt::Key_N:
        nextUnread();
        return true;
    case Qt::Key_Space: {
        if (shift)
            return false;
        QScrollBar *bar = m_browser->verticalScrollBar();
        if (m_store.article(m_article) && bar->value() < bar->maximum())
            bar->setValue(bar->value() + bar->pageStep());
        else
            nextUnread();
        return true;
    }
    case Qt::Key_R:
        if (shift)
            checkFeeds(true);
        else
            toggleRead();
        return true;
    case Qt::Key_S:
        toggleStar();
        return true;
    case Qt::Key_O:
        openInBrowser();
        return true;
    case Qt::Key_V:
        showReaderView();
        return true;
    case Qt::Key_A:
        if (!shift)
            return false;
        markAllRead(true);
        return true;
    default:
        return false;
    }
}

void RssReaderFeature::sourceContextMenu(const QPoint &position)
{
    QTreeWidgetItem *item = m_sources->itemAt(position);
    if (!item)
        return;
    const int kind = item->data(0, KindRole).toInt();
    const QString id = item->data(0, IdRole).toString();
    if (kind == SectionItem) {
        QMenu menu(this);
        if (id == QLatin1String("smart")) {
            menu.addAction(QStringLiteral("New Smart Feed…"), this, [this] { showSmartFeedEditor(QString()); });
        } else if (id == QLatin1String("local")) {
            menu.addAction(QStringLiteral("Add Feed…"), this, [this] { showAddFeed(); });
            menu.addAction(QStringLiteral("New Folder…"), this, [this] { newFolder(this); });
            menu.addAction(QStringLiteral("New Smart Feed…"), this, [this] { showSmartFeedEditor(QString()); });
            menu.addSeparator();
            addCatchUpMenu(&menu, true);
        } else {
            return;
        }
        menu.exec(m_sources->viewport()->mapToGlobal(position));
        return;
    }
    QMenu menu(this);
    const Source target = kind == FeedItem     ? Source{SourceKind::Feed, id}
                          : kind == FolderItem ? Source{SourceKind::Folder, id}
                                               : smartSource(id);
    auto markAll = [this, target] {
        selectSource(target);
        markAllRead(true);
    };
    QAction *markAction = menu.addAction(QStringLiteral("Mark All as Read"), this, markAll);
    markAction->setEnabled(m_sourceDelegate->count(m_sources->indexFromItem(item)) > 0);
    QMenu *older = menu.addMenu(QStringLiteral("Mark Older as Read"));
    for (const int days : catchUpDays()) {
        older->addAction(catchUpLabel(days), this, [this, target, days] {
            selectSource(target);
            catchUp(days, false, true);
        });
    }
    if (kind == SmartItem && target.kind == SourceKind::Smart) {
        const QString smartId = target.id;
        menu.addSeparator();
        menu.addAction(QStringLiteral("Edit Smart Feed…"), this, [this, smartId] { showSmartFeedEditor(smartId); });
        menu.addAction(QStringLiteral("Delete Smart Feed…"), this, [this, smartId] { deleteSmartFeed(smartId); });
    }
    if (kind == FeedItem) {
        const Feed *owner = m_store.feed(id);
        if (!owner)
            return;
        menu.addSeparator();
        menu.addAction(QStringLiteral("Feed Info…"), this, [this, id] { showFeedInfo(id); });
        QString home = owner->homePage;
        QAction *open = menu.addAction(QStringLiteral("Open Home Page"), this, [home] {
            QDesktopServices::openUrl(QUrl(home));
        });
        open->setEnabled(!home.isEmpty());
        const QString address = owner->url;
        menu.addAction(QStringLiteral("Copy Feed Address"), this, [address] {
            QGuiApplication::clipboard()->setText(address);
        });
        menu.addAction(QStringLiteral("Check Now"), this, [this, id] { checkNow(id, {}); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Unsubscribe…"), this, [this, id] { unsubscribe(id); });
    } else if (kind == FolderItem) {
        menu.addSeparator();
        menu.addAction(QStringLiteral("Rename…"), this, [this, id] { renameFolder(id); });
        menu.addAction(QStringLiteral("Delete…"), this, [this, id] { deleteFolder(id); });
    }
    menu.exec(m_sources->viewport()->mapToGlobal(position));
}

void RssReaderFeature::timelineContextMenu(const QPoint &position)
{
    const QModelIndex index = m_timeline->indexAt(position);
    const TimelineRow *row = index.isValid() ? m_timelineModel->rowAt(index.row()) : nullptr;
    if (!row || row->header)
        return;
    const Article *item = m_store.article(row->ref);
    if (!item)
        return;
    const ArticleRef ref = row->ref;
    QMenu menu(this);
    menu.addAction(item->read ? QStringLiteral("Mark as Unread") : QStringLiteral("Mark as Read"), this, [this, ref] {
        const Article *target = m_store.article(ref);
        if (!target)
            return;
        m_store.setRead(ref, !target->read);
        m_keptRead.insert(keptKey(ref));
        m_timelineModel->touch(m_timelineModel->indexOf(ref));
        updateCounts();
        updateArticleToolbar();
        scheduleSave();
    });
    menu.addAction(item->starred ? QStringLiteral("Unstar") : QStringLiteral("Star"), this, [this, ref] {
        const Article *target = m_store.article(ref);
        if (!target)
            return;
        m_store.setStarred(ref, !target->starred);
        m_timelineModel->touch(m_timelineModel->indexOf(ref));
        updateCounts();
        updateArticleToolbar();
        scheduleSave();
    });
    menu.addAction(item->readLater ? QStringLiteral("Remove from Read Later") : QStringLiteral("Read Later"), this, [this, ref] {
        const Article *target = m_store.article(ref);
        if (!target)
            return;
        m_store.setReadLater(ref, !target->readLater);
        m_timelineModel->touch(m_timelineModel->indexOf(ref));
        updateCounts();
        updateArticleToolbar();
        scheduleSave();
    });
    const QString link = item->link;
    const bool hasLink = QUrl(link).isValid() && !QUrl(link).host().isEmpty();
    menu.addSeparator();
    QAction *open = menu.addAction(QStringLiteral("Open in Browser"), this, [link] { QDesktopServices::openUrl(QUrl(link)); });
    open->setEnabled(hasLink);
    QAction *copy = menu.addAction(QStringLiteral("Copy Link"), this, [link] { QGuiApplication::clipboard()->setText(link); });
    copy->setEnabled(hasLink);
    if (m_host.chatTargets && m_host.sendChatMessage) {
        QAction *share = menu.addAction(QStringLiteral("Share to Chat…"), this, [this, ref] { showShareDialog(ref); });
        share->setEnabled(hasLink);
    }
    menu.exec(m_timeline->viewport()->mapToGlobal(position));
}

void RssReaderFeature::subscribe(const QString &address, const QString &title, const QString &folder,
                                 std::function<void(bool, const QString &)> done)
{
    loadStore();
    auto finish = [done](bool ok, const QString &message) {
        if (done)
            done(ok, message);
    };
    const QUrl url = normalizeAddress(address, true);
    if (!url.isValid()) {
        finish(false, QStringLiteral("Enter a feed address or a site address."));
        return;
    }
    if (m_store.feeds().size() >= kMaxFeeds) {
        finish(false, QStringLiteral("RSS Reader follows at most %1 feeds.").arg(kMaxFeeds));
        return;
    }
    if (const Feed *existing = m_store.feedByUrl(url)) {
        finish(false, QStringLiteral("You already follow %1.").arg(existing->displayName()));
        return;
    }
    if (!m_feedQueue->available() && !url.isLocalFile()) {
        finish(false, QStringLiteral("The desktop network is not available."));
        return;
    }
    discoverFeed(m_feedQueue.get(), url, [this, title, folder, finish](const DiscoveryResult &result) {
        if (!result.ok) {
            finish(false, result.error);
            return;
        }
        const qint64 at = now();
        QString error;
        const QString id = m_store.addFeed(result.feedUrl, result.feed.title, folder, at, &error);
        if (id.isEmpty()) {
            finish(false, error);
            return;
        }
        if (!title.trimmed().isEmpty())
            m_store.renameFeed(id, title);
        m_store.merge(id, result.feed, at);
        m_store.recordSuccess(id, at, result.fetch.etag, result.fetch.lastModified, QUrl());
        if (const Feed *added = m_store.feed(id))
            m_store.logActivity(at, QStringLiteral("Subscribed to %1 (%2)").arg(added->displayName(), added->url));
        rebuildSources();
        updateCounts();
        selectSource({SourceKind::Feed, id});
        updateStatus();
        scheduleSave();
        announceChanges();
        fetchMissingIcons();
        finish(true, QString());
    });
}

void RssReaderFeature::showAddFeed()
{
    loadStore();
    auto *dialog = new QDialog(this);
    dialog->setObjectName(QStringLiteral("rssAddFeedDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Add a Feed"));
    dialog->setMinimumWidth(440);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);
    auto *form = new QFormLayout;
    form->setSpacing(8);
    auto *address = new QLineEdit;
    address->setObjectName(QStringLiteral("rssAddAddress"));
    address->setPlaceholderText(QStringLiteral("Feed or site address, like example.com"));
    auto *name = new QLineEdit;
    name->setPlaceholderText(QStringLiteral("Optional; the feed's own title by default"));
    auto *folder = new QComboBox;
    folder->addItem(QStringLiteral("None"), QString());
    QStringList names;
    for (const Folder &entry : m_store.folders())
        names.append(entry.name);
    names.sort(Qt::CaseInsensitive);
    for (const QString &entry : std::as_const(names))
        folder->addItem(entry, entry);
    if (m_source.kind == SourceKind::Folder)
        folder->setCurrentIndex(qMax(0, folder->findData(m_source.id)));
    form->addRow(QStringLiteral("Address"), address);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Folder"), folder);
    layout->addLayout(form);
    auto *status = new QLabel;
    status->setObjectName(QStringLiteral("fieldHint"));
    status->setWordWrap(true);
    status->setText(QStringLiteral("RSS Reader looks for the site's feed if you enter a page address."));
    layout->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *add = buttons->addButton(QStringLiteral("Add"), QDialogButtonBox::AcceptRole);
    add->setObjectName(QStringLiteral("primaryButton"));
    add->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    QPointer<QDialog> guard(dialog);
    connect(add, &QPushButton::clicked, dialog, [this, guard, address, name, folder, status, add] {
        add->setEnabled(false);
        address->setEnabled(false);
        status->setText(QStringLiteral("Looking for a feed…"));
        subscribe(address->text(), name->text(), folder->currentData().toString(),
                  [guard, address, status, add](bool ok, const QString &message) {
            if (!guard)
                return;
            if (ok) {
                guard->accept();
                return;
            }
            add->setEnabled(true);
            address->setEnabled(true);
            status->setText(message);
        });
    });
    dialog->show();
    address->setFocus();
}

void RssReaderFeature::showFeedInfo(const QString &feedId)
{
    const Feed *owner = m_store.feed(feedId);
    if (!owner)
        return;
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("rssFeedInfoDialog"));
    dialog.setWindowTitle(QStringLiteral("Feed Info"));
    dialog.setMinimumWidth(480);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 14);
    auto *form = new QFormLayout;
    form->setSpacing(8);
    auto *name = new QLineEdit(owner->displayName());
    name->setPlaceholderText(owner->title);
    auto *folder = new QComboBox;
    folder->addItem(QStringLiteral("None"), QString());
    QStringList names;
    for (const Folder &entry : m_store.folders())
        names.append(entry.name);
    names.sort(Qt::CaseInsensitive);
    for (const QString &entry : std::as_const(names))
        folder->addItem(entry, entry);
    folder->setCurrentIndex(qMax(0, folder->findData(owner->folder)));
    auto selectable = [](const QString &text) {
        auto *label = new QLabel(text);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
        label->setTextFormat(Qt::PlainText);
        return label;
    };
    auto *home = new QLabel;
    home->setTextFormat(Qt::RichText);
    home->setOpenExternalLinks(true);
    home->setWordWrap(true);
    home->setText(owner->homePage.isEmpty()
                      ? QStringLiteral("None")
                      : QStringLiteral("<a href=\"%1\">%2</a>").arg(escapeHtml(owner->homePage), escapeHtml(owner->homePage)));
    auto *address = new QLineEdit(owner->url);
    address->setObjectName(QStringLiteral("rssFeedAddress"));
    auto *addressError = new QLabel;
    addressError->setObjectName(QStringLiteral("fieldError"));
    addressError->setWordWrap(true);
    addressError->setStyleSheet(QStringLiteral("color: %1;").arg(kWarningColor.name()));
    addressError->hide();
    auto *addressBox = new QVBoxLayout;
    addressBox->setSpacing(4);
    addressBox->addWidget(address);
    addressBox->addWidget(addressError);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Folder"), folder);
    form->addRow(QStringLiteral("Home page"), home);
    form->addRow(QStringLiteral("Feed address"), addressBox);
    if (!owner->previousUrls.isEmpty())
        form->addRow(QStringLiteral("Previous addresses"), selectable(owner->previousUrls.join(QLatin1Char('\n'))));
    form->addRow(QStringLiteral("Last check"), selectable(whenText(owner->lastCheckedMs)));
    form->addRow(QStringLiteral("Last error"), selectable(owner->lastError.isEmpty() ? QStringLiteral("None") : owner->lastError));
    if (owner->failures > 0)
        form->addRow(QStringLiteral("Next check"), selectable(whenText(owner->nextDueMs)));
    layout->addLayout(form);
    auto *paused = new QCheckBox(QStringLiteral("Pause Updates"));
    paused->setChecked(owner->paused);
    auto *hideToday = new QCheckBox(QStringLiteral("Hide from Today"));
    hideToday->setChecked(owner->excludeFromToday);
    auto *hideUnread = new QCheckBox(QStringLiteral("Hide from Unread Counts"));
    hideUnread->setChecked(owner->excludeFromUnread);
    for (QCheckBox *box : {paused, hideToday, hideUnread})
        layout->addWidget(box);
    auto *checkRow = new QHBoxLayout;
    auto *checkButton = new QPushButton(QStringLiteral("Check Now"));
    checkButton->setObjectName(QStringLiteral("rssCheckNow"));
    auto *checkResult = new QLabel;
    checkResult->setObjectName(QStringLiteral("rssCheckResult"));
    checkResult->setWordWrap(true);
    checkResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    checkRow->addWidget(checkButton);
    checkRow->addWidget(checkResult, 1);
    layout->addLayout(checkRow);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QPointer<QDialog> guard(&dialog);
    connect(checkButton, &QPushButton::clicked, &dialog, [this, feedId, guard, checkButton, checkResult] {
        checkButton->setEnabled(false);
        checkResult->setText(QStringLiteral("Checking…"));
        checkNow(feedId, [guard, checkButton, checkResult](const QString &line) {
            if (!guard)
                return;
            checkButton->setEnabled(true);
            checkResult->setText(line);
        });
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [this, feedId, guard, address, addressError] {
        const Feed *current = m_store.feed(feedId);
        if (!current) {
            guard->reject();
            return;
        }
        if (address->text().trimmed() != current->url) {
            const QString error = changeFeedAddress(feedId, address->text());
            if (!error.isEmpty()) {
                addressError->setText(error);
                addressError->show();
                address->setFocus();
                return;
            }
        }
        guard->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !m_store.feed(feedId))
        return;
    m_store.renameFeed(feedId, name->text());
    m_store.moveFeed(feedId, folder->currentData().toString());
    setFeedFlags(feedId, paused->isChecked(), hideToday->isChecked(), hideUnread->isChecked());
}

void RssReaderFeature::setFeedFlags(const QString &feedId, bool paused, bool excludeFromToday, bool excludeFromUnread)
{
    loadStore();
    m_store.setFeedFlags(feedId, paused, excludeFromToday, excludeFromUnread);
    rebuildSources();
    updateCounts();
    rebuildTimeline();
    updateTimelineHeader();
    m_timeline->viewport()->update();
    scheduleSave();
    announceChanges();
}

void RssReaderFeature::showSmartFeedEditor(const QString &id)
{
    loadStore();
    const SmartFeed *existing = id.isEmpty() ? nullptr : m_store.smartFeed(id);
    if (!id.isEmpty() && !existing)
        return;
    if (!existing && m_store.settings().smartFeeds.size() >= kMaxSmartFeeds) {
        QMessageBox::information(this, QStringLiteral("New Smart Feed"),
                                 QStringLiteral("RSS Reader keeps at most %1 smart feeds.").arg(kMaxSmartFeeds));
        return;
    }
    const SmartFeed original = existing ? *existing : SmartFeed();
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("rssSmartFeedDialog"));
    dialog.setWindowTitle(existing ? QStringLiteral("Edit Smart Feed") : QStringLiteral("New Smart Feed"));
    dialog.setMinimumWidth(460);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 14);
    auto *form = new QFormLayout;
    form->setSpacing(8);
    auto *name = new QLineEdit(original.name);
    auto *words = new QLineEdit(original.words);
    words->setPlaceholderText(QStringLiteral("Any article"));
    auto *regex = new QCheckBox(QStringLiteral("Regular Expression"));
    regex->setChecked(original.regex);
    auto *scope = new QComboBox;
    scope->addItem(QStringLiteral("All Feeds"), QString());
    QStringList folderNames;
    for (const Folder &entry : m_store.folders())
        folderNames.append(entry.name);
    folderNames.sort(Qt::CaseInsensitive);
    for (const QString &entry : std::as_const(folderNames))
        scope->addItem(QStringLiteral("Folder: %1").arg(entry), QStringLiteral("folder:") + entry);
    QList<const Feed *> feeds;
    for (const Feed &owner : m_store.feeds())
        feeds.append(&owner);
    std::sort(feeds.begin(), feeds.end(), [](const Feed *a, const Feed *b) {
        return a->displayName().compare(b->displayName(), Qt::CaseInsensitive) < 0;
    });
    for (const Feed *owner : std::as_const(feeds))
        scope->addItem(QStringLiteral("Feed: %1").arg(owner->displayName()), QStringLiteral("feed:") + owner->id);
    scope->setCurrentIndex(qMax(0, scope->findData(original.scope)));
    auto *unreadOnly = new QCheckBox(QStringLiteral("Unread Only"));
    unreadOnly->setChecked(original.unreadOnly);
    auto *starredOnly = new QCheckBox(QStringLiteral("Starred Only"));
    starredOnly->setChecked(original.starredOnly);
    auto *age = new QSpinBox;
    age->setRange(0, 3650);
    age->setSpecialValueText(QStringLiteral("Any Age"));
    age->setSuffix(QStringLiteral(" days"));
    age->setValue(original.maxAgeDays);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Words"), words);
    form->addRow(QString(), regex);
    form->addRow(QStringLiteral("Feeds"), scope);
    form->addRow(QString(), unreadOnly);
    form->addRow(QString(), starredOnly);
    form->addRow(QStringLiteral("Newer than"), age);
    layout->addLayout(form);
    auto *error = new QLabel;
    error->setObjectName(QStringLiteral("fieldError"));
    error->setStyleSheet(QStringLiteral("color: %1;").arg(kWarningColor.name()));
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    auto validate = [words, regex, error] {
        const bool bad = regex->isChecked() && !words->text().trimmed().isEmpty() &&
                         !TextMatcher(words->text().trimmed(), true).isValid();
        error->setText(bad ? QStringLiteral("Invalid regular expression") : QString());
        error->setVisible(bad);
    };
    connect(words, &QLineEdit::textChanged, &dialog, validate);
    connect(regex, &QCheckBox::toggled, &dialog, validate);
    QString savedId = original.id;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        SmartFeed edited = original;
        edited.name = name->text();
        edited.words = words->text();
        edited.regex = regex->isChecked();
        edited.scope = scope->currentData().toString();
        edited.unreadOnly = unreadOnly->isChecked();
        edited.starredOnly = starredOnly->isChecked();
        edited.maxAgeDays = age->value();
        QString problem;
        if (!m_store.saveSmartFeed(edited, &problem)) {
            error->setText(problem);
            error->show();
            return;
        }
        if (savedId.isEmpty()) {
            const QList<SmartFeed> &all = m_store.settings().smartFeeds;
            savedId = all.isEmpty() ? QString() : all.last().id;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    scheduleSave();
    rebuildSources();
    updateCounts();
    if (!savedId.isEmpty())
        selectSource({SourceKind::Smart, savedId});
    else
        rebuildTimeline();
}

void RssReaderFeature::deleteSmartFeed(const QString &id)
{
    const SmartFeed *smart = m_store.smartFeed(id);
    if (!smart)
        return;
    QMessageBox box(QMessageBox::Question, QStringLiteral("Delete Smart Feed"),
                    QStringLiteral("Delete the smart feed “%1”?").arg(smart->name), QMessageBox::Cancel, this);
    box.setInformativeText(QStringLiteral("Its articles stay in their feeds."));
    QPushButton *remove = box.addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
    box.exec();
    if (box.clickedButton() != remove)
        return;
    m_store.removeSmartFeed(id);
    if (m_source.kind == SourceKind::Smart && m_source.id == id)
        m_source = {SourceKind::AllUnread, QString()};
    rebuildSources();
    updateCounts();
    rebuildTimeline();
    scheduleSave();
}

bool RssReaderFeature::showFilterEditor(QWidget *parent, const QString &id)
{
    const ContentFilter *existing = nullptr;
    for (const ContentFilter &candidate : m_store.settings().filters) {
        if (candidate.id == id)
            existing = &candidate;
    }
    if (!existing && m_store.settings().filters.size() >= kMaxFilters) {
        QMessageBox::information(parent, QStringLiteral("Filters"),
                                 QStringLiteral("RSS Reader keeps at most %1 filters.").arg(kMaxFilters));
        return false;
    }
    const ContentFilter original = existing ? *existing : ContentFilter();
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("rssFilterDialog"));
    dialog.setWindowTitle(existing ? QStringLiteral("Edit Filter") : QStringLiteral("New Filter"));
    dialog.setMinimumWidth(440);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(18, 16, 18, 14);
    auto *form = new QFormLayout;
    form->setSpacing(8);
    auto *pattern = new QLineEdit(original.pattern);
    auto *regex = new QCheckBox(QStringLiteral("Regular Expression"));
    regex->setChecked(original.regex);
    auto *field = new QComboBox;
    for (const char *value : {"title", "titleOrText", "author"})
        field->addItem(filterFieldLabel(QString::fromLatin1(value)), QString::fromLatin1(value));
    field->setCurrentIndex(qMax(0, field->findData(original.field)));
    auto *action = new QComboBox;
    for (const char *value : {"hide", "markRead"})
        action->addItem(filterActionLabel(QString::fromLatin1(value)), QString::fromLatin1(value));
    action->setCurrentIndex(qMax(0, action->findData(original.action)));
    auto *feed = new QComboBox;
    feed->addItem(QStringLiteral("All Feeds"), QString());
    QList<const Feed *> feeds;
    for (const Feed &owner : m_store.feeds())
        feeds.append(&owner);
    std::sort(feeds.begin(), feeds.end(), [](const Feed *a, const Feed *b) {
        return a->displayName().compare(b->displayName(), Qt::CaseInsensitive) < 0;
    });
    for (const Feed *owner : std::as_const(feeds))
        feed->addItem(owner->displayName(), owner->id);
    feed->setCurrentIndex(qMax(0, feed->findData(original.feedId)));
    form->addRow(QStringLiteral("Pattern"), pattern);
    form->addRow(QString(), regex);
    form->addRow(QStringLiteral("Match"), field);
    form->addRow(QStringLiteral("Action"), action);
    form->addRow(QStringLiteral("Feed"), feed);
    layout->addLayout(form);
    auto *hint = new QLabel(QStringLiteral("Matching ignores case. Mark as Read applies to new articles; Hide applies to "
                                           "every article until the filter is removed. Starred articles are never filtered."));
    hint->setObjectName(QStringLiteral("fieldHint"));
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *error = new QLabel;
    error->setObjectName(QStringLiteral("fieldError"));
    error->setStyleSheet(QStringLiteral("color: %1;").arg(kWarningColor.name()));
    error->setWordWrap(true);
    error->hide();
    layout->addWidget(error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    auto validate = [pattern, regex, error] {
        const bool bad = regex->isChecked() && !pattern->text().trimmed().isEmpty() &&
                         !TextMatcher(pattern->text().trimmed(), true).isValid();
        error->setText(bad ? QStringLiteral("Invalid regular expression") : QString());
        error->setVisible(bad);
    };
    connect(pattern, &QLineEdit::textChanged, &dialog, validate);
    connect(regex, &QCheckBox::toggled, &dialog, validate);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        ContentFilter edited = original;
        edited.pattern = pattern->text();
        edited.regex = regex->isChecked();
        edited.field = field->currentData().toString();
        edited.action = action->currentData().toString();
        edited.feedId = feed->currentData().toString();
        QString problem;
        if (!m_store.saveFilter(edited, &problem)) {
            error->setText(problem);
            error->show();
            return;
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    filtersEdited();
    return true;
}

void RssReaderFeature::filtersEdited()
{
    scheduleSave();
    rebuildSources();
    updateCounts();
    rebuildTimeline();
    announceChanges();
}

void RssReaderFeature::unsubscribe(const QString &feedId)
{
    const Feed *owner = m_store.feed(feedId);
    if (!owner)
        return;
    int starred = 0;
    for (const Article &item : m_store.articles(feedId))
        starred += item.starred ? 1 : 0;
    QString detail = QStringLiteral("Its articles are removed from this computer.");
    if (starred > 0)
        detail = QStringLiteral("Its articles, including %1 starred, are removed from this computer.").arg(starred);
    QMessageBox box(QMessageBox::Question, QStringLiteral("Unsubscribe"),
                    QStringLiteral("Unsubscribe from “%1”?").arg(owner->displayName()),
                    QMessageBox::Cancel, this);
    box.setInformativeText(detail);
    QPushButton *remove = box.addButton(QStringLiteral("Unsubscribe"), QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != remove)
        return;
    m_store.logActivity(now(), QStringLiteral("Unsubscribed from %1").arg(owner->displayName()));
    m_store.removeFeed(feedId);
    m_disk->removeFeedFiles(feedId);
    m_icons.remove(feedId);
    m_scaledIcons.clear();
    m_inFlight.remove(feedId);
    if (m_article.feedId == feedId)
        m_article = {};
    if (m_source.kind == SourceKind::Feed && m_source.id == feedId)
        m_source = {SourceKind::AllUnread, QString()};
    rebuildSources();
    updateCounts();
    rebuildTimeline();
    showArticle();
    updateStatus();
    scheduleSave();
    announceChanges();
}

void RssReaderFeature::renameFolder(const QString &name)
{
    bool ok = false;
    const QString renamed = QInputDialog::getText(this, QStringLiteral("Rename Folder"), QStringLiteral("Folder name"),
                                                  QLineEdit::Normal, name, &ok);
    if (!ok || renamed.trimmed() == name)
        return;
    QString error;
    if (!m_store.renameFolder(name, renamed, &error)) {
        QMessageBox::warning(this, QStringLiteral("Rename Folder"), error);
        return;
    }
    if (m_source.kind == SourceKind::Folder && m_source.id == name)
        m_source.id = m_store.folder(renamed)->name;
    rebuildSources();
    updateCounts();
    updateTimelineHeader();
    scheduleSave();
}

void RssReaderFeature::deleteFolder(const QString &name)
{
    int feeds = 0;
    for (const Feed &owner : m_store.feeds())
        feeds += owner.folder == name ? 1 : 0;
    QMessageBox box(QMessageBox::Question, QStringLiteral("Delete Folder"),
                    QStringLiteral("Delete the folder “%1”?").arg(name), QMessageBox::Cancel, this);
    if (feeds > 0)
        box.setInformativeText(QStringLiteral("Its %1 %2 stay subscribed and move out of the folder.")
                                   .arg(feeds)
                                   .arg(feeds == 1 ? QStringLiteral("feed") : QStringLiteral("feeds")));
    QPushButton *remove = box.addButton(QStringLiteral("Delete"), QMessageBox::DestructiveRole);
    box.exec();
    if (box.clickedButton() != remove)
        return;
    m_store.deleteFolder(name);
    if (m_source.kind == SourceKind::Folder && m_source.id == name)
        m_source = {SourceKind::AllUnread, QString()};
    rebuildSources();
    updateCounts();
    rebuildTimeline();
    scheduleSave();
}

void RssReaderFeature::newFolder(QWidget *parent)
{
    loadStore();
    bool ok = false;
    const QString name = QInputDialog::getText(parent, QStringLiteral("New Folder"), QStringLiteral("Folder name"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok)
        return;
    QString error;
    if (!m_store.addFolder(name, &error)) {
        QMessageBox::warning(parent, QStringLiteral("New Folder"), error);
        return;
    }
    rebuildSources();
    updateCounts();
    scheduleSave();
}

void RssReaderFeature::importOpml(QWidget *parent)
{
    loadStore();
    const QString path = QFileDialog::getOpenFileName(
        parent, QStringLiteral("Import Subscriptions"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStringLiteral("OPML files (*.opml *.xml);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 8 * 1024 * 1024) {
        QMessageBox::warning(parent, QStringLiteral("Import Subscriptions"),
                             QStringLiteral("%1 could not be read.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    const OpmlDocument document = parseOpml(file.readAll());
    if (!document.ok) {
        QMessageBox::warning(parent, QStringLiteral("Import Subscriptions"), document.error);
        return;
    }
    const OpmlImportResult result = m_store.importOpml(document, now(), true);
    m_store.logActivity(now(), QStringLiteral("Imported %1 feeds from %2").arg(result.added).arg(QFileInfo(path).fileName()));
    rebuildSources();
    updateCounts();
    scheduleSave();
    QMessageBox::information(parent, QStringLiteral("Import Subscriptions"), importSummary(result));
    checkFeeds(false);
}

QString RssReaderFeature::importSummary(const OpmlImportResult &result) const
{
    QString message = QStringLiteral("Imported %1 %2").arg(result.added).arg(result.added == 1 ? QStringLiteral("feed") : QStringLiteral("feeds"));
    if (result.folders > 0)
        message += QStringLiteral(" and %1 %2").arg(result.folders).arg(result.folders == 1 ? QStringLiteral("folder") : QStringLiteral("folders"));
    message += QLatin1Char('.');
    if (result.skipped > 0)
        message += QStringLiteral(" %1 skipped: already followed, not a web address, or over the %2-feed limit.")
                       .arg(result.skipped)
                       .arg(kMaxFeeds);
    return message;
}

void RssReaderFeature::importOpmlFromUrl(const QString &address, std::function<void(bool, const QString &)> done)
{
    loadStore();
    auto finish = [done](bool ok, const QString &message) {
        if (done)
            done(ok, message);
    };
    const QUrl url = normalizeAddress(address);
    if (!url.isValid()) {
        finish(false, QStringLiteral("Enter the web address of an OPML file."));
        return;
    }
    if (!m_feedQueue->available()) {
        finish(false, QStringLiteral("The desktop network is not available."));
        return;
    }
    FetchRequest request;
    request.url = url;
    request.accept = QByteArrayLiteral("text/x-opml, application/xml;q=0.9, text/xml;q=0.9, */*;q=0.5");
    m_feedQueue->get(request, [this, url, finish](const FetchResult &result) {
        if (!result.ok() || result.notModified) {
            finish(false, result.error.isEmpty() ? QStringLiteral("The OPML file could not be loaded.") : result.error);
            return;
        }
        const OpmlDocument document = parseOpml(result.body);
        if (!document.ok) {
            finish(false, document.error);
            return;
        }
        const qint64 at = now();
        const OpmlImportResult imported = m_store.importOpml(document, at, false);
        m_store.logActivity(at, QStringLiteral("Imported %1 feeds from %2").arg(imported.added).arg(url.toString()));
        rebuildSources();
        updateCounts();
        scheduleSave();
        checkFeeds(false);
        finish(true, importSummary(imported));
    });
}

void RssReaderFeature::importOpmlFromUrlDialog(QWidget *parent)
{
    loadStore();
    bool ok = false;
    const QString address = QInputDialog::getText(parent, QStringLiteral("Import from URL"),
                                                  QStringLiteral("Address of an OPML file"), QLineEdit::Normal,
                                                  QString(), &ok);
    if (!ok || address.trimmed().isEmpty())
        return;
    QPointer<QWidget> guard(parent);
    importOpmlFromUrl(address, [guard](bool, const QString &message) {
        if (guard)
            QMessageBox::information(guard, QStringLiteral("Import from URL"), message);
    });
}

void RssReaderFeature::exportOpml(QWidget *parent)
{
    loadStore();
    const QString suggested = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation))
                                  .filePath(QStringLiteral("RSS Reader Subscriptions.opml"));
    const QString path = QFileDialog::getSaveFileName(parent, QStringLiteral("Export Subscriptions"), suggested,
                                                      QStringLiteral("OPML files (*.opml)"));
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    const QByteArray bytes = writeOpml(m_store.feeds(), m_store.folders(), now());
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        QMessageBox::warning(parent, QStringLiteral("Export Subscriptions"),
                             QStringLiteral("%1 could not be written.").arg(QDir::toNativeSeparators(path)));
        return;
    }
    m_store.logActivity(now(), QStringLiteral("Exported %1 feeds to %2").arg(m_store.feeds().size()).arg(QFileInfo(path).fileName()));
    scheduleSave();
}

void RssReaderFeature::addBlogFeed(QWidget *parent)
{
    QPointer<QWidget> guard(parent);
    subscribe(forkMeshBlogFeed(), QString(), QString(), [guard](bool ok, const QString &message) {
        if (!ok && guard)
            QMessageBox::information(guard, QStringLiteral("Add the ForkMesh Blog Feed"), message);
    });
}

void RssReaderFeature::settingsChanged(bool timeline, bool article)
{
    m_store.markExportDirty();
    scheduleSave();
    if (timeline) {
        rebuildTimeline();
        m_timeline->doItemsLayout();
        m_timeline->viewport()->update();
    }
    if (article) {
        applyArticleStyle();
        showArticle();
    }
}

void RssReaderFeature::showLog(const QString &title, const QString &text)
{
    QDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(620, 420);
    auto *layout = new QVBoxLayout(&dialog);
    auto *view = new QPlainTextEdit;
    view->setReadOnly(true);
    view->setPlainText(text);
    layout->addWidget(view);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void RssReaderFeature::showSettings()
{
    loadStore();
    if (m_settingsDialog) {
        m_settingsDialog->raise();
        m_settingsDialog->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    m_settingsDialog = dialog;
    dialog->setObjectName(QStringLiteral("rssSettingsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("RSS Reader Settings"));
    dialog->setMinimumWidth(500);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(14, 12, 14, 12);
    auto *tabs = new QTabWidget;
    layout->addWidget(tabs);
    Settings &settings = m_store.settings();

    auto *feeds = new QWidget;
    auto *feedsLayout = new QVBoxLayout(feeds);
    feedsLayout->setContentsMargins(12, 14, 12, 12);
    feedsLayout->setSpacing(8);
    auto addButton = [](QVBoxLayout *target, const QString &text, const QString &hint) {
        auto *row = new QHBoxLayout;
        auto *button = new QPushButton(text);
        button->setMinimumWidth(230);
        row->addWidget(button);
        auto *label = new QLabel(hint);
        label->setObjectName(QStringLiteral("fieldHint"));
        label->setWordWrap(true);
        row->addWidget(label, 1);
        target->addLayout(row);
        return button;
    };
    QPushButton *import = addButton(feedsLayout, QStringLiteral("Import Subscriptions…"), QStringLiteral("Add the feeds in an OPML file."));
    QPushButton *importUrl = addButton(feedsLayout, QStringLiteral("Import from URL…"), QStringLiteral("Add the feeds in an OPML file on the web."));
    QPushButton *exportButton = addButton(feedsLayout, QStringLiteral("Export Subscriptions…"), QStringLiteral("Save your feeds and folders as OPML."));
    QPushButton *folder = addButton(feedsLayout, QStringLiteral("New Folder…"), QString());
    QPushButton *smartButton = addButton(feedsLayout, QStringLiteral("New Smart Feed…"), QStringLiteral("Collect articles by words, feed, age, unread or starred."));
    QPushButton *blog = addButton(feedsLayout, QStringLiteral("Add the ForkMesh Blog Feed"), forkMeshBlogFeed());
    auto *sortUnread = new QCheckBox(QStringLiteral("Sort by Unread Count"));
    sortUnread->setChecked(settings.sortFeedsByUnread);
    feedsLayout->addWidget(sortUnread);
    feedsLayout->addStretch();
    connect(sortUnread, &QCheckBox::toggled, this, [this](bool on) {
        updateSettings([on](Settings &value) { value.sortFeedsByUnread = on; }, false, false);
    });
    connect(importUrl, &QPushButton::clicked, dialog, [this, dialog] { importOpmlFromUrlDialog(dialog); });
    connect(smartButton, &QPushButton::clicked, dialog, [this] { showSmartFeedEditor(QString()); });
    connect(import, &QPushButton::clicked, dialog, [this, dialog] { importOpml(dialog); });
    connect(exportButton, &QPushButton::clicked, dialog, [this, dialog] { exportOpml(dialog); });
    connect(folder, &QPushButton::clicked, dialog, [this, dialog] { newFolder(dialog); });
    connect(blog, &QPushButton::clicked, dialog, [this, dialog] { addBlogFeed(dialog); });
    tabs->addTab(feeds, QStringLiteral("Feeds"));

    auto *timeline = new QWidget;
    auto *timelineLayout = new QVBoxLayout(timeline);
    timelineLayout->setContentsMargins(12, 14, 12, 12);
    timelineLayout->setSpacing(8);
    auto check = [this, &settings](QVBoxLayout *target, const QString &text, bool Settings::*field, bool timelineChange,
                                   bool articleChange) {
        auto *box = new QCheckBox(text);
        box->setChecked(settings.*field);
        connect(box, &QCheckBox::toggled, this, [this, field, timelineChange, articleChange](bool on) {
            m_store.settings().*field = on;
            settingsChanged(timelineChange, articleChange);
        });
        target->addWidget(box);
        return box;
    };
    check(timelineLayout, QStringLiteral("Sort Oldest to Newest"), &Settings::oldestFirst, true, false);
    check(timelineLayout, QStringLiteral("Group by Feed"), &Settings::groupByFeed, true, false);
    check(timelineLayout, QStringLiteral("Refresh to Clear Read Articles"), &Settings::refreshClearsRead, false, false);
    check(timelineLayout, QStringLiteral("Confirm Mark All as Read"), &Settings::confirmMarkAllRead, false, false);
    check(timelineLayout, QStringLiteral("Mark All as Read Skips Starred"), &Settings::markAllKeepsStarred, false, false);
    auto *hideRead = new QCheckBox(QStringLiteral("Hide Read Articles"));
    hideRead->setChecked(settings.unreadOnlyTimeline);
    timelineLayout->addWidget(hideRead);
    connect(hideRead, &QCheckBox::toggled, this, [this](bool on) {
        if (m_unreadTimelineButton->isChecked() != on)
            m_unreadTimelineButton->setChecked(on);
    });
    connect(m_unreadTimelineButton, &QToolButton::toggled, hideRead, [hideRead](bool on) {
        if (hideRead->isChecked() != on)
            hideRead->setChecked(on);
    });
    check(timelineLayout, QStringLiteral("Show Feed Icons"), &Settings::showFeedIcons, true, false);
    check(timelineLayout, QStringLiteral("Show Thumbnails"), &Settings::showThumbnails, true, false);
    auto *linesRow = new QHBoxLayout;
    linesRow->addWidget(new QLabel(QStringLiteral("Summary lines")));
    auto *lines = new QSpinBox;
    lines->setRange(0, 4);
    lines->setValue(settings.summaryLines);
    linesRow->addWidget(lines);
    linesRow->addStretch();
    timelineLayout->addLayout(linesRow);
    timelineLayout->addStretch();
    connect(lines, &QSpinBox::valueChanged, this, [this](int value) {
        m_store.settings().summaryLines = value;
        settingsChanged(true, false);
    });
    tabs->addTab(timeline, QStringLiteral("Timeline"));

    auto *articles = new QWidget;
    auto *articlesLayout = new QFormLayout(articles);
    articlesLayout->setContentsMargins(12, 14, 12, 12);
    articlesLayout->setSpacing(8);
    auto *theme = new QComboBox;
    const std::pair<QString, QString> themes[] = {{QStringLiteral("default"), QStringLiteral("Default")},
                                                  {QStringLiteral("sepia"), QStringLiteral("Sepia")},
                                                  {QStringLiteral("paper"), QStringLiteral("Paper")},
                                                  {QStringLiteral("contrast"), QStringLiteral("High contrast")}};
    for (const auto &entry : themes)
        theme->addItem(entry.second, entry.first);
    theme->setCurrentIndex(qMax(0, theme->findData(settings.theme)));
    auto *font = new QComboBox;
    font->addItem(QStringLiteral("System"), QString());
    for (const QString &family : QFontDatabase::families())
        font->addItem(family, family);
    font->setCurrentIndex(qMax(0, font->findData(settings.fontFamily)));
    font->setMaxVisibleItems(16);
    auto *size = new QSpinBox;
    size->setRange(10, 32);
    size->setSuffix(QStringLiteral(" px"));
    size->setValue(settings.textSize);
    auto *images = new QCheckBox(QStringLiteral("Load Remote Images"));
    images->setChecked(settings.loadRemoteImages);
    auto *imagesHint = new QLabel(QStringLiteral("Off by default: a remote image tells its server that you read the article."));
    imagesHint->setObjectName(QStringLiteral("fieldHint"));
    imagesHint->setWordWrap(true);
    articlesLayout->addRow(QStringLiteral("Theme"), theme);
    articlesLayout->addRow(QStringLiteral("Font"), font);
    articlesLayout->addRow(QStringLiteral("Text size"), size);
    articlesLayout->addRow(QString(), images);
    articlesLayout->addRow(QString(), imagesHint);
    connect(theme, &QComboBox::currentIndexChanged, this, [this, theme] {
        m_store.settings().theme = theme->currentData().toString();
        settingsChanged(false, true);
    });
    connect(font, &QComboBox::currentIndexChanged, this, [this, font] {
        m_store.settings().fontFamily = font->currentData().toString();
        settingsChanged(false, true);
    });
    connect(size, &QSpinBox::valueChanged, this, [this](int value) {
        m_store.settings().textSize = value;
        settingsChanged(false, true);
    });
    connect(images, &QCheckBox::toggled, this, [this](bool on) {
        m_store.settings().loadRemoteImages = on;
        settingsChanged(false, true);
    });
    tabs->addTab(articles, QStringLiteral("Articles"));

    auto *filters = new QWidget;
    auto *filtersLayout = new QVBoxLayout(filters);
    filtersLayout->setContentsMargins(12, 14, 12, 12);
    filtersLayout->setSpacing(8);
    auto *fold = new QCheckBox(QStringLiteral("Mark Duplicate Links as Read"));
    fold->setChecked(settings.foldDuplicates);
    filtersLayout->addWidget(fold);
    auto *filterList = new QListWidget;
    filterList->setObjectName(QStringLiteral("rssFilterList"));
    filtersLayout->addWidget(filterList, 1);
    auto *filterButtons = new QHBoxLayout;
    auto *addFilter = new QPushButton(QStringLiteral("Add…"));
    auto *editFilter = new QPushButton(QStringLiteral("Edit…"));
    auto *deleteFilter = new QPushButton(QStringLiteral("Delete"));
    filterButtons->addWidget(addFilter);
    filterButtons->addWidget(editFilter);
    filterButtons->addWidget(deleteFilter);
    filterButtons->addStretch();
    filtersLayout->addLayout(filterButtons);
    auto fillFilters = [this, filterList, editFilter, deleteFilter, addFilter] {
        const QString selected = filterList->currentItem() ? filterList->currentItem()->data(Qt::UserRole).toString() : QString();
        filterList->clear();
        for (const ContentFilter &filter : m_store.settings().filters) {
            const Feed *owner = filter.feedId.isEmpty() ? nullptr : m_store.feed(filter.feedId);
            const QString feedName = filter.feedId.isEmpty() ? QStringLiteral("All Feeds")
                                     : owner                 ? owner->displayName()
                                                             : QStringLiteral("Removed feed");
            QString pattern = QStringLiteral("“%1”").arg(filter.pattern);
            if (filter.regex)
                pattern += QStringLiteral(" (regular expression)");
            if (filter.regex && !TextMatcher(filter.pattern, true).isValid())
                pattern += QStringLiteral(" · Invalid regular expression");
            auto *row = new QListWidgetItem(QStringLiteral("%1 · %2 · %3 · %4")
                                                .arg(pattern, filterFieldLabel(filter.field),
                                                     filterActionLabel(filter.action), feedName));
            row->setData(Qt::UserRole, filter.id);
            filterList->addItem(row);
            if (filter.id == selected)
                filterList->setCurrentItem(row);
        }
        const bool has = filterList->currentItem() != nullptr;
        editFilter->setEnabled(has);
        deleteFilter->setEnabled(has);
        addFilter->setEnabled(m_store.settings().filters.size() < kMaxFilters);
    };
    fillFilters();
    connect(filterList, &QListWidget::currentItemChanged, dialog, [editFilter, deleteFilter](QListWidgetItem *item) {
        editFilter->setEnabled(item != nullptr);
        deleteFilter->setEnabled(item != nullptr);
    });
    connect(addFilter, &QPushButton::clicked, dialog, [this, dialog, fillFilters] {
        if (showFilterEditor(dialog, QString()))
            fillFilters();
    });
    auto edit = [this, dialog, filterList, fillFilters] {
        if (!filterList->currentItem())
            return;
        if (showFilterEditor(dialog, filterList->currentItem()->data(Qt::UserRole).toString()))
            fillFilters();
    };
    connect(editFilter, &QPushButton::clicked, dialog, edit);
    connect(filterList, &QListWidget::itemDoubleClicked, dialog, edit);
    connect(deleteFilter, &QPushButton::clicked, dialog, [this, filterList, fillFilters] {
        if (!filterList->currentItem())
            return;
        m_store.removeFilter(filterList->currentItem()->data(Qt::UserRole).toString());
        filtersEdited();
        fillFilters();
    });
    connect(fold, &QCheckBox::toggled, this, [this](bool on) {
        m_store.settings().foldDuplicates = on;
        settingsChanged(false, false);
    });
    tabs->addTab(filters, QStringLiteral("Filters"));

    auto *trouble = new QWidget;
    auto *troubleLayout = new QVBoxLayout(trouble);
    troubleLayout->setContentsMargins(12, 14, 12, 12);
    troubleLayout->setSpacing(8);
    QPushButton *errors = addButton(troubleLayout, QStringLiteral("Error Log"), QStringLiteral("Feeds that could not be checked, newest first."));
    QPushButton *activity = addButton(troubleLayout, QStringLiteral("Activity Log"), QStringLiteral("Refreshes, subscriptions and imports."));
    QPushButton *stats = addButton(troubleLayout, QStringLiteral("Stats"), QStringLiteral("What RSS Reader keeps on this computer."));
    troubleLayout->addStretch();
    auto logText = [](const QList<LogEntry> &log) {
        QStringList lines;
        for (const LogEntry &entry : log)
            lines << QStringLiteral("%1  %2").arg(whenText(entry.atMs), entry.text);
        return lines.isEmpty() ? QStringLiteral("Nothing yet.") : lines.join(QLatin1Char('\n'));
    };
    connect(errors, &QPushButton::clicked, dialog, [this, logText] { showLog(QStringLiteral("Error Log"), logText(m_store.errorLog())); });
    connect(activity, &QPushButton::clicked, dialog, [this, logText] { showLog(QStringLiteral("Activity Log"), logText(m_store.activityLog())); });
    connect(stats, &QPushButton::clicked, dialog, [this] {
        saveNow();
        const Stats figures = m_store.stats();
        const QLocale locale;
        const QString text = QStringLiteral(
                                 "Feeds: %1 of %2\nFolders: %3 of %4\nArticles kept: %5\nUnread: %6\nStarred: %7\n"
                                 "Feeds with errors: %8\nStorage: %9\nData folder: %10")
                                 .arg(locale.toString(figures.feeds), locale.toString(kMaxFeeds),
                                      locale.toString(figures.folders), locale.toString(kMaxFolders),
                                      locale.toString(figures.articles), locale.toString(figures.unread),
                                      locale.toString(figures.starred), locale.toString(figures.failing),
                                      sizeLabel(m_disk->sizeOnDisk()))
                                 .arg(QDir::toNativeSeparators(m_disk->directory()));
        showLog(QStringLiteral("Stats"), text);
    });
    tabs->addTab(trouble, QStringLiteral("Troubleshooting"));

    auto *about = new QWidget;
    auto *aboutLayout = new QVBoxLayout(about);
    aboutLayout->setContentsMargins(12, 14, 12, 12);
    auto *aboutTitle = new QLabel(QStringLiteral("About RSS Reader"));
    aboutTitle->setObjectName(QStringLiteral("groupHeading"));
    aboutLayout->addWidget(aboutTitle);
    auto *aboutText = new QLabel;
    aboutText->setObjectName(QStringLiteral("rssAboutText"));
    aboutText->setWordWrap(true);
    aboutText->setTextFormat(Qt::RichText);
    aboutText->setOpenExternalLinks(true);
    aboutText->setText(QStringLiteral(
        "<p>RSS Reader follows sites by their RSS, Atom and JSON feeds. Your feeds stay on this computer "
        "and are checked from it; nothing is sent to ForkMesh.</p>"
        "<p>Inspired by NetNewsWire, the free and open source RSS reader created by Brent Simmons and "
        "developed by Ranchero Software and its contributors (MIT licensed). RSS Reader shares no code with "
        "NetNewsWire and is not affiliated with or endorsed by it.</p>"
        "<p>RSS Reader&#39;s smart feeds, timeline, article view and settings follow NetNewsWire&#39;s design, and its custom smart feeds, Read Later, content filters, timeline thumbnails, Media RSS images and other recent features were chosen from the feature requests NetNewsWire&#39;s users have filed on GitHub. Thanks to Brent Simmons and the NetNewsWire community.</p>"
        "<p><a href=\"https://netnewswire.com/\">netnewswire.com</a> · "
        "<a href=\"https://github.com/Ranchero-Software/NetNewsWire\">github.com/Ranchero-Software/NetNewsWire</a> · "
        "<a href=\"https://github.com/Ranchero-Software/NetNewsWire/issues\">NetNewsWire feature requests</a></p>"));
    aboutLayout->addWidget(aboutText);
    aboutLayout->addStretch();
    tabs->addTab(about, QStringLiteral("About"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->show();
}

QPixmap RssReaderFeature::letterTile(const QString &name, int side) const
{
    QChar letter = QLatin1Char('#');
    for (const QChar c : name) {
        if (c.isLetterOrNumber()) {
            letter = c.toUpper();
            break;
        }
    }
    const qreal ratio = devicePixelRatioF();
    const QString key = QStringLiteral("%1|%2|%3").arg(letter).arg(side).arg(ratio);
    const auto found = m_tiles.constFind(key);
    if (found != m_tiles.constEnd())
        return found.value();
    QPixmap pixmap(QSize(side, side) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor fill = palette().color(QPalette::PlaceholderText);
    fill.setAlphaF(0.28);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(QRectF(0, 0, side, side), side * 0.22, side * 0.22);
    QFont font = this->font();
    font.setPixelSize(qMax(7, int(side * 0.58)));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(QRectF(0, 0, side, side), Qt::AlignCenter, QString(letter));
    painter.end();
    m_tiles.insert(key, pixmap);
    return pixmap;
}

QPixmap RssReaderFeature::feedIcon(const QString &feedId, int side) const
{
    const qreal ratio = devicePixelRatioF();
    const QString scaledKey = QStringLiteral("%1|%2|%3").arg(feedId).arg(side).arg(ratio);
    const auto scaled = m_scaledIcons.constFind(scaledKey);
    if (scaled != m_scaledIcons.constEnd())
        return scaled.value();
    auto found = m_icons.constFind(feedId);
    if (found == m_icons.constEnd() && !m_iconMisses.contains(feedId) && m_disk) {
        const QPixmap loaded(m_disk->iconPath(feedId));
        if (!loaded.isNull())
            found = m_icons.insert(feedId, loaded);
        else
            m_iconMisses.insert(feedId);
    }
    if (found == m_icons.constEnd()) {
        const Feed *owner = m_store.feed(feedId);
        return letterTile(owner ? owner->displayName() : QString(), side);
    }
    QPixmap result = found.value().scaled(QSize(side, side) * ratio, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    result.setDevicePixelRatio(ratio);
    m_scaledIcons.insert(scaledKey, result);
    return result;
}

struct IconSearch {
    QString feedId;
    QStringList candidates;
    QStringList tried;
    bool pageChecked = false;
    QImage fallback;
};

void RssReaderFeature::fetchMissingIcons()
{
    if (!m_disk || !m_assetQueue)
        return;
    const qint64 at = now();
    QList<std::shared_ptr<IconSearch>> searches;
    for (const Feed &owner : m_store.feeds()) {
        if (m_iconAttempts.contains(owner.id) ||
            !iconFetchDue(owner, QFile::exists(m_disk->iconPath(owner.id)), at))
            continue;
        auto search = std::make_shared<IconSearch>();
        search->feedId = owner.id;
        if (!owner.iconUrl.isEmpty())
            search->candidates << owner.iconUrl;
        searches.append(search);
    }
    for (const auto &search : std::as_const(searches)) {
        m_iconAttempts.insert(search->feedId);
        m_store.recordIconAttempt(search->feedId, at);
    }
    for (const auto &search : std::as_const(searches))
        fetchIcon(search);
    if (!searches.isEmpty())
        scheduleSave();
}

void RssReaderFeature::fetchIcon(const std::shared_ptr<IconSearch> &search)
{
    if (!m_store.feed(search->feedId)) {
        m_iconAttempts.remove(search->feedId);
        return;
    }
    while (!search->candidates.isEmpty() && search->tried.contains(search->candidates.first()))
        search->candidates.removeFirst();
    if (search->candidates.isEmpty()) {
        if (!search->pageChecked) {
            search->pageChecked = true;
            fetchIconPage(search);
            return;
        }
        if (!search->fallback.isNull())
            storeIcon(search->feedId, search->fallback);
        m_iconAttempts.remove(search->feedId);
        return;
    }
    const QString url = search->candidates.takeFirst();
    search->tried << url;
    FetchRequest request;
    request.url = QUrl(url);
    request.maxBytes = kMaxIconBytes;
    request.accept = QByteArrayLiteral("image/png,image/x-icon,image/vnd.microsoft.icon,image/*;q=0.8");
    m_assetQueue->get(request, [this, search](const FetchResult &result) {
        const QImage image = result.ok() && !result.notModified ? decodeIcon(result.body) : QImage();
        if (visibleIcon(image) && m_store.feed(search->feedId)) {
            if (squareEnough(image)) {
                storeIcon(search->feedId, image);
                m_iconAttempts.remove(search->feedId);
                return;
            }
            if (search->fallback.isNull())
                search->fallback = image;
        }
        fetchIcon(search);
    });
}

void RssReaderFeature::fetchIconPage(const std::shared_ptr<IconSearch> &search)
{
    const Feed *owner = m_store.feed(search->feedId);
    if (!owner) {
        m_iconAttempts.remove(search->feedId);
        return;
    }
    const QUrl page = iconPageUrl(*owner);
    if (!page.isValid()) {
        search->candidates = fallbackIconUrls(QUrl(owner->homePage), QUrl(owner->url));
        fetchIcon(search);
        return;
    }
    FetchRequest request;
    request.url = page;
    request.accept = QByteArrayLiteral("text/html,application/xhtml+xml;q=0.9,*/*;q=0.5");
    m_assetQueue->get(request, [this, search, page](const FetchResult &result) {
        const Feed *current = m_store.feed(search->feedId);
        if (!current) {
            m_iconAttempts.remove(search->feedId);
            return;
        }
        QStringList candidates;
        if (result.ok() && !result.notModified && looksLikeHtml(result.body, result.contentType))
            candidates = discoverIconLinks(QString::fromUtf8(result.body),
                                           result.finalUrl.isValid() ? result.finalUrl : page,
                                           svgIconsSupported());
        candidates += fallbackIconUrls(QUrl(current->homePage), QUrl(current->url));
        search->candidates = candidates;
        fetchIcon(search);
    });
}

void RssReaderFeature::storeIcon(const QString &feedId, const QImage &source)
{
    if (!m_disk)
        return;
    QImage image = source;
    if (image.width() > kIconSide || image.height() > kIconSide)
        image = image.scaled(kIconSide, kIconSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    const QString path = m_disk->iconPath(feedId);
    QDir().mkpath(QFileInfo(path).absolutePath());
    image.save(path, "PNG");
    m_icons.insert(feedId, QPixmap::fromImage(image));
    m_iconMisses.remove(feedId);
    m_scaledIcons.clear();
    m_sources->viewport()->update();
    m_timeline->viewport()->update();
    if (m_article.feedId == feedId)
        m_renderTimer->start();
}

void RssReaderFeature::setHostSearchQuery(const QString &query)
{
    loadStore();
    if (m_searchField->text() != query)
        m_searchField->setText(query);
}

QList<forkmesh::market::NativePackageSearchHit> RssReaderFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (!m_loaded || needle.size() < 2 || limit <= 0)
        return hits;
    const QChar separator(0x1f);
    for (const Feed &owner : m_store.feeds()) {
        if (hits.size() >= limit)
            return hits;
        if (owner.displayName().contains(needle, Qt::CaseInsensitive)) {
            const int unread = m_store.feedUnread(owner.id);
            hits.append({QStringLiteral("f") + separator + owner.id, owner.displayName(),
                         unread > 0 ? QStringLiteral("Feed · %1 unread").arg(unread) : QStringLiteral("Feed")});
        }
    }
    const qint64 at = now();
    for (const Feed &owner : m_store.feeds()) {
        for (const Article &item : m_store.articles(owner.id)) {
            if (hits.size() >= limit)
                return hits;
            if (!item.title.contains(needle, Qt::CaseInsensitive) || m_store.isHidden(item))
                continue;
            hits.append({QStringLiteral("a") + separator + owner.id + separator + item.guid, item.title,
                         QStringLiteral("%1 · %2").arg(owner.displayName(), timelineDateText(item.publishedMs, at))});
        }
    }
    return hits;
}

void RssReaderFeature::activateSearchHit(const QString &id)
{
    loadStore();
    const QStringList parts = id.split(QChar(0x1f));
    if (parts.size() >= 2 && parts.first() == QLatin1String("f")) {
        selectSource({SourceKind::Feed, parts.at(1)});
        return;
    }
    if (parts.size() >= 3 && parts.first() == QLatin1String("a")) {
        const ArticleRef ref{parts.at(1), parts.mid(2).join(QChar(0x1f))};
        if (!m_store.article(ref))
            return;
        if (!m_search.isEmpty())
            m_searchField->clear();
        selectSource({SourceKind::Feed, ref.feedId});
        m_keptRead.insert(keptKey(ref));
        rebuildTimeline();
        selectArticle(ref);
    }
}

}

namespace {
const forkmesh::market::NativePackageRegistration rssReaderRegistration({
    QStringLiteral("builtin:rss-reader"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Reader"),
    QStringLiteral("broadcast"),
    QString(),
    QString(),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::rssreader::RssReaderFeature(host, parent);
    },
    QString(),
});
}
