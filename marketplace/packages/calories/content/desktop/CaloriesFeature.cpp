#include "CaloriesFeature.h"
#include "DesignTokens.h"

#include <QAction>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QFormLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QImage>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStyle>
#include <QTime>
#include <QUrl>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSet>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimeEdit>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <utility>

namespace forkmesh::calories {
namespace {

constexpr double kKgPerLb = 0.45359237;
constexpr double kCmPerIn = 2.54;
constexpr int kEntryIdRole = Qt::UserRole + 1;

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().trimmed();
}

double number(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toDouble();
}

QString grams(double value)
{
    return QString::number(value, 'f', value == std::floor(value) ? 0 : 1);
}

QString kcal(double value)
{
    return QLocale().toString(qRound(value));
}

QString dayLabel(const QString &day)
{
    const QDate date = QDate::fromString(day, Qt::ISODate);
    return date.isValid() ? QLocale().toString(date, QStringLiteral("ddd MMM d")) : day;
}

QString mealLabel(const QString &meal)
{
    if (meal.isEmpty())
        return QStringLiteral("Snack");
    QString result = meal;
    result[0] = result.at(0).toUpper();
    return result;
}

QString guessMeal()
{
    const int hour = QTime::currentTime().hour();
    if (hour < 11)
        return QStringLiteral("breakfast");
    if (hour < 15)
        return QStringLiteral("lunch");
    if (hour < 21)
        return QStringLiteral("dinner");
    return QStringLiteral("snack");
}

QComboBox *mealCombo(const QString &selected)
{
    auto *combo = new QComboBox;
    for (const char *meal : {"breakfast", "lunch", "dinner", "snack"})
        combo->addItem(mealLabel(QLatin1String(meal)), QLatin1String(meal));
    const int index = combo->findData(selected);
    combo->setCurrentIndex(index >= 0 ? index : 3);
    return combo;
}

QString kindLabel(const QString &kind)
{
    if (kind == QLatin1String("weight_loss_percent"))
        return QStringLiteral("Lose the most percent of body weight");
    if (kind == QLatin1String("weight_loss_kg"))
        return QStringLiteral("Lose the most weight");
    if (kind == QLatin1String("calorie_streak"))
        return QStringLiteral("Most days within the calorie goal");
    if (kind == QLatin1String("log_streak"))
        return QStringLiteral("Most days logged");
    return kind;
}

QString statusLabel(const QString &status)
{
    if (status == QLatin1String("active"))
        return QStringLiteral("Active");
    if (status == QLatin1String("upcoming"))
        return QStringLiteral("Upcoming");
    if (status == QLatin1String("closed"))
        return QStringLiteral("Finished");
    return status;
}

QString mineLabel(const QString &status)
{
    if (status == QLatin1String("invited"))
        return QStringLiteral("Invited");
    if (status == QLatin1String("accepted"))
        return QStringLiteral("In");
    if (status == QLatin1String("declined"))
        return QStringLiteral("Declined");
    if (status == QLatin1String("left"))
        return QStringLiteral("Left");
    return QString::fromUtf8("\xE2\x80\x94");
}

QLineEdit *lineEdit(const QString &value = QString())
{
    auto *edit = new QLineEdit(value);
    edit->setClearButtonEnabled(true);
    return edit;
}

// Friends are invited by ForkMesh username: "alice, @bob carol" becomes
// ["alice", "bob", "carol"]. The Worker lowercases and validates each name.
QJsonArray usernames(const QString &value)
{
    QJsonArray names;
    QStringList seen;
    static const QRegularExpression separators(QStringLiteral("[\\s,;]+"));
    for (QString name : value.split(separators, Qt::SkipEmptyParts)) {
        while (name.startsWith(QLatin1Char('@')))
            name.remove(0, 1);
        name = name.toLower();
        if (name.isEmpty() || seen.contains(name))
            continue;
        seen << name;
        names.append(name);
    }
    return names;
}

QStringList names(const QJsonValue &value)
{
    QStringList result;
    for (const QJsonValue &item : value.toArray())
        if (!item.toString().trimmed().isEmpty())
            result << item.toString().trimmed();
    return result;
}

QDoubleSpinBox *gramsBox(double value)
{
    auto *box = new QDoubleSpinBox;
    box->setRange(0, 5000);
    box->setDecimals(1);
    box->setValue(value);
    return box;
}

QLabel *mutedLabel(const QString &value)
{
    auto *label = new QLabel(value);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QProgressBar *goalBar()
{
    auto *bar = new QProgressBar;
    bar->setRange(0, 1000);
    bar->setTextVisible(false);
    bar->setFixedHeight(8);
    return bar;
}

void setGoalBar(QProgressBar *bar, double value, double goal)
{
    bar->setValue(goal > 0 ? int(qBound(0.0, value / goal, 1.0) * 1000) : 0);
}

// The dataviz reference accent (slot 1) for one emphasised series, the
// slot-2 orange only for days over the goal, and a dashed slot-4 goal line.
QColor accentColor(bool dark) { return QColor(dark ? "#3987e5" : "#2a78d6"); }
QColor overColor(bool dark) { return QColor(dark ? "#d95926" : "#eb6834"); }
QColor goalColor(bool dark) { return QColor(dark ? "#c98500" : "#eda100"); }
QColor weightColor(bool dark) { return QColor(dark ? "#199e70" : "#1baf7a"); }

constexpr int kQuickListRole = Qt::UserRole + 2;
constexpr int kQuickIndexRole = Qt::UserRole + 3;

QString foodKey(const QString &name)
{
    return name.trimmed().toLower();
}

QString singularWord(QString word)
{
    static const QRegularExpression plural(QStringLiteral("(ch|sh|ss|x)es$"));
    static const QRegularExpression trailing(QStringLiteral("([^s])s$"));
    word.replace(plural, QStringLiteral("\\1"));
    word.replace(trailing, QStringLiteral("\\1"));
    return word;
}

QString normaliseFood(const QString &name)
{
    static const QRegularExpression junk(QStringLiteral("[^a-z0-9 ]+"));
    QString cleaned = name.toLower();
    cleaned.replace(junk, QStringLiteral(" "));
    QStringList words;
    for (const QString &word : cleaned.split(QLatin1Char(' '), Qt::SkipEmptyParts))
        words << singularWord(word);
    return words.join(QLatin1Char(' '));
}

QString promptMeal(const QString &value)
{
    static const QRegularExpression meal(
        QStringLiteral("\\b(breakfast|brunch|lunch|dinner|supper|snacks?|dessert)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    const QString word = meal.match(value).captured(1).toLower();
    if (word == QLatin1String("brunch"))
        return QStringLiteral("breakfast");
    if (word == QLatin1String("supper"))
        return QStringLiteral("dinner");
    if (word == QLatin1String("snacks") || word == QLatin1String("dessert"))
        return QStringLiteral("snack");
    return word;
}

QString promptDay(const QString &value, const QString &fallback)
{
    static const QRegularExpression yesterday(QStringLiteral("\\byesterday\\b"),
                                              QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression today(
        QStringLiteral("\\btoday\\b|\\bthis (morning|afternoon|evening)\\b|\\btonight\\b"),
        QRegularExpression::CaseInsensitiveOption);
    if (yesterday.match(value).hasMatch())
        return QDate::currentDate().addDays(-1).toString(Qt::ISODate);
    if (today.match(value).hasMatch())
        return QDate::currentDate().toString(Qt::ISODate);
    return fallback;
}

QStringList promptParts(const QString &value)
{
    static const QRegularExpression mealWith(
        QStringLiteral("\\b(for|at|with)\\s+(breakfast|brunch|lunch|dinner|supper|snacks?|dessert)\\b"));
    static const QRegularExpression mealColon(
        QStringLiteral("\\b(breakfast|brunch|lunch|dinner|supper|snacks?|dessert)\\s*:"));
    static const QRegularExpression days(
        QStringLiteral("\\b(yesterday|today|tonight|this (morning|afternoon|evening))\\b"));
    static const QRegularExpression lead(
        QStringLiteral("^\\s*(i\\s+)?(just\\s+)?(had|ate|drank|log|add)\\b"));
    static const QRegularExpression separators(
        QStringLiteral(",|;|\\n|\\+|&|\\band\\b"));
    QString cleaned = value.toLower();
    cleaned.replace(mealWith, QStringLiteral(" "));
    cleaned.replace(mealColon, QStringLiteral(" "));
    cleaned.replace(days, QStringLiteral(" "));
    cleaned.replace(lead, QStringLiteral(" "));
    QStringList parts;
    for (const QString &part : cleaned.split(separators)) {
        if (!part.trimmed().isEmpty())
            parts << part.trimmed();
    }
    return parts;
}

double numberWord(const QString &word)
{
    static const QHash<QString, double> words{
        {QStringLiteral("a"), 1}, {QStringLiteral("an"), 1}, {QStringLiteral("one"), 1},
        {QStringLiteral("two"), 2}, {QStringLiteral("three"), 3}, {QStringLiteral("four"), 4},
        {QStringLiteral("five"), 5}, {QStringLiteral("six"), 6}, {QStringLiteral("seven"), 7},
        {QStringLiteral("eight"), 8}, {QStringLiteral("nine"), 9}, {QStringLiteral("ten"), 10},
        {QStringLiteral("half"), 0.5}, {QStringLiteral("a half"), 0.5},
        {QStringLiteral("couple"), 2}, {QStringLiteral("a couple of"), 2},
        {QStringLiteral("a few"), 3}};
    return words.value(word, 0);
}

QJsonObject foodFields(const QJsonObject &food)
{
    QJsonObject result;
    for (const char *key : {"name", "brand", "portion", "calories", "proteinG", "carbsG", "fatG"})
        result.insert(QLatin1String(key), food.value(QLatin1String(key)));
    return result;
}

QString jpegDataUrl(const QString &path, int maxSide)
{
    QImage image(path);
    if (image.isNull())
        return QString();
    if (image.width() > maxSide || image.height() > maxSide)
        image = image.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QByteArray bytes;
    int quality = 85;
    do {
        bytes.clear();
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.convertToFormat(QImage::Format_RGB888).save(&buffer, "JPEG", quality);
        quality -= 15;
    } while (bytes.size() > 1000000 && quality > 30);
    return QStringLiteral("data:image/jpeg;base64,") + QString::fromLatin1(bytes.toBase64());
}

QString aiError(const QJsonObject &payload, const QString &error, const QString &what)
{
    const QString code = text(payload, "error");
    if (code == QLatin1String("rate_limited"))
        return QStringLiteral("You have used this hour's AI reads. Try again later, or use Quick add.");
    if (code == QLatin1String("ai_unavailable"))
        return QStringLiteral("AI estimates are unavailable on this host. Use foods from Quick add, like “2 eggs and a banana”.");
    if (code == QLatin1String("nothing_identified"))
        return QStringLiteral("Nothing recognisable as food there. Try naming the food and amount.");
    if (code == QLatin1String("no_foods_on_receipt"))
        return QStringLiteral("No foods were found on that receipt. Try a sharper, flatter photo.");
    return QStringLiteral("%1: %2").arg(what, error);
}

constexpr int kGroupHeadingRole = Qt::UserRole + 10;
constexpr int kGroupKcalRole = Qt::UserRole + 11;
constexpr int kNextGroupBand = 36;
constexpr int kNextEntryRow = 40;

QFont caloriesFont(const QFont &base, int pixelSize, QFont::Weight weight)
{
    QFont font(base);
    font.setPixelSize(pixelSize);
    font.setWeight(weight);
    return font;
}

QIcon caloriesOcticon(const QString &name, const QColor &color, int side)
{
    QString path = QStringLiteral(":/icons/octicons/%1.svg").arg(name);
    if (!QFile::exists(path))
        path = QStringLiteral(":/icons/codicons/%1.svg").arg(name);
    QSvgRenderer renderer(path);
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(side, side) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        if (renderer.isValid()) {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing);
            renderer.render(&painter, QRectF(0, 0, side, side));
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.fillRect(QRectF(0, 0, side, side), color);
        }
        icon.addPixmap(pixmap);
    }
    return icon;
}

QIcon caloriesGlyph(const QString &kind, const QColor &color, int side)
{
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(side, side) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(side / 16.0, side / 16.0);
        painter.setPen(QPen(color, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        if (kind == QLatin1String("camera")) {
            QPainterPath body;
            body.moveTo(5.2, 4.2);
            body.lineTo(6.2, 2.6);
            body.lineTo(9.8, 2.6);
            body.lineTo(10.8, 4.2);
            body.lineTo(13, 4.2);
            body.quadTo(14.4, 4.2, 14.4, 5.6);
            body.lineTo(14.4, 12);
            body.quadTo(14.4, 13.4, 13, 13.4);
            body.lineTo(3, 13.4);
            body.quadTo(1.6, 13.4, 1.6, 12);
            body.lineTo(1.6, 5.6);
            body.quadTo(1.6, 4.2, 3, 4.2);
            body.closeSubpath();
            painter.drawPath(body);
            painter.drawEllipse(QPointF(8, 8.6), 2.5, 2.5);
        } else if (kind == QLatin1String("barcode")) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            const qreal bars[][2] = {{2, 1.2}, {4.2, 0.7}, {5.8, 1.6}, {8.4, 0.7}, {10, 1.3}, {12.3, 0.7}, {13.6, 1.2}};
            for (const auto &bar : bars)
                painter.drawRect(QRectF(bar[0], 3, bar[1], 10));
        } else if (kind == QLatin1String("scale")) {
            painter.drawLine(QPointF(8, 2.4), QPointF(8, 13.6));
            painter.drawLine(QPointF(5, 13.6), QPointF(11, 13.6));
            painter.drawLine(QPointF(2.6, 4.4), QPointF(13.4, 4.4));
            for (const qreal centre : {3.2, 12.8}) {
                QPainterPath pan;
                pan.moveTo(centre, 4.4);
                pan.lineTo(centre - 2.2, 9.6);
                pan.quadTo(centre, 11.6, centre + 2.2, 9.6);
                pan.lineTo(centre, 4.4);
                painter.drawPath(pan);
            }
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

int firstVisibleColumn(const QTableView *view)
{
    const QHeaderView *header = view->horizontalHeader();
    for (int visual = 0; visual < header->count(); ++visual) {
        const int logical = header->logicalIndex(visual);
        if (!header->isSectionHidden(logical))
            return logical;
    }
    return 0;
}

bool rowUnderCursor(const QAbstractItemView *view, const QModelIndex &index)
{
    QWidget *viewport = view ? view->viewport() : nullptr;
    if (!viewport || !viewport->underMouse())
        return false;
    const QModelIndex hovered = view->indexAt(viewport->mapFromGlobal(QCursor::pos()));
    return hovered.isValid() && hovered.row() == index.row() && hovered.parent() == index.parent();
}

class CaloriesHoverRepaint final : public QObject
{
public:
    explicit CaloriesHoverRepaint(QAbstractItemView *view) : QObject(view), m_view(view)
    {
        view->viewport()->setAttribute(Qt::WA_Hover, true);
        view->viewport()->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        if (Theme::isNext() && (type == QEvent::MouseMove || type == QEvent::Leave ||
                                type == QEvent::HoverMove || type == QEvent::HoverLeave)) {
            const QModelIndex hovered =
                m_view->indexAt(m_view->viewport()->mapFromGlobal(QCursor::pos()));
            const int row = type == QEvent::Leave || type == QEvent::HoverLeave || !hovered.isValid()
                                ? -1 : hovered.row();
            if (row != m_row) {
                m_row = row;
                m_view->viewport()->update();
            }
        }
        return false;
    }

private:
    QAbstractItemView *m_view;
    int m_row = -1;
};

class CaloriesEntryDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const auto *view = qobject_cast<const QTableView *>(option.widget);
        if (!Theme::isNext() || !view) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        painter->save();
        QRect row = option.rect;
        const QColor line = Theme::color(Theme::Role::Border);
        const QString heading = index.data(kGroupHeadingRole).toString();
        if (!heading.isEmpty()) {
            const QRect band(row.left(), row.top(), row.width(), kNextGroupBand);
            if (index.row() > 0)
                painter->fillRect(QRect(band.left(), band.top(), band.width(), 1), line);
            painter->fillRect(QRect(band.left(), band.bottom(), band.width(), 1), line);
            if (index.column() == firstVisibleColumn(view)) {
                const QFont headingFont = caloriesFont(option.font, 12, QFont::Medium);
                const QFont sumFont = caloriesFont(option.font, 12, QFont::Normal);
                const int left = band.left() + 16;
                const int width = QFontMetrics(headingFont).horizontalAdvance(heading);
                painter->setFont(headingFont);
                painter->setPen(Theme::color(Theme::Role::Fg));
                painter->drawText(QRect(left, band.top(), width + 1, band.height()),
                                  Qt::AlignLeft | Qt::AlignVCenter, heading);
                const QString sum = index.data(kGroupKcalRole).toString();
                const int room = band.right() - (left + width + 8);
                if (!sum.isEmpty() && room > 8) {
                    painter->setFont(sumFont);
                    painter->setPen(Theme::color(Theme::Role::FgSubtle));
                    painter->drawText(QRect(left + width + 8, band.top(), room, band.height()),
                                      Qt::AlignLeft | Qt::AlignVCenter,
                                      QFontMetrics(sumFont).elidedText(sum, Qt::ElideRight, room));
                }
            }
            row.setTop(band.bottom() + 1);
        }
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        if (selected)
            painter->fillRect(row, Theme::color(Theme::Role::AccentSubtle));
        else if (rowUnderCursor(view, index))
            painter->fillRect(row, Theme::color(Theme::Role::SurfaceRaised));
        if (index.row() < index.model()->rowCount() - 1 &&
            index.model()->index(index.row() + 1, 0).data(kGroupHeadingRole).toString().isEmpty())
            painter->fillRect(QRect(row.left(), row.bottom(), row.width(), 1), line);
        const int column = index.column();
        const bool food = column == 1;
        const bool source = column == 7;
        const bool numeric = column >= 3 && column <= 6;
        const QFont font = caloriesFont(option.font, source ? 12 : 13, QFont::Normal);
        QRect textRect = row.adjusted(food ? 32 : source ? 24 : 12, 0, numeric ? -12 : -8, 0);
        const QString value = index.data(Qt::DisplayRole).toString();
        painter->setFont(font);
        painter->setPen(Theme::color(food || column == 3 ? Theme::Role::Fg : Theme::Role::FgMuted));
        painter->drawText(textRect, (numeric ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                          QFontMetrics(font).elidedText(value, Qt::ElideRight, textRect.width()));
        painter->restore();
    }
};

class CaloriesQuickDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (!Theme::isNext())
            return QStyledItemDelegate::sizeHint(option, index);
        if (!index.siblingAtColumn(0).data(kQuickListRole).toString().isEmpty())
            return QSize(0, 32);
        if (headingRow(index))
            return QSize(0, 30);
        const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
        const int width = qMax(80, (view ? view->viewport()->width() : 240) - 24);
        const QFont font = caloriesFont(option.font, 12, QFont::Normal);
        const QRect bounds = QFontMetrics(font).boundingRect(
            QRect(0, 0, width, 1000), Qt::TextWordWrap, index.data(Qt::DisplayRole).toString());
        return QSize(0, bounds.height() + 10);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        const auto *view = qobject_cast<const QAbstractItemView *>(option.widget);
        if (!Theme::isNext() || !view) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect cell = option.rect;
        const QString value = index.data(Qt::DisplayRole).toString();
        const bool food = !index.siblingAtColumn(0).data(kQuickListRole).toString().isEmpty();
        if (!food) {
            if (index.column() == 0) {
                const QRect text = QRect(cell.left() + 10, cell.top(), view->viewport()->width() - 20,
                                         cell.height());
                if (headingRow(index)) {
                    QFont font = caloriesFont(option.font, 11, QFont::Medium);
                    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.6);
                    painter->setFont(font);
                    painter->setPen(Theme::color(Theme::Role::FgSubtle));
                    painter->drawText(text.adjusted(0, 0, 0, -4), Qt::AlignLeft | Qt::AlignBottom, value);
                } else {
                    painter->setFont(caloriesFont(option.font, 12, QFont::Normal));
                    painter->setPen(Theme::color(Theme::Role::FgMuted));
                    painter->drawText(text.adjusted(0, 2, 0, 0), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                                      value);
                }
            }
            painter->restore();
            return;
        }
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = rowUnderCursor(view, index);
        if (index.column() == 0 && (selected || hovered)) {
            const QRect band(cell.left(), cell.top(), view->viewport()->width() - cell.left(), cell.height());
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(selected ? Theme::Role::AccentSubtle : Theme::Role::SurfaceRaised));
            painter->drawRoundedRect(QRectF(band).adjusted(0, 1, 0, -1), 6, 6);
        }
        const bool name = index.column() == 0;
        const QFont font = caloriesFont(option.font, name ? 13 : 12, QFont::Normal);
        const QRect text = cell.adjusted(name ? 10 : 4, 0, name ? -4 : -10, 0);
        painter->setFont(font);
        painter->setPen(Theme::color(name ? Theme::Role::Fg : Theme::Role::FgMuted));
        painter->drawText(text, (name ? Qt::AlignLeft : Qt::AlignRight) | Qt::AlignVCenter,
                          QFontMetrics(font).elidedText(value, Qt::ElideRight, text.width()));
        painter->restore();
    }

private:
    static bool headingRow(const QModelIndex &index)
    {
        return index.siblingAtColumn(0).data(Qt::FontRole).value<QFont>().bold();
    }
};

} // namespace

// Calories per day for the last 30 days against the daily goal.
class DayChart final : public QWidget
{
public:
    explicit DayChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(180);
        setMouseTracking(true);
    }

    void setDays(const QJsonArray &days, int goal)
    {
        m_days = days;
        m_goal = goal;
        setAccessibleName(QStringLiteral("Calories per day for the last %1 days")
                              .arg(days.size()));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        QColor grid = palette().color(QPalette::PlaceholderText);
        grid.setAlphaF(0.35);
        const QRectF plot(44, 10, qMax<qreal>(20, width() - 56), qMax<qreal>(20, height() - 36));
        double peak = qMax<double>(500.0, m_goal * 1.15);
        for (const QJsonValue &value : m_days)
            peak = qMax(peak, number(value.toObject(), "calories"));
        painter.setPen(QPen(grid, 1));
        for (int step = 0; step <= 4; ++step) {
            const qreal y = plot.bottom() - plot.height() * step / 4;
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRectF(0, y - 8, plot.left() - 6, 16),
                             Qt::AlignRight | Qt::AlignVCenter,
                             kcal(peak * step / 4));
            painter.setPen(QPen(grid, 1));
        }
        if (m_days.isEmpty())
            return;
        const qreal slot = plot.width() / m_days.size();
        const qreal barWidth = qMin<qreal>(22, qMax<qreal>(3, slot - 4));
        painter.setPen(Qt::NoPen);
        for (int index = 0; index < m_days.size(); ++index) {
            const QJsonObject day = m_days.at(index).toObject();
            const double calories = number(day, "calories");
            const bool logged = day.value(QStringLiteral("entries")).toInt() > 0;
            const qreal x = plot.left() + index * slot + (slot - barWidth) / 2;
            if (index == m_hover) {
                QColor band = grid;
                band.setAlphaF(0.18);
                painter.fillRect(QRectF(plot.left() + index * slot, plot.top(), slot, plot.height()), band);
            }
            if (!logged)
                continue;
            const qreal h = qMax<qreal>(2, plot.height() * calories / peak);
            painter.setBrush(m_goal > 0 && calories > m_goal ? overColor(dark) : accentColor(dark));
            painter.save();
            painter.setClipRect(QRectF(x - 1, plot.top(), barWidth + 2, plot.height()));
            painter.drawRoundedRect(QRectF(x, plot.bottom() - h, barWidth, h + 4), 3, 3);
            painter.restore();
            if (index % 5 == 0 || index == m_days.size() - 1) {
                painter.setPen(palette().color(QPalette::PlaceholderText));
                painter.drawText(QRectF(x + barWidth / 2 - 30, plot.bottom() + 4, 60, 16),
                                 Qt::AlignHCenter | Qt::AlignTop,
                                 QDate::fromString(text(day, "day"), Qt::ISODate)
                                     .toString(QStringLiteral("M/d")));
                painter.setPen(Qt::NoPen);
            }
        }
        if (m_goal > 0) {
            const qreal y = plot.bottom() - plot.height() * m_goal / peak;
            QPen pen(goalColor(dark), 2, Qt::DashLine);
            painter.setPen(pen);
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(QRectF(plot.right() - 120, y - 18, 120, 16),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("Goal %1").arg(kcal(m_goal)));
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip)
            return QWidget::event(event);
        const auto *help = static_cast<QHelpEvent *>(event);
        const int index = dayAt(help->pos().x());
        if (index < 0)
            return true;
        const QJsonObject day = m_days.at(index).toObject();
        QToolTip::showText(help->globalPos(),
                           QStringLiteral("%1\n%2 kcal · P %3 g · C %4 g · F %5 g")
                               .arg(dayLabel(text(day, "day")), kcal(number(day, "calories")),
                                    grams(number(day, "proteinG")), grams(number(day, "carbsG")),
                                    grams(number(day, "fatG"))),
                           this);
        return true;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int index = dayAt(event->position().x());
        if (index != m_hover) {
            m_hover = index;
            update();
        }
    }

    void leaveEvent(QEvent *event) override
    {
        m_hover = -1;
        update();
        QWidget::leaveEvent(event);
    }

private:
    int dayAt(qreal x) const
    {
        const qreal left = 44, right = width() - 12;
        if (m_days.isEmpty() || x < left || x >= right)
            return -1;
        return qBound(0, int((x - left) / ((right - left) / m_days.size())), int(m_days.size()) - 1);
    }

    QJsonArray m_days;
    int m_goal = 0;
    int m_hover = -1;
};

// Weigh-ins over time with the target weight as a dashed line.
class WeightChart final : public QWidget
{
public:
    explicit WeightChart(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumHeight(180);
    }

    void setPoints(const QJsonArray &points, double target, bool imperial)
    {
        m_points = points;
        m_target = target;
        m_imperial = imperial;
        setAccessibleName(QStringLiteral("Weight trend"));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = palette().color(QPalette::Window).lightness() < 128;
        const QColor muted = palette().color(QPalette::PlaceholderText);
        if (m_points.isEmpty()) {
            painter.setPen(muted);
            painter.drawText(rect(), Qt::AlignCenter,
                             QStringLiteral("No weigh-ins yet. Use Weigh in on the Today tab."));
            return;
        }
        QColor grid = muted;
        grid.setAlphaF(0.35);
        const QRectF plot(48, 10, qMax<qreal>(20, width() - 60), qMax<qreal>(20, height() - 36));
        double low = 1e9, high = -1e9;
        for (const QJsonValue &value : m_points) {
            const double w = display(number(value.toObject(), "weightKg"));
            low = qMin(low, w);
            high = qMax(high, w);
        }
        if (m_target > 0) {
            low = qMin(low, display(m_target));
            high = qMax(high, display(m_target));
        }
        low = std::floor(low - 1);
        high = std::ceil(high + 1);
        const qint64 first = QDate::fromString(text(m_points.first().toObject(), "day"), Qt::ISODate).toJulianDay();
        const qint64 last = QDate::fromString(text(m_points.last().toObject(), "day"), Qt::ISODate).toJulianDay();
        const qreal span = qMax<qreal>(1, last - first);
        auto px = [&](const QString &day) {
            return plot.left() + plot.width() * (QDate::fromString(day, Qt::ISODate).toJulianDay() - first) / span;
        };
        auto py = [&](double value) { return plot.bottom() - plot.height() * (value - low) / (high - low); };
        painter.setPen(QPen(grid, 1));
        for (int step = 0; step <= 3; ++step) {
            const double value = low + (high - low) * step / 3;
            painter.drawLine(QPointF(plot.left(), py(value)), QPointF(plot.right(), py(value)));
            painter.setPen(muted);
            painter.drawText(QRectF(0, py(value) - 8, plot.left() - 6, 16),
                             Qt::AlignRight | Qt::AlignVCenter, grams(value));
            painter.setPen(QPen(grid, 1));
        }
        if (m_target > 0) {
            painter.setPen(QPen(goalColor(dark), 2, Qt::DashLine));
            painter.drawLine(QPointF(plot.left(), py(display(m_target))),
                             QPointF(plot.right(), py(display(m_target))));
            painter.setPen(muted);
            painter.drawText(QRectF(plot.right() - 140, py(display(m_target)) - 18, 140, 16),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QStringLiteral("Target %1").arg(grams(display(m_target))));
        }
        QPainterPath path;
        for (int index = 0; index < m_points.size(); ++index) {
            const QJsonObject point = m_points.at(index).toObject();
            const QPointF at(px(text(point, "day")), py(display(number(point, "weightKg"))));
            if (index == 0)
                path.moveTo(at);
            else
                path.lineTo(at);
        }
        painter.setPen(QPen(weightColor(dark), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
        painter.setPen(QPen(palette().color(QPalette::Window), 2));
        painter.setBrush(weightColor(dark));
        for (const QJsonValue &value : m_points) {
            const QJsonObject point = value.toObject();
            painter.drawEllipse(QPointF(px(text(point, "day")), py(display(number(point, "weightKg")))), 4, 4);
        }
        painter.setPen(muted);
        painter.drawText(QRectF(plot.left(), plot.bottom() + 4, 80, 16), Qt::AlignLeft,
                         QDate::fromString(text(m_points.first().toObject(), "day"), Qt::ISODate)
                             .toString(QStringLiteral("M/d")));
        painter.drawText(QRectF(plot.right() - 80, plot.bottom() + 4, 80, 16), Qt::AlignRight,
                         QDate::fromString(text(m_points.last().toObject(), "day"), Qt::ISODate)
                             .toString(QStringLiteral("M/d")));
    }

private:
    double display(double kilograms) const { return m_imperial ? kilograms / kKgPerLb : kilograms; }

    QJsonArray m_points;
    double m_target = 0;
    bool m_imperial = false;
};

class CaloriesRing final : public QWidget
{
public:
    explicit CaloriesRing(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(132, 132);
    }

    void setValues(double calories, int goal)
    {
        m_calories = calories;
        m_goal = goal;
        setAccessibleName(goal > 0 ? QStringLiteral("%1 of %2 kcal today").arg(kcal(calories), kcal(goal))
                                   : QStringLiteral("%1 kcal today").arg(kcal(calories)));
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal stroke = 10;
        const QRectF box = QRectF(rect()).adjusted(stroke / 2, stroke / 2, -stroke / 2, -stroke / 2);
        painter.setPen(QPen(Theme::color(Theme::Role::SurfaceActive), stroke));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(box);
        const double fraction = m_goal > 0 ? qBound(0.0, m_calories / m_goal, 1.0) : 0.0;
        if (fraction > 0) {
            painter.setPen(QPen(Theme::color(m_calories > m_goal ? Theme::Role::Warning : Theme::Role::Accent),
                                stroke, Qt::SolidLine, Qt::RoundCap));
            painter.drawArc(box, 90 * 16, -qRound(fraction * 360 * 16));
        }
        const qreal mid = height() / 2.0;
        painter.setFont(caloriesFont(font(), 24, QFont::DemiBold));
        painter.setPen(Theme::color(Theme::Role::Fg));
        painter.drawText(QRectF(0, mid - 22, width(), 30), Qt::AlignCenter, kcal(m_calories));
        painter.setFont(caloriesFont(font(), 11, QFont::Normal));
        painter.setPen(Theme::color(Theme::Role::FgMuted));
        painter.drawText(QRectF(0, mid + 8, width(), 16), Qt::AlignCenter,
                         m_goal > 0 ? QStringLiteral("of %1 kcal").arg(kcal(m_goal))
                                    : QStringLiteral("kcal today"));
    }

private:
    double m_calories = 0;
    int m_goal = 0;
};

CaloriesFeature::CaloriesFeature(
    const forkmesh::market::NativePackageHost &host, QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged),
      m_promptTargetChanged(host.promptTargetChanged)
{
    setObjectName(QStringLiteral("caloriesSection"));
    const bool next = Theme::isNext();
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Calorie Counter"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    QLabel *lede = mutedLabel(QStringLiteral("Your private food diary, goals, reminders, and challenges with friends"));
    heading->addWidget(lede);
    heading->addStretch();
    auto *refreshButton = new QPushButton(QStringLiteral("Refresh"));
    m_exportButton = new QPushButton(QStringLiteral("Export my data"));
    m_exportButton->setToolTip(QStringLiteral("Save your diary, weigh-ins, goals, reminders, and challenges as JSON"));
    m_uninstallButton = new QPushButton(QStringLiteral("Uninstall…"));
    for (QPushButton *button : {refreshButton, m_exportButton, m_uninstallButton})
        button->setObjectName(QStringLiteral("ghostButton"));
    heading->addWidget(refreshButton);
    heading->addWidget(m_exportButton);
    heading->addWidget(m_uninstallButton);
    outer->addLayout(heading);

    m_status = mutedLabel(QStringLiteral("Loading Calorie Counter…"));
    outer->addWidget(m_status);

    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    if (next)
        installLayout->setContentsMargins(24, 48, 24, 24);
    m_installNote = new QLabel(QStringLiteral(
        "Calorie Counter is private to your ForkMesh account. Install it once and the same diary "
        "appears on the web dashboard, this desktop app, and the mobile app. Every entry, weigh-in, "
        "and goal is encrypted at rest with a key minted for your account alone."));
    m_installNote->setAlignment(Qt::AlignHCenter);
    m_installNote->setWordWrap(true);
    installLayout->addWidget(m_installNote);
    auto *installRow = new QHBoxLayout;
    installRow->addStretch();
    m_installButton = new QPushButton(QStringLiteral("Install Calorie Counter"));
    m_installButton->setObjectName(QStringLiteral("primaryButton"));
    installRow->addWidget(m_installButton);
    installRow->addStretch();
    installLayout->addLayout(installRow);
    installLayout->addStretch();
    outer->addWidget(m_installPanel, 1);

    m_tabs = new QTabWidget;
    m_tabs->setObjectName(QStringLiteral("caloriesTabs"));

    // --- Diary ---------------------------------------------------------
    auto *diaryPage = new QSplitter(Qt::Horizontal);
    diaryPage->setChildrenCollapsible(false);
    auto *diaryColumn = new QWidget;
    auto *diaryLayout = new QVBoxLayout(diaryColumn);
    diaryLayout->setContentsMargins(8, 10, 8, 8);
    if (next) {
        diaryPage->setProperty("caloriesRole", QStringLiteral("diary"));
        diaryPage->setHandleWidth(1);
        diaryLayout->setContentsMargins(24, 20, 20, 16);
        diaryLayout->setSpacing(16);
    }
    auto *toolbar = new QHBoxLayout;
    m_day = new QDateEdit(QDate::currentDate());
    m_day->setToolTip(QStringLiteral("The day a photo, barcode, search, or new food is logged to"));
    m_day->setCalendarPopup(true);
    m_day->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    auto *todayButton = new QPushButton(QStringLiteral("Today"));
    todayButton->setObjectName(QStringLiteral("ghostButton"));
    m_todayButton = todayButton;
    m_foodSearch = lineEdit();
    m_foodSearch->setObjectName(QStringLiteral("caloriesFoodSearch"));
    m_foodSearch->setPlaceholderText(QStringLiteral("Search foods: trader joes buffalo chicken wrap…"));
    m_foodSearch->setToolTip(QStringLiteral("Start typing a food or brand; pick a match to log it. Open Food Facts, plus USDA with your own key."));
    m_foodSearch->setMaxLength(80);
    m_foodSearch->setMinimumWidth(260);
    m_foodModel = new QStandardItemModel(this);
    m_foodCompleter = new QCompleter(m_foodModel, this);
    m_foodCompleter->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    m_foodCompleter->setMaxVisibleItems(12);
    m_foodCompleter->setWidget(m_foodSearch);
    m_foodTimer = new QTimer(this);
    m_foodTimer->setSingleShot(true);
    m_foodTimer->setInterval(250);
    auto *photoButton = new QPushButton(QStringLiteral("Photo…"));
    photoButton->setObjectName(QStringLiteral("primaryButton"));
    photoButton->setToolTip(QStringLiteral("Pick a meal photo; the host AI model estimates its calories"));
    auto *describeButton = new QPushButton(QStringLiteral("Describe…"));
    auto *barcodeButton = new QPushButton(QStringLiteral("Barcode…"));
    auto *addButton = new QPushButton(QStringLiteral("Add food"));
    auto *weighButton = new QPushButton(QStringLiteral("Weigh in"));
    for (QPushButton *button : {describeButton, barcodeButton, addButton, weighButton})
        button->setObjectName(QStringLiteral("ghostButton"));
    QToolButton *earlierDay = nullptr;
    QToolButton *laterDay = nullptr;
    if (next) {
        toolbar->setSpacing(8);
        m_day->setProperty("caloriesRole", QStringLiteral("day"));
        todayButton->setProperty("caloriesRole", QStringLiteral("outline"));
        auto *stepper = new QFrame;
        stepper->setObjectName(QStringLiteral("caloriesDayStepper"));
        auto *stepperLayout = new QHBoxLayout(stepper);
        stepperLayout->setContentsMargins(0, 0, 0, 0);
        stepperLayout->setSpacing(0);
        earlierDay = new QToolButton;
        earlierDay->setToolTip(QStringLiteral("Log to the day before"));
        earlierDay->setAccessibleName(QStringLiteral("Previous day"));
        earlierDay->setIcon(caloriesOcticon(QStringLiteral("chevron-left"), Theme::color(Theme::Role::FgMuted), 14));
        laterDay = new QToolButton;
        laterDay->setToolTip(QStringLiteral("Log to the day after"));
        laterDay->setAccessibleName(QStringLiteral("Next day"));
        laterDay->setIcon(caloriesOcticon(QStringLiteral("chevron-right"), Theme::color(Theme::Role::FgMuted), 14));
        for (QToolButton *step : {earlierDay, laterDay}) {
            step->setCursor(Qt::PointingHandCursor);
            step->setIconSize(QSize(14, 14));
            step->setAutoRaise(true);
        }
        stepperLayout->addWidget(earlierDay);
        stepperLayout->addWidget(m_day);
        stepperLayout->addWidget(laterDay);
        m_foodSearch->setPlaceholderText(QStringLiteral("Search foods, e.g. trader joe's buffalo chicken"));
        m_foodSearch->setMinimumWidth(240);
        m_foodSearch->addAction(caloriesOcticon(QStringLiteral("search"), Theme::color(Theme::Role::FgSubtle), 14),
                                QLineEdit::LeadingPosition);
        auto *tools = new QFrame;
        tools->setObjectName(QStringLiteral("caloriesToolGroup"));
        auto *toolsLayout = new QHBoxLayout(tools);
        toolsLayout->setContentsMargins(2, 2, 2, 2);
        toolsLayout->setSpacing(2);
        const QColor glyph = Theme::color(Theme::Role::FgMuted);
        const struct { QPushButton *button; QIcon icon; QString tip; } tooled[] = {
            {photoButton, caloriesGlyph(QStringLiteral("camera"), glyph, 16), QString()},
            {describeButton, caloriesOcticon(QStringLiteral("comment"), glyph, 16),
             QStringLiteral("Describe a meal; the host AI model estimates its calories")},
            {barcodeButton, caloriesGlyph(QStringLiteral("barcode"), glyph, 16),
             QStringLiteral("Look up a packaged food by its barcode")},
            {weighButton, caloriesGlyph(QStringLiteral("scale"), glyph, 16),
             QStringLiteral("Record a weigh-in")},
        };
        for (const auto &tool : tooled) {
            tool.button->setAccessibleName(tool.button->text());
            if (!tool.tip.isEmpty())
                tool.button->setToolTip(tool.tip);
            tool.button->setText(QString());
            tool.button->setIcon(tool.icon);
            tool.button->setIconSize(QSize(16, 16));
            tool.button->setProperty("caloriesRole", QStringLiteral("tool"));
            tool.button->setCursor(Qt::PointingHandCursor);
            toolsLayout->addWidget(tool.button);
        }
        addButton->setProperty("caloriesRole", QStringLiteral("create"));
        addButton->setIcon(caloriesOcticon(QStringLiteral("plus"), Theme::color(Theme::Role::Bg), 14));
        addButton->setIconSize(QSize(14, 14));
        addButton->setCursor(Qt::PointingHandCursor);
        toolbar->addWidget(todayButton);
        toolbar->addWidget(stepper);
        toolbar->addWidget(m_foodSearch, 1);
        toolbar->addWidget(tools);
        toolbar->addWidget(addButton);
    } else {
        toolbar->addWidget(mutedLabel(QStringLiteral("Log to")));
        toolbar->addWidget(m_day);
        toolbar->addWidget(todayButton);
        toolbar->addWidget(m_foodSearch, 1);
        for (QPushButton *button : {photoButton, describeButton, barcodeButton, addButton, weighButton})
            toolbar->addWidget(button);
    }
    diaryLayout->addLayout(toolbar);

    m_caloriesLabel = new QLabel(QStringLiteral("0"));
    m_caloriesLabel->setObjectName(QStringLiteral("pageTitle"));
    m_caloriesBar = goalBar();
    m_caloriesCaption = mutedLabel(QString());
    m_proteinBar = goalBar();
    m_carbsBar = goalBar();
    m_fatBar = goalBar();
    m_proteinLabel = mutedLabel(QString());
    m_carbsLabel = mutedLabel(QString());
    m_fatLabel = mutedLabel(QString());
    m_streakLabel = new QLabel;
    m_streakLabel->setWordWrap(true);
    m_weightLabel = new QLabel;
    m_weightLabel->setWordWrap(true);
    if (next) {
        const auto card = [] {
            auto *frame = new QFrame;
            frame->setProperty("caloriesRole", QStringLiteral("card"));
            return frame;
        };
        const auto roleLabel = [](const QString &value, const char *role) {
            auto *label = new QLabel(value);
            label->setProperty("caloriesRole", QLatin1String(role));
            return label;
        };
        auto *stats = new QHBoxLayout;
        stats->setSpacing(12);
        QFrame *ringCard = card();
        auto *ringLayout = new QHBoxLayout(ringCard);
        ringLayout->setContentsMargins(20, 18, 24, 18);
        ringLayout->setSpacing(20);
        m_ring = new CaloriesRing;
        ringLayout->addWidget(m_ring, 0, Qt::AlignVCenter);
        auto *ringCopy = new QVBoxLayout;
        ringCopy->setSpacing(2);
        ringCopy->addStretch();
        m_remainingTitle = roleLabel(QStringLiteral("Remaining"), "caption");
        m_remainingLabel = roleLabel(QString(), "value");
        m_entriesTodayLabel = roleLabel(QString(), "faint");
        ringCopy->addWidget(m_remainingTitle);
        ringCopy->addWidget(m_remainingLabel);
        ringCopy->addWidget(m_entriesTodayLabel);
        ringCopy->addStretch();
        ringLayout->addLayout(ringCopy);
        stats->addWidget(ringCard);

        QFrame *macroCard = card();
        auto *macroLayout = new QVBoxLayout(macroCard);
        macroLayout->setContentsMargins(20, 18, 20, 18);
        macroLayout->setSpacing(14);
        macroLayout->addStretch();
        for (auto [label, bar, caption] : {
                 std::make_tuple(QStringLiteral("Protein"), m_proteinBar, m_proteinLabel),
                 std::make_tuple(QStringLiteral("Carbs"), m_carbsBar, m_carbsLabel),
                 std::make_tuple(QStringLiteral("Fat"), m_fatBar, m_fatLabel)}) {
            auto *row = new QHBoxLayout;
            row->setSpacing(8);
            row->addWidget(roleLabel(label, "macro"));
            row->addStretch();
            caption->setWordWrap(false);
            caption->setTextFormat(Qt::RichText);
            caption->setProperty("caloriesRole", QStringLiteral("macroValue"));
            row->addWidget(caption);
            auto *block = new QVBoxLayout;
            block->setSpacing(6);
            block->addLayout(row);
            bar->setFixedHeight(6);
            bar->setProperty("caloriesRole", QStringLiteral("macro"));
            block->addWidget(bar);
            macroLayout->addLayout(block);
        }
        macroLayout->addStretch();
        stats->addWidget(macroCard, 1);

        auto *side = new QVBoxLayout;
        side->setSpacing(12);
        const auto smallCard = [&](const QIcon &icon, QLabel **value, QLabel **caption) {
            QFrame *frame = card();
            auto *layout = new QHBoxLayout(frame);
            layout->setContentsMargins(16, 12, 16, 12);
            layout->setSpacing(12);
            auto *glyph = new QLabel;
            glyph->setPixmap(icon.pixmap(QSize(16, 16), devicePixelRatioF()));
            glyph->setProperty("caloriesRole", QStringLiteral("glyph"));
            layout->addWidget(glyph, 0, Qt::AlignVCenter);
            auto *copy = new QVBoxLayout;
            copy->setSpacing(0);
            *value = roleLabel(QString(), "value");
            *caption = roleLabel(QString(), "caption");
            (*caption)->setWordWrap(true);
            copy->addWidget(*value);
            copy->addWidget(*caption);
            layout->addLayout(copy, 1);
            side->addWidget(frame, 1);
        };
        smallCard(caloriesOcticon(QStringLiteral("flame"), Theme::color(Theme::Role::FgMuted), 16),
                  &m_streakValue, &m_streakCaption);
        smallCard(caloriesGlyph(QStringLiteral("scale"), Theme::color(Theme::Role::FgMuted), 16),
                  &m_weightValue, &m_weightCaption);
        auto *sideHost = new QWidget;
        sideHost->setFixedWidth(240);
        sideHost->setLayout(side);
        side->setContentsMargins(0, 0, 0, 0);
        stats->addWidget(sideHost);
        diaryLayout->addLayout(stats);
        for (QWidget *spare : std::initializer_list<QWidget *>{m_caloriesLabel, m_caloriesBar, m_caloriesCaption,
                                                               m_streakLabel, m_weightLabel}) {
            spare->setParent(diaryColumn);
            spare->hide();
        }
    } else {
        auto *stats = new QHBoxLayout;
        auto *caloriesCard = new QVBoxLayout;
        caloriesCard->addWidget(mutedLabel(QStringLiteral("CALORIES")));
        caloriesCard->addWidget(m_caloriesLabel);
        caloriesCard->addWidget(m_caloriesBar);
        caloriesCard->addWidget(m_caloriesCaption);
        stats->addLayout(caloriesCard, 2);
        auto *macros = new QFormLayout;
        for (auto [label, bar, caption] : {
                 std::make_tuple(QStringLiteral("Protein"), m_proteinBar, m_proteinLabel),
                 std::make_tuple(QStringLiteral("Carbs"), m_carbsBar, m_carbsLabel),
                 std::make_tuple(QStringLiteral("Fat"), m_fatBar, m_fatLabel)}) {
            auto *row = new QVBoxLayout;
            row->addWidget(bar);
            row->addWidget(caption);
            macros->addRow(label, row);
        }
        stats->addLayout(macros, 3);
        auto *streakCard = new QVBoxLayout;
        streakCard->addWidget(mutedLabel(QStringLiteral("STREAKS")));
        streakCard->addWidget(m_streakLabel);
        streakCard->addWidget(mutedLabel(QStringLiteral("WEIGHT")));
        streakCard->addWidget(m_weightLabel);
        streakCard->addStretch();
        stats->addLayout(streakCard, 2);
        diaryLayout->addLayout(stats);
    }

    m_entriesTable = new QTableWidget(0, 8);
    m_entriesTable->setObjectName(QStringLiteral("caloriesEntriesTable"));
    m_entriesTable->setHorizontalHeaderLabels(
        {QStringLiteral("Day"), QStringLiteral("Food"), QStringLiteral("Meal"),
         QStringLiteral("kcal"), QStringLiteral("Protein g"), QStringLiteral("Carbs g"),
         QStringLiteral("Fat g"), QStringLiteral("Source")});
    m_entriesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_entriesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_entriesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_entriesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_entriesTable->setAlternatingRowColors(true);
    if (next) {
        m_entriesTable->setHorizontalHeaderLabels(
            {QStringLiteral("Day"), QStringLiteral("Food"), QStringLiteral("Meal"),
             QStringLiteral("kcal"), QStringLiteral("Protein"), QStringLiteral("Carbs"),
             QStringLiteral("Fat"), QStringLiteral("Source")});
        for (int column = 3; column < 7; ++column)
            m_entriesTable->horizontalHeaderItem(column)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_entriesTable->setAlternatingRowColors(false);
        m_entriesTable->setShowGrid(false);
        m_entriesTable->setWordWrap(false);
        m_entriesTable->setFrameShape(QFrame::NoFrame);
        m_entriesTable->verticalHeader()->hide();
        m_entriesTable->verticalHeader()->setDefaultSectionSize(kNextEntryRow);
        m_entriesTable->horizontalHeader()->setHighlightSections(false);
        m_entriesTable->horizontalHeader()->setFixedHeight(36);
        m_entriesTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
        m_entriesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        for (const auto &[column, width] : {std::pair{3, 80}, std::pair{4, 84}, std::pair{5, 84},
                                            std::pair{6, 72}, std::pair{7, 140}})
            m_entriesTable->setColumnWidth(column, width);
        m_entriesTable->setColumnHidden(0, true);
        m_entriesTable->setColumnHidden(2, true);
        m_entriesTable->setItemDelegate(new CaloriesEntryDelegate(m_entriesTable));
        new CaloriesHoverRepaint(m_entriesTable);
    }
    diaryLayout->addWidget(m_entriesTable, 1);
    m_entriesEmpty = mutedLabel(QStringLiteral("Nothing logged yet. Pick a photo, scan a barcode, search, or add a meal."));
    m_entriesEmpty->setAlignment(Qt::AlignHCenter);
    diaryLayout->addWidget(m_entriesEmpty);
    m_entriesMore = mutedLabel(QString());
    m_entriesMore->setAlignment(Qt::AlignHCenter);
    diaryLayout->addWidget(m_entriesMore);
    auto *entryActions = new QHBoxLayout;
    entryActions->addStretch();
    auto *editButton = new QPushButton(QStringLiteral("Edit"));
    auto *deleteButton = new QPushButton(QStringLiteral("Delete"));
    deleteButton->setObjectName(QStringLiteral("dangerButton"));
    if (next) {
        entryActions->setSpacing(6);
        editButton->setProperty("caloriesRole", QStringLiteral("small"));
        deleteButton->setProperty("caloriesRole", QStringLiteral("small"));
        editButton->setToolTip(QStringLiteral("Edit the selected entry (or double-click it)"));
        deleteButton->setToolTip(QStringLiteral("Delete the selected entry"));
    }
    entryActions->addWidget(editButton);
    entryActions->addWidget(deleteButton);
    diaryLayout->addLayout(entryActions);
    diaryPage->addWidget(diaryColumn);

    auto *quickPanel = new QWidget;
    quickPanel->setObjectName(QStringLiteral("caloriesQuickAdd"));
    quickPanel->setMinimumWidth(240);
    auto *quickLayout = new QVBoxLayout(quickPanel);
    quickLayout->setContentsMargins(8, 10, 8, 8);
    auto *quickHeading = new QHBoxLayout;
    auto *quickTitle = new QLabel(QStringLiteral("Quick add"));
    quickTitle->setObjectName(QStringLiteral("sectionTitle"));
    quickHeading->addWidget(quickTitle);
    quickHeading->addStretch();
    auto *receiptButton = new QPushButton(QStringLiteral("Receipt…"));
    receiptButton->setObjectName(QStringLiteral("ghostButton"));
    receiptButton->setToolTip(QStringLiteral("Pick a photo of a grocery or restaurant receipt; its foods join My foods"));
    quickHeading->addWidget(receiptButton);
    auto *quickControls = new QHBoxLayout;
    m_quickMeal = mealCombo(guessMeal());
    m_quickMeal->setToolTip(QStringLiteral("The meal a quick-added food is logged to"));
    m_quickFilter = lineEdit();
    m_quickFilter->setPlaceholderText(QStringLiteral("Filter foods"));
    m_quickFilter->setClearButtonEnabled(true);
    quickControls->addWidget(m_quickMeal);
    quickControls->addWidget(m_quickFilter, 1);
    QVBoxLayout *quickTop = quickLayout;
    if (next) {
        quickPanel->setAttribute(Qt::WA_StyledBackground, true);
        quickPanel->setMinimumWidth(280);
        quickLayout->setContentsMargins(0, 0, 0, 0);
        quickLayout->setSpacing(0);
        auto *quickHead = new QWidget;
        quickHead->setProperty("caloriesRole", QStringLiteral("quickHead"));
        quickHead->setAttribute(Qt::WA_StyledBackground, true);
        quickTop = new QVBoxLayout(quickHead);
        quickTop->setContentsMargins(12, 12, 12, 12);
        quickTop->setSpacing(8);
        quickHeading->setContentsMargins(4, 0, 0, 0);
        quickControls->setSpacing(6);
        quickTitle->setProperty("caloriesRole", QStringLiteral("quickTitle"));
        receiptButton->setText(QStringLiteral("Scan receipt"));
        receiptButton->setProperty("caloriesRole", QStringLiteral("headerAction"));
        quickLayout->addWidget(quickHead);
    }
    quickTop->addLayout(quickHeading);
    quickTop->addLayout(quickControls);
    m_quickTree = new QTreeWidget;
    m_quickTree->setObjectName(QStringLiteral("caloriesQuickTree"));
    m_quickTree->setColumnCount(2);
    m_quickTree->setHeaderHidden(true);
    m_quickTree->setRootIsDecorated(false);
    m_quickTree->setUniformRowHeights(true);
    m_quickTree->header()->setStretchLastSection(false);
    m_quickTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_quickTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    if (next) {
        m_quickTree->setUniformRowHeights(false);
        m_quickTree->setIndentation(0);
        m_quickTree->setFrameShape(QFrame::NoFrame);
        m_quickTree->setItemDelegate(new CaloriesQuickDelegate(m_quickTree));
        new CaloriesHoverRepaint(m_quickTree);
    }
    quickLayout->addWidget(m_quickTree, 1);
    auto *quickActions = new QHBoxLayout;
    m_quickLogButton = new QPushButton(QStringLiteral("Log"));
    m_quickLogButton->setObjectName(QStringLiteral("primaryButton"));
    m_quickLogButton->setToolTip(QStringLiteral("Log one serving of the selected food (or double-click it)"));
    m_quickStarButton = new QPushButton(QString::fromUtf8("\xE2\x98\x86 Save"));
    m_quickStarButton->setToolTip(QStringLiteral("Save the selected food to My foods"));
    m_quickRemoveButton = new QPushButton(QStringLiteral("Remove"));
    m_quickRemoveButton->setToolTip(QStringLiteral("Remove the selected food from My foods"));
    for (QPushButton *button : {m_quickStarButton, m_quickRemoveButton})
        button->setObjectName(QStringLiteral("ghostButton"));
    quickActions->addWidget(m_quickLogButton);
    quickActions->addWidget(m_quickStarButton);
    quickActions->addWidget(m_quickRemoveButton);
    quickActions->addStretch();
    m_quickStatus = mutedLabel(QStringLiteral("Type what you ate in the prompt box below, or pick a food here."));
    m_quickStatus->setWordWrap(true);
    if (next) {
        quickActions->setContentsMargins(12, 8, 12, 8);
        quickActions->setSpacing(6);
        m_quickLogButton->setIcon(caloriesOcticon(QStringLiteral("plus"), Theme::color(Theme::Role::OnAccent), 12));
        m_quickStarButton->setText(QStringLiteral("Save"));
        m_quickStarButton->setIcon(caloriesOcticon(QStringLiteral("star"), Theme::color(Theme::Role::FgMuted), 12));
        for (QPushButton *button : {m_quickLogButton, m_quickStarButton, m_quickRemoveButton}) {
            button->setProperty("caloriesRole", QStringLiteral("small"));
            button->setIconSize(QSize(12, 12));
        }
        m_quickStatus->setText(QStringLiteral("Or type what you ate in the prompt box; an agent logs it for you."));
        m_quickStatus->setProperty("caloriesRole", QStringLiteral("quickFooter"));
    }
    quickLayout->addLayout(quickActions);
    quickLayout->addWidget(m_quickStatus);
    diaryPage->addWidget(quickPanel);
    diaryPage->setStretchFactor(0, 3);
    diaryPage->setStretchFactor(1, 1);
    m_tabs->addTab(diaryPage, QStringLiteral("Diary"));

    // --- Trends --------------------------------------------------------
    auto *trendsPage = new QWidget;
    auto *trendsLayout = new QVBoxLayout(trendsPage);
    trendsLayout->setContentsMargins(8, 10, 8, 8);
    QLabel *dayChartTitle = mutedLabel(QStringLiteral("Calories · last 30 days (blue within goal, orange over, dashed goal line)"));
    m_dayChart = new DayChart;
    m_trendsSummary = mutedLabel(QString());
    QLabel *weightChartTitle = mutedLabel(QStringLiteral("Weight · last 180 days"));
    m_weightChart = new WeightChart;
    if (next) {
        trendsLayout->setContentsMargins(24, 20, 24, 24);
        trendsLayout->setSpacing(16);
        const auto chartCard = [&trendsLayout](QLabel *heading, QWidget *chart, QLabel *summary, int stretch) {
            auto *frame = new QFrame;
            frame->setProperty("caloriesRole", QStringLiteral("card"));
            auto *layout = new QVBoxLayout(frame);
            layout->setContentsMargins(16, 14, 16, 14);
            layout->setSpacing(10);
            heading->setProperty("caloriesRole", QStringLiteral("cardTitle"));
            layout->addWidget(heading);
            layout->addWidget(chart, 1);
            if (summary) {
                summary->setProperty("caloriesRole", QStringLiteral("caption"));
                layout->addWidget(summary);
            }
            trendsLayout->addWidget(frame, stretch);
        };
        chartCard(dayChartTitle, m_dayChart, m_trendsSummary, 3);
        chartCard(weightChartTitle, m_weightChart, nullptr, 2);
    } else {
        trendsLayout->addWidget(dayChartTitle);
        trendsLayout->addWidget(m_dayChart, 3);
        trendsLayout->addWidget(m_trendsSummary);
        trendsLayout->addWidget(weightChartTitle);
        trendsLayout->addWidget(m_weightChart, 2);
    }
    m_tabs->addTab(trendsPage, QStringLiteral("Trends"));

    // --- Goals & reminders ---------------------------------------------
    auto *goalsPage = new QWidget;
    auto *goalsLayout = new QHBoxLayout(goalsPage);
    goalsLayout->setContentsMargins(8, 10, 8, 8);
    auto *goalsForm = new QFormLayout;
    m_goalCalories = new QSpinBox;
    m_goalCalories->setRange(0, 20000);
    m_goalCalories->setSingleStep(50);
    m_goalProtein = new QSpinBox;
    m_goalProtein->setRange(0, 2000);
    m_goalCarbs = new QSpinBox;
    m_goalCarbs->setRange(0, 3000);
    m_goalFat = new QSpinBox;
    m_goalFat->setRange(0, 1500);
    m_units = new QComboBox;
    m_units->addItem(QStringLiteral("Automatic (match my location)"), QStringLiteral("auto"));
    m_units->addItem(QStringLiteral("Metric (kg, cm)"), QStringLiteral("metric"));
    m_units->addItem(QStringLiteral("Imperial (lb, in)"), QStringLiteral("imperial"));
    m_targetWeight = new QDoubleSpinBox;
    m_startWeight = new QDoubleSpinBox;
    m_height = new QDoubleSpinBox;
    for (QDoubleSpinBox *box : {m_targetWeight, m_startWeight, m_height}) {
        box->setRange(0, 1500);
        box->setDecimals(1);
        box->setSpecialValueText(QStringLiteral("not set"));
    }
    goalsForm->addRow(QStringLiteral("Calories (kcal)"), m_goalCalories);
    goalsForm->addRow(QStringLiteral("Protein (g)"), m_goalProtein);
    goalsForm->addRow(QStringLiteral("Carbs (g)"), m_goalCarbs);
    goalsForm->addRow(QStringLiteral("Fat (g)"), m_goalFat);
    goalsForm->addRow(QStringLiteral("Units"), m_units);
    goalsForm->addRow(QStringLiteral("Target weight"), m_targetWeight);
    goalsForm->addRow(QStringLiteral("Starting weight"), m_startWeight);
    goalsForm->addRow(QStringLiteral("Height"), m_height);
    m_bmiLabel = mutedLabel(QString());
    goalsForm->addRow(QString(), m_bmiLabel);
    auto *saveGoals = new QPushButton(QStringLiteral("Save goals and reminders"));
    saveGoals->setObjectName(QStringLiteral("primaryButton"));
    auto *exportMine = new QPushButton(QStringLiteral("Export my diary"));
    exportMine->setObjectName(QStringLiteral("ghostButton"));
    auto *goalButtons = new QHBoxLayout;
    goalButtons->addWidget(saveGoals);
    goalButtons->addWidget(exportMine);
    goalButtons->addStretch();
    goalsForm->addRow(QString(), goalButtons);
    auto *databasePanel = new QWidget;
    auto *adminLayout = new QVBoxLayout(databasePanel);
    adminLayout->setContentsMargins(0, 12, 0, 0);
    adminLayout->addWidget(mutedLabel(QStringLiteral(
        "Food databases: Open Food Facts is always used for barcodes and search. "
        "Add your own free USDA FoodData Central API key to also search the USDA database; "
        "it is stored encrypted on your account.")));
    auto *adminRow = new QHBoxLayout;
    m_usdaKey = lineEdit();
    m_usdaKey->setPlaceholderText(QStringLiteral("USDA FoodData Central API key"));
    m_usdaKey->setEchoMode(QLineEdit::Password);
    adminRow->addWidget(m_usdaKey, 1);
    auto *saveKey = new QPushButton(QStringLiteral("Save key"));
    saveKey->setObjectName(QStringLiteral("ghostButton"));
    adminRow->addWidget(saveKey);
    m_usdaStatus = mutedLabel(QString());
    adminRow->addWidget(m_usdaStatus);
    adminLayout->addLayout(adminRow);
    goalsForm->addRow(databasePanel);
    auto *goalsColumn = new QWidget;
    goalsColumn->setLayout(goalsForm);
    goalsLayout->addWidget(goalsColumn, 1);
    if (next) {
        goalsLayout->setContentsMargins(24, 20, 24, 24);
        goalsLayout->setSpacing(16);
        goalsColumn->setProperty("caloriesRole", QStringLiteral("card"));
        goalsColumn->setAttribute(Qt::WA_StyledBackground, true);
        goalsForm->setContentsMargins(20, 18, 20, 18);
        goalsForm->setHorizontalSpacing(16);
        goalsForm->setVerticalSpacing(10);
    }

    auto *remindersColumn = new QVBoxLayout;
    auto *remindersHeading = new QHBoxLayout;
    remindersHeading->addWidget(new QLabel(QStringLiteral("Reminders")));
    remindersHeading->addStretch();
    auto *addReminder = new QPushButton(QStringLiteral("Add reminder"));
    auto *removeReminder = new QPushButton(QStringLiteral("Remove"));
    for (QPushButton *button : {addReminder, removeReminder})
        button->setObjectName(QStringLiteral("ghostButton"));
    remindersHeading->addWidget(addReminder);
    remindersHeading->addWidget(removeReminder);
    remindersColumn->addLayout(remindersHeading);
    remindersColumn->addWidget(mutedLabel(QStringLiteral(
        "Delivered as pings in this computer's time zone. Up to eight per day.")));
    m_remindersTable = new QTableWidget(0, 4);
    m_remindersTable->setObjectName(QStringLiteral("caloriesRemindersTable"));
    m_remindersTable->setHorizontalHeaderLabels(
        {QStringLiteral("Time"), QStringLiteral("Label"),
         QStringLiteral("Only if nothing logged"), QStringLiteral("On")});
    m_remindersTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_remindersTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_remindersTable->setSelectionMode(QAbstractItemView::SingleSelection);
    remindersColumn->addWidget(m_remindersTable, 1);
    if (next) {
        auto *remindersCard = new QWidget;
        remindersCard->setProperty("caloriesRole", QStringLiteral("card"));
        remindersCard->setAttribute(Qt::WA_StyledBackground, true);
        remindersCard->setLayout(remindersColumn);
        remindersColumn->setContentsMargins(20, 18, 20, 18);
        remindersColumn->setSpacing(10);
        m_remindersTable->setProperty("dataTable", true);
        goalsLayout->addWidget(remindersCard, 1);
    } else {
        goalsLayout->addLayout(remindersColumn, 1);
    }
    m_tabs->addTab(goalsPage, next ? QStringLiteral("Goals and reminders") : QStringLiteral("Goals & reminders"));

    // --- Challenges ----------------------------------------------------
    auto *challengesPage = new QWidget;
    auto *challengesLayout = new QVBoxLayout(challengesPage);
    challengesLayout->setContentsMargins(8, 10, 8, 8);
    auto *challengesHeading = new QHBoxLayout;
    challengesHeading->addWidget(mutedLabel(QStringLiteral(
        "Challenge friends by their ForkMesh username to lose weight or keep a logging streak. Standings show progress only, never anyone's diary.")), 1);
    auto *openChallenge = new QPushButton(QStringLiteral("Open"));
    openChallenge->setObjectName(QStringLiteral("ghostButton"));
    auto *newChallenge = new QPushButton(QStringLiteral("New challenge"));
    newChallenge->setObjectName(QStringLiteral("primaryButton"));
    challengesHeading->addWidget(openChallenge);
    challengesHeading->addWidget(newChallenge);
    challengesLayout->addLayout(challengesHeading);
    m_challengesTable = new QTableWidget(0, 6);
    m_challengesTable->setObjectName(QStringLiteral("caloriesChallengesTable"));
    m_challengesTable->setHorizontalHeaderLabels(
        {QStringLiteral("Challenge"), QStringLiteral("Kind"), QStringLiteral("Status"),
         QStringLiteral("Dates"), QStringLiteral("Participants"), QStringLiteral("You")});
    m_challengesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_challengesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_challengesTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_challengesTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_challengesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_challengesTable->setAlternatingRowColors(true);
    challengesLayout->addWidget(m_challengesTable, 1);
    m_challengesEmpty = mutedLabel(QStringLiteral("No challenges yet. Start one and invite your friends."));
    m_challengesEmpty->setAlignment(Qt::AlignHCenter);
    challengesLayout->addWidget(m_challengesEmpty);
    if (next) {
        challengesLayout->setContentsMargins(24, 20, 24, 24);
        challengesLayout->setSpacing(12);
        challengesHeading->setSpacing(8);
        m_challengesTable->setProperty("dataTable", true);
        for (QPushButton *button : {openChallenge, newChallenge})
            button->setProperty("caloriesRole", QStringLiteral("small"));
    }
    m_tabs->addTab(challengesPage, QStringLiteral("Challenges"));
    if (next)
        m_tabs->setDocumentMode(true);
    outer->addWidget(m_tabs, 1);

    connect(refreshButton, &QPushButton::clicked, this, [this] { load(); });
    connect(m_installButton, &QPushButton::clicked, this, [this] { installPlugin(); });
    connect(m_exportButton, &QPushButton::clicked, this, [this] { this->exportMine(); });
    connect(m_uninstallButton, &QPushButton::clicked, this, [this] { uninstallPlugin(); });
    // The list below is the whole diary, so picking a day neither reloads it
    // nor scopes it: it only moves where the next food lands.
    connect(m_day, &QDateEdit::dateChanged, this,
            [this](const QDate &) { if (!m_rendering) m_dayPicked = true; });
    connect(todayButton, &QPushButton::clicked, this, [this] {
        m_day->setDate(QDate::currentDate());
        m_dayPicked = false;
    });
    connect(photoButton, &QPushButton::clicked, this, [this] { photoEstimate(); });
    connect(receiptButton, &QPushButton::clicked, this, [this] { pickReceipt(); });
    connect(m_quickFilter, &QLineEdit::textChanged, this, [this] { renderQuick(); });
    connect(m_quickTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item) { quickAdd(item); });
    connect(m_quickTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        const QString list = item ? item->data(0, kQuickListRole).toString() : QString();
        m_quickLogButton->setEnabled(!list.isEmpty());
        m_quickStarButton->setEnabled(!list.isEmpty() && list != QLatin1String("mine"));
        m_quickRemoveButton->setEnabled(list == QLatin1String("mine"));
    });
    connect(m_quickLogButton, &QPushButton::clicked, this,
            [this] { quickAdd(m_quickTree->currentItem()); });
    connect(m_quickStarButton, &QPushButton::clicked, this,
            [this] { starFood(quickFood(m_quickTree->currentItem())); });
    connect(m_quickRemoveButton, &QPushButton::clicked, this,
            [this] { removeMyFood(quickFood(m_quickTree->currentItem())); });
    connect(describeButton, &QPushButton::clicked, this, [this] { describeMeal(); });
    connect(barcodeButton, &QPushButton::clicked, this, [this] { barcodeLookup(); });
    connect(m_foodSearch, &QLineEdit::textEdited, m_foodTimer, qOverload<>(&QTimer::start));
    connect(m_foodTimer, &QTimer::timeout, this, [this] { searchFoods(); });
    connect(m_foodSearch, &QLineEdit::returnPressed, this, [this] {
        const QModelIndex current = m_foodCompleter->popup()->currentIndex();
        if (m_foodCompleter->popup()->isVisible() && current.isValid())
            pickFood(current.data(Qt::UserRole).toInt());
        else
            searchFoods(true);
    });
    connect(m_foodCompleter, qOverload<const QModelIndex &>(&QCompleter::activated), this,
            [this](const QModelIndex &index) { pickFood(index.data(Qt::UserRole).toInt()); });
    connect(addButton, &QPushButton::clicked, this, [this] { editEntry(); });
    connect(weighButton, &QPushButton::clicked, this, [this] { weighIn(); });
    connect(editButton, &QPushButton::clicked, this,
            [this] { editEntry(entryAt(m_entriesTable->currentRow())); });
    connect(m_entriesTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *item) { editEntry(entryAt(item->row())); });
    connect(deleteButton, &QPushButton::clicked, this,
            [this] { deleteEntry(text(entryAt(m_entriesTable->currentRow()), "id")); });
    connect(saveGoals, &QPushButton::clicked, this, [this] { this->saveGoals(); });
    connect(exportMine, &QPushButton::clicked, this, [this] { this->exportMine(); });
    connect(saveKey, &QPushButton::clicked, this, [this] { saveSettings(); });
    connect(addReminder, &QPushButton::clicked, this, [this] { addReminderRow(); });
    connect(removeReminder, &QPushButton::clicked, this, [this] {
        const int row = m_remindersTable->currentRow();
        if (row >= 0)
            m_remindersTable->removeRow(row);
    });
    connect(m_units, &QComboBox::currentIndexChanged, this, [this](int) {
        const bool imperialUnits = formImperial();
        m_targetWeight->setSuffix(imperialUnits ? QStringLiteral(" lb") : QStringLiteral(" kg"));
        m_startWeight->setSuffix(imperialUnits ? QStringLiteral(" lb") : QStringLiteral(" kg"));
        m_height->setSuffix(imperialUnits ? QStringLiteral(" in") : QStringLiteral(" cm"));
    });
    connect(newChallenge, &QPushButton::clicked, this, [this] { this->newChallenge(); });
    connect(openChallenge, &QPushButton::clicked, this, [this] {
        const int row = m_challengesTable->currentRow();
        if (row >= 0 && row < m_challenges.size())
            this->openChallenge(text(m_challenges.at(row).toObject(), "id"));
    });
    connect(m_challengesTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *item) {
        if (item->row() < m_challenges.size())
            this->openChallenge(text(m_challenges.at(item->row()).toObject(), "id"));
    });
    for (QPushButton *button : {m_quickLogButton, m_quickStarButton, m_quickRemoveButton})
        button->setEnabled(false);
    if (next) {
        connect(earlierDay, &QToolButton::clicked, this, [this] { m_day->setDate(m_day->date().addDays(-1)); });
        connect(laterDay, &QToolButton::clicked, this, [this] { m_day->setDate(m_day->date().addDays(1)); });
        connect(m_day, &QDateEdit::dateChanged, this, [this] { syncNextDay(); });
        syncNextDay();
        arrangeNextHeader(heading, title, lede, refreshButton);
    }
    applyNextStyle();
    applyInstallState();
}

void CaloriesFeature::request(const QByteArray &method, const QString &path,
                              const QJsonObject &body, ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("Calorie Counter network access is unavailable."));
        return;
    }
    const QPointer<CaloriesFeature> guard(this);
    m_request(method, path, body,
              [guard, handler = std::move(handler)](
                  bool ok, const QJsonObject &payload, const QString &error) mutable {
        if (!guard)
            return;
        handler(ok, payload, error);
        // Uninstalled from the web or the phone meanwhile: show the install
        // panel again instead of leaving a stale diary on screen.
        if (guard && !ok && text(payload, "error") == QLatin1String("plugin_not_installed"))
            guard->load();
    });
}

QString CaloriesFeature::apiPath(const QString &suffix) const
{
    return QStringLiteral("/api/calories") + suffix;
}

void CaloriesFeature::refresh()
{
    load();
}

QList<forkmesh::market::NativePackageSearchHit>
CaloriesFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty() || !m_installed)
        return hits;
    const auto full = [&] { return limit > 0 && hits.size() >= limit; };
    for (const QJsonValue &value : m_entries) {
        if (full())
            break;
        const QJsonObject entry = value.toObject();
        bool matches = mealLabel(text(entry, "meal")).contains(needle, Qt::CaseInsensitive);
        for (const char *field : {"name", "brand", "portion", "notes", "source"})
            matches = matches || text(entry, field).contains(needle, Qt::CaseInsensitive);
        if (!matches)
            continue;
        forkmesh::market::NativePackageSearchHit hit;
        hit.id = QStringLiteral("entry:") + text(entry, "id");
        hit.title = text(entry, "name");
        QStringList detail{mealLabel(text(entry, "meal")), kcal(number(entry, "calories"))};
        if (!text(entry, "brand").isEmpty())
            detail << text(entry, "brand");
        hit.detail = detail.join(QString::fromUtf8(" \xC2\xB7 "));
        hits.append(hit);
    }
    for (const QJsonValue &value : m_challenges) {
        if (full())
            break;
        const QJsonObject challenge = value.toObject();
        if (!text(challenge, "title").contains(needle, Qt::CaseInsensitive) &&
            !kindLabel(text(challenge, "kind")).contains(needle, Qt::CaseInsensitive))
            continue;
        forkmesh::market::NativePackageSearchHit hit;
        hit.id = QStringLiteral("challenge:") + text(challenge, "id");
        hit.title = text(challenge, "title");
        hit.detail = QStringLiteral("Challenge") + QString::fromUtf8(" \xC2\xB7 ") +
                     statusLabel(text(challenge, "status"));
        hits.append(hit);
    }
    return hits;
}

void CaloriesFeature::activateSearchHit(const QString &id)
{
    m_focusHit = id;
    focusSearchHit();
}

void CaloriesFeature::focusSearchHit()
{
    if (m_focusHit.isEmpty() || !m_installed)
        return;
    const bool challenge = m_focusHit.startsWith(QLatin1String("challenge:"));
    const QString id = m_focusHit.mid(m_focusHit.indexOf(QLatin1Char(':')) + 1);
    const QJsonArray &records = challenge ? m_challenges : m_entries;
    QTableWidget *table = challenge ? m_challengesTable : m_entriesTable;
    for (int row = 0; row < records.size() && row < table->rowCount(); ++row) {
        if (text(records.at(row).toObject(), "id") != id)
            continue;
        for (QWidget *page = table; page; page = page->parentWidget()) {
            if (m_tabs->indexOf(page) >= 0) {
                m_tabs->setCurrentIndex(m_tabs->indexOf(page));
                break;
            }
        }
        table->selectRow(row);
        table->scrollToItem(table->item(row, 0));
        // Kept while a reload is in flight: it rebuilds the table, and the
        // row is found again when the diary lands.
        if (!m_loading)
            m_focusHit.clear();
        return;
    }
}

void CaloriesFeature::applyInstallState()
{
    m_installPanel->setVisible(m_checked && !m_installed);
    m_tabs->setVisible(m_installed);
    m_exportButton->setVisible(m_installed);
    m_uninstallButton->setVisible(m_installed);
    if (m_promptTargetChanged)
        m_promptTargetChanged();
}

void CaloriesFeature::installPlugin()
{
    m_installButton->setEnabled(false);
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/install")),
            QJsonObject{{QStringLiteral("tzOffsetMinutes"), -QDateTime::currentDateTime().offsetFromUtc() / 60}},
            [this](bool ok, const QJsonObject &, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok)
            return setStatus(QStringLiteral("Calorie Counter was not installed: %1").arg(error), true);
        setStatus(QStringLiteral("Calorie Counter installed for your account."));
        load();
    });
}

QString CaloriesFeature::logDay() const
{
    return m_day->date().toString(Qt::ISODate);
}

void CaloriesFeature::load()
{
    if (m_loading)
        return;
    m_loading = true;
    setStatus(m_installed ? QStringLiteral("Loading your diary…") : QStringLiteral("Loading Calorie Counter…"));
    request(QByteArrayLiteral("GET"),
            apiPath(QStringLiteral("?day=") + QDate::currentDate().toString(Qt::ISODate)), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_loading = false;
        if (!ok) {
            setStatus(QStringLiteral("Calorie Counter could not load: %1").arg(error), true);
            return;
        }
        m_checked = true;
        m_installed = payload.value(QStringLiteral("installed")).toBool();
        applyInstallState();
        if (!m_installed) {
            m_overview = QJsonObject();
            m_entries = QJsonArray();
            m_challenges = QJsonArray();
            setStatus(QStringLiteral("Calorie Counter is not installed for your account yet."));
            return;
        }
        applyOverview(payload);
    });
}

void CaloriesFeature::applyOverview(const QJsonObject &payload)
{
    m_overview = payload;
    m_entries = payload.value(QStringLiteral("entries")).toArray();
    m_challenges = payload.value(QStringLiteral("challenges")).toArray();
    m_rendering = true;
    renderDiary();
    renderQuick();
    renderTrends();
    renderGoals();
    renderChallenges();
    m_rendering = false;
    focusSearchHit();
    if (m_searchIndexChanged)
        m_searchIndexChanged();
    const QJsonObject sources = payload.value(QStringLiteral("sources")).toObject();
    const QString counted = QStringLiteral("%1 entr%2 logged.")
                                .arg(m_entries.size())
                                .arg(m_entries.size() == 1 ? QStringLiteral("y") : QStringLiteral("ies"));
    setStatus(sources.value(QStringLiteral("ai")).toBool()
        ? counted
        : counted + QStringLiteral(" AI photo estimates are unavailable on this host; "
                                   "add meals by hand, barcode, or search."));
    if (!m_afterLoadStatus.isEmpty()) {
        setStatus(m_afterLoadStatus);
        m_afterLoadStatus.clear();
    }
}

bool CaloriesFeature::imperial() const
{
    return text(m_overview.value(QStringLiteral("profile")).toObject(), "units") == QLatin1String("imperial");
}

double CaloriesFeature::displayWeight(double value) const
{
    return imperial() ? value / kKgPerLb : value;
}

bool CaloriesFeature::formImperial() const
{
    // "auto" shows what the Worker resolved for this location, which differs
    // from the unit on screen while the member is leaving a pinned one.
    const QString choice = m_units->currentData().toString();
    if (choice != QLatin1String("auto"))
        return choice == QLatin1String("imperial");
    const QString autoUnits =
        text(m_overview.value(QStringLiteral("profile")).toObject(), "autoUnits");
    return autoUnits.isEmpty() ? imperial() : autoUnits == QLatin1String("imperial");
}

double CaloriesFeature::kilograms(double displayed) const
{
    return formImperial() ? displayed * kKgPerLb : displayed;
}

QString CaloriesFeature::weightText(double value) const
{
    if (value <= 0)
        return QString::fromUtf8("\xE2\x80\x94");
    return grams(displayWeight(value)) + (imperial() ? QStringLiteral(" lb") : QStringLiteral(" kg"));
}

void CaloriesFeature::renderDiary()
{
    const QJsonObject profile = m_overview.value(QStringLiteral("profile")).toObject();
    const QJsonObject totals = m_overview.value(QStringLiteral("todayTotals")).toObject();
    const int goal = profile.value(QStringLiteral("dailyCalories")).toInt();
    const double calories = number(totals, "calories");
    if (!m_dayPicked)
        m_day->setDate(QDate::currentDate());
    m_caloriesLabel->setText(QStringLiteral("%1 kcal").arg(kcal(calories)));
    setGoalBar(m_caloriesBar, calories, goal);
    m_caloriesCaption->setText(goal <= 0
        ? QStringLiteral("Set a daily calorie goal on the Goals tab.")
        : calories <= goal ? QStringLiteral("%1 kcal left of %2").arg(kcal(goal - calories), kcal(goal))
                           : QStringLiteral("%1 kcal over the %2 goal").arg(kcal(calories - goal), kcal(goal)));
    const struct { QProgressBar *bar; QLabel *label; const char *key; const char *goalKey; } macros[] = {
        {m_proteinBar, m_proteinLabel, "proteinG", "proteinG"},
        {m_carbsBar, m_carbsLabel, "carbsG", "carbsG"},
        {m_fatBar, m_fatLabel, "fatG", "fatG"},
    };
    for (const auto &macro : macros) {
        setGoalBar(macro.bar, number(totals, macro.key), number(profile, macro.goalKey));
        macro.label->setText(QStringLiteral("%1 g of %2 g")
                                 .arg(grams(number(totals, macro.key)), grams(number(profile, macro.goalKey))));
    }
    const QJsonObject streaks = m_overview.value(QStringLiteral("streaks")).toObject();
    m_streakLabel->setText(QStringLiteral("%1 days logged in a row · %2 days within goal")
                               .arg(streaks.value(QStringLiteral("logged")).toInt())
                               .arg(streaks.value(QStringLiteral("onGoal")).toInt()));
    const QJsonObject latest = m_overview.value(QStringLiteral("latestWeight")).toObject();
    const double target = number(profile, "targetWeightKg");
    const double latestKg = number(latest, "weightKg");
    QString weight = latestKg > 0 ? weightText(latestKg) + QStringLiteral(" on ") + dayLabel(text(latest, "day"))
                                  : QStringLiteral("No weigh-in yet");
    if (latestKg > 0 && target > 0)
        weight += QStringLiteral(" · %1 %2").arg(weightText(std::fabs(latestKg - target)),
                                                 latestKg > target ? QStringLiteral("to go") : QStringLiteral("below target"));
    m_weightLabel->setText(weight);

    // Every entry the account has logged, newest first, with the day it
    // belongs to in its own column instead of a day-scoped list.
    m_entriesTable->setRowCount(0);
    const QString today = text(m_overview, "today");
    for (const QJsonValue &value : m_entries) {
        const QJsonObject entry = value.toObject();
        const int row = m_entriesTable->rowCount();
        m_entriesTable->insertRow(row);
        const QString day = text(entry, "day");
        m_entriesTable->setItem(row, 0, new QTableWidgetItem(
            day == today ? QStringLiteral("Today") : dayLabel(day)));
        QString food = text(entry, "name");
        const QString detail = QStringList{text(entry, "portion"), text(entry, "brand")}
                                   .filter(QRegularExpression(QStringLiteral(".+"))).join(QStringLiteral(" · "));
        if (!detail.isEmpty())
            food += QStringLiteral(" (") + detail + QLatin1Char(')');
        auto *nameItem = new QTableWidgetItem(food);
        nameItem->setData(kEntryIdRole, text(entry, "id"));
        m_entriesTable->setItem(row, 1, nameItem);
        m_entriesTable->setItem(row, 2, new QTableWidgetItem(mealLabel(text(entry, "meal"))));
        for (int column = 3; column < 7; ++column) {
            const char *keys[] = {"calories", "proteinG", "carbsG", "fatG"};
            auto *item = new QTableWidgetItem(column == 3 ? kcal(number(entry, keys[0]))
                                                          : grams(number(entry, keys[column - 3])));
            item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_entriesTable->setItem(row, column, item);
        }
        m_entriesTable->setItem(row, 7, new QTableWidgetItem(text(entry, "source")));
    }
    m_entriesEmpty->setVisible(m_entries.isEmpty());
    const bool truncated = m_overview.value(QStringLiteral("entriesTruncated")).toBool();
    m_entriesMore->setText(truncated
        ? QStringLiteral("Showing the %1 most recent entries. Older ones stay in your export.")
              .arg(m_entries.size())
        : QString());
    m_entriesMore->setVisible(truncated);
    if (Theme::isNext())
        renderNextDiary();
}

void CaloriesFeature::renderTrends()
{
    const QJsonObject profile = m_overview.value(QStringLiteral("profile")).toObject();
    m_dayChart->setDays(m_overview.value(QStringLiteral("days")).toArray(),
                        profile.value(QStringLiteral("dailyCalories")).toInt());
    m_weightChart->setPoints(m_overview.value(QStringLiteral("weights")).toArray(),
                             number(profile, "targetWeightKg"), imperial());
    m_trendsSummary->setText(QStringLiteral("Seven-day average %1 kcal · goal %2 kcal")
                                 .arg(kcal(number(m_overview, "weekAverage")),
                                      kcal(profile.value(QStringLiteral("dailyCalories")).toInt())));
}

void CaloriesFeature::renderGoals()
{
    const QJsonObject profile = m_overview.value(QStringLiteral("profile")).toObject();
    m_goalCalories->setValue(profile.value(QStringLiteral("dailyCalories")).toInt());
    m_goalProtein->setValue(profile.value(QStringLiteral("proteinG")).toInt());
    m_goalCarbs->setValue(profile.value(QStringLiteral("carbsG")).toInt());
    m_goalFat->setValue(profile.value(QStringLiteral("fatG")).toInt());
    const QString autoUnits = text(profile, "autoUnits");
    m_units->setItemText(0, autoUnits.isEmpty()
        ? QStringLiteral("Automatic (match my location)")
        : QStringLiteral("Automatic (match my location: %1)")
              .arg(autoUnits == QLatin1String("imperial") ? QStringLiteral("lb, in")
                                                          : QStringLiteral("kg, cm")));
    const QString preference = text(profile, "unitsPreference");
    m_units->setCurrentIndex(qMax(0, m_units->findData(
        preference.isEmpty() ? text(profile, "units") : preference)));
    const bool imperialUnits = imperial();
    // setCurrentIndex is silent when the index did not move, so the suffixes
    // are set here too: on "auto" they change with the country, not the pick.
    m_targetWeight->setSuffix(imperialUnits ? QStringLiteral(" lb") : QStringLiteral(" kg"));
    m_startWeight->setSuffix(imperialUnits ? QStringLiteral(" lb") : QStringLiteral(" kg"));
    m_height->setSuffix(imperialUnits ? QStringLiteral(" in") : QStringLiteral(" cm"));
    m_targetWeight->setValue(imperialUnits ? number(profile, "targetWeightKg") / kKgPerLb : number(profile, "targetWeightKg"));
    m_startWeight->setValue(imperialUnits ? number(profile, "startWeightKg") / kKgPerLb : number(profile, "startWeightKg"));
    m_height->setValue(imperialUnits ? number(profile, "heightCm") / kCmPerIn : number(profile, "heightCm"));
    const double latest = number(m_overview.value(QStringLiteral("latestWeight")).toObject(), "weightKg");
    const double heightCm = number(profile, "heightCm");
    m_bmiLabel->setText(latest > 0 && heightCm > 0
        ? QStringLiteral("BMI %1 from your latest weigh-in.").arg(QString::number(latest / std::pow(heightCm / 100.0, 2), 'f', 1))
        : QString());
    m_remindersTable->setRowCount(0);
    for (const QJsonValue &value : profile.value(QStringLiteral("reminders")).toArray())
        addReminderRow(value.toObject());
    const QJsonObject sources = m_overview.value(QStringLiteral("sources")).toObject();
    m_usdaStatus->setText(sources.value(QStringLiteral("usda")).toBool()
        ? QStringLiteral("A USDA key is saved.") : QStringLiteral("No USDA key saved."));
}

void CaloriesFeature::addReminderRow(const QJsonObject &reminder)
{
    if (m_remindersTable->rowCount() >= 8)
        return;
    const int row = m_remindersTable->rowCount();
    m_remindersTable->insertRow(row);
    auto *time = new QTimeEdit(QTime::fromString(text(reminder, "time").isEmpty() ? QStringLiteral("12:30") : text(reminder, "time"), QStringLiteral("HH:mm")));
    time->setDisplayFormat(QStringLiteral("HH:mm"));
    m_remindersTable->setCellWidget(row, 0, time);
    m_remindersTable->setCellWidget(row, 1, lineEdit(reminder.isEmpty() ? QStringLiteral("Log lunch") : text(reminder, "label")));
    auto *unlogged = new QCheckBox;
    unlogged->setChecked(reminder.value(QStringLiteral("onlyIfUnlogged")).toBool());
    m_remindersTable->setCellWidget(row, 2, unlogged);
    auto *enabled = new QCheckBox;
    enabled->setChecked(reminder.isEmpty() || reminder.value(QStringLiteral("enabled")).toBool(true));
    m_remindersTable->setCellWidget(row, 3, enabled);
}

QJsonArray CaloriesFeature::remindersPayload() const
{
    QJsonArray reminders;
    for (int row = 0; row < m_remindersTable->rowCount(); ++row) {
        const auto *time = qobject_cast<QTimeEdit *>(m_remindersTable->cellWidget(row, 0));
        const auto *label = qobject_cast<QLineEdit *>(m_remindersTable->cellWidget(row, 1));
        const auto *unlogged = qobject_cast<QCheckBox *>(m_remindersTable->cellWidget(row, 2));
        const auto *enabled = qobject_cast<QCheckBox *>(m_remindersTable->cellWidget(row, 3));
        if (!time || !label || !unlogged || !enabled)
            continue;
        reminders.append(QJsonObject{
            {QStringLiteral("time"), time->time().toString(QStringLiteral("HH:mm"))},
            {QStringLiteral("label"), label->text().trimmed()},
            {QStringLiteral("onlyIfUnlogged"), unlogged->isChecked()},
            {QStringLiteral("enabled"), enabled->isChecked()},
        });
    }
    return reminders;
}

void CaloriesFeature::saveGoals()
{
    const bool imperialUnits = formImperial();
    const QJsonObject body{
        {QStringLiteral("dailyCalories"), m_goalCalories->value()},
        {QStringLiteral("proteinG"), m_goalProtein->value()},
        {QStringLiteral("carbsG"), m_goalCarbs->value()},
        {QStringLiteral("fatG"), m_goalFat->value()},
        {QStringLiteral("unitsPreference"), m_units->currentData().toString()},
        {QStringLiteral("targetWeightKg"), kilograms(m_targetWeight->value())},
        {QStringLiteral("startWeightKg"), kilograms(m_startWeight->value())},
        {QStringLiteral("heightCm"), imperialUnits ? m_height->value() * kCmPerIn : m_height->value()},
        // JavaScript's getTimezoneOffset convention: minutes to add to local
        // time to reach UTC, which the worker uses to ring reminders.
        {QStringLiteral("tzOffsetMinutes"), -QDateTime::currentDateTime().offsetFromUtc() / 60},
        {QStringLiteral("reminders"), remindersPayload()},
    };
    request(QByteArrayLiteral("PUT"), apiPath(QStringLiteral("/profile")), body,
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Goals were not saved: %1").arg(error), true);
        setStatus(QStringLiteral("Goals and reminders saved."));
        load();
    });
}

void CaloriesFeature::saveSettings()
{
    const QString key = m_usdaKey->text().trimmed();
    request(QByteArrayLiteral("PUT"), apiPath(QStringLiteral("/settings")),
            QJsonObject{{QStringLiteral("usdaApiKey"), key}},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Food database settings were not saved: %1").arg(error), true);
        m_usdaKey->clear();
        setStatus(QStringLiteral("Food database settings saved."));
        load();
    });
}

QJsonObject CaloriesFeature::entryAt(int row) const
{
    return row >= 0 && row < m_entries.size() ? m_entries.at(row).toObject() : QJsonObject();
}

void CaloriesFeature::editEntry(const QJsonObject &entry)
{
    const bool editing = !text(entry, "id").isEmpty();
    QDialog dialog(this);
    dialog.setWindowTitle(editing ? QStringLiteral("Edit food") : QStringLiteral("Add food"));
    auto *form = new QFormLayout(&dialog);
    auto *name = lineEdit(text(entry, "name"));
    name->setMaxLength(160);
    auto *portion = lineEdit(text(entry, "portion"));
    portion->setPlaceholderText(QStringLiteral("1 bowl, 150 g…"));
    auto *meal = mealCombo(editing ? text(entry, "meal") : guessMeal());
    auto *day = new QDateEdit(editing ? QDate::fromString(text(entry, "day"), Qt::ISODate)
                                      : m_day->date());
    day->setCalendarPopup(true);
    day->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    auto *calories = new QSpinBox;
    calories->setRange(0, 50000);
    calories->setValue(int(number(entry, "calories")));
    auto *protein = gramsBox(number(entry, "proteinG"));
    auto *carbs = gramsBox(number(entry, "carbsG"));
    auto *fat = gramsBox(number(entry, "fatG"));
    auto *notes = new QPlainTextEdit(text(entry, "notes"));
    notes->setFixedHeight(64);
    form->addRow(QStringLiteral("Food"), name);
    form->addRow(QStringLiteral("Day"), day);
    form->addRow(QStringLiteral("Portion"), portion);
    form->addRow(QStringLiteral("Meal"), meal);
    form->addRow(QStringLiteral("Calories (kcal)"), calories);
    form->addRow(QStringLiteral("Protein (g)"), protein);
    form->addRow(QStringLiteral("Carbs (g)"), carbs);
    form->addRow(QStringLiteral("Fat (g)"), fat);
    form->addRow(QStringLiteral("Notes"), notes);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || name->text().trimmed().isEmpty())
        return;
    QJsonObject body{
        {QStringLiteral("day"), day->date().toString(Qt::ISODate)},
        {QStringLiteral("name"), name->text().trimmed()},
        {QStringLiteral("portion"), portion->text().trimmed()},
        {QStringLiteral("meal"), meal->currentData().toString()},
        {QStringLiteral("calories"), calories->value()},
        {QStringLiteral("proteinG"), protein->value()},
        {QStringLiteral("carbsG"), carbs->value()},
        {QStringLiteral("fatG"), fat->value()},
        {QStringLiteral("notes"), notes->toPlainText()},
    };
    if (!editing)
        body.insert(QStringLiteral("source"), QStringLiteral("manual"));
    request(editing ? QByteArrayLiteral("PATCH") : QByteArrayLiteral("POST"),
            apiPath(editing ? QStringLiteral("/entries/") + text(entry, "id") : QStringLiteral("/entries")),
            body, [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The entry was not saved: %1").arg(error), true);
        setStatus(QStringLiteral("Saved to your diary."));
        load();
    });
}

void CaloriesFeature::deleteEntry(const QString &id)
{
    if (id.isEmpty())
        return;
    if (QMessageBox::question(this, QStringLiteral("Delete entry"),
                              QStringLiteral("Delete this diary entry?")) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("DELETE"), apiPath(QStringLiteral("/entries/") + id), {},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The entry was not deleted: %1").arg(error), true);
        load();
    });
}

void CaloriesFeature::addEntries(const QJsonArray &items, const QString &meal, const QString &source,
                                 const QString &day, const QString &doneMessage)
{
    if (items.isEmpty())
        return;
    // Post the items one after another; the diary reloads once the last lands.
    auto remaining = std::make_shared<QJsonArray>(items);
    auto step = std::make_shared<std::function<void()>>();
    const QString targetDay = day.isEmpty() ? logDay() : day;
    *step = [this, remaining, meal, source, targetDay, doneMessage, step] {
        if (remaining->isEmpty()) {
            setStatus(QStringLiteral("Added to your diary."));
            if (!doneMessage.isEmpty()) {
                m_afterLoadStatus = doneMessage;
                setQuickStatus(doneMessage);
            }
            load();
            return;
        }
        QJsonObject body = remaining->first().toObject();
        remaining->removeFirst();
        body.insert(QStringLiteral("day"), targetDay);
        body.insert(QStringLiteral("meal"), meal);
        body.insert(QStringLiteral("source"), source);
        request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/entries")), body,
                [this, step](bool ok, const QJsonObject &, const QString &error) {
            if (!ok) {
                setStatus(QStringLiteral("An item was not saved: %1").arg(error), true);
                load();
                return;
            }
            (*step)();
        });
    };
    (*step)();
}

void CaloriesFeature::photoEstimate()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose a meal photo"), QString(),
        QStringLiteral("Images (*.jpg *.jpeg *.png *.webp)"));
    if (path.isEmpty())
        return;
    const QString image = jpegDataUrl(path, 1024);
    if (image.isEmpty())
        return setStatus(QStringLiteral("That file is not an image the desktop can read."), true);
    const QString hint = QInputDialog::getText(
        this, QStringLiteral("Anything the model should know?"),
        QStringLiteral("Optional hint (half portion, brown rice…)"));
    setStatus(QStringLiteral("Asking the model about the photo…"));
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/analyze")),
            QJsonObject{{QStringLiteral("image"), image},
                        {QStringLiteral("hint"), hint.trimmed().left(300)}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The photo could not be estimated: %1").arg(error), true);
        showSuggestions(payload);
    });
}

void CaloriesFeature::describeMeal()
{
    const QString description = QInputDialog::getMultiLineText(
        this, QStringLiteral("Describe a meal"),
        QStringLiteral("The host AI model estimates calories and macros from your description."),
        QStringLiteral("2 scrambled eggs, 1 slice of toast with butter")).trimmed();
    if (description.isEmpty())
        return;
    setStatus(QStringLiteral("Asking the model about the meal…"));
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/analyze")),
            QJsonObject{{QStringLiteral("description"), description.left(1000)}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The meal could not be estimated: %1").arg(error), true);
        showSuggestions(payload);
    });
}

void CaloriesFeature::showSuggestions(const QJsonObject &payload)
{
    const QJsonArray items = payload.value(QStringLiteral("items")).toArray();
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Suggested foods"));
    dialog.resize(560, 420);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(mutedLabel(QStringLiteral("Untick anything wrong and correct the calories before adding. %1")
                                     .arg(text(payload, "summary"))));
    auto *table = new QTableWidget(items.size(), 4);
    table->setHorizontalHeaderLabels({QStringLiteral("Add"), QStringLiteral("Food"), QStringLiteral("Portion"), QStringLiteral("kcal")});
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    for (int row = 0; row < items.size(); ++row) {
        const QJsonObject item = items.at(row).toObject();
        auto *add = new QCheckBox;
        add->setChecked(true);
        table->setCellWidget(row, 0, add);
        auto *name = new QTableWidgetItem(text(item, "name"));
        name->setFlags(name->flags() & ~Qt::ItemIsEditable);
        name->setToolTip(QStringLiteral("Confidence %1% · P %2 g · C %3 g · F %4 g")
                             .arg(qRound(number(item, "confidence") * 100))
                             .arg(grams(number(item, "proteinG")), grams(number(item, "carbsG")), grams(number(item, "fatG"))));
        table->setItem(row, 1, name);
        auto *portion = new QTableWidgetItem(text(item, "portion"));
        portion->setFlags(portion->flags() & ~Qt::ItemIsEditable);
        table->setItem(row, 2, portion);
        auto *calories = new QSpinBox;
        calories->setRange(0, 50000);
        calories->setValue(int(number(item, "calories")));
        table->setCellWidget(row, 3, calories);
    }
    layout->addWidget(table, 1);
    auto *mealRow = new QHBoxLayout;
    mealRow->addWidget(new QLabel(QStringLiteral("Meal")));
    auto *meal = mealCombo(guessMeal());
    mealRow->addWidget(meal);
    mealRow->addStretch();
    layout->addLayout(mealRow);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Add selected to diary"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    setStatus(QStringLiteral("%1 item(s) suggested, about %2 kcal.")
                  .arg(items.size()).arg(kcal(number(payload, "totalCalories"))));
    if (dialog.exec() != QDialog::Accepted)
        return;
    QJsonArray chosen;
    for (int row = 0; row < items.size(); ++row) {
        const auto *add = qobject_cast<QCheckBox *>(table->cellWidget(row, 0));
        const auto *calories = qobject_cast<QSpinBox *>(table->cellWidget(row, 3));
        if (!add || !add->isChecked() || !calories)
            continue;
        QJsonObject item = items.at(row).toObject();
        item.insert(QStringLiteral("calories"), calories->value());
        chosen.append(item);
    }
    addEntries(chosen, meal->currentData().toString(), text(payload, "source"));
}

void CaloriesFeature::barcodeLookup()
{
    const QString code = QInputDialog::getText(
        this, QStringLiteral("Look up a barcode"),
        QStringLiteral("Type the digits printed under the barcode (Open Food Facts%1).")
            .arg(m_overview.value(QStringLiteral("sources")).toObject().value(QStringLiteral("usda")).toBool()
                     ? QStringLiteral(" and USDA FoodData Central") : QString()))
        .trimmed().remove(QRegularExpression(QStringLiteral("\\D")));
    if (code.size() < 6 || code.size() > 14)
        return;
    setStatus(QStringLiteral("Looking up %1…").arg(code));
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/barcode/") + code), {},
            [this, code](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("No product found for %1 (%2). Add it by hand or search by name.").arg(code, error), true);
        addProduct(payload.value(QStringLiteral("product")).toObject(), QStringLiteral("barcode"));
    });
}

void CaloriesFeature::searchFoods(bool now)
{
    m_foodTimer->stop();
    const QString value = m_foodSearch->text().simplified();
    const QString key = value.toLower();
    if (value.size() < 2) {
        m_foodQuery.clear();
        m_foodCompleter->popup()->hide();
        return;
    }
    if (key == m_foodQuery) {
        if (now && !m_foods.isEmpty())
            pickFood(0);
        return;
    }
    m_foodQuery = key;
    if (m_foodCache.contains(key)) {
        showFoods(value, m_foodCache.value(key));
        return;
    }
    request(QByteArrayLiteral("GET"),
            apiPath(QStringLiteral("/foods?q=") + QString::fromLatin1(QUrl::toPercentEncoding(value))), {},
            [this, value, key](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            if (key == m_foodQuery) {
                m_foodQuery.clear();
                setStatus(QStringLiteral("Food search failed: %1").arg(error), true);
            }
            return;
        }
        const QJsonArray foods = payload.value(QStringLiteral("foods")).toArray();
        if (m_foodCache.size() >= 60)
            m_foodCache.clear();
        m_foodCache.insert(key, foods);
        if (key == m_foodQuery)
            showFoods(value, foods);
    });
}

void CaloriesFeature::showFoods(const QString &query, const QJsonArray &foods)
{
    m_foods = foods;
    m_foodModel->clear();
    for (int row = 0; row < foods.size(); ++row) {
        const QJsonObject food = foods.at(row).toObject();
        const QJsonObject per100 = food.value(QStringLiteral("per100g")).toObject();
        const QJsonObject perServing = food.value(QStringLiteral("perServing")).toObject();
        QString line = text(food, "name");
        if (!text(food, "brand").isEmpty())
            line += QStringLiteral(" · ") + text(food, "brand");
        line += QStringLiteral(" — %1 kcal/100 g").arg(kcal(number(per100, "calories")));
        if (!perServing.isEmpty())
            line += QStringLiteral(", %1 kcal per %2")
                        .arg(kcal(number(perServing, "calories")),
                             text(food, "servingSize").isEmpty() ? QStringLiteral("serving") : text(food, "servingSize"));
        auto *item = new QStandardItem(line);
        item->setData(row, Qt::UserRole);
        item->setToolTip(QStringLiteral("P %1 · C %2 · F %3 per 100 g · %4")
                             .arg(grams(number(per100, "proteinG")), grams(number(per100, "carbsG")),
                                  grams(number(per100, "fatG")),
                                  text(food, "source") == QLatin1String("usda") ? QStringLiteral("USDA FoodData Central")
                                                                                : QStringLiteral("Open Food Facts")));
        m_foodModel->appendRow(item);
    }
    if (foods.isEmpty()) {
        auto *item = new QStandardItem(QStringLiteral("Nothing found for “%1” yet. Keep typing, try the brand first, or add it by hand.").arg(query));
        item->setData(-1, Qt::UserRole);
        item->setFlags(Qt::ItemIsEnabled);
        m_foodModel->appendRow(item);
    }
    if (!m_foodSearch->hasFocus() && !m_foodCompleter->popup()->isVisible())
        return;
    m_foodCompleter->setCompletionPrefix(QString());
    QRect anchor = m_foodSearch->rect();
    anchor.setWidth(qMax(anchor.width(), 520));
    m_foodCompleter->complete(anchor);
}

void CaloriesFeature::pickFood(int row)
{
    if (row < 0 || row >= m_foods.size())
        return;
    const QJsonObject food = m_foods.at(row).toObject();
    m_foodCompleter->popup()->hide();
    m_foodSearch->clear();
    m_foodQuery.clear();
    m_foods = QJsonArray();
    addProduct(food, QStringLiteral("search"));
}

void CaloriesFeature::addProduct(const QJsonObject &product, const QString &source)
{
    const QJsonObject per100 = product.value(QStringLiteral("per100g")).toObject();
    const QJsonObject perServing = product.value(QStringLiteral("perServing")).toObject();
    QDialog dialog(this);
    dialog.setWindowTitle(text(product, "name"));
    auto *form = new QFormLayout(&dialog);
    form->addRow(mutedLabel(QStringList{text(product, "brand"), text(product, "quantity"),
                                        text(product, "source") == QLatin1String("usda") ? QStringLiteral("USDA FoodData Central")
                                                                                          : QStringLiteral("Open Food Facts")}
                                .filter(QRegularExpression(QStringLiteral(".+"))).join(QStringLiteral(" · "))));
    auto *basis = new QComboBox;
    if (!perServing.isEmpty())
        basis->addItem(QStringLiteral("1 serving%1").arg(text(product, "servingSize").isEmpty() ? QString()
                           : QStringLiteral(" (") + text(product, "servingSize") + QLatin1Char(')')), QStringLiteral("serving"));
    basis->addItem(QStringLiteral("100 g"), QStringLiteral("100g"));
    basis->addItem(QStringLiteral("1 g"), QStringLiteral("1g"));
    auto *amount = new QDoubleSpinBox;
    amount->setRange(0.1, 1000);
    amount->setDecimals(1);
    amount->setValue(1);
    auto *meal = mealCombo(guessMeal());
    auto *preview = mutedLabel(QString());
    form->addRow(QStringLiteral("Basis"), basis);
    form->addRow(QStringLiteral("Amount"), amount);
    form->addRow(QStringLiteral("Meal"), meal);
    form->addRow(preview);
    auto compute = [&]() {
        const QString kind = basis->currentData().toString();
        const QJsonObject per = kind == QLatin1String("serving") ? perServing : per100;
        const double factor = kind == QLatin1String("1g") ? amount->value() / 100.0 : amount->value();
        QJsonObject values{
            {QStringLiteral("name"), text(product, "name")},
            {QStringLiteral("brand"), text(product, "brand")},
            {QStringLiteral("barcode"), text(product, "barcode")},
            {QStringLiteral("calories"), qRound(number(per, "calories") * factor)},
            {QStringLiteral("proteinG"), std::round(number(per, "proteinG") * factor * 10) / 10},
            {QStringLiteral("carbsG"), std::round(number(per, "carbsG") * factor * 10) / 10},
            {QStringLiteral("fatG"), std::round(number(per, "fatG") * factor * 10) / 10},
            {QStringLiteral("portion"), kind == QLatin1String("serving")
                 ? QStringLiteral("%1 serving(s)%2").arg(amount->value()).arg(text(product, "servingSize").isEmpty() ? QString()
                       : QStringLiteral(" (") + text(product, "servingSize") + QLatin1Char(')'))
                 : QStringLiteral("%1 g").arg(kind == QLatin1String("1g") ? amount->value() : amount->value() * 100)},
        };
        preview->setText(QStringLiteral("%1 kcal · P %2 g · C %3 g · F %4 g")
                             .arg(kcal(number(values, "calories")), grams(number(values, "proteinG")),
                                  grams(number(values, "carbsG")), grams(number(values, "fatG"))));
        return values;
    };
    compute();
    connect(basis, &QComboBox::currentIndexChanged, &dialog, [&compute](int) { compute(); });
    connect(amount, &QDoubleSpinBox::valueChanged, &dialog, [&compute](double) { compute(); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Add to diary"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    addEntries(QJsonArray{compute()}, meal->currentData().toString(), source);
}

void CaloriesFeature::weighIn(const QString &day)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Weigh in"));
    auto *form = new QFormLayout(&dialog);
    auto *date = new QDateEdit(day.isEmpty() ? m_day->date() : QDate::fromString(day, Qt::ISODate));
    date->setCalendarPopup(true);
    date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    auto *weight = new QDoubleSpinBox;
    weight->setRange(1, 1500);
    weight->setDecimals(1);
    weight->setSuffix(imperial() ? QStringLiteral(" lb") : QStringLiteral(" kg"));
    const double latest = number(m_overview.value(QStringLiteral("latestWeight")).toObject(), "weightKg");
    if (latest > 0)
        weight->setValue(displayWeight(latest));
    form->addRow(QStringLiteral("Day"), date);
    form->addRow(QStringLiteral("Weight"), weight);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    auto *remove = buttons->addButton(QStringLiteral("Remove"), QDialogButtonBox::DestructiveRole);
    form->addRow(buttons);
    bool removing = false;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(remove, &QPushButton::clicked, &dialog, [&] { removing = true; dialog.accept(); });
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString path = apiPath(QStringLiteral("/weights/") + date->date().toString(Qt::ISODate));
    const double value = imperial() ? weight->value() * kKgPerLb : weight->value();
    request(removing ? QByteArrayLiteral("DELETE") : QByteArrayLiteral("PUT"), path,
            removing ? QJsonObject() : QJsonObject{{QStringLiteral("weightKg"), value}},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The weigh-in was not saved: %1").arg(error), true);
        setStatus(QStringLiteral("Weigh-in saved."));
        load();
    });
}

QJsonArray CaloriesFeature::frequentFoods() const
{
    QSet<QString> saved;
    for (const QJsonValue &value : m_overview.value(QStringLiteral("quickFoods")).toArray())
        saved.insert(foodKey(text(value.toObject(), "name")));
    QStringList order;
    QHash<QString, QPair<int, QJsonObject>> counts;
    for (const QJsonValue &value : m_entries) {
        const QJsonObject entry = value.toObject();
        const QString key = foodKey(text(entry, "name"));
        if (key.isEmpty() || saved.contains(key))
            continue;
        if (counts.contains(key)) {
            counts[key].first += 1;
            continue;
        }
        order << key;
        counts.insert(key, {1, foodFields(entry)});
    }
    std::stable_sort(order.begin(), order.end(), [&counts](const QString &a, const QString &b) {
        return counts.value(a).first > counts.value(b).first;
    });
    QJsonArray result;
    for (const QString &key : std::as_const(order)) {
        const auto &[times, food] = counts.value(key);
        if (times < 2 || result.size() >= 8)
            break;
        QJsonObject item = food;
        item.insert(QStringLiteral("times"), times);
        result.append(item);
    }
    return result;
}

QJsonArray CaloriesFeature::promptMatches(const QString &value) const
{
    static const QRegularExpression numeric(
        QStringLiteral("^(\\d+/\\d+|\\d+(?:[.,]\\d+)?)\\s*(?:x\\s+)?"));
    static const QRegularExpression worded(QStringLiteral(
        "^(a couple of|a few|a half|an|a|one|two|three|four|five|six|seven|eight|nine|ten|half|couple)\\s+"));
    static const QRegularExpression filler(QStringLiteral("^(?:(?:of|some|an?)\\s+)+"));
    QHash<QString, QJsonObject> index;
    for (const QJsonArray &list : {m_overview.value(QStringLiteral("quickFoods")).toArray(), m_frequentFoods,
                                   m_overview.value(QStringLiteral("commonFoods")).toArray()}) {
        for (const QJsonValue &item : list) {
            const QJsonObject food = item.toObject();
            const QString key = normaliseFood(text(food, "name"));
            if (!key.isEmpty() && !index.contains(key))
                index.insert(key, food);
        }
    }
    QJsonArray matched;
    for (const QString &part : promptParts(value)) {
        QString rest = part;
        double amount = 1;
        const QRegularExpressionMatch counted = numeric.match(rest);
        const QRegularExpressionMatch word = worded.match(rest);
        if (counted.hasMatch()) {
            const QString digits = counted.captured(1);
            const QStringList fraction = digits.split(QLatin1Char('/'));
            amount = fraction.size() == 2 ? fraction.at(0).toDouble() / fraction.at(1).toDouble()
                                          : QString(digits).replace(QLatin1Char(','), QLatin1Char('.')).toDouble();
            rest = rest.mid(counted.capturedLength(0));
        } else if (word.hasMatch()) {
            amount = numberWord(word.captured(1));
            rest = rest.mid(word.capturedLength(0));
        }
        rest.remove(filler);
        const QJsonObject food = index.value(normaliseFood(rest));
        if (food.isEmpty() || !(amount > 0) || amount > 20)
            return {};
        const auto scaled = [&food, amount](const char *key) {
            return std::round(number(food, key) * amount * 10) / 10;
        };
        const QString portion = text(food, "portion");
        matched.append(QJsonObject{
            {QStringLiteral("name"), text(food, "name")},
            {QStringLiteral("brand"), text(food, "brand")},
            {QStringLiteral("portion"), amount == 1 ? portion
                 : QStringLiteral("%1 × %2").arg(grams(amount), portion.isEmpty() ? QStringLiteral("serving") : portion)},
            {QStringLiteral("calories"), qRound(number(food, "calories") * amount)},
            {QStringLiteral("proteinG"), scaled("proteinG")},
            {QStringLiteral("carbsG"), scaled("carbsG")},
            {QStringLiteral("fatG"), scaled("fatG")},
        });
    }
    return matched;
}

QString CaloriesFeature::loggedMessage(const QJsonArray &items, const QString &meal,
                                       const QString &day) const
{
    double total = 0;
    QStringList parts;
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        total += number(item, "calories");
        parts << QStringLiteral("%1 (%2 kcal)").arg(text(item, "name"), kcal(number(item, "calories")));
    }
    const bool today = day == QDate::currentDate().toString(Qt::ISODate);
    return QStringLiteral("Logged to %1, %2: %3. %4 kcal in all.")
        .arg(meal, today ? QStringLiteral("today") : dayLabel(day), parts.join(QStringLiteral(", ")), kcal(total));
}

forkmesh::market::NativePackagePromptTarget CaloriesFeature::hostPromptTarget() const
{
    if (!m_installed)
        return {};
    return {QStringLiteral("what did you eat? “2 eggs and toast for breakfast”, “weighed 72.5 kg”, "
                           "or attach a meal photo or receipt"),
            QStringLiteral("Log this to your Calorie Counter diary")};
}

bool CaloriesFeature::submitHostPrompt(const QString &value, const QStringList &attachments)
{
    if (!m_installed)
        return false;
    static const QRegularExpression receiptWords(QStringLiteral("\\b(receipt|grocer(y|ies)|shopping)\\b"),
                                                 QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression weighed(
        QStringLiteral("^(?:i\\s+)?(?:weigh(?:ed|t)?(?:\\s+in)?|my weight(?:\\s+is)?|weight(?:\\s+is)?)\\s*(?:at|is|was|:|=)?\\s*"
                       "(\\d{2,3}(?:[.,]\\d+)?)\\s*(kg|kgs|kilos?|kilograms?|lb|lbs|pounds?)?\\s*(?:today|yesterday|this morning)?\\.?$"),
        QRegularExpression::CaseInsensitiveOption);
    QString image;
    for (const QString &path : attachments) {
        if (!QImageReader::imageFormat(path).isEmpty()) {
            image = path;
            break;
        }
    }
    if (value.isEmpty() && image.isEmpty()) {
        setStatus(QStringLiteral("Type what you ate, or attach a meal photo or receipt."), true);
        return false;
    }
    if (!image.isEmpty() && (receiptWords.match(value).hasMatch() ||
                             (value.isEmpty() && QFileInfo(image).fileName().contains(QLatin1String("receipt"), Qt::CaseInsensitive)))) {
        readReceipt(image);
        return true;
    }
    const QString mealWord = promptMeal(value);
    const QString meal = mealWord.isEmpty() ? guessMeal() : mealWord;
    const QString day = promptDay(value, logDay());
    const auto logEstimate = [this, meal, day](const QString &source) {
        return [this, meal, day, source](bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok)
                return setStatus(aiError(payload, error, QStringLiteral("Could not estimate")), true);
            const QJsonArray items = payload.value(QStringLiteral("items")).toArray();
            if (items.isEmpty())
                return setStatus(QStringLiteral("Nothing recognisable as food there. Try naming the food and amount."), true);
            addEntries(items, meal, source, day, loggedMessage(items, meal, day));
        };
    };
    if (!image.isEmpty()) {
        const QString url = jpegDataUrl(image, 1024);
        if (url.isEmpty()) {
            setStatus(QStringLiteral("That file is not an image the desktop can read."), true);
            return false;
        }
        setStatus(QStringLiteral("Asking the model about the photo…"));
        request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/analyze")),
                QJsonObject{{QStringLiteral("image"), url}, {QStringLiteral("hint"), value.left(300)}},
                logEstimate(QStringLiteral("photo")));
        return true;
    }
    const QRegularExpressionMatch weight = weighed.match(value);
    if (weight.hasMatch()) {
        const double amount = weight.captured(1).replace(QLatin1Char(','), QLatin1Char('.')).toDouble();
        const QString unit = weight.captured(2).toLower();
        const double kg = unit.startsWith(QLatin1Char('k')) ? amount
                          : !unit.isEmpty() || imperial() ? amount * kKgPerLb : amount;
        const QString weightDay = promptDay(value, logDay());
        request(QByteArrayLiteral("PUT"), apiPath(QStringLiteral("/weights/") + weightDay),
                QJsonObject{{QStringLiteral("weightKg"), kg}},
                [this, kg, weightDay](bool ok, const QJsonObject &, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("The weigh-in was not saved: %1").arg(error), true);
            m_afterLoadStatus = QStringLiteral("Saved %1 for %2.").arg(
                weightText(kg), weightDay == QDate::currentDate().toString(Qt::ISODate) ? QStringLiteral("today")
                                                                                        : dayLabel(weightDay));
            load();
        });
        return true;
    }
    const QJsonArray local = promptMatches(value);
    if (!local.isEmpty()) {
        addEntries(local, meal, QStringLiteral("quick"), day, loggedMessage(local, meal, day));
        return true;
    }
    setStatus(QStringLiteral("Asking the model about the meal…"));
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/analyze")),
            QJsonObject{{QStringLiteral("description"), value.left(1000)}}, logEstimate(QStringLiteral("text")));
    return true;
}

void CaloriesFeature::renderQuick()
{
    m_frequentFoods = frequentFoods();
    const QString filter = foodKey(m_quickFilter->text());
    const QString selectedList = m_quickTree->currentItem() ? m_quickTree->currentItem()->data(0, kQuickListRole).toString() : QString();
    const QString selectedName = m_quickTree->currentItem() ? foodKey(m_quickTree->currentItem()->text(0)) : QString();
    m_quickTree->clear();
    QTreeWidgetItem *reselect = nullptr;
    const auto section = [&](const QString &title, const QString &list, const QJsonArray &foods, const QString &empty) {
        auto *heading = new QTreeWidgetItem(m_quickTree, {title.toUpper()});
        heading->setFlags(Qt::ItemIsEnabled);
        QFont font = heading->font(0);
        font.setBold(true);
        heading->setFont(0, font);
        int shown = 0;
        for (int index = 0; index < foods.size(); ++index) {
            const QJsonObject food = foods.at(index).toObject();
            if (!filter.isEmpty() && !foodKey(text(food, "name")).contains(filter) &&
                !foodKey(text(food, "brand")).contains(filter))
                continue;
            QStringList meta{text(food, "brand"), text(food, "portion")};
            if (food.contains(QStringLiteral("times")))
                meta << QStringLiteral("logged %1×").arg(food.value(QStringLiteral("times")).toInt());
            meta.removeAll(QString());
            auto *row = new QTreeWidgetItem(m_quickTree, {text(food, "name"), QStringLiteral("%1 kcal").arg(kcal(number(food, "calories")))});
            row->setData(0, kQuickListRole, list);
            row->setData(0, kQuickIndexRole, index);
            row->setToolTip(0, QStringLiteral("%1 · P %2 g · C %3 g · F %4 g")
                                   .arg(meta.join(QStringLiteral(" · ")), grams(number(food, "proteinG")),
                                        grams(number(food, "carbsG")), grams(number(food, "fatG"))));
            row->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            if (list == selectedList && foodKey(text(food, "name")) == selectedName)
                reselect = row;
            ++shown;
        }
        if (!shown && !empty.isEmpty()) {
            auto *note = new QTreeWidgetItem(m_quickTree, {empty});
            note->setFlags(Qt::ItemIsEnabled);
            note->setForeground(0, palette().brush(QPalette::Disabled, QPalette::Text));
        }
    };
    section(QStringLiteral("My foods"), QStringLiteral("mine"), m_overview.value(QStringLiteral("quickFoods")).toArray(),
            filter.isEmpty() ? QStringLiteral("Nothing saved yet. Upload a receipt or save a food.") : QStringLiteral("No match."));
    if (!m_frequentFoods.isEmpty())
        section(QStringLiteral("You log often"), QStringLiteral("frequent"), m_frequentFoods, QStringLiteral("No match."));
    section(QStringLiteral("Common foods"), QStringLiteral("common"), m_overview.value(QStringLiteral("commonFoods")).toArray(),
            QStringLiteral("No match."));
    if (Theme::isNext()) {
        for (int index = 0; index < m_quickTree->topLevelItemCount(); ++index) {
            QTreeWidgetItem *item = m_quickTree->topLevelItem(index);
            if (item->data(0, kQuickListRole).toString().isEmpty())
                item->setFirstColumnSpanned(true);
        }
    }
    m_quickTree->setCurrentItem(reselect);
    if (!reselect) {
        for (QPushButton *button : {m_quickLogButton, m_quickStarButton, m_quickRemoveButton})
            button->setEnabled(false);
    }
}

QJsonObject CaloriesFeature::quickFood(const QTreeWidgetItem *item) const
{
    if (!item)
        return {};
    const QString list = item->data(0, kQuickListRole).toString();
    const int index = item->data(0, kQuickIndexRole).toInt();
    const QJsonArray foods = list == QLatin1String("mine") ? m_overview.value(QStringLiteral("quickFoods")).toArray()
                             : list == QLatin1String("frequent") ? m_frequentFoods
                             : list == QLatin1String("common") ? m_overview.value(QStringLiteral("commonFoods")).toArray()
                                                               : QJsonArray();
    return index >= 0 && index < foods.size() ? foods.at(index).toObject() : QJsonObject();
}

void CaloriesFeature::quickAdd(QTreeWidgetItem *item)
{
    const QJsonObject food = quickFood(item);
    if (food.isEmpty())
        return;
    const QString meal = m_quickMeal->currentData().toString();
    setQuickStatus(QStringLiteral("Logging %1…").arg(text(food, "name")));
    addEntries(QJsonArray{foodFields(food)}, meal, QStringLiteral("quick"), QString(),
               QStringLiteral("Logged %1 (%2 kcal) to %3.").arg(text(food, "name"), kcal(number(food, "calories")), meal));
}

void CaloriesFeature::saveMyFoods(const QJsonArray &foods, const QString &note)
{
    request(QByteArrayLiteral("PUT"), apiPath(QStringLiteral("/quick-foods")),
            QJsonObject{{QStringLiteral("foods"), foods}},
            [this, note](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setQuickStatus(QStringLiteral("My foods were not saved: %1").arg(error));
        m_overview.insert(QStringLiteral("quickFoods"), payload.value(QStringLiteral("quickFoods")).toArray());
        renderQuick();
        setQuickStatus(note);
    });
}

void CaloriesFeature::starFood(const QJsonObject &food)
{
    if (food.isEmpty())
        return;
    const QJsonArray mine = m_overview.value(QStringLiteral("quickFoods")).toArray();
    for (const QJsonValue &value : mine) {
        if (foodKey(text(value.toObject(), "name")) == foodKey(text(food, "name")))
            return setQuickStatus(QStringLiteral("%1 is already in My foods.").arg(text(food, "name")));
    }
    QJsonArray foods{foodFields(food)};
    for (const QJsonValue &value : mine)
        foods.append(value);
    saveMyFoods(foods, QStringLiteral("Saved %1 to My foods.").arg(text(food, "name")));
}

void CaloriesFeature::removeMyFood(const QJsonObject &food)
{
    if (food.isEmpty())
        return;
    QJsonArray foods;
    for (const QJsonValue &value : m_overview.value(QStringLiteral("quickFoods")).toArray()) {
        if (foodKey(text(value.toObject(), "name")) != foodKey(text(food, "name")))
            foods.append(value);
    }
    saveMyFoods(foods, QStringLiteral("Removed %1 from My foods.").arg(text(food, "name")));
}

void CaloriesFeature::pickReceipt()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Choose a receipt photo"), QString(),
        QStringLiteral("Images (*.jpg *.jpeg *.png *.webp)"));
    if (!path.isEmpty())
        readReceipt(path);
}

void CaloriesFeature::readReceipt(const QString &path)
{
    const QString image = jpegDataUrl(path, 1600);
    if (image.isEmpty())
        return setQuickStatus(QStringLiteral("That file is not an image the desktop can read."));
    setQuickStatus(QStringLiteral("Reading the receipt…"));
    setStatus(QStringLiteral("Reading the receipt…"));
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/receipt")),
            QJsonObject{{QStringLiteral("image"), image}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            const QString message = aiError(payload, error, QStringLiteral("Could not read the receipt"));
            setQuickStatus(message);
            return setStatus(message, true);
        }
        m_overview.insert(QStringLiteral("quickFoods"), payload.value(QStringLiteral("quickFoods")).toArray());
        renderQuick();
        const QStringList added = names(payload.value(QStringLiteral("added")));
        const int found = payload.value(QStringLiteral("found")).toArray().size();
        const QString from = text(payload, "store").isEmpty() ? QString() : QStringLiteral(" from ") + text(payload, "store");
        const QString message = added.isEmpty()
            ? QStringLiteral("Read %1 food%2%3; all were already in My foods.").arg(found).arg(found == 1 ? QString() : QStringLiteral("s"), from)
            : QStringLiteral("Added %1 food%2%3 to My foods: %4.")
                  .arg(added.size()).arg(added.size() == 1 ? QString() : QStringLiteral("s"), from, added.join(QStringLiteral(", ")));
        setQuickStatus(message);
        setStatus(message);
    });
}

void CaloriesFeature::setQuickStatus(const QString &value)
{
    m_quickStatus->setText(value);
}

void CaloriesFeature::renderChallenges()
{
    m_challengesTable->setRowCount(0);
    for (const QJsonValue &value : m_challenges) {
        const QJsonObject item = value.toObject();
        const int row = m_challengesTable->rowCount();
        m_challengesTable->insertRow(row);
        m_challengesTable->setItem(row, 0, new QTableWidgetItem(text(item, "title")));
        m_challengesTable->setItem(row, 1, new QTableWidgetItem(kindLabel(text(item, "kind"))));
        m_challengesTable->setItem(row, 2, new QTableWidgetItem(statusLabel(text(item, "status"))));
        m_challengesTable->setItem(row, 3, new QTableWidgetItem(
            dayLabel(text(item, "startsOn")) + QStringLiteral(" → ") + dayLabel(text(item, "endsOn"))));
        m_challengesTable->setItem(row, 4, new QTableWidgetItem(
            QStringLiteral("%1 in%2").arg(item.value(QStringLiteral("participants")).toInt())
                .arg(item.value(QStringLiteral("invited")).toInt() > 0
                         ? QStringLiteral(" · %1 invited").arg(item.value(QStringLiteral("invited")).toInt()) : QString())));
        m_challengesTable->setItem(row, 5, new QTableWidgetItem(mineLabel(text(item, "myStatus"))));
    }
    m_challengesEmpty->setVisible(m_challenges.isEmpty());
    int invites = 0;
    for (const QJsonValue &value : m_challenges)
        if (text(value.toObject(), "myStatus") == QLatin1String("invited") &&
            text(value.toObject(), "status") != QLatin1String("closed"))
            ++invites;
    m_tabs->setTabText(3, invites ? QStringLiteral("Challenges (%1)").arg(invites) : QStringLiteral("Challenges"));
}

void CaloriesFeature::newChallenge()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New challenge"));
    auto *form = new QFormLayout(&dialog);
    auto *title = lineEdit();
    title->setMaxLength(120);
    auto *kind = new QComboBox;
    for (const char *id : {"weight_loss_percent", "weight_loss_kg", "calorie_streak", "log_streak"})
        kind->addItem(kindLabel(QLatin1String(id)), QLatin1String(id));
    auto *starts = new QDateEdit(m_day->date());
    auto *ends = new QDateEdit(m_day->date().addDays(28));
    for (QDateEdit *edit : {starts, ends}) {
        edit->setCalendarPopup(true);
        edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    }
    auto *description = new QPlainTextEdit;
    description->setFixedHeight(64);
    auto *invitees = lineEdit();
    invitees->setPlaceholderText(QStringLiteral("alice, bob"));
    invitees->setToolTip(QStringLiteral("ForkMesh usernames, separated by commas or spaces"));
    form->addRow(QStringLiteral("Title"), title);
    form->addRow(QStringLiteral("Kind"), kind);
    form->addRow(QStringLiteral("Starts"), starts);
    form->addRow(QStringLiteral("Ends"), ends);
    form->addRow(QStringLiteral("Description"), description);
    form->addRow(QStringLiteral("Invite"), invitees);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || title->text().trimmed().isEmpty())
        return;
    const QJsonArray invite = usernames(invitees->text());
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/challenges")),
            QJsonObject{{QStringLiteral("title"), title->text().trimmed()},
                        {QStringLiteral("kind"), kind->currentData().toString()},
                        {QStringLiteral("startsOn"), starts->date().toString(Qt::ISODate)},
                        {QStringLiteral("endsOn"), ends->date().toString(Qt::ISODate)},
                        {QStringLiteral("description"), description->toPlainText()},
                        {QStringLiteral("invite"), invite}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The challenge was not created: %1").arg(error), true);
        setStatus(QStringLiteral("Challenge created."));
        load();
        reportInvites(payload);
        showBoard(payload.value(QStringLiteral("challenge")).toObject());
    });
}

void CaloriesFeature::openChallenge(const QString &id)
{
    if (id.isEmpty())
        return;
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/challenges/") + id), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The challenge could not be opened: %1").arg(error), true);
        showBoard(payload.value(QStringLiteral("challenge")).toObject());
    });
}

void CaloriesFeature::showBoard(const QJsonObject &challenge)
{
    const QString id = text(challenge, "id");
    if (id.isEmpty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(text(challenge, "title"));
    dialog.resize(560, 420);
    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(mutedLabel(QStringLiteral("%1 · %2 → %3 · started by %4%5")
                                     .arg(kindLabel(text(challenge, "kind")), dayLabel(text(challenge, "startsOn")),
                                          dayLabel(text(challenge, "endsOn")), text(challenge, "createdBy"),
                                          text(challenge, "winner").isEmpty() ? QString()
                                              : QStringLiteral(" · winner: ") + text(challenge, "winner"))));
    if (!text(challenge, "description").isEmpty())
        layout->addWidget(mutedLabel(text(challenge, "description")));
    auto *board = new QTableWidget(0, 3);
    board->setHorizontalHeaderLabels({QStringLiteral("Rank"), QStringLiteral("Member"), QStringLiteral("Progress")});
    board->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    board->setEditTriggers(QAbstractItemView::NoEditTriggers);
    const QString me = text(m_overview, "actor");
    for (const QJsonValue &value : challenge.value(QStringLiteral("leaderboard")).toArray()) {
        const QJsonObject member = value.toObject();
        const int row = board->rowCount();
        board->insertRow(row);
        const int rank = member.value(QStringLiteral("rank")).toInt();
        board->setItem(row, 0, new QTableWidgetItem(rank ? QString::number(rank) : QStringLiteral("–")));
        board->setItem(row, 1, new QTableWidgetItem(text(member, "name") + (text(member, "name") == me ? QStringLiteral(" (you)") : QString())));
        board->setItem(row, 2, new QTableWidgetItem(text(member, "detail")));
    }
    layout->addWidget(board, 1);
    QStringList pending;
    for (const QJsonValue &value : challenge.value(QStringLiteral("pending")).toArray())
        pending << text(value.toObject(), "name") + QStringLiteral(" (") + text(value.toObject(), "status") + QLatin1Char(')');
    if (!pending.isEmpty())
        layout->addWidget(mutedLabel(QStringLiteral("Invited or left: ") + pending.join(QStringLiteral(", "))));
    auto *buttons = new QDialogButtonBox;
    const QString status = text(challenge, "status");
    const QString mine = text(challenge, "myStatus");
    // Only the creator edits, invites to, ends, or deletes a challenge.
    const bool owner = challenge.value(QStringLiteral("isCreator")).toBool();
    QString action;
    if (status != QLatin1String("closed")) {
        if (mine == QLatin1String("invited")) {
            connect(buttons->addButton(QStringLiteral("Accept challenge"), QDialogButtonBox::AcceptRole),
                    &QPushButton::clicked, &dialog, [&] { action = QStringLiteral("accept"); dialog.accept(); });
            connect(buttons->addButton(QStringLiteral("Decline"), QDialogButtonBox::ActionRole),
                    &QPushButton::clicked, &dialog, [&] { action = QStringLiteral("decline"); dialog.accept(); });
        } else if (mine == QLatin1String("accepted") && !challenge.value(QStringLiteral("isCreator")).toBool()) {
            connect(buttons->addButton(QStringLiteral("Leave challenge"), QDialogButtonBox::ActionRole),
                    &QPushButton::clicked, &dialog, [&] { action = QStringLiteral("decline"); dialog.accept(); });
        }
        if (owner) {
            connect(buttons->addButton(QStringLiteral("Invite friends…"), QDialogButtonBox::ActionRole),
                    &QPushButton::clicked, &dialog, [&] { action = QStringLiteral("invite"); dialog.accept(); });
            connect(buttons->addButton(QStringLiteral("End now"), QDialogButtonBox::ActionRole),
                    &QPushButton::clicked, &dialog, [&] { action = QStringLiteral("close"); dialog.accept(); });
        }
    }
    if (owner)
        connect(buttons->addButton(QStringLiteral("Delete"), QDialogButtonBox::DestructiveRole),
                &QPushButton::clicked, &dialog, [&] { action = QStringLiteral("delete"); dialog.accept(); });
    connect(buttons->addButton(QDialogButtonBox::Close), &QPushButton::clicked, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted || action.isEmpty())
        return;
    auto done = [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The challenge action failed: %1").arg(error), true);
        setStatus(QStringLiteral("Challenge updated."));
        load();
    };
    if (action == QLatin1String("accept") || action == QLatin1String("decline")) {
        request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/challenges/") + id + QStringLiteral("/respond")),
                QJsonObject{{QStringLiteral("accept"), action == QLatin1String("accept")}},
                [this](bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok)
                return setStatus(QStringLiteral("The challenge action failed: %1").arg(error), true);
            // Declining or leaving can drop the challenge from view entirely.
            setStatus(payload.value(QStringLiteral("left")).toBool() && !payload.contains(QStringLiteral("challenge"))
                          ? QStringLiteral("You left the challenge.") : QStringLiteral("Challenge updated."));
            load();
        });
    } else if (action == QLatin1String("invite")) {
        inviteFriends(id);
    } else if (action == QLatin1String("close")) {
        if (QMessageBox::question(this, QStringLiteral("End challenge"),
                                  QStringLiteral("End this challenge now? Standings freeze at today's values.")) != QMessageBox::Yes)
            return;
        const QString now = QDate::currentDate().toString(Qt::ISODate);
        const QString today = now < text(challenge, "startsOn") ? text(challenge, "startsOn") : now;
        request(QByteArrayLiteral("PATCH"), apiPath(QStringLiteral("/challenges/") + id),
                QJsonObject{{QStringLiteral("title"), text(challenge, "title")},
                            {QStringLiteral("status"), QStringLiteral("closed")},
                            {QStringLiteral("endsOn"), today}}, done);
    } else if (action == QLatin1String("delete")) {
        if (QMessageBox::question(this, QStringLiteral("Delete challenge"),
                                  QStringLiteral("Delete this challenge for everyone?")) != QMessageBox::Yes)
            return;
        request(QByteArrayLiteral("DELETE"), apiPath(QStringLiteral("/challenges/") + id), {}, done);
    }
}

void CaloriesFeature::inviteFriends(const QString &id)
{
    bool accepted = false;
    const QJsonArray members = usernames(QInputDialog::getText(
        this, QStringLiteral("Invite friends"),
        QStringLiteral("ForkMesh usernames, separated by commas or spaces"),
        QLineEdit::Normal, QString(), &accepted));
    if (!accepted || members.isEmpty())
        return;
    request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/challenges/") + id + QStringLiteral("/invite")),
            QJsonObject{{QStringLiteral("members"), members}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The invitations were not sent: %1").arg(error), true);
        setStatus(QStringLiteral("Challenge updated."));
        load();
        reportInvites(payload);
    });
}

void CaloriesFeature::reportInvites(const QJsonObject &payload)
{
    const QStringList invited = names(payload.value(QStringLiteral("invited")));
    const QStringList skipped = names(payload.value(QStringLiteral("skipped")));
    if (!invited.isEmpty())
        setStatus(QStringLiteral("Invited %1.").arg(invited.join(QStringLiteral(", "))));
    if (skipped.isEmpty())
        return;
    QMessageBox::information(
        this, QStringLiteral("Some friends were not invited"),
        QStringLiteral("These usernames were not invited: %1\n\nCheck the spelling of each ForkMesh "
                       "username, or the challenge may already be full.")
            .arg(skipped.join(QStringLiteral(", "))));
}

bool CaloriesFeature::saveExportBundle(const QJsonObject &payload)
{
    const QString suggested = text(payload, "filename").isEmpty()
        ? QStringLiteral("forkmesh-calories-export.json") : text(payload, "filename");
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save the Calorie Counter export"), suggested,
        QStringLiteral("JSON files (*.json)"));
    if (path.isEmpty())
        return false;
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(payload.value(QStringLiteral("export")).toObject())
                                 .toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        setStatus(QStringLiteral("The export file could not be written."), true);
        return false;
    }
    setStatus(QStringLiteral("Exported to %1.").arg(path));
    return true;
}

void CaloriesFeature::exportMine()
{
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The diary export failed: %1").arg(error), true);
        saveExportBundle(payload);
    });
}

bool CaloriesFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    if (m_checked && !m_installed)
        return false;
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [done = std::move(done)](bool ok, const QJsonObject &payload,
                                     const QString &error) {
        // The diary export has no hasData flag of its own; an empty diary
        // with no weigh-ins or challenges is nothing worth saving.
        QJsonObject result = payload;
        const QJsonObject bundle = payload.value(QStringLiteral("export")).toObject();
        result.insert(QStringLiteral("hasData"),
                      !bundle.value(QStringLiteral("entries")).toArray().isEmpty() ||
                          !bundle.value(QStringLiteral("weights")).toArray().isEmpty() ||
                          !bundle.value(QStringLiteral("challenges")).toArray().isEmpty());
        done(ok, result, error);
    });
    return true;
}

void CaloriesFeature::uninstallPlugin()
{
    if (QMessageBox::warning(
            this, QStringLiteral("Uninstall Calorie Counter"),
            QStringLiteral(
                "Uninstall Calorie Counter from your account?\n\nYour data is saved to a file first, "
                "then your diary, weigh-ins, goals, reminders, USDA key, and the challenges you started "
                "are deleted from the web dashboard, this desktop app, and the mobile app. You can "
                "install it again at any time."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    setStatus(QStringLiteral("Preparing the data download…"));
    // The export is saved before anything is deleted, so your data always
    // survives the uninstall.
    request(QByteArrayLiteral("GET"), apiPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("Uninstall stopped — the data export failed: %1").arg(error), true);
            return;
        }
        const QJsonObject bundle = payload.value(QStringLiteral("export")).toObject();
        const bool hasData = !bundle.value(QStringLiteral("entries")).toArray().isEmpty() ||
                             !bundle.value(QStringLiteral("weights")).toArray().isEmpty() ||
                             !bundle.value(QStringLiteral("challenges")).toArray().isEmpty();
        if (hasData && !saveExportBundle(payload)) {
            setStatus(QStringLiteral("Uninstall cancelled: the data file was not saved."), true);
            return;
        }
        request(QByteArrayLiteral("POST"), apiPath(QStringLiteral("/uninstall")), {},
                [this](bool removed, const QJsonObject &, const QString &removeError) {
            if (!removed) {
                setStatus(QStringLiteral("Calorie Counter was not uninstalled: %1").arg(removeError), true);
                return;
            }
            m_checked = true;
            m_installed = false;
            m_overview = QJsonObject();
            m_entries = QJsonArray();
            m_challenges = QJsonArray();
            applyInstallState();
            setStatus(QStringLiteral("Calorie Counter uninstalled from your account. Install it again at any time."));
        });
    });
}

void CaloriesFeature::syncNextDay()
{
    if (!Theme::isNext() || !m_day)
        return;
    const QDate day = m_day->date();
    const QDate today = QDate::currentDate();
    const QString format = day == today ? QStringLiteral("'Today, 'MMM d")
                           : day.year() == today.year() ? QStringLiteral("ddd, MMM d")
                                                        : QStringLiteral("ddd, MMM d, yyyy");
    if (m_day->displayFormat() != format)
        m_day->setDisplayFormat(format);
    if (m_todayButton)
        m_todayButton->setVisible(day != today);
}

void CaloriesFeature::renderNextDiary()
{
    if (!m_ring)
        return;
    const QJsonObject profile = m_overview.value(QStringLiteral("profile")).toObject();
    const QJsonObject totals = m_overview.value(QStringLiteral("todayTotals")).toObject();
    const int goal = profile.value(QStringLiteral("dailyCalories")).toInt();
    const double calories = number(totals, "calories");
    const QString today = text(m_overview, "today");
    m_ring->setValues(calories, goal);
    m_remainingTitle->setText(goal > 0 && calories > goal ? QStringLiteral("Over goal") : QStringLiteral("Remaining"));
    m_remainingLabel->setText(goal > 0 ? QStringLiteral("%1 kcal").arg(kcal(std::fabs(goal - calories)))
                                       : QStringLiteral("No goal"));
    int todayCount = 0;
    for (const QJsonValue &value : m_entries) {
        if (text(value.toObject(), "day") == today)
            ++todayCount;
    }
    m_entriesTodayLabel->setText(goal <= 0 ? QStringLiteral("Set one on Goals and reminders")
                                 : QStringLiteral("%1 entr%2 today").arg(todayCount)
                                       .arg(todayCount == 1 ? QStringLiteral("y") : QStringLiteral("ies")));
    const QString strong = Theme::hex(Theme::Role::Fg);
    const struct { QLabel *label; const char *key; } macros[] = {
        {m_proteinLabel, "proteinG"}, {m_carbsLabel, "carbsG"}, {m_fatLabel, "fatG"}};
    for (const auto &macro : macros) {
        macro.label->setText(QStringLiteral("<span style=\"color:%1\">%2 g</span> of %3 g")
                                 .arg(strong, grams(number(totals, macro.key)),
                                      grams(number(profile, macro.key))));
    }
    const QJsonObject streaks = m_overview.value(QStringLiteral("streaks")).toObject();
    const int logged = streaks.value(QStringLiteral("logged")).toInt();
    m_streakValue->setText(QStringLiteral("%1 day%2").arg(logged).arg(logged == 1 ? QString() : QStringLiteral("s")));
    m_streakCaption->setText(QStringLiteral("logged in a row · %1 within goal")
                                 .arg(streaks.value(QStringLiteral("onGoal")).toInt()));
    const QJsonObject latest = m_overview.value(QStringLiteral("latestWeight")).toObject();
    const double target = number(profile, "targetWeightKg");
    const double latestKg = number(latest, "weightKg");
    m_weightValue->setText(weightText(latestKg));
    QString weight = latestKg > 0 ? dayLabel(text(latest, "day")) : QStringLiteral("No weigh-in yet");
    if (latestKg > 0 && target > 0)
        weight += QStringLiteral(" · %1 %2").arg(weightText(std::fabs(latestKg - target)),
                                                 latestKg > target ? QStringLiteral("to go") : QStringLiteral("below target"));
    m_weightCaption->setText(weight);

    QHash<QString, double> sums;
    const auto groupKey = [](const QJsonObject &entry) {
        return text(entry, "day") + QLatin1Char('|') + mealLabel(text(entry, "meal"));
    };
    for (const QJsonValue &value : m_entries)
        sums[groupKey(value.toObject())] += number(value.toObject(), "calories");
    QString previous;
    for (int row = 0; row < m_entriesTable->rowCount() && row < m_entries.size(); ++row) {
        const QJsonObject entry = m_entries.at(row).toObject();
        const QString key = groupKey(entry);
        const bool starts = row == 0 || key != previous;
        previous = key;
        const QString day = text(entry, "day");
        const QString heading = !starts ? QString()
                                : day == today ? mealLabel(text(entry, "meal"))
                                               : mealLabel(text(entry, "meal")) + QStringLiteral(" · ") + dayLabel(day);
        const QString sum = starts ? QStringLiteral("%1 kcal").arg(kcal(sums.value(key))) : QString();
        for (int column = 0; column < m_entriesTable->columnCount(); ++column) {
            QTableWidgetItem *item = m_entriesTable->item(row, column);
            if (!item)
                continue;
            item->setData(kGroupHeadingRole, heading);
            item->setData(kGroupKcalRole, sum);
            if (column >= 4 && column <= 6)
                item->setText(item->text() + QStringLiteral(" g"));
        }
        m_entriesTable->setRowHeight(row, starts ? kNextEntryRow + kNextGroupBand : kNextEntryRow);
    }
}

void CaloriesFeature::arrangeNextHeader(QHBoxLayout *heading, QLabel *title, QLabel *lede,
                                        QPushButton *refreshButton)
{
    auto *outer = qobject_cast<QVBoxLayout *>(layout());
    if (!outer || !heading)
        return;
    while (QLayoutItem *item = heading->takeAt(0))
        delete item;
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    heading->setContentsMargins(24, 14, 18, 8);
    heading->setSpacing(4);
    title->setText(QStringLiteral("Calorie counter"));
    title->setProperty("caloriesRole", QStringLiteral("title"));
    lede->setText(QStringLiteral("Your private food diary, goals, reminders and challenges with friends"));
    lede->setProperty("caloriesRole", QStringLiteral("lede"));
    lede->setWordWrap(false);
    lede->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *titleColumn = new QVBoxLayout;
    titleColumn->setContentsMargins(0, 0, 0, 0);
    titleColumn->setSpacing(2);
    titleColumn->addWidget(title);
    titleColumn->addWidget(lede);
    heading->addLayout(titleColumn, 1);
    outer->removeWidget(m_status);
    m_status->setProperty("caloriesRole", QStringLiteral("status"));
    m_status->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_status->setMaximumWidth(360);
    heading->addWidget(m_status, 0, Qt::AlignVCenter);
    heading->addSpacing(8);
    m_exportButton->setProperty("caloriesRole", QStringLiteral("headerAction"));
    m_exportButton->setIcon(caloriesOcticon(QStringLiteral("download"), Theme::color(Theme::Role::FgMuted), 14));
    m_exportButton->setIconSize(QSize(14, 14));
    m_exportButton->setCursor(Qt::PointingHandCursor);
    heading->addWidget(m_exportButton, 0, Qt::AlignVCenter);

    m_menuHolder = new QWidget(this);
    m_menuHolder->setObjectName(QStringLiteral("caloriesMenuHolder"));
    m_menuHolder->hide();
    auto *holderLayout = new QHBoxLayout(m_menuHolder);
    const QList<QPushButton *> menuButtons{refreshButton, m_uninstallButton};
    for (QPushButton *button : menuButtons) {
        const bool shown = !(button->testAttribute(Qt::WA_WState_ExplicitShowHide) && button->isHidden());
        holderLayout->addWidget(button);
        button->setVisible(shown);
    }
    auto *more = new QToolButton;
    more->setObjectName(QStringLiteral("caloriesMoreButton"));
    more->setProperty("caloriesRole", QStringLiteral("icon"));
    more->setToolTip(QStringLiteral("Refresh, or uninstall Calorie Counter"));
    more->setAccessibleName(QStringLiteral("More actions"));
    more->setCursor(Qt::PointingHandCursor);
    more->setPopupMode(QToolButton::InstantPopup);
    more->setIcon(caloriesOcticon(QStringLiteral("kebab-horizontal"), Theme::color(Theme::Role::FgMuted), 16));
    more->setIconSize(QSize(16, 16));
    auto *menu = new QMenu(more);
    menu->setToolTipsVisible(true);
    connect(menu, &QMenu::aboutToShow, this, [menu, menuButtons] {
        menu->clear();
        for (QPushButton *button : menuButtons) {
            if (button->testAttribute(Qt::WA_WState_ExplicitShowHide) && button->isHidden())
                continue;
            QAction *action = menu->addAction(button->text());
            action->setToolTip(button->toolTip());
            action->setEnabled(button->isEnabled());
            QObject::connect(action, &QAction::triggered, button, &QPushButton::click);
        }
    });
    more->setMenu(menu);
    heading->addWidget(more, 0, Qt::AlignVCenter);
}

void CaloriesFeature::applyNextStyle()
{
    if (!Theme::isNext())
        return;
    const QString sheet = Theme::expandTokens(QStringLiteral(
        "QLabel#pageTitle[caloriesRole=\"title\"] { color: @fg; font-size: 15px; font-weight: 600; background: transparent; }"
        "QLabel#mutedLabel[caloriesRole=\"lede\"] { color: @fgMuted; font-size: 12px; background: transparent; }"
        "QLabel#mutedLabel[caloriesRole=\"status\"] { color: @fgSubtle; font-size: 12px; background: transparent; }"
        "QLabel#errorLabel[caloriesRole=\"status\"] { color: @danger; }"
        "QPushButton[caloriesRole=\"headerAction\"] { background: transparent; border: none; border-radius: 6px;"
        " color: @fgMuted; padding: 0px 8px; min-height: 28px; max-height: 28px; font-size: 12px; font-weight: 500; }"
        "QPushButton[caloriesRole=\"headerAction\"]:hover { background-color: @surfaceRaised; color: @fg; }"
        "QPushButton[caloriesRole=\"headerAction\"]:pressed { background-color: @surfaceActive; }"
        "QToolButton[caloriesRole=\"icon\"] { background: transparent; border: none; border-radius: 6px; padding: 0px;"
        " min-width: 28px; max-width: 28px; min-height: 28px; max-height: 28px; }"
        "QToolButton[caloriesRole=\"icon\"]:hover { background-color: @surfaceRaised; }"
        "QToolButton[caloriesRole=\"icon\"]:pressed { background-color: @surfaceActive; }"
        "QToolButton[caloriesRole=\"icon\"]::menu-indicator { image: none; width: 0px; }"
        "QTabWidget#caloriesTabs::pane { border: none; border-top: 1px solid @border; background: transparent; top: -1px; }"
        "QTabWidget#caloriesTabs::tab-bar { left: 24px; }"
        "QTabWidget#caloriesTabs > QTabBar { background: transparent; border: none; }"
        "QTabWidget#caloriesTabs > QTabBar::tab { background: transparent; border: none;"
        " border-bottom: 2px solid transparent; border-radius: 0px; color: @fgMuted; padding: 0px 10px;"
        " margin: 0px 4px 0px 0px; min-height: 34px; font-size: 13px; font-weight: 500; }"
        "QTabWidget#caloriesTabs > QTabBar::tab:hover { color: @fg; }"
        "QTabWidget#caloriesTabs > QTabBar::tab:selected { color: @fg; border-bottom: 2px solid @fg; }"
        "QFrame#caloriesDayStepper { background-color: @surface; border: 1px solid @border; border-radius: 8px; }"
        "QFrame#caloriesDayStepper QToolButton { background: transparent; border: none; border-radius: 6px;"
        " min-width: 28px; max-width: 28px; min-height: 30px; max-height: 30px; padding: 0px; }"
        "QFrame#caloriesDayStepper QToolButton:hover { background-color: @surfaceRaised; }"
        "QDateEdit[caloriesRole=\"day\"] { background: transparent; border: none; color: @fg; font-size: 13px;"
        " font-weight: 500; padding: 0px 2px; min-height: 30px; max-height: 30px; }"
        "QDateEdit[caloriesRole=\"day\"]::drop-down { width: 14px; border: none; background: transparent; }"
        "QPushButton[caloriesRole=\"outline\"] { background-color: @surface; border: 1px solid @border;"
        " border-radius: 8px; color: @fg; padding: 0px 12px; min-height: 32px; max-height: 32px;"
        " font-size: 12px; font-weight: 500; }"
        "QPushButton[caloriesRole=\"outline\"]:hover { background-color: @surfaceRaised; }"
        "QLineEdit#caloriesFoodSearch { background-color: @surface; border: 1px solid @border; border-radius: 8px;"
        " min-height: 30px; max-height: 32px; padding: 0px 8px 0px 2px; font-size: 13px; color: @fg; }"
        "QLineEdit#caloriesFoodSearch:focus { border-color: @accent; }"
        "QFrame#caloriesToolGroup { background-color: @surface; border: 1px solid @border; border-radius: 8px; }"
        "QPushButton[caloriesRole=\"tool\"] { background: transparent; border: none; border-radius: 6px; padding: 0px;"
        " min-width: 28px; max-width: 28px; min-height: 28px; max-height: 28px; }"
        "QPushButton[caloriesRole=\"tool\"]:hover { background-color: @surfaceRaised; }"
        "QPushButton[caloriesRole=\"tool\"]:pressed { background-color: @surfaceActive; }"
        "QPushButton[caloriesRole=\"create\"] { background-color: @fg; color: @bg; border: none; border-radius: 8px;"
        " padding: 0px 12px 0px 10px; min-height: 32px; max-height: 32px; font-size: 13px; font-weight: 500; }"
        "QPushButton[caloriesRole=\"create\"]:hover { background-color: @fgSecondary; }"
        "QPushButton[caloriesRole=\"create\"]:pressed { background-color: @fgMuted; }"
        "QPushButton[caloriesRole=\"small\"] { min-height: 26px; max-height: 28px; padding: 0px 10px;"
        " border-radius: 6px; font-size: 12px; font-weight: 500; }"
        "QFrame[caloriesRole=\"card\"], QWidget[caloriesRole=\"card\"] { background-color: @surface;"
        " border: 1px solid @border; border-radius: 12px; }"
        "QFrame[caloriesRole=\"card\"] QLabel, QWidget[caloriesRole=\"card\"] QLabel { background: transparent; border: none; }"
        "QLabel[caloriesRole=\"value\"] { color: @fg; font-size: 18px; font-weight: 600; }"
        "QLabel[caloriesRole=\"caption\"], QLabel#mutedLabel[caloriesRole=\"caption\"] { color: @fgMuted; font-size: 12px; }"
        "QLabel[caloriesRole=\"faint\"] { color: @fgSubtle; font-size: 12px; }"
        "QLabel[caloriesRole=\"macro\"] { color: @fg; font-size: 13px; }"
        "QLabel#mutedLabel[caloriesRole=\"macroValue\"] { color: @fgMuted; font-size: 13px; }"
        "QLabel#mutedLabel[caloriesRole=\"cardTitle\"] { color: @fg; font-size: 13px; font-weight: 500; }"
        "QProgressBar[caloriesRole=\"macro\"] { background-color: @surfaceActive; border: none; border-radius: 3px;"
        " min-height: 6px; max-height: 6px; }"
        "QProgressBar[caloriesRole=\"macro\"]::chunk { background-color: @accent; border-radius: 3px; }"
        "QTableWidget#caloriesEntriesTable { background-color: @bg; border: 1px solid @border; border-radius: 12px;"
        " outline: none; padding: 0px; }"
        "QTableWidget#caloriesEntriesTable QHeaderView { background: transparent; border: none; }"
        "QTableWidget#caloriesEntriesTable QHeaderView::section { background: transparent; border: none;"
        " border-bottom: 1px solid @border; color: @fgSubtle; font-size: 12px; font-weight: 400;"
        " padding: 0px 12px; }"
        "QTableWidget#caloriesEntriesTable QHeaderView::section:first { padding-left: 16px; }"
        "QSplitter[caloriesRole=\"diary\"]::handle { background-color: @border; }"
        "QWidget#caloriesQuickAdd { background-color: @bg; }"
        "QWidget[caloriesRole=\"quickHead\"] { background: transparent; border: none; border-bottom: 1px solid @border; }"
        "QLabel#sectionTitle[caloriesRole=\"quickTitle\"] { color: @fg; font-size: 13px; font-weight: 500; }"
        "QWidget[caloriesRole=\"quickHead\"] QComboBox, QWidget[caloriesRole=\"quickHead\"] QLineEdit {"
        " min-height: 30px; max-height: 32px; border-radius: 8px; font-size: 13px; }"
        "QTreeWidget#caloriesQuickTree { background: transparent; border: none; outline: none; padding: 6px; }"
        "QTreeWidget#caloriesQuickTree::item, QTreeWidget#caloriesQuickTree::item:hover,"
        " QTreeWidget#caloriesQuickTree::item:selected { background: transparent; border: none; }"
        "QTreeWidget#caloriesQuickTree::branch { background: transparent; border: none; }"
        "QLabel#mutedLabel[caloriesRole=\"quickFooter\"] { color: @fgSubtle; font-size: 12px;"
        " border-top: 1px solid @border; padding: 12px; background: transparent; }"),
        Theme::Design::Next, Theme::activeDark());
    if (styleSheet() != sheet)
        setStyleSheet(sheet);
}

void CaloriesFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setObjectName(error ? QStringLiteral("errorLabel") : QStringLiteral("mutedLabel"));
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

} // namespace forkmesh::calories

namespace {
// The desktop rail uses Octicons; "pie-chart" ships in desktop/resources/icons.qrc.
const forkmesh::market::NativePackageRegistration caloriesRegistration({
    QStringLiteral("builtin:calories"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Calories"),
    QStringLiteral("pie-chart"),
    QStringLiteral("/api/calories"),
    QStringLiteral("forkmesh-calories-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::calories::CaloriesFeature(host, parent);
    },
});
} // namespace
