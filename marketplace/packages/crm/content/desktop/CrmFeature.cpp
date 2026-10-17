#include "CrmFeature.h"
#include "CrmStageStrip.h"
#include "CrmTeamChart.h"
#include "NetworkReplyError.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateEdit>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QAbstractItemView>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStringListModel>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTextDocumentFragment>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QVector>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <memory>
#include <limits>
#include <tuple>
#include <utility>

namespace forkmesh::crm {
namespace {

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().trimmed();
}

// Data rows per request of a split CSV upload; the web page uses the same.
constexpr int kImportPartRows = 100;
// Data rows the import step sends for the worker to guess each column from
// (the worker's CSV_PREVIEW_ROWS).
constexpr int kImportPreviewRows = 20;
// A follower list is read this many accounts a page (Mastodon's maximum), up
// to the most one CSV import takes.
constexpr int kMastodonFollowersPage = 80;
constexpr int kMastodonFollowersMax = 2000;
// Flows past this many are dropped by the worker when settings are saved.
constexpr int kMaxFlows = 12;
// Every list and pipeline is a tab ahead of All activity, Zoho Mail boxes and
// Import CSV list. A pipeline no list is bound to gets a client-side tab under
// this id prefix, and "No pipeline" holds contacts whose flow is none of them.
constexpr char kPipelineTabPrefix[] = "pipeline:";
constexpr char kNoPipelineTabId[] = "no-pipeline";
constexpr char kSheetIdProperty[] = "crmSheetId";
const QStringList kKeptColumns{QStringLiteral("name"), QStringLiteral("email"),
                               QStringLiteral("flow"), QStringLiteral("stage")};
// Contact rows put into the table per event-loop turn.
constexpr int kFillChunkRows = 100;
constexpr int kActivityContactColumn = 2;
constexpr int kActivityIdColumn = 5;
// The ForkMesh browser extension is its own Market package; its dashboard
// page installs and updates it, so this package pins no extension version.
constexpr char kExtensionPage[] = "https://app.forkmesh.com/dashboard/extension";
constexpr char kCrmPage[] = "https://app.forkmesh.com/dashboard/crm";

QString crmPageLink(const QString &param, const QString &id)
{
    return QLatin1String(kCrmPage) + QLatin1Char('?') + param + QLatin1Char('=') +
           QString::fromLatin1(QUrl::toPercentEncoding(id));
}

// A list the worker stores, as opposed to a client-side pipeline tab.
bool storedSheet(const QJsonObject &sheet)
{
    return !sheet.isEmpty() && !sheet.value(QStringLiteral("virtual")).toBool();
}

// The share glyph on a list's tab and in the lists table: three linked nodes.
QIcon shareIcon(const QColor &color)
{
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(14, 14) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF top(10.5, 3.0), middle(3.5, 7.0), bottom(10.5, 11.0);
        painter.setPen(QPen(color, 1.3));
        painter.drawLine(top, middle);
        painter.drawLine(middle, bottom);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (const QPointF &node : {top, middle, bottom})
            painter.drawEllipse(node, 2.2, 2.2);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

// The dot beside a stage cell, in the color its chevron has in the strip.
QIcon stageDotIcon(const QColor &color)
{
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(10, 10) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QRectF(1.0, 1.0, 8.0, 8.0));
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

// The refresh glyph on a Mastodon cell: an open ring with an arrowhead.
QIcon refreshIcon(const QColor &color)
{
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(14, 14) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(2.5, 2.5, 9.0, 9.0), 90 * 16, 280 * 16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(QPolygonF{QPointF(6.0, 0.0), QPointF(10.0, 2.5),
                                      QPointF(6.0, 5.0)});
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

// The kinds of pipeline a flow can be (the worker's FLOW_ICONS), each with the
// name its picker shows and the color its glyph is drawn in.
struct FlowIconKind
{
    const char *id;
    const char *label;
    QRgb color;
};
constexpr FlowIconKind kFlowIconKinds[] = {
    {"pipeline", "Pipeline", 0xff6e7f96},
    {"investors", "Investors", 0xff2da44e},
    {"hiring", "Hiring", 0xff8250df},
    {"partners", "Partnerships", 0xff1b9aaa},
    {"customers", "Customers", 0xff388bfd},
    {"sales", "Sales", 0xffd29922},
    {"support", "Support", 0xffdb6158},
    {"projects", "Projects", 0xff57606a},
    {"events", "Events", 0xffe16f24},
    {"grants", "Grants", 0xffbf8700},
};

const FlowIconKind &flowIconKind(const QString &id)
{
    for (const FlowIconKind &kind : kFlowIconKinds) {
        if (id == QLatin1String(kind.id))
            return kind;
    }
    return kFlowIconKinds[0];
}

// A pipeline's type glyph, drawn on a 16px grid in the kind's color.
QIcon flowTypeIcon(const QString &id)
{
    const FlowIconKind &kind = flowIconKind(id);
    const QString which = QLatin1String(kind.id);
    const QColor color = QColor::fromRgba(kind.color);
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(16, 16) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        if (which == QLatin1String("investors")) {
            // A coin with a dollar sign.
            painter.drawEllipse(QRectF(1.75, 1.75, 12.5, 12.5));
            QFont font = painter.font();
            font.setPixelSize(10);
            font.setWeight(QFont::Bold);
            painter.setFont(font);
            painter.drawText(QRectF(0, 0, 16, 16), Qt::AlignCenter, QStringLiteral("$"));
        } else if (which == QLatin1String("hiring")) {
            // A person: head and shoulders.
            painter.drawEllipse(QRectF(5.25, 1.75, 5.5, 5.5));
            QPainterPath shoulders;
            shoulders.moveTo(2.25, 14.25);
            shoulders.cubicTo(2.25, 8.75, 13.75, 8.75, 13.75, 14.25);
            painter.drawPath(shoulders);
        } else if (which == QLatin1String("partners")) {
            // Two linked rings.
            painter.drawEllipse(QRectF(1.5, 4.0, 8.0, 8.0));
            painter.drawEllipse(QRectF(6.5, 4.0, 8.0, 8.0));
        } else if (which == QLatin1String("customers")) {
            // A star.
            QPolygonF star;
            for (int point = 0; point < 10; ++point) {
                const qreal radius = point % 2 ? 2.9 : 6.5;
                const qreal angle = qDegreesToRadians(-90.0 + point * 36.0);
                star << QPointF(8.0 + radius * std::cos(angle),
                                8.5 + radius * std::sin(angle));
            }
            painter.drawPolygon(star);
        } else if (which == QLatin1String("sales")) {
            // Rising bars.
            painter.setPen(Qt::NoPen);
            painter.setBrush(color);
            painter.drawRoundedRect(QRectF(2.0, 9.0, 3.0, 5.0), 0.8, 0.8);
            painter.drawRoundedRect(QRectF(6.5, 6.0, 3.0, 8.0), 0.8, 0.8);
            painter.drawRoundedRect(QRectF(11.0, 2.5, 3.0, 11.5), 0.8, 0.8);
        } else if (which == QLatin1String("support")) {
            // A speech bubble.
            QPainterPath bubble;
            bubble.addRoundedRect(QRectF(1.75, 2.25, 12.5, 8.75), 2.5, 2.5);
            painter.drawPath(bubble);
            painter.drawPolyline(QPolygonF{QPointF(4.5, 11.0), QPointF(4.0, 14.0),
                                           QPointF(8.0, 11.0)});
        } else if (which == QLatin1String("projects")) {
            // A ticked box.
            painter.drawRoundedRect(QRectF(1.75, 1.75, 12.5, 12.5), 2.5, 2.5);
            painter.drawPolyline(QPolygonF{QPointF(4.75, 8.25), QPointF(7.0, 10.5),
                                           QPointF(11.25, 5.5)});
        } else if (which == QLatin1String("grants")) {
            // A rosette: a seal with two ribbons.
            painter.drawEllipse(QRectF(3.75, 1.5, 8.5, 8.5));
            painter.drawPolyline(QPolygonF{QPointF(5.5, 9.25), QPointF(4.75, 14.5),
                                           QPointF(8.0, 12.75), QPointF(11.25, 14.5),
                                           QPointF(10.5, 9.25)});
        } else if (which == QLatin1String("events")) {
            // A calendar page.
            painter.drawRoundedRect(QRectF(1.75, 3.0, 12.5, 11.25), 2.0, 2.0);
            painter.drawLine(QPointF(1.75, 6.75), QPointF(14.25, 6.75));
            painter.drawLine(QPointF(5.0, 1.5), QPointF(5.0, 4.0));
            painter.drawLine(QPointF(11.0, 1.5), QPointF(11.0, 4.0));
        } else {
            // A funnel: contacts narrowing through the stages.
            painter.drawPolygon(QPolygonF{QPointF(1.75, 2.25), QPointF(14.25, 2.25),
                                          QPointF(9.5, 8.5), QPointF(9.5, 13.75),
                                          QPointF(6.5, 12.25), QPointF(6.5, 8.5)});
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

// A plain list's tab: three ruled lines.
QIcon plainListIcon(const QColor &color)
{
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        QPixmap pixmap(QSize(16, 16) * scale);
        pixmap.setDevicePixelRatio(scale);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
        for (const qreal y : {4.0, 8.0, 12.0}) {
            painter.drawPoint(QPointF(2.5, y));
            painter.drawLine(QPointF(5.5, y), QPointF(13.5, y));
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

// The worker's _slug for a stage: lowercase words joined by dashes.
QString stageSlug(const QString &label)
{
    QString slug = label.toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    slug.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    return slug.left(40);
}

// The contact picture beside a name, in logical pixels.
constexpr int kAvatarSize = 24;
// Picture reads in flight at once, across every source.
constexpr int kAvatarInFlight = 4;
// Set on a cell that carries a contact picture, so the delegate gives its
// decoration the picture's size instead of the style's small icon size.
constexpr int kAvatarRole = Qt::UserRole + 3;
// Set on a Mastodon cell that carries a refresh button at its right edge, so
// the delegate elides the text short of the button.
constexpr int kRowButtonRole = Qt::UserRole + 4;
// Room the row refresh button takes at the right of its cell.
constexpr int kRowButtonWidth = 28;

// The contact picture on the detail card, in logical pixels: the rows read
// their pictures at their own size, and a card reads one when it opens.
constexpr int kDetailPhotoSize = 96;
constexpr int kPeopleAvatarSize = 56;
constexpr int kListAvatarSize = 22;
constexpr int kListAvatars = 6;
constexpr int kAccessAvatarSize = 18;
constexpr int kAccessAvatars = 3;
constexpr int kAvatarKeep = kPeopleAvatarSize * 2;

QIcon octicon(const QString &name, const QColor &color, int side = 20)
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

class ListStageBar final : public QWidget
{
public:
    struct Segment
    {
        QColor color;
        int count = 0;
    };

    explicit ListStageBar(const QList<Segment> &segments, QWidget *parent = nullptr)
        : QWidget(parent), m_segments(segments)
    {
        setFixedHeight(6);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath track;
        track.addRoundedRect(bounds, 3, 3);
        painter.setClipPath(track);
        QColor empty = palette().color(QPalette::Mid);
        empty.setAlpha(70);
        painter.fillRect(bounds, empty);
        int total = 0;
        for (const Segment &segment : std::as_const(m_segments))
            total += segment.count;
        if (total <= 0)
            return;
        qreal left = bounds.left();
        for (const Segment &segment : std::as_const(m_segments)) {
            if (segment.count <= 0)
                continue;
            const qreal width = bounds.width() * segment.count / total;
            painter.fillRect(QRectF(left, bounds.top(), width, bounds.height()), segment.color);
            left += width;
        }
    }

private:
    QList<Segment> m_segments;
};

class DetailReads final : public QObject
{
public:
    using QObject::QObject;
    QStringList sources;
    QStringList photoSources;
    QHash<QString, QUrl> pictures;
    QSet<QString> asked;
    bool shown = false;
    QLabel *photo = nullptr;
    QList<QLabel *> posts;
    std::function<void()> applyPhoto;
    std::function<void(bool)> readAccounts;
};
// Posts, or public events, read from one connected account for a card.
constexpr int kAccountUpdates = 5;

// A contact picture: the image cropped to a circle, `side` logical pixels
// wide, for a screen of `scale` device pixels per logical pixel.
QPixmap circlePixmap(const QImage &image, int side, qreal scale)
{
    const int pixels = qRound(side * scale);
    const QImage scaled = image.scaled(pixels, pixels, Qt::KeepAspectRatioByExpanding,
                                       Qt::SmoothTransformation);
    QPixmap pixmap(pixels, pixels);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QPainterPath circle;
    circle.addEllipse(QRectF(0, 0, pixels, pixels));
    painter.setClipPath(circle);
    painter.drawImage(QPointF((pixels - scaled.width()) / 2.0,
                              (pixels - scaled.height()) / 2.0), scaled);
    painter.end();
    pixmap.setDevicePixelRatio(scale);
    return pixmap;
}

QIcon avatarIcon(const QImage &image)
{
    QIcon icon;
    for (const qreal scale : {1.0, 2.0})
        icon.addPixmap(circlePixmap(image, kAvatarSize, scale));
    return icon;
}

// The stand-in picture: the contact's initials on the web page's six tones,
// at whatever size the row or the card wants them.
QPixmap initialsPixmap(const QString &initials, int tone, const QColor &textColor,
                       int side, qreal scale)
{
    static const QColor tones[] = {
        QColor(125, 138, 156, 77), QColor(56, 139, 253, 66),
        QColor(45, 164, 138, 71), QColor(210, 153, 34, 77),
        QColor(219, 97, 88, 71), QColor(132, 163, 60, 77),
    };
    QPixmap pixmap(QSize(side, side) * scale);
    pixmap.setDevicePixelRatio(scale);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(tones[qBound(0, tone, 5)]);
    const QRectF bounds(0, 0, side, side);
    painter.drawEllipse(bounds);
    QFont font = painter.font();
    // The row's 24px circle carried 9px initials for a pair and 11px for one.
    font.setPixelSize(qRound(side * (initials.size() > 1 ? 0.375 : 0.458)));
    font.setWeight(QFont::DemiBold);
    painter.setFont(font);
    painter.setPen(textColor);
    painter.drawText(bounds, Qt::AlignCenter, initials);
    painter.end();
    return pixmap;
}

QIcon initialsIcon(const QString &initials, int tone, const QColor &textColor)
{
    QIcon icon;
    for (const qreal scale : {1.0, 2.0})
        icon.addPixmap(initialsPixmap(initials, tone, textColor, kAvatarSize, scale));
    return icon;
}

// The web page's crmInitials: the first letters of the first and last words
// of the name (or the address before its "@"), or a single word's first two.
QString contactInitials(const QJsonObject &contact)
{
    QString source = contact.value(QStringLiteral("name")).toString().trimmed();
    if (source.isEmpty())
        source = contact.value(QStringLiteral("email")).toString().trimmed()
                     .section(QLatin1Char('@'), 0, 0);
    static const QRegularExpression separators(QStringLiteral("[\\s._-]+"));
    const QStringList words = source.split(separators, Qt::SkipEmptyParts);
    if (words.isEmpty())
        return QStringLiteral("?");
    const auto firstLetters = [](const QString &word, int count) {
        QString letters;
        for (qsizetype at = 0; at < word.size() && count > 0; --count) {
            const int length = word.at(at).isHighSurrogate() && at + 1 < word.size() ? 2 : 1;
            letters += word.mid(at, length);
            at += length;
        }
        return letters;
    };
    if (words.size() == 1)
        return firstLetters(words.first(), 2).toUpper();
    return (firstLetters(words.first(), 1) + firstLetters(words.last(), 1)).toUpper();
}

// The web page's crmAvatar tone: a hash of the email (or name), so a person
// keeps the same color on every render and every teammate's screen.
int contactAvatarTone(const QJsonObject &contact)
{
    QString key = contact.value(QStringLiteral("email")).toString();
    if (key.isEmpty())
        key = contact.value(QStringLiteral("name")).toString();
    int hash = 0;
    for (const char32_t point : key.toUcs4())
        hash = int((qint64(hash) * 31 + point) % 9973);
    return hash % 6;
}

// The worker's own default for the tab an import opens (_sheet_name).
QString sheetNameFromFile(const QString &fileName)
{
    QString name = fileName.section(QLatin1Char('/'), -1);
    name.remove(QRegularExpression(QStringLiteral("\\.[A-Za-z0-9]{1,8}$")));
    name.replace(QRegularExpression(QStringLiteral("[_\\-]+")), QStringLiteral(" "));
    name = name.trimmed().left(60);
    return name.isEmpty() ? QStringLiteral("Imported contacts") : name;
}

QString autoEmailTip()
{
    return QStringLiteral("A model writes the email from their record, our past emails and what "
                          "got opened or clicked, then sends it at once from the outreach sender. "
                          "It carries their tracked deck link (investor pipelines) or your referral "
                          "link, and you get a ping when they open it or click.");
}

QString autoEmailError(const QString &code, const QString &fallback)
{
    static const QHash<QString, QString> known{
        {QStringLiteral("outreach_forbidden"),
         QStringLiteral("only owners, admins and the outreach team can send automated emails.")},
        {QStringLiteral("recently_emailed"),
         QStringLiteral("they already got an email from the CRM in the last three days.")},
        {QStringLiteral("contact_closed"), QStringLiteral("they are at an ended stage.")},
        {QStringLiteral("contact_has_no_email"), QStringLiteral("they have no email address.")},
        {QStringLiteral("daily_limit_reached"), QStringLiteral("you reached today's outreach limit.")},
        {QStringLiteral("ai_unavailable"),
         QStringLiteral("this host has no AI model. Pick Claude Code or Codex as the default "
                        "agent in Settings to write it on this computer.")},
        {QStringLiteral("draft_unusable"),
         QStringLiteral("the draft had placeholders or links we could not check. Try again.")},
        {QStringLiteral("rate_limited"), QStringLiteral("the AI model is busy. Try again in a minute.")},
        {QStringLiteral("send_failed"), QStringLiteral("the email provider refused the send.")},
        {QStringLiteral("zoho_send_not_allowed"),
         QStringLiteral("Zoho Mail did not let ForkMesh send from your mailbox. Reconnect Zoho "
                        "Mail in the CRM settings so it may send as well as read.")},
    };
    const QString message = known.value(code);
    if (!message.isEmpty())
        return message;
    return fallback.isEmpty() ? QStringLiteral("no answer came back.") : fallback;
}

// Import failures the worker already closed with a "failed" ping of its own.
bool workerClosedImport(const QJsonObject &payload)
{
    const QString code = payload.value(QStringLiteral("error")).toString();
    return code == QLatin1String("csv_required") ||
           code == QLatin1String("invalid_csv") ||
           code == QLatin1String("csv_header_required") ||
           code == QLatin1String("too_many_csv_rows");
}

QString timestamp(qint64 value)
{
    if (value <= 0)
        return QString::fromUtf8("\xE2\x80\x94");
    return QDateTime::fromMSecsSinceEpoch(value)
        .toLocalTime()
        .toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString text(const QJsonObject &object, const QString &key)
{
    return object.value(key).toString().trimmed();
}

// The built-in contact columns in canonical order: the field name the worker
// stores, the header label, and the width the column opens at. Widths are
// generous enough that handles, stage slugs, and note excerpts read without a
// resize; the table scrolls horizontally rather than squeezing every column
// into the viewport. Custom columns (a CSV header the CRM has no field for, or
// one added with the "+" header) follow these, then the computed columns.
struct ContactColumn
{
    const char *field;
    const char *label;
    int width;
};

const QVector<ContactColumn> &contactColumns()
{
    static const QVector<ContactColumn> columns{
        {"name", "Name", 170}, {"email", "Email", 230},
        {"company", "Company", 180}, {"title", "Title", 170},
        {"phone", "Phone", 140}, {"twitter", "Twitter", 160},
        {"mastodon", "Mastodon", 200}, {"bluesky", "Bluesky", 190},
        {"linkedin", "LinkedIn", 210}, {"github", "GitHub", 150},
        {"website", "Website", 200}, {"location", "Location", 160},
        {"flow", "Flow", 140}, {"stage", "Stage", 180},
        {"value", "Deal value", 130}, {"nextStep", "Next step", 260},
        {"owner", "Owner", 140}, {"source", "Source", 120},
        {"tags", "Tags", 170}, {"notes", "Notes", 320},
    };
    return columns;
}

constexpr char kMastodonLatestColumnId[] = "mastodon-latest";
constexpr char kLastContactedColumnId[] = "last-contacted";
constexpr char kLinkedEmailsColumnId[] = "linked-emails";
constexpr char kLinkedTootsColumnId[] = "linked-toots";
// The Last contacted cell's stamp, which its date editor reads and writes.
constexpr int kContactedStampRole = Qt::UserRole + 2;
constexpr char kActionsColumnId[] = "actions";
constexpr char kAddColumnId[] = "add-column";
constexpr char kCustomColumnPrefix[] = "field:";
constexpr char kDefaultTableView[] = "default";
constexpr char kAllTableView[] = "all";
const QStringList kDefaultViewFields{
    QStringLiteral("name"), QStringLiteral("email"), QStringLiteral("mastodon"),
    QStringLiteral("bluesky"), QStringLiteral("twitter"), QStringLiteral("linkedin"),
    QStringLiteral("github"), QStringLiteral("lastContactedAt")};
constexpr int kMastodonInFlight = 4;
constexpr int kMastodonPerHost = 2;
constexpr qint64 kMastodonBackoffMs = 5 * 60 * 1000;
constexpr int kReachMastodonReads = 40;
constexpr int kNextUpcoming = 5;
constexpr int kNextRenderDelayMs = 400;
constexpr qint64 kMastodonFreshMs = 15 * 60 * 1000;
constexpr qint64 kDayMs = 24LL * 60 * 60 * 1000;
constexpr int kNextReplyDays = 60;
constexpr int kNextFollowUpDays = 14;
constexpr int kNextPhotoSize = 64;
const QSet<QString> kWonStages{
    QStringLiteral("won"), QStringLiteral("closed-won"), QStringLiteral("customer"),
    QStringLiteral("renewal"), QStringLiteral("committed"), QStringLiteral("signed"),
    QStringLiteral("paid"), QStringLiteral("active"), QStringLiteral("awarded")};
const QSet<QString> kEndedStages{
    QStringLiteral("passed"), QStringLiteral("rejected"), QStringLiteral("closed"),
    QStringLiteral("inactive"), QStringLiteral("hired"), QStringLiteral("lost"),
    QStringLiteral("closed-lost"), QStringLiteral("churned"), QStringLiteral("declined"),
    QStringLiteral("unsubscribed"), QStringLiteral("do-not-contact")};
const QSet<QString> kUnsentStatuses{
    QStringLiteral("draft"), QStringLiteral("sending"), QStringLiteral("failed"),
    QStringLiteral("reject")};
constexpr char kAiChatGptUrl[] = "https://chatgpt.com/";
constexpr char kAiClaudeUrl[] = "https://claude.ai/new";
constexpr int kAiUrlLimit = 14000;
constexpr int kAiMessageLimit = 6;
constexpr int kAiMessageChars = 900;
constexpr int kAiNotesChars = 2500;
constexpr int kAiFieldLimit = 40;
constexpr int kAiBriefLimit = 8000;
const QStringList kForkMeshBrief{
    QStringLiteral("ForkMesh (https://forkmesh.com) lets developers collaborate on, mirror and preserve source-code repositories across independent hosts, without relying on any single hosting provider. It is open source."),
    QStringLiteral("- Git stays the source of truth. Repository bytes live on developer-operated hosts and mirrors; clones are served from a live, signed, integrity-checked mirror, so a repository stays reachable when its source host goes offline."),
    QStringLiteral("- Collaboration travels with the code: issues, discussions, pull requests, reviews, projects and releases are signed records stored in the repository, not only in a hosted database."),
    QStringLiteral("- Stock Git works with no account: git clone https://forkmesh.com/<owner>/<repo>. ForkMesh's own source is https://forkmesh.com/forkmesh/forkmesh."),
    QStringLiteral("- The desktop app (Linux, macOS, Windows) is a full node: it hosts and mirrors repositories and runs Codex or Claude Code agents in isolated worktrees whose work becomes pull requests. iPhone and Android apps are access clients."),
    QStringLiteral("- The protocol, self-hosting, public read and clone, and portable Git data are free; only bounded hosted services are paid."),
    QStringLiteral("- The public proof uses ordinary developer actions: collaborate, add a mirror, take the source host offline, and clone again."),
    QStringLiteral("Links:"),
    QStringLiteral("- Website: https://forkmesh.com"),
    QStringLiteral("- Web app: https://app.forkmesh.com"),
    QStringLiteral("- 3D ForkMesh World: https://world.forkmesh.com"),
    QStringLiteral("- Docs: https://forkmesh.com/docs/"),
    QStringLiteral("- Blog: https://forkmesh.com/blog"),
    QStringLiteral("- Changelog: https://forkmesh.com/changelog"),
    QStringLiteral("- Press kit: https://forkmesh.com/press"),
    QStringLiteral("- Status: https://forkmesh.com/status"),
    QStringLiteral("- Install the desktop node on Linux or macOS: curl -fsSL https://forkmesh.com/install.sh | bash"),
    QStringLiteral("Recent updates: read the blog and the changelog above for what shipped lately."),
};

QString aiMessageText(const QString &raw)
{
    QStringList lines;
    static const QRegularExpression wrote(QStringLiteral("^On .{4,200}wrote:$"));
    static const QRegularExpression original(
        QStringLiteral("^-{2,}\\s*Original Message\\s*-{2,}$"),
        QRegularExpression::CaseInsensitiveOption);
    QString normalized = raw;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    for (const QString &line : normalized.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (wrote.match(trimmed).hasMatch() || original.match(trimmed).hasMatch())
            break;
        if (!line.trimmed().startsWith(QLatin1Char('>')))
            lines << line;
    }
    QString flat = lines.join(QLatin1Char('\n'));
    flat.replace(QRegularExpression(QStringLiteral("\n{3,}")), QStringLiteral("\n\n"));
    flat = flat.trimmed();
    return flat.size() > kAiMessageChars
        ? flat.left(kAiMessageChars - 1) + QString::fromUtf8("\xE2\x80\xA6") : flat;
}

QString aiProviderLabel(const QJsonObject &email)
{
    const QString provider = text(email, "provider");
    if (provider == QLatin1String("gmail"))
        return QStringLiteral("Gmail");
    if (provider == QLatin1String("zoho"))
        return QStringLiteral("Zoho Mail");
    if (provider == QLatin1String("mastodon"))
        return QStringLiteral("Mastodon");
    return text(email, "transport") == QLatin1String("mailtrap") ? QStringLiteral("Mailtrap")
                                                                 : QStringLiteral("Email");
}

QString aiAngle(const QString &group, const QString &flow)
{
    const QString name = group.isEmpty() ? flow : group;
    if (name.isEmpty())
        return {};
    const QString audience = (group + QLatin1Char(' ') + flow).toLower();
    const auto mentions = [&audience](std::initializer_list<const char *> words) {
        for (const char *word : words) {
            if (audience.contains(QLatin1String(word)))
                return true;
        }
        return false;
    };
    if (mentions({"grant"}))
        return QStringLiteral("They are on our \"%1\" list as a grant funder, so write to their "
                              "published priorities and evaluation criteria: problem, approach, "
                              "impact, team, budget and milestones. Never invent traction, "
                              "numbers, partners or names.").arg(name);
    if (mentions({"journalist", "press", "media", "reporter", "writer", "editor", "podcast",
                  "newsletter", "blog"}))
        return QStringLiteral("They are on our \"%1\" list as a journalist, so pitch a story: why "
                              "it matters now and what is new, and offer an interview or a demo. "
                              "No sales talk.").arg(name);
    if (mentions({"investor", "angel", "vc", "fund", "capital"}))
        return QStringLiteral("They are on our \"%1\" list as an investor, so lead with the "
                              "opportunity and ask for a short call; never invent traction, "
                              "numbers or names.").arg(name);
    if (mentions({"partner", "integration", "alliance"}))
        return QStringLiteral("They are on our \"%1\" list as a potential partner, so show how we "
                              "fit what they build and suggest exploring working together.")
            .arg(name);
    if (mentions({"customer", "lead", "user", "developer", "maintainer", "community",
                  "open source"}))
        return QStringLiteral("They are on our \"%1\" list as a potential user, so give one "
                              "concrete way we help their projects and invite them to try it.")
            .arg(name);
    return QStringLiteral("They are on our \"%1\" list, so pitch it the way that suits that group.")
        .arg(name);
}

LeadMarks leadMarks(const QJsonObject &contact)
{
    LeadMarks marks;
    marks.value = qMax<qint64>(0, qint64(contact.value(QStringLiteral("value")).toDouble()));
    const QJsonObject fields = contact.value(QStringLiteral("fields")).toObject();
    for (auto entry = fields.constBegin(); entry != fields.constEnd(); ++entry) {
        const QString key = entry.key().toLower();
        const QJsonValue raw = entry.value();
        bool ok = raw.isDouble();
        const double number = ok ? raw.toDouble() : raw.toString().trimmed().toDouble(&ok);
        if (!ok || !std::isfinite(number) || number <= 0)
            continue;
        if ((key == QLatin1String("rank") || key.endsWith(QLatin1String("_rank"))) &&
            (marks.rank <= 0 || number < marks.rank)) {
            marks.rank = number;
            marks.rankField = entry.key();
        }
        if ((key == QLatin1String("score") || key.endsWith(QLatin1String("_score"))) &&
            number > marks.score) {
            marks.score = number;
            marks.scoreField = entry.key();
        }
    }
    return marks;
}

// Custom column keys follow the worker's CSV header rule: lowercase, digits and
// underscores, so a column typed here lands on the same key a CSV header would.
QString customColumnKey(const QString &label)
{
    QString key = label.trimmed().toLower();
    key.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
                QStringLiteral("_"));
    while (key.startsWith(QLatin1Char('_')))
        key.remove(0, 1);
    while (key.endsWith(QLatin1Char('_')))
        key.chop(1);
    return key.left(64);
}

QString flowSlug(const QString &label)
{
    QString base = label.toLower();
    base.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    base.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (!base.contains(QRegularExpression(QStringLiteral("^[a-z]"))))
        base = (QStringLiteral("pipeline-") + base)
                   .remove(QRegularExpression(QStringLiteral("-+$")));
    return base.left(34).remove(QRegularExpression(QStringLiteral("-+$")));
}

QString customColumnLabel(const QString &key)
{
    QString label = key;
    label.replace(QLatin1Char('_'), QLatin1Char(' '));
    if (!label.isEmpty())
        label[0] = label.at(0).toUpper();
    return label;
}

QString plainExcerpt(const QString &html, int limit)
{
    const QString plain =
        QTextDocumentFragment::fromHtml(html).toPlainText().simplified();
    if (plain.size() <= limit)
        return plain;
    return plain.left(limit - 1) + QString::fromUtf8("\xE2\x80\xA6");
}

// The web page's crmProfileUrl: the https address a handle names, whether it
// was typed as "@name", "name@instance", a bare domain, or a profile link.
// Anything else, http included, gets no link at all.
QUrl profileUrl(const QString &field, const QString &raw)
{
    const QString value = raw.trimmed();
    if (value.isEmpty())
        return {};
    const QString handle = QString::fromUtf8(QUrl::toPercentEncoding(
        value.startsWith(QLatin1Char('@')) ? value.mid(1) : value));
    QString address;
    if (value.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        address = value;
    else if (value.startsWith(QLatin1String("http:"), Qt::CaseInsensitive))
        address.clear();
    else if (field == QLatin1String("twitter"))
        address = QStringLiteral("https://x.com/") + handle;
    else if (field == QLatin1String("mastodon"))
        address = CrmFeature::mastodonProfileUrl(value).toString();
    else if (field == QLatin1String("bluesky"))
        address = QStringLiteral("https://bsky.app/profile/") + handle;
    else if (field == QLatin1String("github"))
        address = QStringLiteral("https://github.com/") + handle;
    else if (field == QLatin1String("linkedin"))
        address = value.contains(QLatin1String("linkedin.com/"), Qt::CaseInsensitive)
            ? QStringLiteral("https://") + value
            : QStringLiteral("https://www.linkedin.com/in/") + handle;
    else if (field == QLatin1String("website"))
        address = QStringLiteral("https://") + value;
    const QUrl url(address);
    return url.isValid() && url.scheme() == QLatin1String("https") ? url : QUrl();
}

// The service a picture source names, as the card labels it.
QString accountServiceLabel(const QString &source)
{
    const QString kind = source.section(QLatin1Char(':'), 0, 0);
    if (kind == QLatin1String("mastodon"))
        return QStringLiteral("Mastodon");
    if (kind == QLatin1String("bluesky"))
        return QStringLiteral("Bluesky");
    return QStringLiteral("GitHub");
}

QUrl gravatarPicture(const QString &email, int size)
{
    const QByteArray hash = QCryptographicHash::hash(email.trimmed().toLower().toUtf8(),
                                                     QCryptographicHash::Sha256).toHex();
    return QUrl(QStringLiteral("https://gravatar.com/avatar/%1?s=%2&d=404")
                    .arg(QString::fromLatin1(hash)).arg(size));
}

// The picture a source names without being asked: GitHub serves one by
// username. Mastodon and Bluesky name theirs in the profile the card reads.
QUrl accountKnownPicture(const QString &source)
{
    const QString kind = source.section(QLatin1Char(':'), 0, 0);
    if (kind == QLatin1String("gravatar"))
        return gravatarPicture(source.section(QLatin1Char(':'), 1), kDetailPhotoSize * 2);
    if (kind != QLatin1String("github"))
        return {};
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(QStringLiteral("avatars.githubusercontent.com"));
    url.setPath(QLatin1Char('/') + source.section(QLatin1Char(':'), 1));
    url.setQuery(QStringLiteral("s=%1").arg(kDetailPhotoSize * 2));
    return url;
}

// One public GitHub event as a line: what the account did, and where.
QString githubEventLine(const QJsonObject &event)
{
    const QJsonObject payload = event.value(QStringLiteral("payload")).toObject();
    const QString type = text(event, "type");
    const QJsonObject pull = payload.value(QStringLiteral("pull_request")).toObject();
    const QJsonObject issue = payload.value(QStringLiteral("issue")).toObject();
    const QJsonArray commits = payload.value(QStringLiteral("commits")).toArray();
    const QString action = text(payload, "action").isEmpty()
        ? QStringLiteral("updated") : text(payload, "action");
    const auto number = [](const QJsonObject &object) {
        const qint64 value = qint64(object.value(QStringLiteral("number")).toDouble());
        return value > 0 ? QStringLiteral(" #%1").arg(value) : QString();
    };
    const QString reference = text(payload, "ref");
    const QString referenceKind = text(payload, "ref_type").isEmpty()
        ? QStringLiteral("a ref") : text(payload, "ref_type");
    QString what;
    if (type == QLatin1String("PushEvent")) {
        const qint64 pushed = qMax<qint64>(
            1, qMax<qint64>(qint64(payload.value(QStringLiteral("size")).toDouble()),
                            commits.size()));
        what = QStringLiteral("pushed %1 commit%2").arg(pushed)
                   .arg(pushed == 1 ? QString() : QStringLiteral("s"));
    } else if (type == QLatin1String("PullRequestEvent")) {
        what = action + QStringLiteral(" pull request") +
               (number(payload).isEmpty() ? number(pull) : number(payload));
    } else if (type == QLatin1String("PullRequestReviewEvent")) {
        what = QStringLiteral("reviewed pull request") + number(pull);
    } else if (type == QLatin1String("PullRequestReviewCommentEvent")) {
        what = QStringLiteral("commented on pull request") + number(pull);
    } else if (type == QLatin1String("IssuesEvent")) {
        what = action + QStringLiteral(" issue") + number(issue);
    } else if (type == QLatin1String("IssueCommentEvent")) {
        what = QStringLiteral("commented on issue") + number(issue);
    } else if (type == QLatin1String("WatchEvent")) {
        what = QStringLiteral("starred");
    } else if (type == QLatin1String("ForkEvent")) {
        what = QStringLiteral("forked");
    } else if (type == QLatin1String("CreateEvent")) {
        what = QStringLiteral("created %1%2").arg(referenceKind,
                   reference.isEmpty() ? QString() : QLatin1Char(' ') + reference);
    } else if (type == QLatin1String("DeleteEvent")) {
        what = QStringLiteral("deleted %1%2").arg(referenceKind,
                   reference.isEmpty() ? QString() : QLatin1Char(' ') + reference);
    } else if (type == QLatin1String("ReleaseEvent")) {
        const QString tag = text(payload.value(QStringLiteral("release")).toObject(),
                                 "tag_name");
        what = QStringLiteral("released ") + (tag.isEmpty() ? QStringLiteral("a version") : tag);
    } else if (type == QLatin1String("PublicEvent")) {
        what = QStringLiteral("made public");
    } else if (type == QLatin1String("GollumEvent")) {
        what = QStringLiteral("edited the wiki");
    } else {
        QString name = type;
        name.remove(QRegularExpression(QStringLiteral("Event$")));
        what = name.isEmpty() ? QStringLiteral("acted") : name.toLower();
    }
    const QString repository = text(event.value(QStringLiteral("repo")).toObject(), "name");
    QString title = text(pull, "title");
    if (title.isEmpty())
        title = text(issue, "title");
    if (title.isEmpty() && !commits.isEmpty())
        title = text(commits.first().toObject(), "message");
    if (title.isEmpty())
        title = text(payload.value(QStringLiteral("comment")).toObject(), "body");
    // "starred acme/widget", but "pushed 2 commits in acme/widget".
    const bool direct = what == QLatin1String("starred") || what == QLatin1String("forked");
    const QString line = repository.isEmpty()
        ? what
        : QStringLiteral("%1 %2%3").arg(what, direct ? QString() : QStringLiteral("in "),
                                        repository);
    return title.isEmpty()
        ? line : QStringLiteral("%1: %2").arg(line, plainExcerpt(title.toHtmlEscaped(), 120));
}

QNetworkRequest jsonRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    request.setTransferTimeout(15000);
    return request;
}

QNetworkRequest instanceRequest(const QUrl &url)
{
    QNetworkRequest request = jsonRequest(url);
    forkmesh::markExpectedTransportFailure(request);
    return request;
}

// The records the worker last sent, and the contact pictures last read, kept
// in the app's cache folder so the page draws them the moment it opens and a
// re-read only asks whether anything changed. Only this OS user can read the
// files; no setting or key is ever written, and an organization's copy goes
// when the plugin is uninstalled or the worker says it is gone.
constexpr int kCacheVersion = 1;
// A saved picture is shown without asking its service again for this long.
constexpr qint64 kAvatarCacheMs = 7LL * 24 * 60 * 60 * 1000;
constexpr qint64 kForkMeshAvatarsMs = 15LL * 60 * 1000;

QString cacheDirectory(const QString &sub = QString())
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
                   QStringLiteral("/crm");
    if (!sub.isEmpty())
        path += QLatin1Char('/') + sub;
    return path;
}

QString cacheKey(const QString &value)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256).toHex().left(32));
}

QString organizationsCachePath()
{
    return cacheDirectory() + QStringLiteral("/organizations.json");
}

QString summaryCachePath(const QString &actor, const QString &organization)
{
    return cacheDirectory(QStringLiteral("summaries")) + QLatin1Char('/') +
           cacheKey(actor + QLatin1Char('\n') + organization) + QStringLiteral(".json");
}

QString avatarCachePath(const QString &source)
{
    return cacheDirectory(QStringLiteral("avatars")) + QLatin1Char('/') +
           cacheKey(source + QStringLiteral("@%1").arg(kAvatarKeep)) + QStringLiteral(".png");
}

QJsonObject readCacheFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject record = QJsonDocument::fromJson(file.readAll()).object();
    return record.value(QStringLiteral("version")).toInt() == kCacheVersion ? record
                                                                             : QJsonObject();
}

// Opens `file` for writing with owner-only permissions, making its folder
// (owner-only too) first.
bool openCacheFile(QSaveFile &file)
{
    const QString folder = QFileInfo(file.fileName()).absolutePath();
    const QFileDevice::Permissions owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    if (!QDir().mkpath(folder))
        return false;
    QFile::setPermissions(folder, owner | QFileDevice::ExeOwner);
    QFile::setPermissions(cacheDirectory(), owner | QFileDevice::ExeOwner);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(owner);
    return true;
}

bool writeCacheFile(const QString &path, QJsonObject record)
{
    record.insert(QStringLiteral("version"), kCacheVersion);
    QSaveFile file(path);
    if (!openCacheFile(file))
        return false;
    file.write(QJsonDocument(record).toJson(QJsonDocument::Compact));
    return file.commit();
}

QLineEdit *lineEdit(const QString &value = QString())
{
    auto *edit = new QLineEdit(value);
    edit->setClearButtonEnabled(true);
    return edit;
}

QString mastodonErrorText(const QString &code, const QString &fallback)
{
    if (code == QLatin1String("invalid_toot_url"))
        return QStringLiteral("That is not a link to a toot (https://instance/@name/id)");
    if (code == QLatin1String("toot_contact_not_found"))
        return QStringLiteral("the toot mentions no contact in the CRM");
    if (code == QLatin1String("contact_not_found"))
        return QStringLiteral("the contact no longer exists");
    if (code == QLatin1String("mastodon_not_connected"))
        return QStringLiteral("connect the Mastodon account in CRM settings first");
    if (code == QLatin1String("mastodon_token_rejected"))
        return QStringLiteral("Mastodon rejected the access token; save a new one with "
                              "the read:statuses scope");
    if (code == QLatin1String("mastodon_unavailable"))
        return QStringLiteral("Mastodon could not be reached; try again later");
    if (code == QLatin1String("mastodon_account_not_found"))
        return QStringLiteral("Mastodon does not know that account");
    if (code == QLatin1String("invalid_mastodon_account"))
        return QStringLiteral("use an account on mastodon.social, mastodon.online, "
                              "fosstodon.org, hachyderm.io, mas.to or infosec.exchange");
    return fallback.isEmpty() ? code : fallback;
}

QString mastodonStateLine(const QJsonObject &mastodon)
{
    const QString account = text(mastodon, "account");
    if (!mastodon.value(QStringLiteral("connected")).toBool())
        return account.isEmpty()
            ? QStringLiteral("Not connected.")
            : QStringLiteral("@%1 has no access token saved.").arg(account);
    const qint64 checked = qint64(mastodon.value(QStringLiteral("lastSyncAt")).toDouble());
    QString line = QStringLiteral("Connected as @%1 · last checked %2")
                       .arg(account, checked > 0 ? timestamp(checked)
                                                 : QStringLiteral("never"));
    const QString error = text(mastodon, "lastError");
    if (!error.isEmpty())
        line += QStringLiteral(" · last error: ") + mastodonErrorText(error, error);
    return line;
}

// The option a searchable combo's text names: the one it spells out, else the
// only option containing it; -1 when it names none or several.
int searchableMatch(const QComboBox *combo, const QString &typed)
{
    const QString needle = typed.trimmed();
    if (needle.isEmpty())
        return -1;
    const int exact = combo->findText(needle, Qt::MatchFixedString);
    if (exact >= 0)
        return exact;
    int found = -1;
    for (int index = 0; index < combo->count(); ++index) {
        if (!combo->itemText(index).contains(needle, Qt::CaseInsensitive))
            continue;
        if (found >= 0)
            return -1;
        found = index;
    }
    return found;
}

// A choice from a fixed set (a pipeline, a stage) picked by typing any part of
// it: the completer lists every option containing the text, and text that
// names no option falls back to the option already chosen, so nothing outside
// the set is ever saved.
void makeSearchable(QComboBox *combo)
{
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    if (QCompleter *completer = combo->completer()) {
        completer->setCompletionMode(QCompleter::PopupCompletion);
        completer->setFilterMode(Qt::MatchContains);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
    }
    QLineEdit *edit = combo->lineEdit();
    edit->setPlaceholderText(QStringLiteral("Type to search"));
    QObject::connect(edit, &QLineEdit::editingFinished, combo, [combo] {
        const int match = searchableMatch(combo, combo->currentText());
        if (match >= 0 && match != combo->currentIndex())
            combo->setCurrentIndex(match);
        combo->setEditText(combo->itemText(combo->currentIndex()));
    });
}

constexpr char kChoiceEditorProperty[] = "crmChoiceEditor";
constexpr char kDateEditorProperty[] = "crmDateEditor";
constexpr char kDateNowProperty[] = "crmDateNow";
constexpr char kDateTouchedProperty[] = "crmDateTouched";
constexpr char kDateClosedProperty[] = "crmDateClosed";

// The contacts table's editor: a searchable combo over a cell with a fixed set
// of values (Flow, Stage), a date picker with a Now button on Last contacted,
// a plain line edit everywhere else. `choices` fills
// in a cell's options as (label, stored value) and says whether it has any.
class ContactChoiceDelegate final : public QStyledItemDelegate
{
public:
    using Choices =
        std::function<bool(const QModelIndex &, QList<QPair<QString, QString>> *)>;

    ContactChoiceDelegate(Choices choices, QObject *parent)
        : QStyledItemDelegate(parent), m_choices(std::move(choices))
    {
    }

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override
    {
        if (index.data(kContactedStampRole).isValid())
            return createDateEditor(parent);
        QList<QPair<QString, QString>> choices;
        if (!m_choices(index, &choices))
            return QStyledItemDelegate::createEditor(parent, option, index);
        auto *combo = new QComboBox(parent);
        combo->setProperty(kChoiceEditorProperty, true);
        for (const auto &choice : std::as_const(choices))
            combo->addItem(choice.first, choice.second);
        makeSearchable(combo);
        // Picking an option, from the list or the search results, saves it
        // right away instead of waiting for the cell to lose focus.
        auto *self = const_cast<ContactChoiceDelegate *>(this);
        connect(combo, &QComboBox::activated, self, [self, combo] {
            emit self->commitData(combo);
            emit self->closeEditor(combo, QAbstractItemDelegate::SubmitModelCache);
        });
        return combo;
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        if (editor->property(kDateEditorProperty).toBool()) {
            const qint64 stamp = index.data(kContactedStampRole).toLongLong();
            if (auto *date = editor->findChild<QDateEdit *>())
                date->setDate(stamp > 0
                    ? QDateTime::fromMSecsSinceEpoch(stamp).toLocalTime().date()
                    : QDate::currentDate());
            editor->setProperty(kDateTouchedProperty, false);
            return;
        }
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo || !combo->property(kChoiceEditorProperty).toBool())
            return QStyledItemDelegate::setEditorData(editor, index);
        const QString value = index.data(Qt::EditRole).toString();
        const int at = combo->findData(value);
        combo->setCurrentIndex(at);
        if (at < 0)
            combo->setEditText(value);
        // Every option shows as the editor opens; typing narrows the list.
        QTimer::singleShot(0, combo, [combo] {
            combo->lineEdit()->selectAll();
            if (QCompleter *completer = combo->completer()) {
                completer->setCompletionPrefix(QString());
                completer->complete();
            }
        });
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        if (editor->property(kDateEditorProperty).toBool()) {
            // Now is this minute; a picked day is noon on it, as the browser
            // extension stores one. An editor left untouched saves nothing.
            const auto *date = editor->findChild<QDateEdit *>();
            if (editor->property(kDateNowProperty).toBool())
                model->setData(index, QDateTime::currentMSecsSinceEpoch(), kContactedStampRole);
            else if (date && editor->property(kDateTouchedProperty).toBool())
                model->setData(index, QDateTime(date->date(), QTime(12, 0)).toMSecsSinceEpoch(),
                               kContactedStampRole);
            return;
        }
        auto *combo = qobject_cast<QComboBox *>(editor);
        if (!combo || !combo->property(kChoiceEditorProperty).toBool())
            return QStyledItemDelegate::setModelData(editor, model, index);
        int at = combo->currentIndex();
        if (at < 0 || combo->itemText(at) != combo->currentText())
            at = searchableMatch(combo, combo->currentText());
        if (at >= 0)
            model->setData(index, combo->itemData(at), Qt::EditRole);
    }

protected:
    void initStyleOption(QStyleOptionViewItem *option,
                         const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (index.data(kAvatarRole).toBool())
            option->decorationSize = QSize(kAvatarSize, kAvatarSize);
        if (index.data(kRowButtonRole).toBool())
            option->text = option->fontMetrics.elidedText(
                option->text, Qt::ElideRight,
                qMax(0, option->rect.width() - kRowButtonWidth - 8));
    }

public:
    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override
    {
        QStyledItemDelegate::updateEditorGeometry(editor, option, index);
        // The picker and its Now button need more room than a narrow column.
        if (editor->property(kDateEditorProperty).toBool())
            editor->resize(qMax(editor->width(), editor->sizeHint().width()), editor->height());
    }

private:
    // Last contacted: a day from the calendar, or Now for a contact made just
    // now. The Now button takes no focus, so clicking it does not count as
    // clicking away from the picker.
    QWidget *createDateEditor(QWidget *parent) const
    {
        auto *editor = new QWidget(parent);
        editor->setProperty(kDateEditorProperty, true);
        editor->setAutoFillBackground(true);
        auto *layout = new QHBoxLayout(editor);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(2);
        auto *date = new QDateEdit(editor);
        date->setCalendarPopup(true);
        date->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        auto *now = new QToolButton(editor);
        now->setText(QStringLiteral("Now"));
        now->setToolTip(QStringLiteral("Record now as the last time you were in touch"));
        now->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(date, 1);
        layout->addWidget(now);
        editor->setFocusProxy(date);
        auto *self = const_cast<ContactChoiceDelegate *>(this);
        const QPointer<QWidget> guard(editor);
        const auto finish = [self, guard] {
            if (!guard || guard->property(kDateClosedProperty).toBool())
                return;
            guard->setProperty(kDateClosedProperty, true);
            emit self->commitData(guard);
            emit self->closeEditor(guard, QAbstractItemDelegate::NoHint);
        };
        connect(now, &QToolButton::clicked, self, [guard, finish] {
            if (guard)
                guard->setProperty(kDateNowProperty, true);
            finish();
        });
        connect(date, &QDateEdit::dateChanged, editor,
                [editor] { editor->setProperty(kDateTouchedProperty, true); });
        // Clicking away saves the picked day, as a text cell does; the
        // calendar popup belongs to the picker, so opening it does not.
        connect(date, &QDateEdit::editingFinished, self, [guard, finish] {
            for (QWidget *widget = QApplication::focusWidget(); widget;
                 widget = widget->parentWidget()) {
                if (widget == guard)
                    return;
            }
            finish();
        });
        return editor;
    }

    Choices m_choices;
};

// When a linked email went out (or came in): the send stamp when the provider
// reported one, else when it was recorded.
qint64 emailSentAt(const QJsonObject &email)
{
    const qint64 sent = qint64(email.value(QStringLiteral("sentAt")).toDouble());
    return sent > 0 ? sent : qint64(email.value(QStringLiteral("createdAt")).toDouble());
}

// One linked email as a line: direction, subject, and when it was sent.
QString linkedEmailLine(const QJsonObject &email)
{
    const QString subject = text(email, "subject");
    return QString::fromUtf8("%1 %2 \xC2\xB7 %3")
        .arg(text(email, "direction") == QLatin1String("inbound")
                 ? QString::fromUtf8("\xE2\x86\x99") : QString::fromUtf8("\xE2\x86\x97"),
             subject.isEmpty() ? QStringLiteral("(no subject)") : subject,
             timestamp(emailSentAt(email)));
}

// The worker's membership verbs, applied to the table's own copy of a contact
// so a move shows before the bulk request is answered: `flow`, `stage` and
// `lastContactedAt` replace, `setViews` replaces the lists (and wins over `views`, which adds),
// and `removeViews` drops.
QJsonObject withContactEdit(QJsonObject contact, const QJsonObject &edit)
{
    for (const QString &key : {QStringLiteral("flow"), QStringLiteral("stage"),
                               QStringLiteral("lastContactedAt")}) {
        if (edit.contains(key))
            contact.insert(key, edit.value(key));
    }
    const bool replace = edit.contains(QStringLiteral("setViews"));
    QStringList views;
    if (!replace) {
        for (const QJsonValue &value : contact.value(QStringLiteral("views")).toArray())
            views << value.toString();
    }
    for (const QJsonValue &value :
         edit.value(replace ? QStringLiteral("setViews") : QStringLiteral("views")).toArray()) {
        if (!views.contains(value.toString()))
            views << value.toString();
    }
    for (const QJsonValue &value : edit.value(QStringLiteral("removeViews")).toArray())
        views.removeAll(value.toString());
    contact.insert(QStringLiteral("views"), QJsonArray::fromStringList(views));
    return contact;
}

class ShareNameCompleter : public QCompleter
{
public:
    using QCompleter::QCompleter;

    QStringList splitPath(const QString &path) const override
    {
        return {path.mid(lastSeparator(path) + 1)};
    }

    QString pathFromIndex(const QModelIndex &index) const override
    {
        const auto *edit = qobject_cast<QLineEdit *>(widget());
        const QString text = edit ? edit->text() : QString();
        const qsizetype at = lastSeparator(text);
        const QString name = QCompleter::pathFromIndex(index);
        if (at < 0)
            return name;
        const QString kept = text.left(at + 1);
        return kept.endsWith(QLatin1Char(' ')) ? kept + name
                                               : kept + QLatin1Char(' ') + name;
    }

private:
    static qsizetype lastSeparator(const QString &text)
    {
        static const QRegularExpression separator(QStringLiteral("[\\s,;]"));
        return text.lastIndexOf(separator);
    }
};

} // namespace

CrmFeature::CrmFeature(const forkmesh::market::NativePackageHost &host,
                       QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent),
      m_request(host.request),
      m_searchIndexChanged(host.searchIndexChanged),
      m_navigationBadgeChanged(host.navigationBadgeChanged),
      m_userDirectory(host.userDirectory),
      m_userRecordsAvailable(host.userRecordsAvailable),
      m_userRecords(host.userRecords),
      m_agentName(host.agentName),
      m_askAgent(host.askAgent)
{
    setObjectName(QStringLiteral("crmSection"));
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    heading->setSpacing(2);
    auto *title = new QLabel(QStringLiteral("CRM"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    heading->addSpacing(14);
    const auto toolButton = [this, heading](const QString &icon, const QString &label,
                                            const QString &tip) {
        auto *button = new QToolButton;
        button->setObjectName(QStringLiteral("crmToolButton"));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setAutoRaise(true);
        button->setIconSize(QSize(20, 20));
        button->setText(label);
        button->setToolTip(tip);
        button->setAccessibleName(label);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(QStringLiteral(
            "QToolButton#crmToolButton { font-size: 11px; padding: 3px 6px; }"));
        m_toolIcons.append({button, icon});
        heading->addWidget(button);
        return button;
    };
    m_newButton = toolButton(QStringLiteral("plus"), QStringLiteral("Add contact"),
                             QStringLiteral("Add a contact to the organization"));
    m_importButton = toolButton(QStringLiteral("upload"), QStringLiteral("Import CSV"),
                                QStringLiteral("Import a CSV file as a new list"));
    m_githubButton = toolButton(QStringLiteral("repo"), QStringLiteral("Import GitHub"),
                                QStringLiteral("Clone a public GitHub repository and import "
                                               "its contributors' email addresses as a new list"));
    m_usersButton = toolButton(QStringLiteral("person"), QStringLiteral("Import users"),
                               QString());
    m_exportButton = toolButton(QStringLiteral("download"), QStringLiteral("Export CSV"),
                                QStringLiteral("Export every contact as CSV"));
    m_peopleButton = toolButton(QStringLiteral("people"), QStringLiteral("People"),
                                QStringLiteral("Only the contacts with a picture, as a grid"));
    m_historyButton = toolButton(QStringLiteral("history"), QStringLiteral("History"),
                                 QStringLiteral("Who opened the open list and what they "
                                                "changed in it"));
    m_historyButton->setEnabled(false);
    m_pipelinesButton = toolButton(QStringLiteral("workflow"), QStringLiteral("Pipelines"),
                                   QStringLiteral("Manage pipelines: their stages, types and lists"));
    m_pipelinesButton->setEnabled(false);
    m_zohoSyncButton = toolButton(QStringLiteral("sync"), QStringLiteral("Sync Zoho Mail"),
                                  QString());
    m_zohoGuideButton = toolButton(QStringLiteral("book"), QStringLiteral("Zoho guide"),
                                   QStringLiteral("Step by step: set up the Zoho OAuth client "
                                                  "and connect Zoho Mail"));
    m_noteDraftsButton = toolButton(QStringLiteral("pencil"), QStringLiteral("Drafts"),
                                    QStringLiteral("Drafted messages in contact notes that "
                                                   "have not been sent yet"));
    auto *openZoho = toolButton(QStringLiteral("paper-airplane"), QStringLiteral("Zoho Mail"),
                                QStringLiteral("Open Zoho Mail on the organization's data center"));
    auto *openGmail = toolButton(QStringLiteral("comment"), QStringLiteral("Gmail"),
                                 QStringLiteral("Open Gmail"));
    auto *extensionButton = toolButton(QStringLiteral("package"), QStringLiteral("Extension"),
        QStringLiteral("Open the ForkMesh browser extension's page. The extension links Gmail "
                       "and Zoho Mail messages to contacts and fills Password Vault logins"));
    m_settingsButton = toolButton(QStringLiteral("gear"), QStringLiteral("Settings"),
                                  QStringLiteral("CRM settings"));
    auto *refreshButton = toolButton(QStringLiteral("loop"), QStringLiteral("Refresh"),
                                     QStringLiteral("Read the organization's records again"));
    heading->addStretch();
    m_organization = new QComboBox;
    m_organization->setMinimumWidth(180);
    m_organization->setAccessibleName(QStringLiteral("CRM organization"));
    heading->addWidget(m_organization);
    outer->addLayout(heading);
    tintToolbar();

    m_summary = new QLabel(QStringLiteral("Shared organization relationships"));
    m_summary->setObjectName(QStringLiteral("mutedLabel"));
    outer->addWidget(m_summary);

    m_status = new QLabel(QStringLiteral("Select an organization to load CRM."));
    m_status->setObjectName(QStringLiteral("mutedLabel"));
    m_status->setWordWrap(true);
    outer->addWidget(m_status);

    // A running CSV import's progress, and the rows it skipped and why.
    m_importProgress = new QProgressBar;
    m_importProgress->setObjectName(QStringLiteral("crmImportProgress"));
    m_importProgress->setAccessibleName(QStringLiteral("CSV import progress"));
    m_importProgress->hide();
    outer->addWidget(m_importProgress);
    m_importErrors = new QPlainTextEdit;
    m_importErrors->setObjectName(QStringLiteral("crmImportErrors"));
    m_importErrors->setAccessibleName(QStringLiteral("CSV import errors"));
    m_importErrors->setReadOnly(true);
    m_importErrors->setMaximumHeight(110);
    m_importErrors->hide();
    outer->addWidget(m_importErrors);

    // Shown instead of the tabs while the selected organization has not
    // installed the plugin. Installing is an owner/admin action; uninstalling
    // later always downloads the data first and reverses the plugin's schema
    // once its last organization is gone.
    m_installPanel = new QWidget;
    auto *installLayout = new QVBoxLayout(m_installPanel);
    installLayout->setContentsMargins(8, 36, 8, 8);
    installLayout->setSpacing(12);
    auto *installTitle = new QLabel(
        QStringLiteral("The CRM plugin is not installed for this organization"));
    installTitle->setAlignment(Qt::AlignHCenter);
    installTitle->setWordWrap(true);
    installLayout->addWidget(installTitle);
    m_installNote = new QLabel;
    m_installNote->setObjectName(QStringLiteral("mutedLabel"));
    m_installNote->setAlignment(Qt::AlignHCenter);
    m_installNote->setWordWrap(true);
    installLayout->addWidget(m_installNote);
    m_installButton = new QPushButton(
        QStringLiteral("Install for this organization"));
    m_installButton->setObjectName(QStringLiteral("primaryButton"));
    auto *installButtonRow = new QHBoxLayout;
    installButtonRow->addStretch();
    installButtonRow->addWidget(m_installButton);
    installButtonRow->addStretch();
    installLayout->addLayout(installButtonRow);
    installLayout->addStretch();
    m_installPanel->hide();
    outer->addWidget(m_installPanel, 1);

    auto *tabs = new QTabWidget;
    m_tabs = tabs;
    tabs->setObjectName(QStringLiteral("crmTabs"));
    tabs->setDocumentMode(true);
    tabs->tabBar()->hide();

    // The table and its filters live in one body widget that moves into
    // whichever pipeline or list tab is open. There is no every-contact tab:
    // while no tab holds the body it waits on this page, which never shows.
    auto *contactsPage = new QWidget(this);
    contactsPage->hide();
    m_contactsPage = contactsPage;
    auto *contactsPageLayout = new QVBoxLayout(contactsPage);
    contactsPageLayout->setContentsMargins(0, 0, 0, 0);
    m_contactsBody = new QWidget;
    auto *contactsLayout = new QVBoxLayout(m_contactsBody);
    contactsLayout->setContentsMargins(8, 10, 8, 8);
    auto *filters = new QHBoxLayout;
    // The page has no search box of its own: the window's top search box
    // filters this table while the CRM is on screen (setHostSearchQuery) and
    // lists matching contacts from anywhere else in the app.
    m_filterNote = new QLabel(
        QStringLiteral("Filter with the search box at the top of the window."));
    m_filterNote->setObjectName(QStringLiteral("mutedLabel"));
    m_stage = new QComboBox;
    m_flow = new QComboBox;
    m_flow->addItem(QStringLiteral("All flows"), QString());
    m_stage->addItem(QStringLiteral("All stages"), QString());
    for (QComboBox *filter : {m_flow, m_stage}) {
        makeSearchable(filter);
        filter->setMinimumWidth(160);
    }
    // The lists are the web page's tabs: a CSV import or sheet, or a pipeline
    // showing every contact in its flow. A list keeps its own
    // hidden fields, shared with everyone it reaches. The tabs pick the list
    // now, so this picker only holds which one is open and stays hidden.
    m_sheet = new QComboBox;
    m_sheet->setObjectName(QStringLiteral("crmSheetPicker"));
    m_sheet->setAccessibleName(QStringLiteral("CRM list"));
    m_sheet->setMinimumWidth(200);
    m_sheet->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_sheet->hide();
    filters->addWidget(m_sheet);
    m_tableView = new QComboBox;
    m_tableView->setObjectName(QStringLiteral("crmTableViewPicker"));
    m_tableView->setAccessibleName(QStringLiteral("Table view"));
    m_tableView->setToolTip(QStringLiteral("The columns the table shows"));
    m_tableView->setMinimumWidth(140);
    m_tableView->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_tableViewMenu = new QToolButton;
    m_tableViewMenu->setObjectName(QStringLiteral("crmTableViewMenu"));
    m_tableViewMenu->setText(QString::fromUtf8("\xE2\x80\xA6"));
    m_tableViewMenu->setToolTip(QStringLiteral("New, rename or delete a view"));
    m_tableViewMenu->setAccessibleName(QStringLiteral("View options"));
    m_tableViewSave = new QToolButton;
    m_tableViewSave->setObjectName(QStringLiteral("crmTableViewSave"));
    m_tableViewSave->setText(QStringLiteral("Save view"));
    m_tableViewSave->setToolTip(QStringLiteral("Save these columns as a new view"));
    m_tableViewSave->hide();
    filters->addWidget(m_tableView);
    filters->addWidget(m_tableViewSave);
    filters->addWidget(m_tableViewMenu);
    filters->addWidget(m_filterNote, 1);
    filters->addWidget(m_flow);
    filters->addWidget(m_stage);
    // Stage counts head the tab, New 34 > In progress 12 > ..., and a click on
    // one narrows the table to it through the stage filter.
    m_stageStrip = new CrmStageStrip(m_contactsBody);
    m_stageStrip->hide();
    m_stageStrip->onPicked = [this](const QString &stage) {
        m_stage->setCurrentIndex(qMax(0, m_stage->findData(stage)));
    };
    // Right-clicking the chevrons edits the open pipeline's stages.
    m_stageStrip->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_stageStrip, &QWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
        const QString flowId = text(activeSheet(), "flow");
        if (flowId.isEmpty())
            return;
        QMenu menu(this);
        menu.setToolTipsVisible(true);
        addEditStagesAction(&menu, flowId);
        menu.exec(m_stageStrip->mapToGlobal(position));
    });
    m_nextCard = new QFrame(m_contactsBody);
    m_nextCard->setObjectName(QStringLiteral("crmNextContact"));
    m_nextCard->setAccessibleName(QStringLiteral("Who to contact next: reach out or follow up"));
    m_nextCard->setStyleSheet(QStringLiteral(
        "QFrame#crmReachOutCard, QFrame#crmFollowUpCard "
        "{ border: 1px solid palette(highlight); border-radius: 10px; }"));
    {
        auto *pair = new QHBoxLayout(m_nextCard);
        pair->setContentsMargins(0, 0, 0, 0);
        pair->setSpacing(10);
    }
    m_nextCard->hide();
    contactsLayout->addWidget(m_nextCard);
    contactsLayout->addWidget(m_stageStrip);
    contactsLayout->addLayout(filters);
    m_contactsTable = new QTableWidget(0, 0);
    m_contactsTable->setObjectName(QStringLiteral("crmContactsTable"));
    // Row actions are a compact strip: only geometry is set here, so the
    // buttons keep the theme's colours (and Delete its red outline) in both
    // light and dark.
    m_contactsTable->setStyleSheet(QStringLiteral(
        "QWidget#crmRowActions QPushButton { padding: 0px 10px; border-radius: 6px; }"));
    QHeaderView *contactHeader = m_contactsTable->horizontalHeader();
    // Every column opens wide enough to read and stays draggable; the table
    // scrolls sideways instead of compressing columns into ellipses. A right
    // click on the header hides or restores columns, dragging reorders them,
    // a click sorts, and the trailing "+" adds a column.
    contactHeader->setStretchLastSection(false);
    contactHeader->setSectionResizeMode(QHeaderView::Interactive);
    contactHeader->setMinimumSectionSize(36);
    contactHeader->setSectionsMovable(true);
    contactHeader->setSectionsClickable(true);
    contactHeader->setSortIndicatorShown(true);
    contactHeader->setContextMenuPolicy(Qt::CustomContextMenu);
    m_contactsTable->setSortingEnabled(false);
    m_contactsTable->setTextElideMode(Qt::ElideRight);
    m_contactsTable->setWordWrap(false);
    m_contactsTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    // Rows have to clear the action buttons, otherwise the cell widget is
    // clipped and its labels are sliced in half.
    m_contactsTable->verticalHeader()->setDefaultSectionSize(36);
    m_contactsTable->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    m_contactsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Ctrl+click and Shift+click pick several contacts; a right click moves
    // them into another pipeline or list.
    m_contactsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_contactsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    // A plain single click into a cell opens its editor (itemClicked below);
    // these triggers cover the keyboard and a double click. No SelectedClicked:
    // a Ctrl+click on a selected row would otherwise open an editor on it.
    m_contactsTable->setEditTriggers(QAbstractItemView::DoubleClicked |
                                     QAbstractItemView::EditKeyPressed |
                                     QAbstractItemView::AnyKeyPressed);
    m_contactsTable->setAlternatingRowColors(true);
    // Flow and Stage cells open a searchable list of the organization's
    // pipelines and that pipeline's stages instead of free text.
    m_contactsTable->setItemDelegate(new ContactChoiceDelegate(
        [this](const QModelIndex &index, QList<QPair<QString, QString>> *choices) {
            return contactFieldChoices(index.row(), index.column(), choices);
        },
        m_contactsTable));
    rebuildContactColumns();
    renderTableViews();
    contactsLayout->addWidget(m_contactsTable, 1);
    contactsPageLayout->addWidget(m_contactsBody);

    auto *emailsPage = new QWidget;
    auto *emailsLayout = new QVBoxLayout(emailsPage);
    emailsLayout->setContentsMargins(8, 10, 8, 8);
    m_emailsTable = new QTableWidget(0, 8);
    m_emailsTable->setObjectName(QStringLiteral("crmEmailsTable"));
    m_emailsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Kind"), QStringLiteral("Subject"),
         QStringLiteral("Contact"), QStringLiteral("Status"),
         QStringLiteral("Transport"), QStringLiteral("Email ID"),
         QStringLiteral("Sender"), QStringLiteral("When")});
    m_emailsTable->horizontalHeader()->setStretchLastSection(true);
    m_emailsTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    for (int column : {kActivityContactColumn, kActivityIdColumn})
        m_emailsTable->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    m_emailsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_emailsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_emailsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_emailsTable->setAlternatingRowColors(true);
    emailsLayout->addWidget(m_emailsTable, 1);
    auto *emailActions = new QHBoxLayout;
    auto *compose = new QPushButton(QStringLiteral("Compose one-off"));
    compose->setObjectName(QStringLiteral("primaryButton"));
    auto *link = new QPushButton(QStringLiteral("Link external send"));
    m_providerOpen = new QPushButton(QStringLiteral("Open provider send"));
    emailActions->addWidget(compose);
    emailActions->addWidget(link);
    emailActions->addStretch();
    emailActions->addWidget(m_providerOpen);
    emailsLayout->addLayout(emailActions);
    emailsPage->setProperty("crmPageIcon", QStringLiteral("history"));
    tabs->addTab(emailsPage, QStringLiteral("All activity"));

    auto *teamPage = new QWidget;
    auto *teamLayout = new QVBoxLayout(teamPage);
    teamLayout->setContentsMargins(8, 10, 8, 8);
    auto *teamHead = new QHBoxLayout;
    auto *teamNote = new QLabel(QStringLiteral(
        "Emails logged or sent, founders outreach, and contacts added by each "
        "member, by day in your time zone. Touches are emails plus outreach."));
    teamNote->setObjectName(QStringLiteral("mutedLabel"));
    teamNote->setWordWrap(true);
    teamHead->addWidget(teamNote, 1);
    m_teamPeriod = new QComboBox;
    m_teamPeriod->setAccessibleName(QStringLiteral("Period"));
    m_teamPeriod->addItem(QStringLiteral("Last 7 days"), 7);
    m_teamPeriod->addItem(QStringLiteral("Last 30 days"), 30);
    m_teamPeriod->setCurrentIndex(1);
    teamHead->addWidget(m_teamPeriod);
    m_teamMember = new QComboBox;
    m_teamMember->setAccessibleName(QStringLiteral("Member shown in the daily chart"));
    m_teamMember->addItem(QStringLiteral("All members"), QString());
    teamHead->addWidget(m_teamMember);
    teamLayout->addLayout(teamHead);
    auto *teamSplit = new QSplitter(Qt::Horizontal);
    teamSplit->setChildrenCollapsible(false);
    m_teamTable = new QTableWidget(0, 6);
    m_teamTable->setObjectName(QStringLiteral("crmTeamTable"));
    m_teamTable->setAccessibleName(QStringLiteral("Team leaderboard"));
    m_teamTable->setHorizontalHeaderLabels(
        {QStringLiteral("#"), QStringLiteral("Member"), QStringLiteral("Emails"),
         QStringLiteral("Outreach"), QStringLiteral("Contacts"),
         QStringLiteral("Touches")});
    m_teamTable->verticalHeader()->hide();
    m_teamTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_teamTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_teamTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_teamTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_teamTable->setAlternatingRowColors(true);
    teamSplit->addWidget(m_teamTable);
    auto *chartBox = new QWidget;
    auto *chartLayout = new QVBoxLayout(chartBox);
    chartLayout->setContentsMargins(8, 0, 0, 0);
    m_teamChartTitle = new QLabel;
    QFont chartTitleFont = m_teamChartTitle->font();
    chartTitleFont.setWeight(QFont::DemiBold);
    m_teamChartTitle->setFont(chartTitleFont);
    chartLayout->addWidget(m_teamChartTitle);
    m_teamChart = new CrmTeamChart;
    chartLayout->addWidget(m_teamChart, 1);
    teamSplit->addWidget(chartBox);
    teamSplit->setStretchFactor(0, 2);
    teamSplit->setStretchFactor(1, 3);
    teamLayout->addWidget(teamSplit, 1);
    connect(m_teamPeriod, &QComboBox::currentIndexChanged, this, [this] { renderTeam(); });
    connect(m_teamMember, &QComboBox::currentIndexChanged, this, [this] { renderTeam(); });
    connect(m_teamTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        const QTableWidgetItem *item = m_teamTable->item(row, 1);
        if (!item)
            return;
        const QString name = item->data(Qt::UserRole).toString();
        const int index = m_teamMember->findData(
            m_teamMember->currentData().toString() == name ? QString() : name);
        m_teamMember->setCurrentIndex(qMax(0, index));
    });
    teamPage->setProperty("crmPageIcon", QStringLiteral("graph"));
    tabs->addTab(teamPage, QStringLiteral("Team leaderboard"));

    auto *mailPage = new QWidget;
    auto *mailLayout = new QVBoxLayout(mailPage);
    mailLayout->setContentsMargins(8, 10, 8, 8);
    auto *mailNote = new QLabel(QStringLiteral(
        "Zoho messages are kept in shared CRM boxes and grouped by thread or "
        "similar subject. Moving a CRM message does not alter its Zoho folder."));
    mailNote->setObjectName(QStringLiteral("mutedLabel"));
    mailNote->setWordWrap(true);
    mailLayout->addWidget(mailNote);
    m_mailTable = new QTableWidget(0, 7);
    m_mailTable->setObjectName(QStringLiteral("crmMailTable"));
    m_mailTable->setHorizontalHeaderLabels(
        {QStringLiteral("Subject"), QStringLiteral("Sender"),
         QStringLiteral("Box"), QStringLiteral("Conversation"),
         QStringLiteral("Messages"), QStringLiteral("Source folder"),
         QStringLiteral("Received")});
    m_mailTable->horizontalHeader()->setStretchLastSection(true);
    m_mailTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_mailTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_mailTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mailTable->setAlternatingRowColors(true);
    mailLayout->addWidget(m_mailTable, 1);
    mailPage->setProperty("crmPageIcon", QStringLiteral("comment"));
    tabs->addTab(mailPage, QStringLiteral("Zoho Mail boxes"));

    // Every CSV import lands in its own list (a sheet on the web), named after
    // the file; this tab picks the file and shows the lists already imported.
    auto *sheetsPage = new QWidget;
    auto *sheetsLayout = new QVBoxLayout(sheetsPage);
    sheetsLayout->setContentsMargins(8, 10, 8, 8);
    auto *sheetsNote = new QLabel(QStringLiteral(
        "Each CSV you import becomes its own list, opened as a tab under the name "
        "you give it, next to a tab for every pipeline. Picking a file first "
        "shows its columns: choose what each one is imported as, keep it as its "
        "own column, or skip it. A pipeline is a list of every contact in its "
        "flow. Double-click a list to open its tab; right-click a list or its tab "
        "to rename or move it. The share badge on a tab counts the people it "
        "reaches. The order is the organization's, so a move is seen by everyone."));
    sheetsNote->setObjectName(QStringLiteral("mutedLabel"));
    sheetsNote->setWordWrap(true);
    sheetsLayout->addWidget(sheetsNote);
    auto *sheetsActions = new QHBoxLayout;
    auto *chooseCsv = new QPushButton(
        QString::fromUtf8("Choose CSV file\xE2\x80\xA6"));
    chooseCsv->setObjectName(QStringLiteral("primaryButton"));
    m_pipelineButton = new QPushButton(
        QString::fromUtf8("New pipeline\xE2\x80\xA6"));
    m_pipelineButton->setObjectName(QStringLiteral("ghostButton"));
    m_pipelineButton->setEnabled(false);
    m_managePipelinesButton = new QPushButton(
        QString::fromUtf8("Manage pipelines\xE2\x80\xA6"));
    m_managePipelinesButton->setObjectName(QStringLiteral("ghostButton"));
    m_managePipelinesButton->setEnabled(false);
    auto *importFollowers = new QPushButton(
        QString::fromUtf8("Import Mastodon followers\xE2\x80\xA6"));
    importFollowers->setObjectName(QStringLiteral("ghostButton"));
    importFollowers->setToolTip(QStringLiteral(
        "Read an account's public followers from its Mastodon instance; they "
        "open as a new list"));
    sheetsActions->addWidget(chooseCsv);
    sheetsActions->addWidget(importFollowers);
    auto *importContributors = new QPushButton(
        QString::fromUtf8("Import GitHub contributors\xE2\x80\xA6"));
    importContributors->setObjectName(QStringLiteral("ghostButton"));
    importContributors->setToolTip(QStringLiteral(
        "Clone a public GitHub repository and import the email addresses of "
        "its contributors as a new list, leaving out masked no-reply addresses"));
    sheetsActions->addWidget(importContributors);
    m_usersPageButton = new QPushButton(QStringLiteral("Import ForkMesh users"));
    m_usersPageButton->setObjectName(QStringLiteral("ghostButton"));
    sheetsActions->addWidget(m_usersPageButton);
    sheetsActions->addWidget(m_pipelineButton);
    sheetsActions->addWidget(m_managePipelinesButton);
    sheetsActions->addStretch();
    sheetsLayout->addLayout(sheetsActions);
    m_sheetsTable = new QTableWidget(0, 7);
    m_sheetsTable->setObjectName(QStringLiteral("crmSheetsTable"));
    m_sheetsTable->setHorizontalHeaderLabels(
        {QStringLiteral("List"), QStringLiteral("File"),
         QStringLiteral("Contacts"), QStringLiteral("Columns"),
         QStringLiteral("Shared with"), QStringLiteral("Owner"),
         QStringLiteral("Imported")});
    m_sheetsTable->horizontalHeader()->setStretchLastSection(true);
    m_sheetsTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_sheetsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_sheetsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_sheetsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sheetsTable->setAlternatingRowColors(true);
    m_sheetsTable->setContextMenuPolicy(Qt::CustomContextMenu);
    sheetsLayout->addWidget(m_sheetsTable, 1);
    m_sheetsPage = sheetsPage;
    sheetsPage->setProperty("crmPageIcon", QStringLiteral("upload"));
    tabs->addTab(sheetsPage, QStringLiteral("Import CSV list"));

    m_peoplePage = new QWidget;
    auto *peopleLayout = new QVBoxLayout(m_peoplePage);
    peopleLayout->setContentsMargins(8, 10, 8, 8);
    auto *peopleHead = new QHBoxLayout;
    auto *peopleTitle = new QLabel(QStringLiteral("People with pictures"));
    QFont peopleFont = peopleTitle->font();
    peopleFont.setWeight(QFont::DemiBold);
    peopleTitle->setFont(peopleFont);
    peopleHead->addWidget(peopleTitle);
    m_peopleNote = new QLabel;
    m_peopleNote->setObjectName(QStringLiteral("mutedLabel"));
    peopleHead->addWidget(m_peopleNote, 1);
    peopleHead->addWidget(new QLabel(QStringLiteral("Sort by")));
    m_peopleSort = new QComboBox;
    m_peopleSort->setAccessibleName(QStringLiteral("Sort people"));
    m_peopleSort->addItem(QStringLiteral("Most recent"), QStringLiteral("recent"));
    m_peopleSort->addItem(QStringLiteral("Last contacted"), QStringLiteral("contacted"));
    m_peopleSort->addItem(QStringLiteral("Name"), QStringLiteral("name"));
    peopleHead->addWidget(m_peopleSort);
    peopleLayout->addLayout(peopleHead);
    m_peopleGrid = new QListWidget;
    m_peopleGrid->setObjectName(QStringLiteral("crmPeopleGrid"));
    m_peopleGrid->setAccessibleName(QStringLiteral("People with pictures"));
    m_peopleGrid->setViewMode(QListView::IconMode);
    m_peopleGrid->setResizeMode(QListView::Adjust);
    m_peopleGrid->setMovement(QListView::Static);
    m_peopleGrid->setWrapping(true);
    m_peopleGrid->setWordWrap(true);
    m_peopleGrid->setUniformItemSizes(true);
    m_peopleGrid->setTextElideMode(Qt::ElideMiddle);
    m_peopleGrid->setSpacing(6);
    m_peopleGrid->setIconSize(QSize(kPeopleAvatarSize, kPeopleAvatarSize));
    m_peopleGrid->setGridSize(QSize(190, kPeopleAvatarSize + 78));
    m_peopleGrid->setSelectionMode(QAbstractItemView::SingleSelection);
    peopleLayout->addWidget(m_peopleGrid, 1);
    m_peoplePage->setProperty("crmPageIcon", QStringLiteral("people"));
    tabs->addTab(m_peoplePage, QStringLiteral("People with pictures"));

    auto *body = new QSplitter(Qt::Horizontal);
    body->setObjectName(QStringLiteral("crmBody"));
    body->setChildrenCollapsible(false);
    m_body = body;
    auto *side = new QWidget;
    auto *sideLayout = new QVBoxLayout(side);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(4);
    auto *sideHead = new QHBoxLayout;
    sideHead->setContentsMargins(6, 0, 0, 0);
    auto *sideTitle = new QLabel(QStringLiteral("Lists"));
    sideTitle->setFont(peopleFont);
    sideHead->addWidget(sideTitle);
    sideHead->addStretch();
    auto *newListButton = new QToolButton;
    newListButton->setAutoRaise(true);
    newListButton->setCursor(Qt::PointingHandCursor);
    newListButton->setToolTip(QStringLiteral("New list"));
    newListButton->setAccessibleName(QStringLiteral("New list"));
    newListButton->setIconSize(QSize(16, 16));
    m_toolIcons.append({newListButton, QStringLiteral("plus")});
    sideHead->addWidget(newListButton);
    sideLayout->addLayout(sideHead);
    m_sheetList = new QListWidget;
    m_sheetList->setObjectName(QStringLiteral("crmSheetList"));
    m_sheetList->setAccessibleName(QStringLiteral("CRM lists"));
    m_sheetList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_sheetList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_sheetList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sheetList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    sideLayout->addWidget(m_sheetList, 1);
    side->setMinimumWidth(220);
    body->addWidget(side);
    body->addWidget(tabs);
    auto *detail = new QFrame;
    detail->setObjectName(QStringLiteral("crmDetailPanel"));
    detail->setFrameShape(QFrame::StyledPanel);
    detail->setMinimumWidth(340);
    auto *detailLayout = new QVBoxLayout(detail);
    detailLayout->setContentsMargins(10, 8, 10, 10);
    auto *detailHead = new QHBoxLayout;
    auto *detailTitle = new QLabel(QStringLiteral("Contact"));
    detailTitle->setFont(peopleFont);
    detailHead->addWidget(detailTitle);
    detailHead->addStretch();
    auto *detailClose = new QToolButton;
    detailClose->setAutoRaise(true);
    detailClose->setCursor(Qt::PointingHandCursor);
    detailClose->setToolTip(QStringLiteral("Close the contact panel"));
    detailClose->setAccessibleName(QStringLiteral("Close the contact panel"));
    detailClose->setIconSize(QSize(16, 16));
    m_toolIcons.append({detailClose, QStringLiteral("x")});
    detailHead->addWidget(detailClose);
    detailLayout->addLayout(detailHead);
    detail->hide();
    m_detailPanel = detail;
    body->addWidget(detail);
    body->setStretchFactor(0, 0);
    body->setStretchFactor(1, 1);
    body->setStretchFactor(2, 0);
    body->setSizes({250, 900, 400});
    outer->addWidget(body, 1);
    tintToolbar();

    // Mastodon lookups reach arbitrary instances, so they use the host's
    // manager and ask the desktop firewall like every other request.
    m_network = host.network ? host.network : new QNetworkAccessManager(this);
    m_workers = new QThreadPool(this);
    m_workers->setMaxThreadCount(1);

    connect(m_organization, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                if (index >= 0 && !m_loading)
                    loadOrganization();
            });
    connect(refreshButton, &QToolButton::clicked, this,
            [this] { refreshOrganizations(); });
    connect(m_newButton, &QToolButton::clicked, this,
            [this] { editContact(); });
    connect(m_importButton, &QToolButton::clicked, this,
            [this] { importCsv(); });
    connect(chooseCsv, &QPushButton::clicked, this,
            [this] { importCsv(); });
    connect(importFollowers, &QPushButton::clicked, this,
            [this] { importMastodonFollowers(); });
    connect(importContributors, &QPushButton::clicked, this,
            [this] { importGithubContributors(); });
    connect(m_githubButton, &QToolButton::clicked, this,
            [this] { importGithubContributors(); });
    connect(m_usersButton, &QToolButton::clicked, this,
            [this] { importForkMeshUsers(); });
    connect(m_usersPageButton, &QPushButton::clicked, this,
            [this] { importForkMeshUsers(); });
    connect(m_exportButton, &QToolButton::clicked, this,
            [this] { exportCsv(); });
    connect(m_installButton, &QPushButton::clicked, this,
            [this] { installPlugin(); });
    connect(m_settingsButton, &QToolButton::clicked, this,
            [this] { editSettings(); });
    connect(m_zohoSyncButton, &QToolButton::clicked, this, [this] {
        const bool connected = m_settings.value(QStringLiteral("zoho")).toObject()
                                   .value(QStringLiteral("connected")).toBool();
        const bool oneClick = m_settings.value(QStringLiteral("zoho")).toObject()
                                  .value(QStringLiteral("oneClick")).toBool();
        if (connected)
            syncZohoMail();
        else if (m_canManageSettings && oneClick)
            connectZohoMail();
        else if (m_canManageSettings)
            showZohoGuide();
        else
            setStatus(QStringLiteral(
                "Ask an organization owner or admin to connect Zoho Mail."), true);
    });
    connect(m_zohoGuideButton, &QToolButton::clicked, this, [this] { showZohoGuide(); });
    connect(m_noteDraftsButton, &QToolButton::clicked, this, [this] { showNoteDrafts(); });
    connect(m_managePipelinesButton, &QPushButton::clicked, this,
            [this] { managePipelines(); });
    connect(m_pipelinesButton, &QToolButton::clicked, this,
            [this] { managePipelines(); });
    connect(m_pipelineButton, &QPushButton::clicked, this,
            [this] { newPipeline(); });
    connect(m_tableView, &QComboBox::activated, this, [this](int index) {
        selectTableView(m_tableView->itemData(index).toString());
    });
    connect(m_tableViewMenu, &QToolButton::clicked, this, [this] { showTableViewMenu(); });
    connect(m_tableViewSave, &QToolButton::clicked, this, [this] { saveTableViewDraft(); });
    connect(m_sheet, &QComboBox::currentIndexChanged, this, [this] {
        applyColumnVisibility();
        // A pipeline tab is one flow already; its flow filter could only
        // empty the tab.
        m_flow->setVisible(text(activeSheet(), "flow").isEmpty());
        selectFirstStage();
        renderContacts();
    });
    connect(m_sheetsTable, &QTableWidget::customContextMenuRequested, this,
            [this](const QPoint &position) { showSheetMenu(position); });
    connect(m_sheetsTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *item) {
        if (QTableWidgetItem *id = item ? m_sheetsTable->item(item->row(), 0) : nullptr)
            openSheet(id->data(Qt::UserRole).toString());
    });
    connect(openGmail, &QToolButton::clicked, this,
            [this] { openMailbox(true); });
    connect(openZoho, &QToolButton::clicked, this,
            [this] { openMailbox(false); });
    connect(extensionButton, &QToolButton::clicked, this,
            [this] { openBrowserExtension(); });
    connect(m_peopleButton, &QToolButton::clicked, this, [this] { showPeople(); });
    connect(m_historyButton, &QToolButton::clicked, this,
            [this] { showSheetHistory(text(activeSheet(), "id")); });
    connect(m_peopleSort, &QComboBox::currentIndexChanged, this, [this] { renderPeople(); });
    connect(m_peopleGrid, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (item)
            showContactDetail(item->data(Qt::UserRole).toString());
    });
    connect(newListButton, &QToolButton::clicked, this, [this] { newList(); });
    connect(detailClose, &QToolButton::clicked, this, [this] { closeContactDetail(); });
    connect(tabs, &QTabWidget::currentChanged, this, [this] {
        m_stageSheet.clear();
        syncSheetTab();
        if (!sheetTabId(m_tabs->currentIndex()).isEmpty())
            selectFirstStage();
        syncSheetList();
        if (m_tabs->currentWidget() == m_peoplePage)
            renderPeople();
        noteSheetViewed();
    });
    connect(m_sheetList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item) {
        auto *page = item ? qobject_cast<QWidget *>(
                                item->data(Qt::UserRole).value<QObject *>())
                          : nullptr;
        if (page && m_tabs->indexOf(page) >= 0)
            m_tabs->setCurrentWidget(page);
    });
    connect(m_sheetList, &QListWidget::customContextMenuRequested, this,
            [this](const QPoint &position) {
        const QListWidgetItem *item = m_sheetList->itemAt(position);
        const QString id = item ? item->data(Qt::UserRole + 1).toString() : QString();
        if (!id.isEmpty())
            showSheetTabMenu(id, m_sheetList->viewport()->mapToGlobal(position));
    });
    connect(m_sheetList, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem *item) {
        const QString id = item ? item->data(Qt::UserRole + 1).toString() : QString();
        if (!id.isEmpty() && id != QLatin1String(kNoPipelineTabId))
            renameSheet(id);
    });
    connect(m_stage, &QComboBox::currentIndexChanged, this, [this] {
        filterContacts();
        renderStageStrip();
    });
    connect(m_flow, &QComboBox::currentIndexChanged, this, [this] {
        filterContacts();
        renderStageStrip();
    });
    connect(contactHeader, &QHeaderView::customContextMenuRequested, this,
            [this](const QPoint &position) { showColumnMenu(position); });
    connect(contactHeader, &QHeaderView::sectionMoved, this, [this] {
        if (!m_orderingColumns && !m_rendering)
            changeViewFields(shownViewFields());
    });
    // Qt flips the indicator itself on a header click; the table is re-laid
    // out from the contact list in that order. The Actions and "+" headers
    // are not sortable, so the indicator goes back where it was.
    connect(contactHeader, &QHeaderView::sortIndicatorChanged, this,
            [this, contactHeader](int column, Qt::SortOrder order) {
        if (column < 0 || column >= m_columns.size())
            return;
        const ContactColumnSpec &spec = m_columns.at(column);
        if (spec.kind == ContactColumnSpec::Actions ||
            spec.kind == ContactColumnSpec::AddColumn) {
            const QSignalBlocker blocker(contactHeader);
            contactHeader->setSortIndicator(
                columnIndexById(m_sortId),
                m_sortDescending ? Qt::DescendingOrder : Qt::AscendingOrder);
            return;
        }
        m_sortId = spec.id;
        m_sortDescending = order == Qt::DescendingOrder;
        refreshContactSnapshot();
    });
    connect(contactHeader, &QHeaderView::sectionClicked, this,
            [this](int column) {
        if (column >= 0 && column < m_columns.size() &&
            m_columns.at(column).kind == ContactColumnSpec::AddColumn)
            addCustomColumn();
    });
    connect(m_contactsTable, &QTableWidget::itemClicked, this,
            [this](QTableWidgetItem *item) {
        if (!item || m_rendering || item->column() >= m_columns.size())
            return;
        // Ctrl/Shift+click only changes the selection.
        if (QGuiApplication::keyboardModifiers() &
            (Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier))
            return;
        const ContactColumnSpec &spec = m_columns.at(item->column());
        if (const QTableWidgetItem *id = m_contactsTable->item(item->row(), 0))
            showContactDetail(id->data(Qt::UserRole).toString());
        if (spec.kind == ContactColumnSpec::Field &&
            spec.key == QLatin1String("name"))
            return;
        if (spec.kind == ContactColumnSpec::Field &&
            spec.key == QLatin1String("email")) {
            // The address is a link: one click writes to it; the text is
            // edited with a double click or F2 like any other cell.
            if (!item->text().isEmpty())
                openMailCompose(item->text());
            return;
        }
        if (spec.kind == ContactColumnSpec::Field &&
            spec.key == QLatin1String("mastodon")) {
            // A handle is a link to its profile; one that is not stays editable.
            const QUrl url = mastodonProfileUrl(item->text());
            if (url.isValid()) {
                QDesktopServices::openUrl(url);
                return;
            }
        }
        if (spec.kind == ContactColumnSpec::LinkedEmails ||
            spec.kind == ContactColumnSpec::LinkedToots) {
            const QTableWidgetItem *id = m_contactsTable->item(item->row(), 0);
            if (id)
                showLinkedEmails(id->data(Qt::UserRole).toString(),
                                 spec.kind == ContactColumnSpec::LinkedToots,
                                 m_contactsTable->viewport()->mapToGlobal(
                                     m_contactsTable->visualItemRect(item).bottomLeft()));
            return;
        }
        if (item->flags() & Qt::ItemIsEditable)
            m_contactsTable->editItem(item);
    });
    connect(m_contactsTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *item) {
        if (!item || item->column() >= m_columns.size())
            return;
        const ContactColumnSpec &spec = m_columns.at(item->column());
        if (spec.kind == ContactColumnSpec::MastodonLatest) {
            const QUrl url(item->data(Qt::UserRole + 1).toString());
            if (url.isValid() && url.scheme() == QLatin1String("https"))
                QDesktopServices::openUrl(url);
            return;
        }
        if (spec.kind == ContactColumnSpec::LinkedEmails ||
            spec.kind == ContactColumnSpec::LinkedToots)
            return; // the first click already opened the message
        if (item->flags() & Qt::ItemIsEditable) {
            m_contactsTable->editItem(item);
            return;
        }
        editContact(selectedContact());
    });
    connect(m_contactsTable, &QTableWidget::customContextMenuRequested, this,
            [this](const QPoint &position) { showContactMenu(position); });
    connect(m_contactsTable, &QTableWidget::itemChanged, this,
            [this](QTableWidgetItem *item) {
        if (!item || m_rendering || item->column() >= m_columns.size())
            return;
        const ContactColumnSpec &spec = m_columns.at(item->column());
        QTableWidgetItem *id = m_contactsTable->item(item->row(), 0);
        if (!id)
            return;
        if (spec.kind == ContactColumnSpec::LastContacted) {
            // Saved once the date editor has closed: the save redraws the rows.
            const QStringList ids{id->data(Qt::UserRole).toString()};
            const qint64 stamp = item->data(kContactedStampRole).toLongLong();
            QTimer::singleShot(0, this, [this, ids, stamp] { markContacted(ids, stamp); });
            return;
        }
        if (spec.kind != ContactColumnSpec::Field &&
            spec.kind != ContactColumnSpec::CustomField)
            return;
        updateContactField(id->data(Qt::UserRole).toString(), spec, item->text());
    });
    connect(compose, &QPushButton::clicked, this,
            [this] { composeEmail(); });
    connect(link, &QPushButton::clicked, this,
            [this] { linkExternalEmail(); });
    connect(m_emailsTable, &QTableWidget::itemSelectionChanged, this,
            [this] { updateActions(); });
    connect(m_emailsTable, &QTableWidget::itemDoubleClicked, this,
            [this](QTableWidgetItem *) { openProviderSend(); });
    connect(m_emailsTable, &QTableWidget::itemClicked, this,
            [this](QTableWidgetItem *item) {
        if (item && item->column() == kActivityIdColumn &&
            QUrl(text(selectedEmail(), "providerUrl")).scheme() == QLatin1String("https"))
            openProviderSend();
    });
    connect(m_providerOpen, &QPushButton::clicked, this,
            [this] { openProviderSend(); });
    // Nothing loads on a timer and nothing loads at app start: the records
    // arrive when the page is opened (refresh()) or when the Refresh button
    // is pressed, and a teammate's edit shows up on the next one. The
    // window's top search box therefore lists CRM contacts once the page has
    // been opened.
    updateActions();
}

void CrmFeature::request(const QByteArray &method, const QString &path,
                         const QJsonObject &body, ReplyHandler handler)
{
    if (!m_request) {
        handler(false, {}, QStringLiteral("CRM network access is unavailable."));
        return;
    }
    // A write may be applied to the local rows ahead of the worker, so the next
    // re-read must bring the whole summary back even if the write failed.
    if (method != QByteArrayLiteral("GET"))
        m_revision.clear();
    const QPointer<CrmFeature> guard(this);
    m_request(method, path, body,
              [guard, handler = std::move(handler)](
                  bool ok, const QJsonObject &payload,
                  const QString &error) mutable {
        if (guard)
            handler(ok, payload, error);
    });
}

QString CrmFeature::organizationPath(const QString &suffix) const
{
    const QString name = m_organization->currentData().toString();
    return name.isEmpty() ? QStringLiteral("/api/crm")
                          : QStringLiteral("/api/crm/") + name + suffix;
}

QString CrmFeature::pluginPath(const QString &suffix) const
{
    return QStringLiteral("/api/plugins/") +
           m_organization->currentData().toString() +
           QStringLiteral("/crm") + suffix;
}

QJsonObject CrmFeature::organizationRecord() const
{
    const QString name = m_organization->currentData().toString();
    for (const QJsonValue &value : m_organizations) {
        const QJsonObject item = value.toObject();
        if (text(item, "name") == name)
            return item;
    }
    return {};
}

void CrmFeature::applyInstallState()
{
    const QJsonObject record = organizationRecord();
    const QString role = text(record, "role");
    const bool canManage = role == QLatin1String("owner") ||
                           role == QLatin1String("admin");
    m_installed = record.value(QStringLiteral("installed")).toBool();
    const bool haveOrganization = m_organization->count() > 0;
    m_installPanel->setVisible(haveOrganization && !m_installed);
    m_body->setVisible(m_installed);
    for (QToolButton *button :
         {m_importButton, m_githubButton, m_exportButton, m_settingsButton, m_newButton,
          m_zohoGuideButton, m_noteDraftsButton,
          m_historyButton, m_peopleButton, m_pipelinesButton})
        button->setVisible(m_installed);
    m_zohoSyncButton->setVisible(m_installed);
    m_zohoSyncButton->setEnabled(m_installed && canManage);
    updateUsersImportAction();
    m_installButton->setVisible(canManage);
    m_installNote->setText(canManage
        ? QStringLiteral(
              "Installing creates the shared CRM for every member: encrypted "
              "contacts, customizable flows, grouped Zoho Mail boxes, CSV "
              "workflows, and email sends linked to your provider. "
              "Uninstalling later downloads the data first, "
              "then removes every record — and once no organization keeps "
              "the plugin installed, its database tables are dropped too.")
        : QStringLiteral(
              "Ask an organization owner or admin to install the CRM plugin."));
}

void CrmFeature::installPlugin()
{
    if (m_organization->currentData().toString().isEmpty())
        return;
    m_installButton->setEnabled(false);
    setStatus(QStringLiteral("Installing the CRM plugin…"));
    request(QByteArrayLiteral("POST"), pluginPath(QStringLiteral("/install")),
            {}, [this](bool ok, const QJsonObject &, const QString &error) {
        m_installButton->setEnabled(true);
        if (!ok) {
            setStatus(QStringLiteral("The plugin was not installed: %1")
                          .arg(error), true);
            return;
        }
        setStatus(QStringLiteral("CRM installed for this organization."));
        refreshOrganizations();
    });
}

bool CrmFeature::saveExportBundle(const QJsonObject &payload)
{
    const QString suggested = text(payload, "filename").isEmpty()
        ? QStringLiteral("forkmesh-crm-export.json")
        : text(payload, "filename");
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save the CRM data export"), suggested,
        QStringLiteral("JSON files (*.json)"));
    if (path.isEmpty())
        return false;
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(
        payload.value(QStringLiteral("export")).toObject())
        .toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        !file.commit()) {
        setStatus(QStringLiteral("The export file could not be written."), true);
        return false;
    }
    setStatus(QStringLiteral("CRM data exported to %1.").arg(path));
    return true;
}

void CrmFeature::exportAllData()
{
    if (m_organization->currentData().toString().isEmpty())
        return;
    request(QByteArrayLiteral("GET"), pluginPath(QStringLiteral("/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("The data export failed: %1").arg(error),
                      true);
            return;
        }
        saveExportBundle(payload);
    });
}

bool CrmFeature::exportData(forkmesh::market::NativePackageReplyHandler done)
{
    const QString path = pluginPath(QStringLiteral("/export"));
    if (m_organization->currentData().toString().isEmpty() || path.isEmpty())
        return false;
    request(QByteArrayLiteral("GET"), path, {}, std::move(done));
    return true;
}

void CrmFeature::uninstallPlugin()
{
    const QString name = m_organization->currentData().toString();
    if (name.isEmpty())
        return;
    if (QMessageBox::warning(
            this, QStringLiteral("Uninstall the CRM plugin"),
            QStringLiteral(
                "Uninstall the CRM plugin from %1?\n\nYour data is saved to "
                "a file first, then every contact, imported mail message, "
                "email record, and setting "
                "is deleted for the whole organization. Once no organization "
                "keeps the plugin installed, its database tables are removed "
                "as well.").arg(name),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    setStatus(QStringLiteral("Preparing the CRM data download…"));
    // The export is saved before anything is deleted, so the organization's
    // data always survives the uninstall.
    request(QByteArrayLiteral("GET"), pluginPath(QStringLiteral("/export")), {},
            [this, name](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral(
                "Uninstall stopped — the data export failed: %1").arg(error),
                true);
            return;
        }
        if (payload.value(QStringLiteral("hasData")).toBool() &&
            !saveExportBundle(payload)) {
            setStatus(QStringLiteral(
                "Uninstall cancelled: the data file was not saved."), true);
            return;
        }
        request(QByteArrayLiteral("POST"),
                pluginPath(QStringLiteral("/uninstall")), {},
                [this, name](bool removed, const QJsonObject &result,
                       const QString &removeError) {
            if (!removed) {
                setStatus(QStringLiteral(
                    "The plugin was not uninstalled: %1").arg(removeError),
                    true);
                return;
            }
            forgetCachedSummary(name);
            setStatus(result.value(QStringLiteral("schemaDropped")).toBool()
                ? QStringLiteral("CRM uninstalled. No organization uses the "
                                 "plugin anymore, so its tables were removed "
                                 "with it.")
                : QStringLiteral("CRM uninstalled for this organization."));
            refreshOrganizations();
        });
    });
}

void CrmFeature::refresh()
{
    refreshOrganizations();
}

int CrmFeature::navigationBadge() const
{
    return m_uncontacted;
}

void CrmFeature::prepareNavigationBadge()
{
    refresh();
}

void CrmFeature::refreshOrganizations()
{
    if (m_loading)
        return;
    // The first open of the session draws what the last one read before the
    // worker is asked anything; the reads below then only confirm it.
    if (!m_cacheRead) {
        m_cacheRead = true;
        drawCachedOrganizations();
    }
    m_loading = true;
    if (m_shownOrganization.isEmpty())
        setStatus(QStringLiteral("Loading your organizations…"));
    const QString previous = m_organization->currentData().toString();
    request(QByteArrayLiteral("GET"), QStringLiteral("/api/crm"), {},
            [this, previous](bool ok, const QJsonObject &payload,
                             const QString &error) {
        m_loading = false;
        if (!ok) {
            setStatus(QStringLiteral("CRM could not load: %1").arg(error), true);
            return;
        }
        const QString actor = text(payload, "actor");
        if (!m_actor.isEmpty() && actor != m_actor && !m_shownOrganization.isEmpty()) {
            // Records drawn from another account's copy go before this
            // account's are read.
            applyOrganization(QJsonObject());
        }
        m_actor = actor;
        m_organizations = payload.value(QStringLiteral("organizations")).toArray();
        fillOrganizations(previous);
        if (m_organization->count() == 0) {
            applyInstallState();
            setStatus(QStringLiteral(
                "Join or create an organization before using CRM."), true);
            return;
        }
        writeCacheFile(organizationsCachePath(),
                       {{QStringLiteral("actor"), m_actor},
                        {QStringLiteral("organizations"), m_organizations},
                        {QStringLiteral("organization"),
                         m_organization->currentData().toString()}});
        loadOrganization();
    });
}

void CrmFeature::fillOrganizations(const QString &previous)
{
    m_organization->blockSignals(true);
    m_organization->clear();
    for (const QJsonValue &value : std::as_const(m_organizations)) {
        const QJsonObject item = value.toObject();
        const QString name = text(item, "name");
        QString label = name + QStringLiteral(" · ") + text(item, "role");
        if (!item.value(QStringLiteral("installed")).toBool())
            label += QStringLiteral(" · not installed");
        m_organization->addItem(label, name);
    }
    int index = m_organization->findData(previous);
    m_organization->setCurrentIndex(index >= 0 ? index : 0);
    m_organization->blockSignals(false);
}

void CrmFeature::drawCachedOrganizations()
{
    const QJsonObject record = readCacheFile(organizationsCachePath());
    const QString actor = text(record, "actor");
    const QJsonArray organizations = record.value(QStringLiteral("organizations")).toArray();
    if (actor.isEmpty() || organizations.isEmpty())
        return;
    m_actor = actor;
    m_organizations = organizations;
    fillOrganizations(text(record, "organization"));
    applyInstallState();
    if (m_installed)
        drawCachedSummary(m_organization->currentData().toString());
}

bool CrmFeature::drawCachedSummary(const QString &organization)
{
    if (m_actor.isEmpty() || organization.isEmpty())
        return false;
    const QJsonObject record = readCacheFile(summaryCachePath(m_actor, organization));
    const QJsonObject summary = record.value(QStringLiteral("summary")).toObject();
    if (text(record, "actor") != m_actor || text(record, "organization") != organization ||
        text(summary, "revision").isEmpty())
        return false;
    applyOrganization(summary);
    m_shownOrganization = organization;
    setStatus(QStringLiteral("Showing the saved copy · checking for changes…"));
    return true;
}

void CrmFeature::cacheSummary(const QJsonObject &payload)
{
    if (m_actor.isEmpty() || m_shownOrganization.isEmpty() ||
        text(payload, "revision").isEmpty())
        return;
    const QString path = summaryCachePath(m_actor, m_shownOrganization);
    const QJsonObject record{{QStringLiteral("actor"), m_actor},
                             {QStringLiteral("organization"), m_shownOrganization},
                             {QStringLiteral("summary"), payload}};
    // Written on the snapshot thread, after the build it queued behind: a
    // large organization's copy is megabytes of JSON.
    m_workers->start([path, record] { writeCacheFile(path, record); });
}

void CrmFeature::forgetCachedSummary(const QString &organization)
{
    if (m_actor.isEmpty() || organization.isEmpty())
        return;
    const QString path = summaryCachePath(m_actor, organization);
    // Behind any write of the same file still queued on the snapshot thread.
    m_workers->start([path] { QFile::remove(path); });
}

void CrmFeature::loadOrganization()
{
    const QString organization = m_organization->currentData().toString();
    if (m_loading || organization.isEmpty())
        return;
    applyInstallState();
    if (!m_installed) {
        setStatus(QStringLiteral(
            "The CRM plugin is not installed for this organization."));
        return;
    }
    // Another organization picked: its saved copy is drawn while the worker
    // is asked whether it is still current. The revision held is only ever
    // the shown organization's.
    if (organization != m_shownOrganization && !drawCachedSummary(organization))
        m_revision.clear();
    m_loading = true;
    if (m_revision.isEmpty())
        setStatus(QStringLiteral("Loading shared CRM records…"));
    QUrlQuery summaryQuery;
    summaryQuery.addQueryItem(
        QStringLiteral("tz"),
        QString::number(QDateTime::currentDateTime().offsetFromUtc() / 60));
    if (!m_revision.isEmpty())
        summaryQuery.addQueryItem(QStringLiteral("since"), m_revision);
    request(QByteArrayLiteral("GET"),
            organizationPath(QStringLiteral("?") + summaryQuery.toString(QUrl::FullyEncoded)), {},
            [this, organization](bool ok, const QJsonObject &payload,
                                 const QString &error) {
        m_loading = false;
        if (ok) {
            // Every load (the page opening, or Refresh) reads the pictures
            // not already in memory, and tries failed ones again.
            m_avatarsWanted = true;
            m_avatarFailed.clear();
            readForkMeshAvatars();
        }
        if (ok && payload.value(QStringLiteral("unchanged")).toBool()) {
            setStatus(QStringLiteral("Organization CRM loaded · role: %1").arg(m_role));
            queueAvatars();
            syncToots(true);
            return;
        }
        if (!ok) {
            if (text(payload, "error") == QLatin1String("plugin_not_installed")) {
                // Another member uninstalled it; re-read the authoritative
                // install state instead of showing a stale page.
                forgetCachedSummary(organization);
                refreshOrganizations();
                return;
            }
            setStatus(organization == m_shownOrganization && !m_revision.isEmpty()
                          ? QStringLiteral("CRM could not load: %1 · showing the saved copy")
                                .arg(error)
                          : QStringLiteral("CRM could not load: %1").arg(error),
                      true);
            return;
        }
        applyOrganization(payload);
        m_shownOrganization = organization;
        cacheSummary(payload);
        syncToots(true);
    });
}

void CrmFeature::applyOrganization(const QJsonObject &payload)
{
    m_revision = text(payload, "revision");
    if (payload.isEmpty())
        m_shownOrganization.clear();
    m_contacts = payload.value(QStringLiteral("contacts")).toArray();
    // Contacts are what the top search box lists; a query typed before they
    // arrived (the first search warms this page) redraws with them.
    if (m_searchIndexChanged)
        m_searchIndexChanged();
    m_emails = payload.value(QStringLiteral("emails")).toArray();
    m_contactEmails.clear();
    m_contactToots.clear();
    for (const QJsonValue &value : std::as_const(m_emails)) {
        const QJsonObject email = value.toObject();
        if (text(email, "contactId").isEmpty())
            continue;
        // A linked Mastodon conversation has its own column beside the mail.
        (text(email, "provider") == QLatin1String("mastodon") ? m_contactToots : m_contactEmails)
            [text(email, "contactId")].append(email);
    }
    for (auto *linked : {&m_contactEmails, &m_contactToots}) {
        for (QList<QJsonObject> &emails : *linked) {
            std::stable_sort(emails.begin(), emails.end(),
                             [](const QJsonObject &a, const QJsonObject &b) {
                                 return emailSentAt(a) > emailSentAt(b);
                             });
        }
    }
    m_mail = payload.value(QStringLiteral("mail")).toArray();
    m_views = payload.value(QStringLiteral("views")).toArray();
    updateUsersImportAction();
    m_members = payload.value(QStringLiteral("members")).toArray();
    m_activity = payload.value(QStringLiteral("activity")).toObject();
    m_decks = payload.value(QStringLiteral("decks")).toArray();
    m_viewerName = text(payload.value(QStringLiteral("outreach")).toObject(), "name");
    m_settings = payload.value(QStringLiteral("settings")).toObject();
    renderTableViews();
    m_role = text(payload, "role");
    m_canManageSettings =
        payload.value(QStringLiteral("canManageSettings")).toBool();
    m_pipelineButton->setEnabled(m_canManageSettings);
    m_managePipelinesButton->setEnabled(m_canManageSettings);
    m_pipelinesButton->setEnabled(m_canManageSettings);
    m_pipelinesButton->setToolTip(m_canManageSettings
        ? QStringLiteral("Manage pipelines: their stages, types and lists")
        : QStringLiteral("Only organization owners and admins can manage pipelines"));
    m_pipelineButton->setToolTip(m_canManageSettings
        ? QStringLiteral("Create a pipeline with its own stages; it opens as its own list")
        : QStringLiteral("Only organization owners and admins can add pipelines"));
    const QJsonObject summaryObject =
        payload.value(QStringLiteral("summary")).toObject();
    // What the pipelines are worth, so the deal value column totals up instead
    // of being just another field; left off entirely when nobody has filled in
    // a value.
    qint64 pipelineValue = 0;
    for (const QJsonValue &value : std::as_const(m_contacts))
        pipelineValue += qint64(value.toObject().value(QStringLiteral("value")).toDouble());
    m_summary->setText(QStringLiteral(
        "%1 contacts · %2 customers · %3 email sends · %4 imported mail groups%5")
        .arg(summaryObject.value(QStringLiteral("contacts")).toInt())
        .arg(summaryObject.value(QStringLiteral("customers")).toInt())
        .arg(summaryObject.value(QStringLiteral("sent")).toInt())
        .arg(summaryObject.value(QStringLiteral("mailGroups")).toInt())
        .arg(pipelineValue > 0
                 ? QStringLiteral(" · %1 in deal value")
                       .arg(QLocale().toString(pipelineValue))
                 : QString()));
    m_tabs->setTabText(m_tabs->indexOf(m_sheetsPage), m_views.isEmpty()
        ? QStringLiteral("Import CSV list")
        : QStringLiteral("Import CSV list (%1)").arg(m_views.size()));
    const QString previousFlow = m_flow->currentData().toString();
    const QString previousStage = m_stage->currentData().toString();
    m_flow->clear();
    m_flow->addItem(QStringLiteral("All flows"), QString());
    m_stage->clear();
    m_stage->addItem(QStringLiteral("All stages"), QString());
    QStringList stages;
    for (const QJsonValue &value :
         m_settings.value(QStringLiteral("flows")).toArray()) {
        const QJsonObject flow = value.toObject();
        m_flow->addItem(flowTypeIcon(text(flow, "icon")), text(flow, "name"), text(flow, "id"));
        for (const QJsonValue &stage : flow.value(QStringLiteral("stages")).toArray()) {
            const QString name = stage.toString();
            if (!name.isEmpty() && !stages.contains(name))
                stages << name;
        }
    }
    for (const QString &stage : stages)
        m_stage->addItem(stage, stage);
    m_flow->setCurrentIndex(qMax(0, m_flow->findData(previousFlow)));
    m_stage->setCurrentIndex(qMax(0, m_stage->findData(previousStage)));
    updateZohoAction();
    rebuildContactIndex();
    rebuildContactColumns();
    rebuildVirtualSheets();
    renderActivity();
    renderTeam();
    renderMail();
    updateActions();
    // The tabs, their counts and the rows follow once the snapshot is built.
    refreshContactSnapshot();
    setStatus(QStringLiteral("Organization CRM loaded · role: %1").arg(m_role));
}

void CrmFeature::renderTeam()
{
    if (!m_teamTable)
        return;
    struct Ranked
    {
        QString name;
        int emails = 0;
        int outreach = 0;
        int contacts = 0;
        int touches() const { return emails + outreach; }
    };
    const QJsonArray days = m_activity.value(QStringLiteral("days")).toArray();
    const QJsonArray members = m_activity.value(QStringLiteral("members")).toArray();
    const int period = qMax(1, m_teamPeriod->currentData().toInt());
    const int from = qMax(0, int(days.size()) - period);
    const auto sum = [from](const QJsonObject &member, const char *key) {
        int total = 0;
        const QJsonArray values = member.value(QLatin1String(key)).toArray();
        for (int index = from; index < values.size(); ++index)
            total += values.at(index).toInt();
        return total;
    };
    QList<Ranked> ranking;
    for (const QJsonValue &value : members) {
        const QJsonObject member = value.toObject();
        ranking.append({text(member, "name"), sum(member, "emails"),
                        sum(member, "outreach"), sum(member, "contacts")});
    }
    std::stable_sort(ranking.begin(), ranking.end(), [](const Ranked &a, const Ranked &b) {
        if (a.touches() != b.touches())
            return a.touches() > b.touches();
        if (a.contacts != b.contacts)
            return a.contacts > b.contacts;
        return a.name.localeAwareCompare(b.name) < 0;
    });

    QString selected = m_teamMember->currentData().toString();
    if (!std::any_of(ranking.cbegin(), ranking.cend(),
                     [&selected](const Ranked &row) { return row.name == selected; }))
        selected.clear();
    {
        const QSignalBlocker blocker(m_teamMember);
        m_teamMember->clear();
        m_teamMember->addItem(QStringLiteral("All members"), QString());
        for (const Ranked &row : std::as_const(ranking))
            m_teamMember->addItem(row.name, row.name);
        m_teamMember->setCurrentIndex(qMax(0, m_teamMember->findData(selected)));
    }

    const QSignalBlocker tableBlocker(m_teamTable);
    m_teamTable->clearSelection();
    m_teamTable->setRowCount(int(ranking.size()));
    const auto number = [](int value) {
        auto *item = new QTableWidgetItem(QLocale().toString(value));
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return item;
    };
    for (int row = 0; row < ranking.size(); ++row) {
        const Ranked &entry = ranking.at(row);
        m_teamTable->setItem(row, 0, number(row + 1));
        auto *name = new QTableWidgetItem(
            entry.name == m_viewerName && !m_viewerName.isEmpty()
                ? QStringLiteral("%1 (you)").arg(entry.name)
                : entry.name);
        name->setData(Qt::UserRole, entry.name);
        m_teamTable->setItem(row, 1, name);
        m_teamTable->setItem(row, 2, number(entry.emails));
        m_teamTable->setItem(row, 3, number(entry.outreach));
        m_teamTable->setItem(row, 4, number(entry.contacts));
        auto *touches = number(entry.touches());
        QFont bold = touches->font();
        bold.setWeight(QFont::DemiBold);
        touches->setFont(bold);
        m_teamTable->setItem(row, 5, touches);
        if (entry.name == selected)
            m_teamTable->selectRow(row);
    }
    m_teamTable->resizeColumnToContents(0);

    QList<CrmTeamChart::Day> daily;
    for (int index = from; index < days.size(); ++index) {
        CrmTeamChart::Day day;
        day.date = QDate::fromString(days.at(index).toString(), Qt::ISODate);
        for (const QJsonValue &value : members) {
            const QJsonObject member = value.toObject();
            if (!selected.isEmpty() && text(member, "name") != selected)
                continue;
            day.emails += member.value(QStringLiteral("emails")).toArray().at(index).toInt();
            day.outreach += member.value(QStringLiteral("outreach")).toArray().at(index).toInt();
            day.contacts += member.value(QStringLiteral("contacts")).toArray().at(index).toInt();
        }
        daily.append(day);
    }
    m_teamChart->setDays(daily);
    m_teamChartTitle->setText(QStringLiteral("Daily activity · %1 · last %2 days")
        .arg(selected.isEmpty() ? QStringLiteral("all members") : selected)
        .arg(daily.size()));
}

void CrmFeature::updateZohoAction()
{
    const bool connected = m_settings.value(QStringLiteral("zoho")).toObject()
                               .value(QStringLiteral("connected")).toBool();
    m_zohoSyncButton->setVisible(m_installed);
    m_zohoSyncButton->setText(connected ? QStringLiteral("Sync Zoho Mail")
                                        : QStringLiteral("Connect Zoho Mail"));
    // Owners/admins manage the encrypted connection. Once it exists, every
    // organization member may refresh the same shared CRM mail boxes.
    m_zohoSyncButton->setEnabled(m_installed &&
                                 (connected || m_canManageSettings));
    m_zohoSyncButton->setToolTip(
        connected
            ? QStringLiteral("Refresh the organization's shared Zoho Mail boxes")
            : m_canManageSettings && m_settings.value(QStringLiteral("zoho")).toObject()
                                         .value(QStringLiteral("oneClick")).toBool()
            ? QStringLiteral("Sign in to Zoho and allow reading the organization's mail and sending CRM emails")
            : m_canManageSettings
            ? QStringLiteral("Set up the Zoho OAuth client: opens the step-by-step guide")
            : QStringLiteral("Ask an owner or admin to connect Zoho Mail"));
}

void CrmFeature::rebuildContactColumns()
{
    // Custom keys: those added this session plus every key any contact
    // carries, so a CSV header the CRM has no field for still gets a column.
    QStringList custom = m_customColumns;
    for (const QJsonValue &value : m_contacts) {
        const QJsonObject fields =
            value.toObject().value(QStringLiteral("fields")).toObject();
        for (const QString &key : fields.keys()) {
            if (!custom.contains(key))
                custom << key;
        }
    }
    QList<ContactColumnSpec> columns;
    const QStringList removed = removedColumns();
    for (const ContactColumn &column : contactColumns()) {
        if (removed.contains(QString::fromLatin1(column.field)))
            continue;
        columns.append({ContactColumnSpec::Field,
                        QString::fromLatin1(column.field),
                        QString::fromLatin1(column.field),
                        QString::fromLatin1(column.label), column.width});
    }
    for (const QString &key : custom) {
        columns.append({ContactColumnSpec::CustomField,
                        QLatin1String(kCustomColumnPrefix) + key, key,
                        customColumnLabel(key), 160});
    }
    columns.append({ContactColumnSpec::MastodonLatest,
                    QLatin1String(kMastodonLatestColumnId), QString(),
                    QStringLiteral("Mastodon last update"), 320});
    columns.append({ContactColumnSpec::LastContacted,
                    QLatin1String(kLastContactedColumnId), QString(),
                    QStringLiteral("Last contacted"), 150});
    columns.append({ContactColumnSpec::LinkedEmails,
                    QLatin1String(kLinkedEmailsColumnId), QString(),
                    QStringLiteral("Linked emails"), 320});
    columns.append({ContactColumnSpec::LinkedToots,
                    QLatin1String(kLinkedTootsColumnId), QString(),
                    QStringLiteral("Linked toots"), 320});
    columns.append({ContactColumnSpec::Actions, QLatin1String(kActionsColumnId),
                    QString(), QStringLiteral("Actions"), 0});
    columns.append({ContactColumnSpec::AddColumn, QLatin1String(kAddColumnId),
                    QString(), QStringLiteral("+"), 36});
    bool unchanged = columns.size() == m_columns.size();
    for (int index = 0; unchanged && index < columns.size(); ++index)
        unchanged = columns.at(index).id == m_columns.at(index).id;
    if (unchanged)
        return;
    m_columns = columns;
    m_rendering = true;
    // The rows carry the old columns' cells; the next snapshot brings new ones.
    ++m_fillGeneration;
    m_filling = false;
    m_tableRows.clear();
    m_contactsTable->setRowCount(0);
    m_contactsTable->setColumnCount(columns.size());
    QStringList labels;
    for (const ContactColumnSpec &column : columns)
        labels << column.label;
    m_contactsTable->setHorizontalHeaderLabels(labels);
    QHeaderView *header = m_contactsTable->horizontalHeader();
    for (int index = 0; index < columns.size(); ++index) {
        const ContactColumnSpec &column = columns.at(index);
        if (column.kind == ContactColumnSpec::Actions) {
            header->setSectionResizeMode(index, QHeaderView::ResizeToContents);
        } else {
            header->setSectionResizeMode(index, QHeaderView::Interactive);
            m_contactsTable->setColumnWidth(index, column.width);
        }
        m_contactsTable->setColumnHidden(index, columnHiddenInView(column));
    }
    if (QTableWidgetItem *plus =
            m_contactsTable->horizontalHeaderItem(columns.size() - 1)) {
        plus->setToolTip(QStringLiteral("Add a column"));
        plus->setTextAlignment(Qt::AlignCenter);
    }
    const int sortColumn = columnIndexById(m_sortId);
    const QSignalBlocker blocker(header);
    header->setSortIndicator(sortColumn, m_sortDescending ? Qt::DescendingOrder
                                                          : Qt::AscendingOrder);
    m_rendering = false;
}

// Position of the first column of `kind`, or of the one carrying `key`.
int CrmFeature::columnIndex(ContactColumnSpec::Kind kind, const QString &key) const
{
    for (int index = 0; index < m_columns.size(); ++index) {
        const ContactColumnSpec &column = m_columns.at(index);
        if (column.kind == kind && (key.isEmpty() || column.key == key))
            return index;
    }
    return -1;
}

// Position of the column with this id; -1 for an empty id, so an unsorted
// table carries no indicator and keeps the worker's order.
int CrmFeature::columnIndexById(const QString &id) const
{
    if (id.isEmpty())
        return -1;
    for (int index = 0; index < m_columns.size(); ++index) {
        if (m_columns.at(index).id == id)
            return index;
    }
    return -1;
}

void CrmFeature::rebuildContactIndex()
{
    m_contactIndex.clear();
    m_contactIndex.reserve(m_contacts.size());
    for (qsizetype at = 0; at < m_contacts.size(); ++at)
        m_contactIndex.insert(text(m_contacts.at(at).toObject(), "id"), at);
}

void CrmFeature::refreshContactSnapshot()
{
    syncNoteDrafts();
    int uncontacted = 0;
    for (const QJsonValue &value : std::as_const(m_contacts)) {
        if (value.toObject().value(QStringLiteral("lastContactedAt")).toDouble() <= 0)
            ++uncontacted;
    }
    if (uncontacted != m_uncontacted) {
        m_uncontacted = uncontacted;
        if (m_navigationBadgeChanged)
            m_navigationBadgeChanged();
    }
    // Everything the build reads is copied here, on the UI thread. Implicit
    // sharing keeps the copies cheap and safe to read on the pool thread.
    const quint64 generation = ++m_snapshotGeneration;
    const QJsonArray contacts = m_contacts;
    const QList<ContactColumnSpec> columns = m_columns;
    const QString sortId = m_sortId;
    const bool descending = m_sortDescending;
    const QHash<QString, QList<QJsonObject>> contactEmails = m_contactEmails;
    const QHash<QString, QList<QJsonObject>> contactToots = m_contactToots;
    QHash<QString, qint64> posted;
    for (auto latest = m_mastodonLatest.cbegin(); latest != m_mastodonLatest.cend(); ++latest)
        posted.insert(latest.key(), latest->postedAt);
    // The tabs contacts are counted into; "No pipeline" is everyone whose flow
    // is none of these pipelines.
    QList<QJsonObject> sheets;
    for (const QJsonValue &value : std::as_const(m_views))
        sheets << value.toObject();
    for (const QJsonObject &sheet : std::as_const(m_virtualSheets)) {
        if (text(sheet, "id") != QLatin1String(kNoPipelineTabId))
            sheets << sheet;
    }
    QSet<QString> pipelines;
    for (const QJsonObject &sheet : std::as_const(sheets)) {
        if (!text(sheet, "flow").isEmpty())
            pipelines.insert(text(sheet, "flow"));
    }
    const QHash<QString, QStringList> lists = pipelineLists(m_views);
    const QPointer<CrmFeature> guard(this);
    // A build still queued behind the running one would only be dropped.
    m_workers->clear();
    m_workers->start([guard, generation, contacts, columns, sortId, descending,
                      contactEmails, contactToots, posted, sheets, pipelines, lists] {
        ContactSnapshot snapshot;
        snapshot.generation = generation;
        for (const ContactColumnSpec &column : columns)
            snapshot.columnIds << column.id;
        QList<QJsonObject> ordered;
        ordered.reserve(contacts.size());
        for (const QJsonValue &value : contacts)
            ordered.append(value.toObject());
        int sortColumn = -1;
        for (int at = 0; !sortId.isEmpty() && at < columns.size(); ++at) {
            if (columns.at(at).id == sortId)
                sortColumn = at;
        }
        if (sortColumn >= 0) {
            const ContactColumnSpec &spec = columns.at(sortColumn);
            const auto number = [&spec, &posted, &contactEmails,
                                 &contactToots](const QJsonObject &contact) -> qint64 {
                if (spec.kind == ContactColumnSpec::LastContacted)
                    return qint64(contact.value(QStringLiteral("lastContactedAt")).toDouble());
                if (spec.kind == ContactColumnSpec::LinkedEmails ||
                    spec.kind == ContactColumnSpec::LinkedToots) {
                    const auto &linked = spec.kind == ContactColumnSpec::LinkedToots
                        ? contactToots : contactEmails;
                    const auto emails = linked.constFind(text(contact, "id"));
                    return emails == linked.cend() || emails->isEmpty()
                        ? 0 : emailSentAt(emails->first());
                }
                return posted.value(mastodonHandleKey(text(contact, "mastodon")));
            };
            const auto string = [&spec](const QJsonObject &contact) -> QString {
                if (spec.kind == ContactColumnSpec::CustomField)
                    return contact.value(QStringLiteral("fields")).toObject()
                        .value(spec.key).toString().trimmed();
                if (spec.key == QLatin1String("tags")) {
                    QStringList tags;
                    for (const QJsonValue &tag : contact.value(QStringLiteral("tags")).toArray())
                        tags << tag.toString();
                    return tags.join(QStringLiteral(", "));
                }
                return text(contact, spec.key);
            };
            const bool numeric = spec.kind == ContactColumnSpec::LastContacted ||
                                 spec.kind == ContactColumnSpec::LinkedEmails ||
                                 spec.kind == ContactColumnSpec::LinkedToots ||
                                 spec.kind == ContactColumnSpec::MastodonLatest;
            std::stable_sort(ordered.begin(), ordered.end(),
                             [&](const QJsonObject &a, const QJsonObject &b) {
                int order = 0;
                if (numeric) {
                    const qint64 left = number(a), right = number(b);
                    order = left < right ? -1 : left > right ? 1 : 0;
                } else {
                    // Empty cells sink to the bottom whichever way the column sorts.
                    const QString left = string(a), right = string(b);
                    if (left.isEmpty() != right.isEmpty())
                        return right.isEmpty();
                    order = left.compare(right, Qt::CaseInsensitive);
                }
                return descending ? order > 0 : order < 0;
            });
        }
        const QLocale locale;
        snapshot.rows.reserve(ordered.size());
        for (const QJsonObject &contact : std::as_const(ordered)) {
            ContactRow row;
            row.id = text(contact, "id");
            row.flow = text(contact, "flow");
            row.stage = text(contact, "stage");
            row.mastodonKey = mastodonHandleKey(text(contact, "mastodon"));
            row.avatarSources = pictureSources(contact);
            row.name = text(contact, "name").isEmpty() ? text(contact, "email")
                                                       : text(contact, "name");
            row.createdAt = qint64(contact.value(QStringLiteral("createdAt")).toDouble());
            row.initials = contactInitials(contact);
            row.avatarTone = contactAvatarTone(contact);
            row.hasEmail = !text(contact, "email").isEmpty();
            row.lastContactedAt =
                qint64(contact.value(QStringLiteral("lastContactedAt")).toDouble());
            row.cells.reserve(columns.size());
            for (const ContactColumnSpec &spec : columns) {
                QString value;
                if (spec.kind == ContactColumnSpec::Field) {
                    if (spec.key == QLatin1String("tags")) {
                        QStringList tags;
                        for (const QJsonValue &tag :
                             contact.value(QStringLiteral("tags")).toArray())
                            tags << tag.toString();
                        value = tags.join(QStringLiteral(", "));
                    } else if (spec.key == QLatin1String("value")) {
                        const qint64 amount = qint64(
                            contact.value(QStringLiteral("value")).toDouble());
                        if (amount > 0)
                            value = locale.toString(amount);
                    } else {
                        value = text(contact, spec.key);
                    }
                } else if (spec.kind == ContactColumnSpec::CustomField) {
                    value = contact.value(QStringLiteral("fields")).toObject()
                                .value(spec.key).toString();
                } else if (spec.kind == ContactColumnSpec::LastContacted) {
                    value = timestamp(qint64(
                        contact.value(QStringLiteral("lastContactedAt")).toDouble()));
                } else if (spec.kind == ContactColumnSpec::LinkedEmails ||
                           spec.kind == ContactColumnSpec::LinkedToots) {
                    // The newest message in the cell; the rest are a click away.
                    const QList<QJsonObject> emails =
                        (spec.kind == ContactColumnSpec::LinkedToots ? contactToots
                                                                     : contactEmails)
                            .value(row.id);
                    if (!emails.isEmpty()) {
                        value = linkedEmailLine(emails.first());
                        if (emails.size() > 1)
                            value += QStringLiteral("  (+%1 more)").arg(emails.size() - 1);
                    }
                }
                row.cells << value;
            }
            row.search = row.cells.join(QLatin1Char('\n'));
            const int index = int(snapshot.rows.size());
            for (const QJsonObject &sheet : sheets) {
                if (contactInSheet(contact, sheet, lists))
                    snapshot.sheetRows[text(sheet, "id")].append(index);
            }
            if (!pipelines.contains(row.flow))
                snapshot.sheetRows[QLatin1String(kNoPipelineTabId)].append(index);
            snapshot.rows.append(std::move(row));
        }
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [guard, snapshot = std::move(snapshot)]() mutable {
                if (guard)
                    guard->applyContactSnapshot(std::move(snapshot));
            },
            Qt::QueuedConnection);
    });
}

void CrmFeature::applyContactSnapshot(ContactSnapshot snapshot)
{
    if (snapshot.generation != m_snapshotGeneration)
        return; // a newer build is on its way
    QStringList columnIds;
    for (const ContactColumnSpec &column : std::as_const(m_columns))
        columnIds << column.id;
    if (snapshot.columnIds != columnIds) {
        refreshContactSnapshot();
        return;
    }
    m_snapshot = std::move(snapshot);
    renderSheetPicker();
    renderSheets();
    applyColumnVisibility();
    m_flow->setVisible(text(activeSheet(), "flow").isEmpty());
    selectFirstStage();
    renderContacts();
    queueAvatars();
    renderPeople();
    if (!m_detailContactId.isEmpty())
        showContactDetail(m_detailContactId);
}

void CrmFeature::renderContacts()
{
    // Coalesced: a load, a tab switch and a column change landing in the same
    // turn put the rows in once.
    if (m_renderQueued)
        return;
    m_renderQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_renderQueued = false;
        // A re-render has to keep a multi-row selection, along with the
        // current row. A fill cut short keeps what it carried for the rows it
        // never put back.
        if (!m_filling) {
            m_fillSelected.clear();
            m_fillCurrent.clear();
        }
        const QString currentId = text(selectedContact(), "id");
        if (!currentId.isEmpty())
            m_fillCurrent = currentId;
        for (const QJsonObject &contact : selectedContacts())
            m_fillSelected.insert(text(contact, "id"));
        m_fillSource = m_snapshot.rows;
        m_fillRows = m_snapshot.sheetRows.value(m_sheet->currentData().toString());
        renderStageStrip();
        renderNextContact();
        m_fillNext = 0;
        ++m_fillGeneration;
        m_filling = true;
        m_rendering = true;
        m_contactsTable->setRowCount(0);
        m_rendering = false;
        m_tableRows.clear();
        fillContactRows();
    });
}

void CrmFeature::fillContactRows()
{
    const int end = qMin(m_fillNext + kFillChunkRows, int(m_fillRows.size()));
    const int first = m_contactsTable->rowCount();
    const QColor linkColor = m_contactsTable->palette().color(QPalette::Link);
    const QString providerLabel =
        text(m_settings, "mailProvider") == QLatin1String("gmail")
            ? QStringLiteral("Gmail") : QStringLiteral("Zoho Mail");
    QHash<QRgb, QIcon> stageDots;
    const QIcon mastodonRefresh =
        refreshIcon(m_contactsTable->palette().color(QPalette::WindowText));
    m_rendering = true;
    m_contactsTable->setRowCount(first + end - m_fillNext);
    for (int at = m_fillNext; at < end; ++at) {
        const int sourceIndex = m_fillRows.at(at);
        const ContactRow &source = m_fillSource.at(sourceIndex);
        const QString contactId = source.id;
        const int row = first + at - m_fillNext;
        m_tableRows.append(sourceIndex);
        for (int column = 0; column < m_columns.size(); ++column) {
            const ContactColumnSpec &spec = m_columns.at(column);
            if (spec.kind == ContactColumnSpec::Actions) {
                auto *actions = new QWidget(m_contactsTable);
                actions->setObjectName(QStringLiteral("crmRowActions"));
                auto *actionLayout = new QHBoxLayout(actions);
                actionLayout->setContentsMargins(8, 0, 8, 0);
                actionLayout->setSpacing(4);
                auto *email = new QPushButton(QStringLiteral("Email"), actions);
                auto *prompt = new QPushButton(QStringLiteral("Copy prompt"), actions);
                prompt->setToolTip(QStringLiteral(
                    "Copy a prompt asking an assistant about ForkMesh and this "
                    "contact, and for a short toot to send them"));
                auto *edit = new QPushButton(QStringLiteral("Edit"), actions);
                auto *remove = new QPushButton(QStringLiteral("Delete"), actions);
                remove->setObjectName(QStringLiteral("dangerButton"));
                QFont labelFont = email->font();
                labelFont.setWeight(QFont::DemiBold);
                const QFontMetrics labelMetrics(labelFont);
                for (QPushButton *button : {email, prompt, edit, remove}) {
                    // A slim button centred in the 36px row (the table's
                    // row-actions style sets the padding), as wide as its
                    // semibold label so no label is clipped. Delete stands a
                    // little apart from the everyday actions.
                    button->setFixedHeight(26);
                    button->setMinimumWidth(labelMetrics.horizontalAdvance(button->text()) + 26);
                    button->setCursor(Qt::PointingHandCursor);
                    if (button == remove)
                        actionLayout->addSpacing(8);
                    actionLayout->addWidget(button);
                }
                email->setEnabled(source.hasEmail);
                connect(email, &QPushButton::clicked, this, [this, contactId] {
                    composeEmail(contactById(contactId));
                });
                connect(prompt, &QPushButton::clicked, this, [this, contactId] {
                    copyOutreachPrompt(contactById(contactId));
                });
                connect(edit, &QPushButton::clicked, this, [this, contactId] {
                    editContact(contactById(contactId));
                });
                connect(remove, &QPushButton::clicked, this, [this, contactId] {
                    deleteContact(contactById(contactId));
                });
                m_contactsTable->setCellWidget(row, column, actions);
                continue;
            }
            auto *item = new QTableWidgetItem;
            const QString value = source.cells.value(column);
            switch (spec.kind) {
            case ContactColumnSpec::Field:
                item->setText(value);
                item->setToolTip(value);
                if (spec.key == QLatin1String("value"))
                    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
                if (spec.key == QLatin1String("name")) {
                    item->setIcon(contactAvatar(source));
                    item->setData(kAvatarRole, true);
                    item->setToolTip(QStringLiteral(
                        "Click the picture or the name to open everything on this "
                        "contact. Press F2 to edit the name here."));
                }
                if (spec.key == QLatin1String("stage") && !value.isEmpty()) {
                    // The stage wears its chevron's color from the strip above.
                    QColor color = CrmStageStrip::stageColor(
                        value, flowStages(source.flow).indexOf(value));
                    auto dot = stageDots.constFind(color.rgb());
                    if (dot == stageDots.constEnd())
                        dot = stageDots.insert(color.rgb(), stageDotIcon(color));
                    item->setIcon(*dot);
                    color.setAlpha(46);
                    item->setBackground(color);
                }
                if (spec.key == QLatin1String("email") && !value.isEmpty()) {
                    QFont font = item->font();
                    font.setUnderline(true);
                    item->setFont(font);
                    item->setForeground(linkColor);
                    item->setToolTip(QStringLiteral(
                        "Click to write to %1 in %2. Double-click or press F2 "
                        "to edit the address.").arg(value, providerLabel));
                }
                if (spec.key == QLatin1String("mastodon")) {
                    const QUrl profile = mastodonProfileUrl(value);
                    if (profile.isValid()) {
                        QFont font = item->font();
                        font.setUnderline(true);
                        item->setFont(font);
                        item->setForeground(linkColor);
                        item->setToolTip(QStringLiteral(
                            "Click to open %1. Double-click or press F2 to edit "
                            "the handle.").arg(profile.toString()));
                    }
                }
                break;
            case ContactColumnSpec::CustomField:
                item->setText(value);
                item->setToolTip(value);
                break;
            case ContactColumnSpec::MastodonLatest: {
                fillMastodonItem(item, source.mastodonKey);
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                const QString key = source.mastodonKey;
                if (key.isEmpty())
                    break;
                // The row's own refresh button, right-aligned over the cell.
                // The holder passes every other click through to the table,
                // so a double click still opens the post.
                item->setData(kRowButtonRole, true);
                auto *holder = new QWidget(m_contactsTable);
                auto *holderLayout = new QHBoxLayout(holder);
                holderLayout->setContentsMargins(0, 0, 4, 0);
                holderLayout->addStretch(1);
                auto *refresh = new QToolButton(holder);
                refresh->setObjectName(QStringLiteral("crmMastodonRefresh"));
                refresh->setProperty("mastodonKey", key);
                refresh->setAutoRaise(true);
                refresh->setCursor(Qt::PointingHandCursor);
                refresh->setIcon(mastodonRefresh);
                refresh->setIconSize(QSize(14, 14));
                refresh->setFixedSize(22, 22);
                refresh->setToolTip(
                    QStringLiteral("Check @%1 for a newer post now").arg(key));
                refresh->setEnabled(!m_mastodonPending.contains(key) &&
                                    !m_mastodonQueue.contains(key));
                holderLayout->addWidget(refresh);
                connect(refresh, &QToolButton::clicked, this,
                        [this, key] { refreshMastodonNow(key); });
                m_contactsTable->setCellWidget(row, column, holder);
                break;
            }
            case ContactColumnSpec::LastContacted:
                // Editable: a click opens a date picker with a Now button.
                item->setText(value);
                item->setToolTip(value +
                                 QStringLiteral("\n\nClick to change the date, or press Now."));
                item->setData(kContactedStampRole, source.lastContactedAt);
                break;
            case ContactColumnSpec::LinkedEmails:
            case ContactColumnSpec::LinkedToots: {
                const bool toots = spec.kind == ContactColumnSpec::LinkedToots;
                item->setText(value);
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                const QList<QJsonObject> emails =
                    (toots ? m_contactToots : m_contactEmails).value(contactId);
                if (emails.isEmpty())
                    break;
                QStringList lines;
                for (const QJsonObject &email : emails)
                    lines << linkedEmailLine(email);
                item->setToolTip(lines.join(QLatin1Char('\n')) +
                                 (toots ? QStringLiteral("\n\nClick to open it on Mastodon.")
                                        : QStringLiteral("\n\nClick to open it in the mailbox.")));
                if (QUrl(text(emails.first(), "providerUrl")).scheme() ==
                    QLatin1String("https")) {
                    QFont font = item->font();
                    font.setUnderline(true);
                    item->setFont(font);
                    item->setForeground(linkColor);
                }
                break;
            }
            default:
                item->setFlags(item->flags() & ~Qt::ItemIsEditable);
                break;
            }
            if (column == 0)
                item->setData(Qt::UserRole, contactId);
            m_contactsTable->setItem(row, column, item);
        }
        m_contactsTable->setRowHidden(row, !contactRowShown(source));
    }
    m_fillNext = end;
    m_rendering = false;
    if (m_fillNext < m_fillRows.size()) {
        // The rest after the window has had a turn to paint and take input.
        const quint64 generation = m_fillGeneration;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation == m_fillGeneration)
                fillContactRows();
        });
        return;
    }
    m_filling = false;
    int currentRow = -1;
    QList<int> selectedRows;
    for (int row = 0; row < m_tableRows.size(); ++row) {
        const QString &id = m_fillSource.at(m_tableRows.at(row)).id;
        if (id == m_fillCurrent)
            currentRow = row;
        if (m_fillSelected.contains(id))
            selectedRows << row;
    }
    m_fillSelected.clear();
    m_fillCurrent.clear();
    // Through the selection model: selectRow() reads the live keyboard
    // modifiers, so a refresh landing mid Ctrl+click would toggle rows.
    if (selectedRows.isEmpty() && currentRow >= 0)
        selectedRows << currentRow;
    QItemSelectionModel *selection = m_contactsTable->selectionModel();
    const QAbstractItemModel *model = m_contactsTable->model();
    if (currentRow >= 0)
        selection->setCurrentIndex(model->index(currentRow, 0),
                                   QItemSelectionModel::NoUpdate);
    for (int row : std::as_const(selectedRows))
        selection->select(model->index(row, 0),
                          QItemSelectionModel::Select | QItemSelectionModel::Rows);
    focusContactRow();
    refreshMastodonUpdates();
}

bool CrmFeature::contactRowShown(const ContactRow &row) const
{
    const QString flow = m_flow->isHidden() ? QString() : m_flow->currentData().toString();
    const QString stage = m_stage->currentData().toString();
    return (flow.isEmpty() || row.flow == flow) &&
           (stage.isEmpty() || row.stage == stage) &&
           (m_query.isEmpty() || row.search.contains(m_query, Qt::CaseInsensitive));
}

void CrmFeature::renderStageStrip()
{
    if (!m_stageStrip || !m_sheet)
        return;
    QHash<QString, int> counts;
    const QStringList stages = stripStages(&counts);
    if (stages.isEmpty()) {
        m_stageStrip->hide();
        return;
    }
    QList<CrmStageStrip::Stage> strip;
    for (const QString &stage : stages)
        strip.append({stage, CrmStageStrip::stageLabel(stage), counts.value(stage)});
    m_stageStrip->setStages(strip, m_stage->currentData().toString());
    m_stageStrip->show();
}

NextContact CrmFeature::nextContact(NextMode mode) const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto ago = [now](qint64 at) {
        const qint64 days = (now - at) / kDayMs;
        return days <= 0 ? QStringLiteral("today")
             : days == 1 ? QStringLiteral("yesterday")
                         : QStringLiteral("%1 days ago").arg(days);
    };
    const QLocale locale;
    NextContact best;
    std::tuple<int, int, qint64, QString> followKey;
    using ReachKey = std::tuple<int, qint64, int, qint64, double, double, int, qint64, QString>;
    struct ReachEntry
    {
        ReachKey key;
        QString id;
        int row = -1;
        QString reason;
    };
    QList<ReachEntry> reachRanked;
    for (const int index : m_snapshot.sheetRows.value(m_sheet->currentData().toString())) {
        if (index < 0 || index >= m_snapshot.rows.size())
            continue;
        const QString id = m_snapshot.rows.at(index).id;
        const QJsonObject contact = contactById(id);
        if (contact.isEmpty() || m_nextSkipped.contains(id))
            continue;
        const QString email = text(contact, "email");
        if (email.isEmpty() && mastodonHandleKey(text(contact, "mastodon")).isEmpty())
            continue;
        const qint64 last = qint64(contact.value(QStringLiteral("lastContactedAt")).toDouble());
        qint64 heardAt = 0;
        qint64 wroteAt = 0;
        QString heardSubject;
        QList<QJsonObject> linked = m_contactEmails.value(id);
        linked.append(m_contactToots.value(id));
        for (const QJsonObject &message : std::as_const(linked)) {
            const qint64 at = emailSentAt(message);
            if (text(message, "direction") == QLatin1String("inbound")) {
                if (at > heardAt) {
                    heardAt = at;
                    heardSubject = text(message, "subject");
                }
            } else if (!kUnsentStatuses.contains(text(message, "status").toLower())) {
                wroteAt = qMax(wroteAt, at);
            }
        }
        const QString stage = text(contact, "stage").toLower();
        const QString step = text(contact, "nextStep");
        const bool waiting =
            heardAt >= now - kNextReplyDays * kDayMs && heardAt > wroteAt && heardAt >= last;
        const bool closed = kWonStages.contains(stage) || kEndedStages.contains(stage);
        QString reason;
        bool better = false;
        if (mode == NextMode::ReachOut) {
            if (waiting || closed || last > 0)
                continue;
            const LeadMarks marks = leadMarks(contact);
            const qint64 created = qint64(contact.value(QStringLiteral("createdAt")).toDouble());
            const auto latestPost =
                m_mastodonLatest.constFind(mastodonHandleKey(text(contact, "mastodon")));
            const qint64 posted =
                latestPost == m_mastodonLatest.constEnd() ? 0 : qMax<qint64>(latestPost->postedAt, 0);
            QStringList qualifies;
            if (posted > 0)
                qualifies << QStringLiteral("posted on Mastodon %1").arg(ago(posted));
            if (!step.isEmpty())
                qualifies << QStringLiteral("next step on the record: %1").arg(step);
            if (marks.value > 0)
                qualifies << QStringLiteral("deal value %1").arg(locale.toString(marks.value));
            if (marks.rank > 0)
                qualifies << QStringLiteral("ranked %1 (%2)").arg(marks.rank).arg(marks.rankField);
            if (marks.score > 0)
                qualifies << QStringLiteral("score %1 (%2)").arg(marks.score).arg(marks.scoreField);
            if (!email.isEmpty())
                qualifies << QStringLiteral("an email address on file");
            reason = (created > 0 ? QStringLiteral("Added %1 and never contacted. ").arg(ago(created))
                                  : QStringLiteral("Never contacted. ")) +
                     (qualifies.isEmpty()
                          ? QStringLiteral("Nothing on the record sets a new lead apart, so the "
                                           "longest on the list goes first.")
                          : QStringLiteral("Best qualified new lead: %1.")
                                .arg(qualifies.join(QStringLiteral("; "))));
            reachRanked.append({std::make_tuple(
                posted > 0 ? 0 : 1, -posted,
                step.isEmpty() ? 1 : 0, -marks.value,
                marks.rank > 0 ? marks.rank : std::numeric_limits<double>::infinity(), -marks.score,
                email.isEmpty() ? 1 : 0, created, id), id, index, reason});
        } else {
            int tier = 0;
            qint64 order = 0;
            if (waiting) {
                order = -heardAt;
                reason = QStringLiteral("They wrote to us %1%2 and have had no answer yet, so the "
                                        "next move is ours.")
                             .arg(ago(heardAt),
                                  heardSubject.isEmpty()
                                      ? QString()
                                      : QString::fromUtf8(" (\xE2\x80\x9C%1\xE2\x80\x9D)")
                                            .arg(heardSubject));
            } else if (!closed && last > 0 && now - last >= kNextFollowUpDays * kDayMs) {
                tier = 1;
                order = last;
                reason = QStringLiteral("Last contacted %1 and nothing has come back since; a "
                                        "follow-up is due after %2 quiet days.")
                             .arg(ago(last))
                             .arg(kNextFollowUpDays);
            } else {
                continue;
            }
            if (!step.isEmpty())
                reason += QStringLiteral(" Next step on the record: %1.").arg(step);
            const auto key = std::make_tuple(tier, step.isEmpty() ? 1 : 0, order, id);
            better = best.id.isEmpty() || key < followKey;
            if (better)
                followKey = key;
        }
        ++best.due;
        if (better) {
            best.id = id;
            best.row = index;
            best.reason = reason;
        }
    }
    if (mode == NextMode::ReachOut && !reachRanked.isEmpty()) {
        std::sort(reachRanked.begin(), reachRanked.end(),
                  [](const ReachEntry &a, const ReachEntry &b) { return a.key < b.key; });
        best.id = reachRanked.first().id;
        best.row = reachRanked.first().row;
        best.reason = reachRanked.first().reason;
        for (int at = 0; at < reachRanked.size(); ++at) {
            best.ranked << reachRanked.at(at).id;
            if (at > 0 && at <= kNextUpcoming)
                best.upcoming << reachRanked.at(at).id;
        }
    }
    return best;
}

void CrmFeature::renderNextContact()
{
    if (!m_nextCard || !m_sheet)
        return;
    auto *cardLayout = qobject_cast<QHBoxLayout *>(m_nextCard->layout());
    while (QLayoutItem *item = cardLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    m_nextPhotos.clear();
    if (m_snapshot.sheetRows.value(m_sheet->currentData().toString()).isEmpty() &&
        m_nextSkipped.isEmpty()) {
        m_nextCard->hide();
        return;
    }
    cardLayout->addWidget(nextContactCard(NextMode::ReachOut), 1);
    cardLayout->addWidget(nextContactCard(NextMode::FollowUp), 1);
    m_nextCard->show();
    queueReachMastodon(nextContact(NextMode::ReachOut).ranked);
}

void CrmFeature::queueReachMastodon(const QStringList &ranked)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int wanted = 0;
    for (const QString &id : ranked) {
        if (wanted >= kReachMastodonReads)
            break;
        const QString key = mastodonHandleKey(text(contactById(id), "mastodon"));
        if (key.isEmpty())
            continue;
        ++wanted;
        const auto cached = m_mastodonLatest.constFind(key);
        if ((cached != m_mastodonLatest.constEnd() && now - cached->fetchedAt < kMastodonFreshMs) ||
            m_mastodonPending.contains(key) || m_mastodonQueue.contains(key))
            continue;
        m_mastodonQueue.append(key);
    }
    refreshMastodonUpdates();
}

void CrmFeature::scheduleNextRender()
{
    if (m_nextRenderQueued)
        return;
    m_nextRenderQueued = true;
    QTimer::singleShot(kNextRenderDelayMs, this, [this] {
        m_nextRenderQueued = false;
        renderNextContact();
    });
}

QLabel *CrmFeature::nextPostLabel(const QJsonObject &contact, bool compact) const
{
    const QString key = mastodonHandleKey(text(contact, "mastodon"));
    if (key.isEmpty())
        return nullptr;
    const auto latest = m_mastodonLatest.constFind(key);
    auto *label = new QLabel;
    label->setObjectName(QStringLiteral("crmNextPost"));
    label->setWordWrap(!compact);
    if (latest == m_mastodonLatest.constEnd() || !latest->error.isEmpty() || latest->postedAt <= 0) {
        QString note;
        if (latest == m_mastodonLatest.constEnd()) {
            if (m_mastodonPending.contains(key) || m_mastodonQueue.contains(key))
                note = QString::fromUtf8("Reading @%1\xE2\x80\xA6").arg(key);
        } else if (!latest->error.isEmpty()) {
            note = QStringLiteral("Mastodon post unavailable: %1").arg(latest->error);
        } else {
            note = QStringLiteral("No public Mastodon posts");
        }
        if (note.isEmpty()) {
            delete label;
            return nullptr;
        }
        label->setObjectName(QStringLiteral("mutedLabel"));
        label->setTextFormat(Qt::PlainText);
        label->setText(note);
        label->setToolTip(note);
        return label;
    }
    const qint64 days = (QDateTime::currentMSecsSinceEpoch() - latest->postedAt) / kDayMs;
    const QString when = days <= 0 ? QStringLiteral("today")
                       : days == 1 ? QStringLiteral("yesterday")
                                   : QStringLiteral("%1 days ago").arg(days);
    const QString excerpt = compact ? plainExcerpt(latest->text, 90) : latest->excerpt;
    const QUrl url(latest->url);
    label->setTextFormat(Qt::RichText);
    label->setText(QString::fromUtf8("%1 \xC2\xB7 %2")
                       .arg(when.toHtmlEscaped(),
                            url.isValid() && url.scheme() == QLatin1String("https")
                                ? QStringLiteral("<a href=\"%1\">%2</a>")
                                      .arg(url.toString().toHtmlEscaped(), excerpt.toHtmlEscaped())
                                : excerpt.toHtmlEscaped()));
    label->setOpenExternalLinks(true);
    label->setTextInteractionFlags(Qt::TextBrowserInteraction);
    label->setToolTip(timestamp(latest->postedAt) + QLatin1Char('\n') + latest->detail);
    return label;
}

QWidget *CrmFeature::nextContactCard(NextMode mode)
{
    const bool reach = mode == NextMode::ReachOut;
    const NextContact next = nextContact(mode);
    const QJsonObject contact = contactById(next.id);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    const QString listName = text(activeSheet(), "name");
    const QString onList = listName.isEmpty() ? QStringLiteral("this list") : listName;
    const QLocale locale;
    auto *body = new QFrame;
    body->setObjectName(reach ? QStringLiteral("crmReachOutCard") : QStringLiteral("crmFollowUpCard"));
    body->setAccessibleName(reach ? QStringLiteral("Reach out: best new lead")
                                  : QStringLiteral("Follow up"));
    body->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);
    auto *title = new QLabel;
    title->setTextFormat(Qt::PlainText);
    title->setWordWrap(true);
    QFont titleFont = title->font();
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    layout->addWidget(title);
    auto *head = new QHBoxLayout;
    head->setSpacing(6);
    const auto addButton = [head](const QString &label, const QString &tip) {
        auto *button = new QPushButton(label);
        button->setToolTip(tip);
        button->setCursor(Qt::PointingHandCursor);
        head->addWidget(button);
        return button;
    };

    if (contact.isEmpty()) {
        title->setText(reach ? QStringLiteral("Reach out") : QStringLiteral("Follow up"));
        auto *none = new QLabel(!m_nextSkipped.isEmpty()
            ? QStringLiteral("No one else on %1 is waiting here. You skipped %2 this session.")
                  .arg(onList)
                  .arg(m_nextSkipped.size())
            : reach
            ? QStringLiteral("No new leads on %1: everyone with an email or Mastodon handle has "
                             "been contacted, or the rest are at a won or ended stage.")
                  .arg(onList)
            : QStringLiteral("Nobody on %1 needs a follow-up: no one has written back without an "
                             "answer, and everyone contacted was reached in the last %2 days or "
                             "is at a won or ended stage.")
                  .arg(onList)
                  .arg(kNextFollowUpDays));
        none->setObjectName(QStringLiteral("mutedLabel"));
        none->setWordWrap(true);
        layout->addWidget(none);
        if (!m_nextSkipped.isEmpty()) {
            QPushButton *again = addButton(QStringLiteral("Show skipped again"),
                                           QStringLiteral("Put the people you skipped back in line"));
            connect(again, &QPushButton::clicked, this, [this] {
                m_nextSkipped.clear();
                renderNextContact();
            });
        }
        head->addStretch(1);
        layout->addLayout(head);
        layout->addStretch(1);
        return body;
    }

    const QString contactId = next.id;
    title->setText(reach
        ? QStringLiteral("Reach out%1best new lead on %2%1%3 never contacted")
              .arg(dot, onList, locale.toString(next.due))
        : QStringLiteral("Follow up%1%2%1%3 due").arg(dot, onList, locale.toString(next.due)));
    const QString email = text(contact, "email");
    QPushButton *write = addButton(
        email.isEmpty() ? QStringLiteral("Open Mastodon") : QStringLiteral("Write email"),
        email.isEmpty() ? QStringLiteral("Open their Mastodon profile")
                        : QStringLiteral("Open a message to %1").arg(email));
    write->setObjectName(QStringLiteral("primaryButton"));
    QPushButton *ai = addButton(QStringLiteral("Write with AI"),
                                QStringLiteral("Gather everything about them and us into a prompt "
                                               "for ChatGPT or Claude to write the message"));
    QPushButton *autoSend = email.isEmpty() ? nullptr : addButton(
        QStringLiteral("Send AI email"), autoEmailTip());
    QPushButton *open = addButton(QStringLiteral("Open contact"),
                                  QStringLiteral("Show everything about them in the side panel"));
    QPushButton *contacted = addButton(QStringLiteral("Mark contacted"),
                                       QStringLiteral("Set last contacted to now and move on"));
    QPushButton *skip = addButton(QStringLiteral("Skip"),
                                  QStringLiteral("Show the next person in line for this session"));
    head->addStretch(1);
    connect(write, &QPushButton::clicked, this, [this, contactId] {
        const QJsonObject target = contactById(contactId);
        if (!text(target, "email").isEmpty())
            openMailCompose(text(target, "email"));
        else
            QDesktopServices::openUrl(mastodonProfileUrl(text(target, "mastodon")));
    });
    const QString reason = next.reason;
    connect(ai, &QPushButton::clicked, this, [this, contactId, reach, reason] {
        showAiWriter(contactId, reach ? QStringLiteral("reach") : QStringLiteral("follow"), reason);
    });
    if (autoSend) {
        connect(autoSend, &QPushButton::clicked, this, [this, contactId, reach, reason] {
            sendAutoEmail(contactId, reach ? QStringLiteral("reach") : QStringLiteral("follow"),
                          reason);
        });
    }
    connect(open, &QPushButton::clicked, this,
            [this, contactId] { showContactDetail(contactId); });
    connect(contacted, &QPushButton::clicked, this, [this, contactId] {
        markContacted({contactId}, QDateTime::currentMSecsSinceEpoch());
    });
    connect(skip, &QPushButton::clicked, this, [this, contactId] {
        m_nextSkipped.insert(contactId);
        renderNextContact();
    });

    auto *photo = new QLabel;
    photo->setFixedSize(kNextPhotoSize, kNextPhotoSize);
    if (next.row >= 0 && next.row < m_snapshot.rows.size())
        photo->setPixmap(contactPicture(m_snapshot.rows.at(next.row), kNextPhotoSize)
                             .pixmap(kNextPhotoSize, kNextPhotoSize));
    m_nextPhotos.insert(contactId, photo);
    auto *name = new QLabel(text(contact, "name").isEmpty()
        ? (email.isEmpty() ? QStringLiteral("Unnamed contact") : email)
        : text(contact, "name"));
    name->setTextFormat(Qt::PlainText);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont nameFont = name->font();
    nameFont.setPointSizeF(nameFont.pointSizeF() * 1.3);
    nameFont.setWeight(QFont::DemiBold);
    name->setFont(nameFont);
    QStringList place;
    for (const char *key : {"title", "company", "location"}) {
        if (!text(contact, key).isEmpty())
            place << text(contact, key);
    }
    const qint64 last = qint64(contact.value(QStringLiteral("lastContactedAt")).toDouble());
    QStringList standing;
    standing << (text(contact, "stage").isEmpty()
                     ? QStringLiteral("No stage")
                     : CrmStageStrip::stageLabel(text(contact, "stage")));
    if (!flowLabel(text(contact, "flow")).isEmpty())
        standing << flowLabel(text(contact, "flow"));
    standing << QStringLiteral("last contacted %1")
                    .arg(last > 0 ? timestamp(last) : QStringLiteral("never"));
    auto *subtitle = new QLabel(place.isEmpty() ? standing.join(dot)
                                                : place.join(dot) + QLatin1Char('\n') +
                                                      standing.join(dot));
    subtitle->setTextFormat(Qt::PlainText);
    subtitle->setWordWrap(true);
    auto *why = new QLabel(QStringLiteral("<b>Why them:</b> %1").arg(next.reason.toHtmlEscaped()));
    why->setTextFormat(Qt::RichText);
    why->setWordWrap(true);
    auto *heading = new QVBoxLayout;
    heading->setSpacing(2);
    heading->addWidget(name);
    heading->addWidget(subtitle);
    if (reach) {
        if (QLabel *post = nextPostLabel(contact, false))
            heading->addWidget(post);
    }
    heading->addWidget(why);
    auto *who = new QHBoxLayout;
    who->setSpacing(12);
    who->addWidget(photo, 0, Qt::AlignTop);
    who->addLayout(heading, 1);
    layout->addLayout(who);
    layout->addLayout(head);
    if (reach && !next.upcoming.isEmpty()) {
        auto *lineTitle = new QLabel(QString::fromUtf8("Next in line \xC2\xB7 newest Mastodon post first"));
        lineTitle->setObjectName(QStringLiteral("mutedLabel"));
        layout->addWidget(lineTitle);
        auto *upcoming = new QVBoxLayout;
        upcoming->setSpacing(2);
        for (const QString &upcomingId : next.upcoming) {
            const QJsonObject person = contactById(upcomingId);
            auto *row = new QHBoxLayout;
            row->setSpacing(6);
            auto *open = new QPushButton(text(person, "name").isEmpty()
                ? (text(person, "email").isEmpty() ? QStringLiteral("Unnamed contact") : text(person, "email"))
                : text(person, "name"));
            open->setObjectName(QStringLiteral("crmNextUpcoming"));
            open->setFlat(true);
            open->setCursor(Qt::PointingHandCursor);
            open->setToolTip(QStringLiteral("Show everything about them in the side panel"));
            connect(open, &QPushButton::clicked, this,
                    [this, upcomingId] { showContactDetail(upcomingId); });
            row->addWidget(open, 0, Qt::AlignTop);
            if (QLabel *post = nextPostLabel(person, true)) {
                post->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                row->addWidget(post, 1);
            } else {
                row->addStretch(1);
            }
            upcoming->addLayout(row);
        }
        layout->addLayout(upcoming);
    }

    const auto valueLabel = [&dash](const QString &value, const QUrl &link) {
        auto *label = new QLabel;
        label->setWordWrap(true);
        if (link.isValid() && link.scheme() == QLatin1String("https")) {
            label->setTextFormat(Qt::RichText);
            label->setText(QStringLiteral("<a href=\"%1\">%2</a>")
                               .arg(link.toString().toHtmlEscaped(), value.toHtmlEscaped()));
            label->setOpenExternalLinks(true);
            label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        } else {
            label->setTextFormat(Qt::PlainText);
            label->setText(value.isEmpty() ? dash : value);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        }
        return label;
    };
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(3);
    constexpr int kPairs = 2;
    int placed = 0;
    const auto addField = [&](const QString &label, QLabel *value, bool wide) {
        if (wide && placed % kPairs)
            placed += kPairs - placed % kPairs;
        auto *key = new QLabel(label);
        key->setObjectName(QStringLiteral("mutedLabel"));
        const int row = placed / kPairs;
        const int column = (placed % kPairs) * 2;
        grid->addWidget(key, row, column, Qt::AlignRight | Qt::AlignTop);
        grid->addWidget(value, row, column + 1, 1, wide ? kPairs * 2 - 1 : 1);
        placed += wide ? kPairs : 1;
    };
    const QSet<QString> shownAbove{QStringLiteral("name"), QStringLiteral("title"),
                                   QStringLiteral("company"), QStringLiteral("location"),
                                   QStringLiteral("flow"), QStringLiteral("stage"),
                                   QStringLiteral("notes")};
    for (const ContactColumn &column : contactColumns()) {
        const QString field = QString::fromLatin1(column.field);
        if (shownAbove.contains(field))
            continue;
        QString value = text(contact, field);
        if (field == QLatin1String("tags")) {
            QStringList tags;
            for (const QJsonValue &tag : contact.value(QStringLiteral("tags")).toArray())
                tags << tag.toString();
            value = tags.join(QStringLiteral(", "));
        } else if (field == QLatin1String("value")) {
            const qint64 amount = qint64(contact.value(QStringLiteral("value")).toDouble());
            value = amount > 0 ? locale.toString(amount) : QString();
        }
        if (value.isEmpty())
            continue;
        const QUrl link = field == QLatin1String("email") ? mailComposeUrl(value)
                                                          : profileUrl(field, value);
        addField(QString::fromLatin1(column.label), valueLabel(value, link), false);
    }
    const QJsonObject custom = contact.value(QStringLiteral("fields")).toObject();
    for (auto entry = custom.constBegin(); entry != custom.constEnd(); ++entry) {
        const QString value = entry.value().toString().trimmed();
        if (!value.isEmpty())
            addField(customColumnLabel(entry.key()), valueLabel(value, {}), false);
    }
    const int emails = int(m_contactEmails.value(contactId).size());
    const int toots = int(m_contactToots.value(contactId).size());
    if (emails || toots) {
        QStringList messages;
        if (emails)
            messages << (emails == 1 ? QStringLiteral("1 email")
                                     : QStringLiteral("%1 emails").arg(emails));
        if (toots)
            messages << (toots == 1 ? QStringLiteral("1 Mastodon conversation")
                                    : QStringLiteral("%1 Mastodon conversations").arg(toots));
        addField(QStringLiteral("Messages"), valueLabel(messages.join(dot), {}), false);
    }
    const qint64 created = qint64(contact.value(QStringLiteral("createdAt")).toDouble());
    if (created > 0) {
        addField(QStringLiteral("Added"),
                 valueLabel(text(contact, "createdBy").isEmpty()
                                ? timestamp(created)
                                : QStringLiteral("%1 by %2").arg(timestamp(created),
                                                                 text(contact, "createdBy")),
                            {}),
                 false);
    }
    const QStringList onLists = contactListNames(contactId);
    if (!onLists.isEmpty())
        addField(QStringLiteral("Lists"), valueLabel(onLists.join(QStringLiteral(", ")), {}), true);
    if (!text(contact, "notes").isEmpty())
        addField(QStringLiteral("Notes"), valueLabel(text(contact, "notes"), {}), true);
    for (int pair = 0; pair < kPairs; ++pair)
        grid->setColumnStretch(pair * 2 + 1, 1);
    layout->addLayout(grid);

    auto *how = new QLabel(reach
        ? QStringLiteral("Picked from the %1 never contacted on this list. The newest Mastodon "
                         "post goes first, then a next step, "
                         "then the highest deal value, the lowest number in a rank column, the "
                         "highest in a score column, an email address over a handle alone, and "
                         "the longest on the list. Won and ended stages are left out.")
              .arg(locale.toString(next.due))
        : QStringLiteral("Picked from the %1 on this list due a follow-up. People who wrote back "
                         "and wait on an answer come first, newest reply first; then people quiet "
                         "for %2 days or more, longest silence first. A next step puts someone "
                         "ahead in their group.")
              .arg(next.due == 1 ? QStringLiteral("one person")
                                 : QStringLiteral("%1 people").arg(locale.toString(next.due)))
              .arg(kNextFollowUpDays));
    how->setObjectName(QStringLiteral("mutedLabel"));
    how->setWordWrap(true);
    layout->addWidget(how);
    layout->addStretch(1);
    return body;
}

void CrmFeature::selectFirstStage()
{
    const QString sheetId = m_sheet->currentData().toString();
    if (sheetId.isEmpty() || sheetId == m_stageSheet)
        return;
    const QStringList stages = stripStages(nullptr);
    if (stages.isEmpty())
        return;
    m_stageSheet = sheetId;
    QString first = stages.first();
    for (const QString &stage : stages) {
        if (stage.compare(QLatin1String("new"), Qt::CaseInsensitive) == 0) {
            first = stage;
            break;
        }
    }
    m_stage->setCurrentIndex(qMax(0, m_stage->findData(first)));
}

QStringList CrmFeature::stripStages(QHash<QString, int> *countsOut) const
{
    // A pipeline tab counts its own flow's stages, zeros included; a plain
    // list counts whichever stages its contacts are at, in pipeline order
    // (or the flow filter's stages when one is picked).
    QString flow = text(activeSheet(), "flow");
    if (flow.isEmpty() && !m_flow->isHidden())
        flow = m_flow->currentData().toString();
    QStringList stages = flowStages(flow);
    QHash<QString, int> counts;
    const QVector<int> rows = m_snapshot.sheetRows.value(m_sheet->currentData().toString());
    for (int index : rows) {
        if (index < 0 || index >= m_snapshot.rows.size())
            continue;
        const ContactRow &row = m_snapshot.rows.at(index);
        if ((!flow.isEmpty() && row.flow != flow) || row.stage.isEmpty())
            continue;
        ++counts[row.stage];
    }
    if (flow.isEmpty()) {
        for (int at = 1; at < m_stage->count(); ++at) {
            const QString stage = m_stage->itemData(at).toString();
            if (counts.contains(stage))
                stages << stage;
        }
        QStringList others;
        for (auto count = counts.cbegin(); count != counts.cend(); ++count) {
            if (!stages.contains(count.key()))
                others << count.key();
        }
        others.sort(Qt::CaseInsensitive);
        stages << others;
    }
    stages.removeAll(QString());
    stages.removeDuplicates();
    if (countsOut)
        *countsOut = counts;
    return stages;
}

// The contact picked from the top search box, once its tab's rows are in.
void CrmFeature::focusContactRow()
{
    if (m_focusContact.isEmpty() || m_filling || m_renderQueued)
        return;
    const int row = rowOfContact(m_focusContact);
    m_focusContact.clear();
    if (row < 0)
        return;
    m_contactsTable->setRowHidden(row, false);
    m_contactsTable->selectRow(row);
    m_contactsTable->scrollToItem(m_contactsTable->item(row, 0),
                                  QAbstractItemView::PositionAtCenter);
    m_contactsTable->setFocus(Qt::OtherFocusReason);
}

void CrmFeature::updateContactField(const QString &id,
                                    const ContactColumnSpec &column,
                                    const QString &value)
{
    if (id.isEmpty() || column.key.isEmpty())
        return;
    // A custom column lives inside the contact's `fields` map; the worker
    // merges one key without touching the others.
    QJsonObject body = column.kind == ContactColumnSpec::CustomField
        ? QJsonObject{{QStringLiteral("fields"), QJsonObject{{column.key, value}}}}
        : QJsonObject{{column.key, value}};
    // A contact moved to another pipeline keeps its stage when that pipeline
    // has it, and otherwise starts at its first stage.
    if (column.kind == ContactColumnSpec::Field && column.key == QLatin1String("flow")) {
        const QStringList stages = flowStages(value);
        if (!stages.isEmpty() && !stages.contains(text(contactById(id), "stage")))
            body.insert(QStringLiteral("stage"), stages.first());
    }
    const QString label = column.label;
    request(QByteArrayLiteral("PATCH"),
            organizationPath(QStringLiteral("/contacts/") + id), body,
            [this, label](bool ok, const QJsonObject &, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("%1 was not saved: %2").arg(label, error), true);
            loadOrganization();
            return;
        }
        setStatus(QStringLiteral("%1 saved.").arg(label));
        loadOrganization();
    });
}

// The options a Flow or Stage cell offers: every pipeline, or the stages of
// the pipeline the row's contact is in. False for any other cell.
bool CrmFeature::contactFieldChoices(int row, int column,
                                     QList<QPair<QString, QString>> *choices) const
{
    if (column < 0 || column >= m_columns.size() || row < 0 ||
        row >= m_tableRows.size() || m_tableRows.at(row) >= m_fillSource.size())
        return false;
    const ContactColumnSpec &spec = m_columns.at(column);
    if (spec.kind != ContactColumnSpec::Field)
        return false;
    if (spec.key == QLatin1String("flow")) {
        for (const QJsonValue &value : m_settings.value(QStringLiteral("flows")).toArray()) {
            const QJsonObject flow = value.toObject();
            const QString id = text(flow, "id");
            if (!id.isEmpty())
                choices->append({text(flow, "name").isEmpty() ? id : text(flow, "name"), id});
        }
    } else if (spec.key == QLatin1String("stage")) {
        for (const QString &stage : flowStages(m_fillSource.at(m_tableRows.at(row)).flow))
            choices->append({stage, stage});
    }
    return !choices->isEmpty();
}

// A click on the Linked emails or Linked toots cell: one message opens straight
// where it came from; several list newest first, each with when it was sent.
void CrmFeature::showLinkedEmails(const QString &contactId, bool toots,
                                  const QPoint &globalPosition)
{
    const QList<QJsonObject> emails =
        (toots ? m_contactToots : m_contactEmails).value(contactId);
    if (emails.isEmpty())
        return;
    const QString noun = toots ? QStringLiteral("toot") : QStringLiteral("email");
    const auto open = [this, noun](const QJsonObject &email) {
        const QUrl url(text(email, "providerUrl"));
        if (!url.isValid() || url.scheme() != QLatin1String("https")) {
            setStatus(QStringLiteral("This %1 has no valid HTTPS provider link.").arg(noun), true);
            return;
        }
        QDesktopServices::openUrl(url);
    };
    if (emails.size() == 1) {
        open(emails.first());
        return;
    }
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    menu.addSection(QStringLiteral("%1 linked %2s").arg(emails.size()).arg(noun));
    for (const QJsonObject &email : emails) {
        QAction *action = menu.addAction(linkedEmailLine(email), this,
                                         [open, email] { open(email); });
        const bool linked =
            QUrl(text(email, "providerUrl")).scheme() == QLatin1String("https");
        action->setEnabled(linked);
        action->setToolTip(linked ? text(email, toots ? "providerUrl" : "to")
                                  : QStringLiteral("No provider link for this %1").arg(noun));
    }
    menu.exec(globalPosition);
}

void CrmFeature::showColumnMenu(const QPoint &headerPosition)
{
    QHeaderView *header = m_contactsTable->horizontalHeader();
    QMenu menu(this);
    menu.addSection(QString::fromUtf8("Columns in the \xE2\x80\x9C%1\xE2\x80\x9D view")
                        .arg(m_tableView->currentText()));
    const int clicked = header->logicalIndexAt(headerPosition);
    const auto hideable = [this](int column) {
        return column >= 0 && column < m_columns.size() &&
               m_columns.at(column).kind != ContactColumnSpec::Actions &&
               m_columns.at(column).kind != ContactColumnSpec::AddColumn;
    };
    QStringList hidden;
    int shown = 0;
    for (int column = 0; column < m_columns.size(); ++column) {
        if (!hideable(column))
            continue;
        if (m_contactsTable->isColumnHidden(column))
            hidden << m_columns.at(column).id;
        else
            ++shown;
    }
    // The last visible field stays: a table with no heading left has nothing
    // to right-click to bring the others back.
    bool clickedActions = false;
    if (hideable(clicked) && !m_contactsTable->isColumnHidden(clicked) && shown > 1) {
        const QString id = m_columns.at(clicked).id;
        menu.addAction(QString::fromUtf8("Hide \xE2\x80\x9C%1\xE2\x80\x9D")
                           .arg(m_columns.at(clicked).label),
                       this, [this, id] { setColumnsHidden({id}, true); });
        clickedActions = true;
    }
    if (clicked >= 0 && clicked < m_columns.size() &&
        m_columns.at(clicked).kind == ContactColumnSpec::CustomField) {
        const QString key = m_columns.at(clicked).key;
        menu.addAction(QString::fromUtf8("Delete \xE2\x80\x9C%1\xE2\x80\x9D\xE2\x80\xA6")
                           .arg(m_columns.at(clicked).label),
                       this, [this, key] { deleteCustomColumn(key); });
        clickedActions = true;
    }
    if (clicked >= 0 && clicked < m_columns.size() &&
        m_columns.at(clicked).kind == ContactColumnSpec::Field &&
        !kKeptColumns.contains(m_columns.at(clicked).key)) {
        const QString key = m_columns.at(clicked).key;
        QAction *remove = menu.addAction(
            QString::fromUtf8("Delete \xE2\x80\x9C%1\xE2\x80\x9D\xE2\x80\xA6")
                .arg(m_columns.at(clicked).label),
            this, [this, key] { deleteBuiltinColumn(key); });
        remove->setEnabled(m_canManageSettings);
        if (!m_canManageSettings)
            remove->setToolTip(QStringLiteral(
                "Only organization owners and admins can delete a built-in column"));
        clickedActions = true;
    }
    if (clickedActions)
        menu.addSeparator();
    for (int column = 0; column < m_columns.size(); ++column) {
        if (!hideable(column))
            continue;
        const ContactColumnSpec &spec = m_columns.at(column);
        QAction *action = menu.addAction(spec.label);
        action->setCheckable(true);
        const bool visible = !m_contactsTable->isColumnHidden(column);
        action->setChecked(visible);
        action->setEnabled(!visible || shown > 1);
        const QString id = spec.id;
        connect(action, &QAction::triggered, this,
                [this, id](bool checked) { setColumnsHidden({id}, !checked); });
    }
    menu.addSeparator();
    QAction *showAll = menu.addAction(QStringLiteral("Show all columns"), this,
                                      [this, hidden] {
        if (builtinTableView()) {
            m_tableViewDraft.clear();
            selectTableView(QString::fromLatin1(kAllTableView));
        } else {
            setColumnsHidden(hidden, false);
        }
    });
    showAll->setEnabled(!hidden.isEmpty() && !allColumnsView());
    menu.addAction(QString::fromUtf8("Save as new view\xE2\x80\xA6"), this,
                   [this] { createTableView(shownViewFields()); });
    menu.addAction(QString::fromUtf8("Add column\xE2\x80\xA6"), this,
                   [this] { addCustomColumn(); });
    const QStringList removed = removedColumns();
    if (!removed.isEmpty()) {
        QMenu *restore = menu.addMenu(QStringLiteral("Restore deleted column"));
        restore->setEnabled(m_canManageSettings);
        for (const ContactColumn &column : contactColumns()) {
            const QString field = QString::fromLatin1(column.field);
            if (removed.contains(field))
                restore->addAction(QString::fromLatin1(column.label), this,
                                   [this, field] { restoreBuiltinColumn(field); });
        }
    }
    menu.setToolTipsVisible(true);
    menu.exec(header->mapToGlobal(headerPosition));
}

void CrmFeature::addCustomColumn()
{
    bool accepted = false;
    const QString label = QInputDialog::getText(
        this, QStringLiteral("Add column"),
        QStringLiteral("Column name"), QLineEdit::Normal, QString(),
        &accepted).trimmed();
    if (!accepted || label.isEmpty())
        return;
    const QString key = customColumnKey(label);
    if (key.isEmpty()) {
        setStatus(QStringLiteral("Use letters or digits in the column name."), true);
        return;
    }
    for (const ContactColumn &column : contactColumns()) {
        if (key == QLatin1String(column.field) && removedColumns().contains(key)) {
            restoreBuiltinColumn(key);
            return;
        }
        if (key == QLatin1String(column.field)) {
            showColumn(key);
            setStatus(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D is a built-in "
                                        "column; it is shown again.")
                          .arg(QString::fromLatin1(column.label)));
            return;
        }
    }
    if (!m_customColumns.contains(key))
        m_customColumns << key;
    rebuildContactColumns();
    // On a list the new column joins its saved layout, shown.
    showColumn(QLatin1String(kCustomColumnPrefix) + key);
    refreshContactSnapshot();
    const int index = columnIndex(ContactColumnSpec::CustomField, key);
    if (index >= 0)
        m_contactsTable->horizontalScrollBar()->setValue(
            m_contactsTable->horizontalHeader()->sectionPosition(index));
    setStatus(QString::fromUtf8(
        "Column \xE2\x80\x9C%1\xE2\x80\x9D added. Click into a cell to fill it; "
        "each value is saved on its contact and shared with the organization.")
                  .arg(customColumnLabel(key)));
}

// Deletes a custom column for the whole organization: the worker erases its
// value from every contact and drops it from every list's saved layout.
void CrmFeature::deleteCustomColumn(const QString &key)
{
    if (key.isEmpty())
        return;
    const QString label = customColumnLabel(key);
    int filled = 0;
    for (const QJsonValue &value : std::as_const(m_contacts)) {
        if (value.toObject().value(QStringLiteral("fields")).toObject().contains(key))
            ++filled;
    }
    if (QMessageBox::warning(
            this, QStringLiteral("Delete column"),
            QString::fromUtf8(
                "Delete the \xE2\x80\x9C%1\xE2\x80\x9D column?\n\nIts value is "
                "erased from %2 for the whole organization and the column is "
                "removed from every list. This cannot be undone.")
                .arg(label, filled == 1 ? QStringLiteral("1 contact")
                                        : QStringLiteral("%1 contacts").arg(filled)),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    setStatus(QString::fromUtf8("Deleting column \xE2\x80\x9C%1\xE2\x80\x9D\xE2\x80\xA6")
                  .arg(label));
    request(QByteArrayLiteral("POST"),
            organizationPath(QStringLiteral("/contacts/delete-column")),
            QJsonObject{{QStringLiteral("field"), key}},
            [this, key, label](bool ok, const QJsonObject &, const QString &error) {
        if (!ok) {
            setStatus(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D was not deleted: %2")
                          .arg(label, error), true);
            return;
        }
        // Drop it here at once; the reload then brings the lists' layouts.
        m_customColumns.removeAll(key);
        for (qsizetype at = 0; at < m_contacts.size(); ++at) {
            QJsonObject contact = m_contacts.at(at).toObject();
            QJsonObject fields = contact.value(QStringLiteral("fields")).toObject();
            if (!fields.contains(key))
                continue;
            fields.remove(key);
            contact.insert(QStringLiteral("fields"), fields);
            m_contacts.replace(at, contact);
        }
        rebuildContactColumns();
        refreshContactSnapshot();
        setStatus(QString::fromUtf8("Column \xE2\x80\x9C%1\xE2\x80\x9D deleted.").arg(label));
        loadOrganization();
    });
}

QStringList CrmFeature::removedColumns() const
{
    QStringList removed;
    for (const QJsonValue &value : m_settings.value(QStringLiteral("removedColumns")).toArray())
        removed << value.toString();
    return removed;
}

void CrmFeature::deleteBuiltinColumn(const QString &field)
{
    QString label = field;
    for (const ContactColumn &column : contactColumns()) {
        if (field == QLatin1String(column.field))
            label = QString::fromLatin1(column.label);
    }
    if (kKeptColumns.contains(field) || label == field)
        return;
    int filled = 0;
    for (const QJsonValue &value : std::as_const(m_contacts)) {
        const QJsonValue stored = value.toObject().value(field);
        if ((stored.isString() && !stored.toString().trimmed().isEmpty()) ||
            (stored.isArray() && !stored.toArray().isEmpty()) ||
            (stored.isDouble() && stored.toDouble() > 0))
            ++filled;
    }
    if (QMessageBox::warning(
            this, QStringLiteral("Delete column"),
            QString::fromUtf8(
                "Delete the \xE2\x80\x9C%1\xE2\x80\x9D column?\n\nIts value is "
                "erased from %2 for the whole organization and the column is "
                "removed from every list. It can be added back later, empty. "
                "This cannot be undone.")
                .arg(label, filled == 1 ? QStringLiteral("1 contact")
                                        : QStringLiteral("%1 contacts").arg(filled)),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    setStatus(QString::fromUtf8("Deleting column \xE2\x80\x9C%1\xE2\x80\x9D\xE2\x80\xA6")
                  .arg(label));
    request(QByteArrayLiteral("POST"),
            organizationPath(QStringLiteral("/contacts/delete-column")),
            QJsonObject{{QStringLiteral("field"), field}},
            [this, label](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D was not deleted: %2")
                          .arg(label, error), true);
            return;
        }
        m_settings.insert(QStringLiteral("removedColumns"),
                          payload.value(QStringLiteral("removedColumns")).toArray());
        rebuildContactColumns();
        refreshContactSnapshot();
        setStatus(QString::fromUtf8("Column \xE2\x80\x9C%1\xE2\x80\x9D deleted.").arg(label));
        loadOrganization();
    });
}

void CrmFeature::restoreBuiltinColumn(const QString &field)
{
    if (!m_canManageSettings) {
        setStatus(QStringLiteral("Only organization owners and admins can restore a column."), true);
        return;
    }
    QStringList removed = removedColumns();
    removed.removeAll(field);
    request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")),
            QJsonObject{{QStringLiteral("removedColumns"), QJsonArray::fromStringList(removed)}},
            [this, field](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("The column was not restored: %1").arg(error), true);
            return;
        }
        m_settings = payload.value(QStringLiteral("settings")).toObject();
        rebuildContactColumns();
        showColumn(field);
        refreshContactSnapshot();
        setStatus(QStringLiteral("Column restored."));
    });
}

void CrmFeature::renderActivity()
{
    // One log for both kinds of record: email sends from the CRM (or linked
    // from a provider) and messages imported from Zoho Mail, newest first.
    struct ActivityRow {
        qint64 at;
        QString id;
        QStringList values;
        QUrl url;
        QString contactTip;
    };
    QList<ActivityRow> rows;
    for (const QJsonValue &value : m_emails) {
        const QJsonObject email = value.toObject();
        const qint64 at = qint64(email.value(QStringLiteral("createdAt")).toDouble());
        const QString counterpart =
            text(email, "direction") == QLatin1String("inbound") ||
                    text(email, "to").isEmpty()
                ? (text(email, "fromEmail").isEmpty() ? text(email, "to")
                                                      : text(email, "fromEmail"))
                : text(email, "to");
        const QJsonObject contact = contactById(text(email, "contactId"));
        const QString name = text(contact, "name");
        const QUrl url(text(email, "providerUrl"));
        rows.append({at, text(email, "id"), {
            QStringLiteral("Email send"), text(email, "subject"),
            name.isEmpty() ? counterpart : name,
            text(email, "status"), text(email, "transport"),
            text(email, "externalId"), text(email, "sender"), timestamp(at)},
            url.scheme() == QLatin1String("https") ? url : QUrl(),
            name.isEmpty() || counterpart.isEmpty()
                ? QString() : QStringLiteral("%1 <%2>").arg(name, counterpart)});
    }
    for (const QJsonValue &value : m_mail) {
        const QJsonObject message = value.toObject();
        const qint64 at =
            qint64(message.value(QStringLiteral("receivedAt")).toDouble());
        const QString from = text(message, "sender").isEmpty()
            ? text(message, "from") : text(message, "sender");
        rows.append({at, text(message, "id"), {
            QStringLiteral("Zoho mail"), text(message, "subject"), from,
            text(message, "box"), QStringLiteral("zoho"),
            text(message, "providerMessageId"), text(message, "sourceFolder"),
            timestamp(at)}, QUrl(), QString()});
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const ActivityRow &a, const ActivityRow &b) {
                         return a.at > b.at;
                     });
    const QString selectedId = text(selectedEmail(), "id");
    const QColor linkColor = m_emailsTable->palette().color(QPalette::Link);
    m_emailsTable->setRowCount(0);
    int selectedRow = -1;
    for (const ActivityRow &entry : rows) {
        const int row = m_emailsTable->rowCount();
        m_emailsTable->insertRow(row);
        for (int column = 0; column < entry.values.size(); ++column) {
            auto *item = new QTableWidgetItem(entry.values.at(column));
            item->setToolTip(entry.values.at(column));
            if (column == 0)
                item->setData(Qt::UserRole, entry.id);
            if (column == kActivityContactColumn && !entry.contactTip.isEmpty())
                item->setToolTip(entry.contactTip);
            if (column == kActivityIdColumn && entry.url.isValid() &&
                !entry.values.at(column).isEmpty()) {
                QFont font = item->font();
                font.setUnderline(true);
                item->setFont(font);
                item->setForeground(linkColor);
                item->setToolTip(QStringLiteral("Click to open it: %1")
                                     .arg(entry.url.toString()));
            }
            m_emailsTable->setItem(row, column, item);
        }
        if (entry.id == selectedId)
            selectedRow = row;
    }
    if (selectedRow >= 0)
        m_emailsTable->selectRow(selectedRow);
}

void CrmFeature::renderMail()
{
    m_mailTable->setRowCount(0);
    QHash<QString, int> groupSizes;
    for (const QJsonValue &value : m_mail)
        ++groupSizes[text(value.toObject(), "groupKey")];
    QSet<QString> renderedGroups;
    for (const QJsonValue &value : m_mail) {
        const QJsonObject message = value.toObject();
        const QString group = text(message, "groupKey");
        if (renderedGroups.contains(group))
            continue;
        renderedGroups.insert(group);
        const int count = groupSizes.value(group);
        const int row = m_mailTable->rowCount();
        m_mailTable->insertRow(row);
        const QStringList values{
            text(message, "subject"),
            text(message, "sender").isEmpty() ? text(message, "from")
                                                : text(message, "sender"),
            text(message, "box"),
            group.left(12),
            QString::number(count),
            text(message, "sourceFolder"),
            timestamp(qint64(message.value(QStringLiteral("receivedAt")).toDouble())),
        };
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setToolTip(values.at(column));
            if (column == 0)
                item->setData(Qt::UserRole, text(message, "id"));
            m_mailTable->setItem(row, column, item);
        }
    }
}

void CrmFeature::renderSheets()
{
    const QTableWidgetItem *current = m_sheetsTable->currentRow() >= 0
        ? m_sheetsTable->item(m_sheetsTable->currentRow(), 0) : nullptr;
    const QString selectedId = current ? current->data(Qt::UserRole).toString()
                                       : QString();
    m_sheetsTable->setRowCount(0);
    QHash<QString, QString> flowNames;
    for (const QJsonValue &value : m_settings.value(QStringLiteral("flows")).toArray())
        flowNames.insert(text(value.toObject(), "id"), text(value.toObject(), "name"));
    const QIcon share = shareIcon(palette().color(QPalette::WindowText));
    const QIcon listIcon = plainListIcon(palette().color(QPalette::WindowText));
    for (const QJsonValue &value : m_views) {
        const QJsonObject view = value.toObject();
        const QString id = text(view, "id");
        int shown = 0;
        const QJsonArray columns = view.value(QStringLiteral("columns")).toArray();
        for (const QJsonValue &column : columns) {
            if (!column.toObject().value(QStringLiteral("hidden")).toBool())
                ++shown;
        }
        // Who the list reaches besides its owner, as a count; the names are
        // the tooltip, like the hover card on the web tab.
        const int people = int(sheetShareNames(view).size());
        const QString peopleLabel = people == 1 ? QStringLiteral("1 person")
                                                : QStringLiteral("%1 people").arg(people);
        const QString reach = text(view, "visibility") == QLatin1String("private")
            ? (people ? QStringLiteral("Shared with %1").arg(peopleLabel)
                      : QStringLiteral("Only its owner"))
            : QString::fromUtf8("Whole organization \xC2\xB7 %1").arg(peopleLabel);
        const QString flow = text(view, "flow");
        const int row = m_sheetsTable->rowCount();
        m_sheetsTable->insertRow(row);
        const QStringList values{
            flow.isEmpty() ? text(view, "name")
                           : QStringLiteral("%1 (pipeline)").arg(text(view, "name")),
            flow.isEmpty() ? text(view, "fileName")
                           : QStringLiteral("Every contact in %1")
                                 .arg(flowNames.value(flow, flow)),
            QString::number(sheetContactCount(view)),
            QStringLiteral("%1 of %2").arg(shown).arg(columns.size()),
            reach, text(view, "owner"),
            timestamp(qint64(view.value(QStringLiteral("createdAt")).toDouble())),
        };
        for (int column = 0; column < values.size(); ++column) {
            auto *item = new QTableWidgetItem(values.at(column));
            item->setToolTip(column == 4 ? sheetShareSummary(view) : values.at(column));
            if (column == 4)
                item->setIcon(share);
            if (column == 0) {
                item->setData(Qt::UserRole, id);
                item->setIcon(flow.isEmpty() ? listIcon : flowIcon(flow));
            }
            m_sheetsTable->setItem(row, column, item);
        }
        if (id == selectedId)
            m_sheetsTable->selectRow(row);
    }
}

void CrmFeature::renderSheetPicker()
{
    const QString wanted = m_pendingSheet.isEmpty() ? m_sheet->currentData().toString()
                                                    : m_pendingSheet;
    int index = 0;
    {
        const QSignalBlocker blocker(m_sheet);
        m_sheet->clear();
        for (const QJsonObject &sheet : tabSheets()) {
            QString label = QStringLiteral("%1 (%2)").arg(text(sheet, "name"))
                                .arg(sheetContactCount(sheet));
            if (!text(sheet, "flow").isEmpty())
                label += QString::fromUtf8(" \xC2\xB7 pipeline");
            if (text(sheet, "flow").isEmpty())
                m_sheet->addItem(label, text(sheet, "id"));
            else
                m_sheet->addItem(flowIcon(text(sheet, "flow")), label, text(sheet, "id"));
            m_sheet->setItemData(m_sheet->count() - 1, sheetShareSummary(sheet),
                                 Qt::ToolTipRole);
        }
        index = wanted.isEmpty() ? 0 : m_sheet->findData(wanted);
        m_sheet->setCurrentIndex(qMax(0, index));
    }
    // A new import or pipeline opens its tab once a load actually carries its
    // list.
    renderSheetTabs(index >= 0 ? m_pendingSheet : QString());
    if (index >= 0)
        m_pendingSheet.clear();
}

QString CrmFeature::sheetTabId(int index) const
{
    const QWidget *page = m_tabs->widget(index);
    return page ? page->property(kSheetIdProperty).toString() : QString();
}

int CrmFeature::sheetTabIndex(const QString &id) const
{
    for (int index = 0; !id.isEmpty() && index < m_tabs->count(); ++index) {
        if (sheetTabId(index) == id)
            return index;
    }
    return -1;
}

void CrmFeature::renderSheetTabs(const QString &select)
{
    const QString previous = sheetTabId(m_tabs->currentIndex());
    const QList<QJsonObject> sheets = tabSheets();
    {
        const QSignalBlocker blocker(m_tabs);
        QStringList ids;
        for (const QJsonObject &sheet : sheets)
            ids << text(sheet, "id");
        for (int index = m_tabs->count() - 1; index >= 0; --index) {
            const QString id = sheetTabId(index);
            if (id.isEmpty() || ids.contains(id))
                continue;
            // Deleted, here or by another member; its page goes, the table stays.
            QWidget *page = m_tabs->widget(index);
            if (m_contactsBody->parentWidget() == page)
                m_contactsPage->layout()->addWidget(m_contactsBody);
            m_tabs->removeTab(index);
            page->deleteLater();
        }
        const QIcon listIcon = plainListIcon(palette().color(QPalette::WindowText));
        QTabBar *bar = m_tabs->tabBar();
        for (int position = 0; position < int(sheets.size()); ++position) {
            const QJsonObject &sheet = sheets.at(position);
            const QString id = text(sheet, "id");
            int index = sheetTabIndex(id);
            if (index < 0) {
                auto *page = new QWidget;
                page->setProperty(kSheetIdProperty, id);
                auto *pageLayout = new QVBoxLayout(page);
                pageLayout->setContentsMargins(0, 0, 0, 0);
                index = m_tabs->insertTab(position, page, QString());
            }
            if (index != position) {
                bar->moveTab(index, position);
                index = position;
            }
            const QString summary = sheetShareSummary(sheet);
            m_tabs->setTabText(index, QStringLiteral("%1 (%2)").arg(text(sheet, "name"))
                                          .arg(sheetContactCount(sheet)));
            // The tab says what kind of list it is: its pipeline's type, or
            // ruled lines for a plain list.
            m_tabs->setTabIcon(index, !text(sheet, "flow").isEmpty()
                ? flowIcon(text(sheet, "flow"))
                : id == QLatin1String(kNoPipelineTabId) ? QIcon() : listIcon);
            m_tabs->setTabToolTip(index, id == QLatin1String(kNoPipelineTabId)
                ? QStringLiteral("Contacts whose pipeline was deleted. Right-click "
                                 "them to move them into another one.")
                : text(sheet, "flow").isEmpty() ? summary
                : QStringLiteral("%1 pipeline\n").arg(QLatin1String(
                      flowIconKind(flowIconId(text(sheet, "flow"))).label)) + summary);
        }
    }
    // Land on the tab asked for, else stay on the open one. The first contact
    // tab opens when the tabs first come in, and when the open one is deleted.
    int target = m_tabs->currentIndex();
    if (sheetTabIndex(select) >= 0)
        target = sheetTabIndex(select);
    else if (!previous.isEmpty())
        target = qMax(0, sheetTabIndex(previous));
    else if (!m_sheetTabsSeeded && !sheets.isEmpty())
        target = 0;
    m_sheetTabsSeeded = m_sheetTabsSeeded || !sheets.isEmpty();
    m_tabs->setCurrentIndex(target);
    syncSheetTab();
    renderSheetList();
}

void CrmFeature::syncSheetTab()
{
    const int index = m_tabs->currentIndex();
    const QString id = sheetTabId(index);
    if (id.isEmpty())
        return;
    QWidget *page = m_tabs->widget(index);
    if (m_contactsBody->parentWidget() != page) {
        page->layout()->addWidget(m_contactsBody);
        m_contactsBody->show();
    }
    // Selecting the tab's list or pipeline applies its fields and puts its
    // contacts in the table.
    const int sheetIndex = m_sheet->findData(id);
    if (sheetIndex >= 0)
        m_sheet->setCurrentIndex(sheetIndex);
}

void CrmFeature::changeEvent(QEvent *event)
{
    forkmesh::market::NativePackageWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        tintToolbar();
        renderSheetList();
    }
}

void CrmFeature::showEvent(QShowEvent *event)
{
    forkmesh::market::NativePackageWidget::showEvent(event);
    noteSheetViewed();
}

void CrmFeature::tintToolbar()
{
    const QColor ink = palette().color(QPalette::WindowText);
    for (const auto &[button, icon] : std::as_const(m_toolIcons)) {
        if (button)
            button->setIcon(octicon(icon, ink, button->iconSize().width()));
    }
}

void CrmFeature::renderSheetList()
{
    if (!m_sheetList || !m_tabs)
        return;
    const int scroll = m_sheetList->verticalScrollBar()->value();
    const QSignalBlocker blocker(m_sheetList);
    m_sheetList->clear();
    const QColor ink = palette().color(QPalette::WindowText);
    const QIcon listIcon = plainListIcon(ink);
    const QIcon share = shareIcon(ink);
    const qreal scale = devicePixelRatioF();
    const QLocale locale;
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const QColor base = palette().color(QPalette::Base);
    const QColor accent = palette().color(QPalette::Highlight);
    QColor edge = ink;
    edge.setAlpha(45);
    QColor fill = ink;
    fill.setAlpha(8);
    QColor tint = accent;
    tint.setAlpha(40);
    const QString css = QStringLiteral(
        "QListWidget#crmSheetList::item,"
        "QListWidget#crmSheetList::item:hover,"
        "QListWidget#crmSheetList::item:selected { border: none; padding: 0; background: transparent; }"
        "QWidget#crmSheetCell { background: transparent; }"
        "QFrame#crmSheetRow { background: %1; border: 1px solid %2; border-radius: 8px; padding: 1px; }"
        "QFrame#crmSheetRow:hover { border-color: %3; }"
        "QFrame#crmSheetRow[current=\"true\"] { background: %4; border: 2px solid %3; padding: 0; }"
        "QFrame#crmSheetRow QLabel, QFrame#crmSheetRow QToolButton { background: transparent; }"
        "QFrame#crmSheetRow QToolButton:hover { background: %2; border-radius: 4px; }")
        .arg(fill.name(QColor::HexArgb), edge.name(QColor::HexArgb),
             accent.name(QColor::HexArgb), tint.name(QColor::HexArgb));
    if (m_sheetList->styleSheet() != css)
        m_sheetList->setStyleSheet(css);
    bool more = false;
    for (int index = 0; index < m_tabs->count(); ++index) {
        QWidget *page = m_tabs->widget(index);
        const QString id = sheetTabId(index);
        if (id.isEmpty() && !more) {
            more = true;
            auto *heading = new QListWidgetItem(QStringLiteral("More"), m_sheetList);
            heading->setFlags(Qt::NoItemFlags);
            QFont font = heading->font();
            font.setPointSizeF(font.pointSizeF() * 0.85);
            font.setWeight(QFont::DemiBold);
            heading->setFont(font);
        }
        auto *item = new QListWidgetItem(m_sheetList);
        item->setData(Qt::UserRole, QVariant::fromValue(static_cast<QObject *>(page)));
        auto *row = new QWidget;
        row->setObjectName(QStringLiteral("crmSheetCell"));
        auto *cellLayout = new QVBoxLayout(row);
        cellLayout->setContentsMargins(4, 3, 4, 3);
        auto *card = new QFrame;
        card->setObjectName(QStringLiteral("crmSheetRow"));
        cellLayout->addWidget(card);
        if (id.isEmpty()) {
            item->setData(Qt::AccessibleTextRole, m_tabs->tabText(index));
            auto *pageLayout = new QHBoxLayout(card);
            pageLayout->setContentsMargins(8, 5, 8, 5);
            pageLayout->setSpacing(8);
            auto *pageIcon = new QLabel;
            pageIcon->setPixmap(octicon(page->property("crmPageIcon").toString(), ink, 16)
                                    .pixmap(QSize(16, 16), scale));
            pageLayout->addWidget(pageIcon);
            auto *pageName = new QLabel(m_tabs->tabText(index));
            pageName->setTextFormat(Qt::PlainText);
            pageName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            pageLayout->addWidget(pageName, 1);
            item->setSizeHint(QSize(0, row->sizeHint().height()));
            m_sheetList->setItemWidget(item, row);
            continue;
        }
        const QJsonObject sheet = sheetById(id);
        const QString flow = text(sheet, "flow");
        const QVector<int> rows = m_snapshot.sheetRows.value(id);
        item->setData(Qt::UserRole + 1, id);
        item->setToolTip(m_tabs->tabToolTip(index));
        item->setData(Qt::AccessibleTextRole, m_tabs->tabText(index));

        auto *rowLayout = new QVBoxLayout(card);
        rowLayout->setContentsMargins(8, 6, 8, 6);
        rowLayout->setSpacing(4);
        auto *top = new QHBoxLayout;
        top->setSpacing(6);
        auto *kind = new QLabel;
        kind->setPixmap((!flow.isEmpty() ? flowIcon(flow)
                         : id == QLatin1String(kNoPipelineTabId) ? octicon(QStringLiteral("circle-slash"), ink, 16)
                                                                 : listIcon)
                            .pixmap(QSize(16, 16), scale));
        top->addWidget(kind);
        auto *name = new QLabel(text(sheet, "name"));
        name->setTextFormat(Qt::PlainText);
        QFont nameFont = name->font();
        nameFont.setWeight(QFont::DemiBold);
        name->setFont(nameFont);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        top->addWidget(name, 1);
        auto *count = new QLabel(locale.toString(qsizetype(rows.size())));
        count->setObjectName(QStringLiteral("mutedLabel"));
        top->addWidget(count);
        if (id != QLatin1String(kNoPipelineTabId)) {
            const QStringList names = sheetShareNames(sheet);
            QStringList access;
            if (!text(sheet, "owner").isEmpty())
                access << text(sheet, "owner");
            access += names;
            auto *badge = new QToolButton;
            badge->setObjectName(QStringLiteral("crmShareBadge"));
            badge->setAutoRaise(true);
            badge->setFocusPolicy(Qt::NoFocus);
            badge->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            badge->setCursor(Qt::PointingHandCursor);
            const int faces = int(qMin<qsizetype>(access.size(), kAccessAvatars));
            if (faces > 0) {
                const int step = kAccessAvatarSize - 4;
                const QSize stripSize(kAccessAvatarSize + step * (faces - 1) + 2,
                                      kAccessAvatarSize + 2);
                QPixmap strip(stripSize * scale);
                strip.setDevicePixelRatio(scale);
                strip.fill(Qt::transparent);
                QPainter painter(&strip);
                painter.setRenderHint(QPainter::Antialiasing);
                for (int at = faces - 1; at >= 0; --at) {
                    const QJsonObject person{{QStringLiteral("name"), access.at(at)}};
                    const QRectF circle(1 + at * step, 1, kAccessAvatarSize, kAccessAvatarSize);
                    painter.drawPixmap(circle.topLeft(),
                                       initialsPixmap(contactInitials(person).left(1).toUpper(),
                                                      contactAvatarTone(person), ink,
                                                      kAccessAvatarSize, scale));
                    painter.setPen(QPen(base, 1.5));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawEllipse(circle);
                }
                painter.end();
                badge->setIconSize(stripSize);
                badge->setIcon(QIcon(strip));
                if (access.size() > faces)
                    badge->setText(QStringLiteral("+%1").arg(access.size() - faces));
                else
                    badge->setToolButtonStyle(Qt::ToolButtonIconOnly);
            } else {
                badge->setIconSize(QSize(14, 14));
                badge->setIcon(share);
                badge->setText(QString::number(names.size()));
            }
            badge->setToolTip(sheetShareSummary(sheet));
            badge->setAccessibleName(access.size() == 1
                ? QStringLiteral("1 person can open this list")
                : QStringLiteral("%1 people can open this list").arg(access.size()));
            connect(badge, &QToolButton::clicked, this, [this, badge, id] {
                showSheetTabMenu(id, badge->mapToGlobal(QPoint(0, badge->height())));
            });
            top->addWidget(badge);
        }
        rowLayout->addLayout(top);

        QVector<int> faces = rows;
        std::stable_sort(faces.begin(), faces.end(), [this](int a, int b) {
            const ContactRow &left = m_snapshot.rows.at(a);
            const ContactRow &right = m_snapshot.rows.at(b);
            const bool leftPicture = m_avatarImages.contains(avatarSource(left));
            const bool rightPicture = m_avatarImages.contains(avatarSource(right));
            if (leftPicture != rightPicture)
                return leftPicture;
            return left.createdAt > right.createdAt;
        });
        const int shown = int(qMin<qsizetype>(faces.size(), kListAvatars));
        if (shown > 0) {
            const int step = kListAvatarSize - 7;
            const int width = kListAvatarSize + step * (shown - 1);
            QPixmap strip(QSize(width + 2, kListAvatarSize + 2) * scale);
            strip.setDevicePixelRatio(scale);
            strip.fill(Qt::transparent);
            QPainter painter(&strip);
            painter.setRenderHint(QPainter::Antialiasing);
            QStringList people;
            for (int at = 0; at < shown; ++at) {
                const ContactRow &contact = m_snapshot.rows.at(faces.at(at));
                people << contact.name;
                const QRectF circle(1 + at * step, 1, kListAvatarSize, kListAvatarSize);
                painter.drawPixmap(circle.topLeft(),
                                   contactPicture(contact, kListAvatarSize)
                                       .pixmap(QSize(kListAvatarSize, kListAvatarSize), scale));
                painter.setPen(QPen(palette().color(QPalette::Base), 1.5));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(circle);
            }
            painter.end();
            auto *avatars = new QHBoxLayout;
            avatars->setSpacing(6);
            auto *faceLabel = new QLabel;
            faceLabel->setPixmap(strip);
            faceLabel->setToolTip(people.join(QLatin1Char('\n')));
            avatars->addWidget(faceLabel);
            if (rows.size() > shown) {
                auto *rest = new QLabel(QStringLiteral("+%1").arg(locale.toString(
                    qsizetype(rows.size() - shown))));
                rest->setObjectName(QStringLiteral("mutedLabel"));
                avatars->addWidget(rest);
            }
            avatars->addStretch(1);
            rowLayout->addLayout(avatars);
        }

        const QStringList stages = flowStages(flow);
        if (!stages.isEmpty()) {
            QHash<QString, int> counts;
            for (int at : rows)
                ++counts[m_snapshot.rows.at(at).stage];
            QList<ListStageBar::Segment> segments;
            QStringList parts;
            for (int at = 0; at < stages.size(); ++at) {
                const QString &stage = stages.at(at);
                segments.append({CrmStageStrip::stageColor(stage, at), counts.value(stage)});
                parts << QStringLiteral("%1 %2").arg(CrmStageStrip::stageLabel(stage),
                                                      locale.toString(counts.value(stage)));
            }
            auto *bar = new ListStageBar(segments);
            bar->setToolTip(parts.join(QLatin1Char('\n')));
            rowLayout->addWidget(bar);
            auto *caption = new QLabel(parts.join(dot));
            caption->setObjectName(QStringLiteral("mutedLabel"));
            caption->setTextFormat(Qt::PlainText);
            caption->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            QFont captionFont = caption->font();
            captionFont.setPointSizeF(captionFont.pointSizeF() * 0.85);
            caption->setFont(captionFont);
            caption->setToolTip(parts.join(QLatin1Char('\n')));
            rowLayout->addWidget(caption);
        }
        item->setSizeHint(QSize(0, row->sizeHint().height()));
        m_sheetList->setItemWidget(item, row);
    }
    syncSheetList();
    m_sheetList->verticalScrollBar()->setValue(scroll);
}

void CrmFeature::syncSheetList()
{
    if (!m_sheetList)
        return;
    const QSignalBlocker blocker(m_sheetList);
    QObject *current = m_tabs->currentWidget();
    QListWidgetItem *open = nullptr;
    for (int at = 0; at < m_sheetList->count(); ++at) {
        QListWidgetItem *item = m_sheetList->item(at);
        const bool here = !open && item->data(Qt::UserRole).value<QObject *>() == current;
        if (here)
            open = item;
        QWidget *cell = m_sheetList->itemWidget(item);
        auto *card = cell ? cell->findChild<QFrame *>(QStringLiteral("crmSheetRow")) : nullptr;
        if (!card || card->property("current").toBool() == here)
            continue;
        card->setProperty("current", here);
        card->style()->unpolish(card);
        card->style()->polish(card);
        card->update();
    }
    m_sheetList->setCurrentItem(open);
}

QIcon CrmFeature::contactPicture(const ContactRow &row, int side)
{
    const QString source = avatarSource(row);
    const auto image = m_avatarImages.constFind(source);
    const QString key = source + QLatin1Char('|') + QString::number(side);
    if (image != m_avatarImages.constEnd()) {
        if (const auto cached = m_pictureIcons.constFind(key); cached != m_pictureIcons.constEnd())
            return *cached;
    }
    const QColor ink = palette().color(QPalette::Text);
    QIcon icon;
    for (const qreal scale : {1.0, 2.0}) {
        icon.addPixmap(image != m_avatarImages.constEnd()
                           ? circlePixmap(*image, side, scale)
                           : initialsPixmap(row.initials, row.avatarTone, ink, side, scale));
    }
    if (image != m_avatarImages.constEnd())
        m_pictureIcons.insert(key, icon);
    return icon;
}

QStringList CrmFeature::contactListNames(const QString &contactId) const
{
    const QJsonObject contact = contactById(contactId);
    QStringList names;
    if (contact.isEmpty())
        return names;
    const QHash<QString, QStringList> lists = pipelineLists(m_views);
    for (const QJsonObject &sheet : tabSheets()) {
        if (text(sheet, "id") != QLatin1String(kNoPipelineTabId) &&
            contactInSheet(contact, sheet, lists))
            names << text(sheet, "name");
    }
    return names;
}

void CrmFeature::showPeople()
{
    m_tabs->setCurrentWidget(m_peoplePage);
}

void CrmFeature::renderPeople()
{
    if (!m_peopleGrid || m_tabs->currentWidget() != m_peoplePage)
        return;
    QHash<int, QStringList> lists;
    for (const QJsonObject &sheet : tabSheets()) {
        if (text(sheet, "id") == QLatin1String(kNoPipelineTabId))
            continue;
        for (int index : m_snapshot.sheetRows.value(text(sheet, "id")))
            lists[index] << text(sheet, "name");
    }
    QVector<int> picked;
    int named = 0;
    for (int index = 0; index < m_snapshot.rows.size(); ++index) {
        const ContactRow &row = m_snapshot.rows.at(index);
        if (!row.avatarSources.isEmpty())
            ++named;
        if (!m_avatarImages.contains(avatarSource(row)))
            continue;
        if (!m_query.isEmpty() && !row.search.contains(m_query, Qt::CaseInsensitive))
            continue;
        picked << index;
    }
    const QString sort = m_peopleSort->currentData().toString();
    std::stable_sort(picked.begin(), picked.end(), [this, &sort](int a, int b) {
        const ContactRow &left = m_snapshot.rows.at(a);
        const ContactRow &right = m_snapshot.rows.at(b);
        if (sort == QLatin1String("name"))
            return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
        if (sort == QLatin1String("contacted"))
            return left.lastContactedAt > right.lastContactedAt;
        return left.createdAt > right.createdAt;
    });
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const QString current = m_peopleGrid->currentItem()
        ? m_peopleGrid->currentItem()->data(Qt::UserRole).toString() : QString();
    const int scroll = m_peopleGrid->verticalScrollBar()->value();
    m_peopleGrid->setUpdatesEnabled(false);
    m_peopleGrid->clear();
    for (int index : std::as_const(picked)) {
        const ContactRow &row = m_snapshot.rows.at(index);
        const QString pipeline = row.flow.isEmpty()
            ? QStringLiteral("No pipeline")
            : row.stage.isEmpty() ? flowLabel(row.flow)
                                  : flowLabel(row.flow) + dot + CrmStageStrip::stageLabel(row.stage);
        const QStringList onLists = lists.value(index);
        const QString listLine = onLists.isEmpty() ? QStringLiteral("On no list")
                                                   : onLists.join(QStringLiteral(", "));
        auto *item = new QListWidgetItem(contactPicture(row, kPeopleAvatarSize),
                                         row.name + QLatin1Char('\n') + listLine +
                                             QLatin1Char('\n') + pipeline,
                                         m_peopleGrid);
        item->setData(Qt::UserRole, row.id);
        item->setSizeHint(QSize(184, kPeopleAvatarSize + 72));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setToolTip(QStringLiteral("%1\nLists: %2\nPipeline: %3\nAdded %4")
                             .arg(row.name, listLine, pipeline, timestamp(row.createdAt)));
        if (row.id == current)
            m_peopleGrid->setCurrentItem(item);
    }
    m_peopleGrid->setUpdatesEnabled(true);
    m_peopleGrid->verticalScrollBar()->setValue(scroll);
    const QString loading = m_avatarQueue.isEmpty() && m_avatarInFlight <= 0
        ? QString() : QString::fromUtf8(" Pictures are still loading\xE2\x80\xA6");
    m_peopleNote->setText(QStringLiteral("%1 of %2 contacts with an account or email "
                                         "address show a picture.%3")
                              .arg(picked.size()).arg(named).arg(loading));
}

void CrmFeature::newList()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New list"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(QStringLiteral(
        "A list holds the contacts you put on it. Give it a pipeline and it walks them "
        "through that pipeline's stages; several lists can share one pipeline without "
        "sharing contacts."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    auto *name = lineEdit();
    name->setPlaceholderText(QStringLiteral("Investors, Q4"));
    form->addRow(QStringLiteral("Name"), name);
    auto *flow = new QComboBox;
    flow->addItem(plainListIcon(palette().color(QPalette::WindowText)),
                  QStringLiteral("No pipeline (plain list)"), QString());
    for (const QJsonValue &value : m_settings.value(QStringLiteral("flows")).toArray()) {
        const QJsonObject item = value.toObject();
        flow->addItem(flowIcon(text(item, "id")), text(item, "name"), text(item, "id"));
    }
    const QString openFlow = text(activeSheet(), "flow");
    flow->setCurrentIndex(qMax(0, flow->findData(openFlow)));
    flow->setEnabled(m_canManageSettings);
    if (!m_canManageSettings)
        flow->setToolTip(QStringLiteral(
            "Only organization owners and admins can put a list on a pipeline"));
    form->addRow(QStringLiteral("Pipeline"), flow);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *create = buttons->addButton(QStringLiteral("Create"), QDialogButtonBox::AcceptRole);
    create->setEnabled(false);
    connect(name, &QLineEdit::textChanged, &dialog, [create](const QString &value) {
        create->setEnabled(!value.trimmed().isEmpty());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted || name->text().trimmed().isEmpty())
        return;
    const QString listName = name->text().trimmed().left(60);
    QJsonObject body{{QStringLiteral("name"), listName},
                     {QStringLiteral("visibility"), QStringLiteral("organization")}};
    if (m_canManageSettings && !flow->currentData().toString().isEmpty())
        body.insert(QStringLiteral("flow"), flow->currentData().toString());
    setStatus(QString::fromUtf8("Creating \xE2\x80\x9C%1\xE2\x80\x9D\xE2\x80\xA6").arg(listName));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/views")), body,
            [this, listName](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D was not created: %2")
                          .arg(listName, error), true);
            return;
        }
        m_pendingSheet = text(payload.value(QStringLiteral("view")).toObject(), "id");
        setStatus(QString::fromUtf8("List \xE2\x80\x9C%1\xE2\x80\x9D created.").arg(listName));
        loadOrganization();
    });
}

void CrmFeature::mergeSheetInto(const QString &id, const QString &targetId)
{
    const QJsonObject source = sheetById(id);
    QJsonObject target = sheetById(targetId);
    if (!storedSheet(source) || !storedSheet(target) || id == targetId)
        return;
    const int moving = sheetContactCount(source);
    const QString flowName = flowLabel(text(target, "flow"));
    if (QMessageBox::question(
            this, QStringLiteral("Merge list"),
            QString::fromUtf8(
                "Merge \xE2\x80\x9C%1\xE2\x80\x9D (%2) into \xE2\x80\x9C%3\xE2\x80\x9D?\n\n"
                "Its contacts move onto \xE2\x80\x9C%3\xE2\x80\x9D, every field it shows is shown "
                "there too, and \xE2\x80\x9C%1\xE2\x80\x9D is then deleted.%4")
                .arg(text(source, "name"),
                     moving == 1 ? QStringLiteral("1 contact")
                                 : QStringLiteral("%1 contacts").arg(moving),
                     text(target, "name"),
                     text(target, "flow").isEmpty()
                         ? QString()
                         : QStringLiteral(" They join the %1 pipeline, keeping a stage it "
                                          "shares and starting the rest at its first stage.")
                               .arg(flowName)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    const QString sourceName = text(source, "name");
    const QString targetName = text(target, "name");

    const QString flowId = text(target, "flow");
    const QStringList stages = flowStages(flowId);
    QStringList dropped = pipelineLists(m_views).value(flowId);
    dropped.removeAll(targetId);
    if (!dropped.contains(id))
        dropped << id;
    QList<QPair<QString, QJsonObject>> edits;
    for (int index : m_snapshot.sheetRows.value(id)) {
        const QString contactId = m_snapshot.rows.at(index).id;
        const QJsonObject contact = contactById(contactId);
        if (contact.isEmpty())
            continue;
        QJsonObject body{{QStringLiteral("views"), QJsonArray{targetId}},
                         {QStringLiteral("removeViews"), QJsonArray::fromStringList(dropped)}};
        if (!flowId.isEmpty() && text(contact, "flow") != flowId) {
            body.insert(QStringLiteral("flow"), flowId);
            const QString stage = text(contact, "stage");
            body.insert(QStringLiteral("stage"),
                        stages.contains(stage) || stages.isEmpty() ? stage : stages.first());
        }
        edits.append({contactId, body});
    }

    QJsonArray layoutColumns = target.value(QStringLiteral("columns")).toArray();
    const auto shownIn = [](const QJsonArray &columns, const QString &field) {
        for (const QJsonValue &value : columns) {
            const QJsonObject column = value.toObject();
            if (text(column, "field") == field)
                return !column.value(QStringLiteral("hidden")).toBool();
        }
        return false;
    };
    bool layoutChanged = false;
    bool targetShows = false;
    for (const QJsonValue &value : std::as_const(layoutColumns))
        targetShows = targetShows || !value.toObject().value(QStringLiteral("hidden")).toBool();
    QJsonArray sourceColumns = source.value(QStringLiteral("columns")).toArray();
    if (!targetShows) {
        QJsonArray builtin;
        for (const ContactColumn &column : contactColumns()) {
            builtin.append(QJsonObject{{QStringLiteral("field"), QString::fromLatin1(column.field)},
                                       {QStringLiteral("label"), QString::fromLatin1(column.label)},
                                       {QStringLiteral("custom"), false},
                                       {QStringLiteral("hidden"), false}});
        }
        for (const QJsonValue &value : std::as_const(sourceColumns))
            builtin.append(value);
        sourceColumns = builtin;
    }
    for (const QJsonValue &value : std::as_const(sourceColumns)) {
        const QJsonObject column = value.toObject();
        const QString field = text(column, "field");
        if (field.isEmpty() || column.value(QStringLiteral("hidden")).toBool() ||
            shownIn(layoutColumns, field))
            continue;
        bool found = false;
        for (int at = 0; at < layoutColumns.size(); ++at) {
            QJsonObject stored = layoutColumns.at(at).toObject();
            if (text(stored, "field") != field)
                continue;
            stored.insert(QStringLiteral("hidden"), false);
            layoutColumns.replace(at, stored);
            found = true;
        }
        if (!found) {
            QJsonObject added = column;
            added.insert(QStringLiteral("hidden"), false);
            layoutColumns.append(added);
        }
        layoutChanged = true;
    }

    const auto finish = [this, id, sourceName, targetName, targetId] {
        request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/views/") + id), {},
                [this, sourceName, targetName, targetId](bool ok, const QJsonObject &,
                                                         const QString &error) {
            if (!ok)
                setStatus(QString::fromUtf8("The contacts moved to \xE2\x80\x9C%1\xE2\x80\x9D but "
                                            "\xE2\x80\x9C%2\xE2\x80\x9D was not deleted: %3")
                              .arg(targetName, sourceName, error), true);
            else
                setStatus(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D merged into "
                                            "\xE2\x80\x9C%2\xE2\x80\x9D.").arg(sourceName, targetName));
            m_pendingSheet = targetId;
            loadOrganization();
        });
    };
    const auto saveLayout = [this, layoutChanged, layoutColumns, targetId, targetName, finish] {
        if (!layoutChanged) {
            finish();
            return;
        }
        request(QByteArrayLiteral("PATCH"), organizationPath(QStringLiteral("/views/") + targetId),
                QJsonObject{{QStringLiteral("columns"), layoutColumns}},
                [this, targetName, finish](bool ok, const QJsonObject &, const QString &error) {
            if (!ok) {
                setStatus(QString::fromUtf8("The fields on \xE2\x80\x9C%1\xE2\x80\x9D were not "
                                            "saved: %2").arg(targetName, error), true);
                loadOrganization();
                return;
            }
            finish();
        });
    };
    setStatus(QString::fromUtf8("Merging \xE2\x80\x9C%1\xE2\x80\x9D into \xE2\x80\x9C%2\xE2\x80\x9D\xE2\x80\xA6")
                  .arg(sourceName, targetName));
    if (edits.isEmpty()) {
        saveLayout();
        return;
    }
    QJsonArray payload;
    for (const auto &[contactId, body] : std::as_const(edits)) {
        QJsonObject edit = body;
        edit.insert(QStringLiteral("id"), contactId);
        payload.append(edit);
    }
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/contacts/bulk")),
            QJsonObject{{QStringLiteral("edits"), payload}},
            [this, sourceName, targetName, saveLayout](bool ok, const QJsonObject &,
                                                       const QString &error) {
        if (!ok) {
            setStatus(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D was not merged into "
                                        "\xE2\x80\x9C%2\xE2\x80\x9D: %3")
                          .arg(sourceName, targetName, error), true);
            loadOrganization();
            return;
        }
        saveLayout();
    });
}

void CrmFeature::showSheetTabMenu(const QString &id, const QPoint &globalPosition)
{
    const QJsonObject sheet = sheetById(id);
    if (sheet.isEmpty())
        return;
    // A client-side tab (a pipeline no list is bound to, or "No pipeline") has
    // no list on the worker to move, retype or delete.
    const bool stored = storedSheet(sheet);
    const QString flowId = text(sheet, "flow");
    int at = -1;
    for (int index = 0; index < m_views.size(); ++index) {
        if (text(m_views.at(index).toObject(), "id") == id)
            at = index;
    }
    QMenu menu(this);
    menu.setToolTipsVisible(true);
    // Who the list reaches: the count heads the menu, then each name and the
    // owner, cut short for a large organization.
    const QStringList lines = sheetShareSummary(sheet).split(QLatin1Char('\n'));
    menu.addSection(shareIcon(palette().color(QPalette::WindowText)), lines.value(0));
    const QStringList people = lines.mid(1, lines.size() - 2);
    constexpr int kShownPeople = 15;
    for (const QString &line : people.mid(0, kShownPeople))
        menu.addAction(line)->setEnabled(false);
    if (people.size() > kShownPeople)
        menu.addAction(QString::fromUtf8("\xE2\x80\xA6" "and %1 more")
                           .arg(people.size() - kShownPeople))->setEnabled(false);
    if (lines.size() > 1)
        menu.addAction(lines.last())->setEnabled(false);
    menu.addSeparator();
    if (id != QLatin1String(kNoPipelineTabId)) {
        menu.addAction(QString::fromUtf8("Rename\xE2\x80\xA6"), this,
                       [this, id] { renameSheet(id); })
            ->setEnabled(stored || m_canManageSettings);
        // Who a list reaches stays with its owner and the organization's
        // owners and admins; a pipeline tab no list is bound to gets one.
        const bool canShare = !stored || sheet.value(QStringLiteral("canManage")).toBool();
        QAction *members = menu.addAction(QString::fromUtf8("Add members\xE2\x80\xA6"), this,
                                          [this, id] { addSheetMembers(id); });
        members->setEnabled(canShare);
        if (!canShare)
            members->setToolTip(QStringLiteral(
                "Only the list's owner and organization owners and admins can add members"));
    }
    if (stored) {
        menu.addAction(QStringLiteral("Copy link"), this, [this, id] { copySheetLink(id); });
        menu.addAction(QString::fromUtf8("Activity history\xE2\x80\xA6"), this,
                       [this, id] { showSheetHistory(id); });
    }
    if (stored && !m_usersButton->isHidden() && id == forkMeshUsersSheetId(m_views))
        menu.addAction(QStringLiteral("Refresh ForkMesh users"), this,
                       [this] { importForkMeshUsers(); })
            ->setEnabled(!importBusy());
    if (!flowId.isEmpty() && flowIconId(flowId) == QLatin1String("grants") &&
        flowContactCount(flowId) == 0) {
        QAction *grants = menu.addAction(QStringLiteral("Load the 100 ranked grants"), this,
                                         [this, id] { loadRankedGrants(id); });
        grants->setEnabled(m_canManageSettings && !importBusy());
        grants->setToolTip(m_canManageSettings
            ? QStringLiteral("Add the grants, funds and credit programs that best fit "
                             "ForkMesh, ranked, with the award, deadline and application "
                             "details of each")
            : QStringLiteral("Only organization owners and admins can load the grants list"));
    }
    if (stored) {
        menu.addAction(QStringLiteral("Move up"), this, [this, id] { moveSheet(id, -1); })
            ->setEnabled(at > 0);
        menu.addAction(QStringLiteral("Move down"), this, [this, id] { moveSheet(id, 1); })
            ->setEnabled(at >= 0 && at < m_views.size() - 1);
        QMenu *merge = menu.addMenu(QStringLiteral("Merge into"));
        merge->menuAction()->setToolTip(QStringLiteral(
            "Put every contact on this list onto another one, carry its fields over, "
            "and delete this list"));
        const QIcon plainIcon = plainListIcon(palette().color(QPalette::WindowText));
        for (const QJsonValue &value : std::as_const(m_views)) {
            const QJsonObject other = value.toObject();
            const QString otherId = text(other, "id");
            if (otherId == id || !other.value(QStringLiteral("canManage")).toBool())
                continue;
            const QString otherFlow = text(other, "flow");
            merge->addAction(otherFlow.isEmpty() ? plainIcon : flowIcon(otherFlow),
                             otherFlow.isEmpty()
                                 ? text(other, "name")
                                 : QString::fromUtf8("%1 \xC2\xB7 %2")
                                       .arg(text(other, "name"), flowLabel(otherFlow)),
                             this, [this, id, otherId] { mergeSheetInto(id, otherId); });
        }
        merge->setEnabled(sheet.value(QStringLiteral("canManage")).toBool() &&
                          !merge->isEmpty());
        // What kind of list this is: a plain one holding the contacts put on
        // it, or one bound to a pipeline, showing every contact in that flow.
        // Which pipeline the whole organization reads off a list stays with
        // its owners and admins, the way the worker gates it.
        menu.addSeparator();
        const QString current = flowId;
        QMenu *type = menu.addMenu(QStringLiteral("Pipeline type"));
        type->setEnabled(m_canManageSettings);
        QAction *plain = type->addAction(plainListIcon(palette().color(QPalette::WindowText)),
                                         QStringLiteral("No pipeline (plain list)"), this,
                                         [this, id] { setSheetFlow(id, QString()); });
        plain->setCheckable(true);
        plain->setChecked(current.isEmpty());
        type->addSeparator();
        for (const QJsonValue &value : m_settings.value(QStringLiteral("flows")).toArray()) {
            const QJsonObject flow = value.toObject();
            const QString typeFlowId = text(flow, "id");
            QAction *action = type->addAction(
                flowIcon(typeFlowId), text(flow, "name"), this,
                [this, id, typeFlowId] { setSheetFlow(id, typeFlowId); });
            action->setCheckable(true);
            action->setChecked(typeFlowId == current);
        }
        type->addSeparator();
        type->addAction(QString::fromUtf8("New list\xE2\x80\xA6"), this,
                        [this] { newList(); });
        type->addAction(QString::fromUtf8("New pipeline\xE2\x80\xA6"), this,
                        [this] { newPipeline(); });
        type->addAction(QString::fromUtf8("Manage pipelines\xE2\x80\xA6"), this,
                        [this] { managePipelines(); });
        if (!flowId.isEmpty())
            addEditStagesAction(&menu, flowId);
    } else {
        menu.addSeparator();
        if (!flowId.isEmpty())
            addEditStagesAction(&menu, flowId);
        menu.addAction(QString::fromUtf8("New pipeline\xE2\x80\xA6"), this,
                       [this] { newPipeline(); })->setEnabled(m_canManageSettings);
        menu.addAction(QString::fromUtf8("Manage pipelines\xE2\x80\xA6"), this,
                       [this] { managePipelines(); })->setEnabled(m_canManageSettings);
    }
    // Deleting: a pipeline goes for the whole organization, with every list
    // bound to it; a plain list for whoever may manage it.
    if (stored && !flowId.isEmpty()) {
        menu.addSeparator();
        menu.addAction(QString::fromUtf8("Delete list\xE2\x80\xA6"), this,
                       [this, id] { deleteList(id); })
            ->setEnabled(sheet.value(QStringLiteral("canManage")).toBool() &&
                         pipelineLists(m_views).value(flowId).size() > 1);
    }
    if (!flowId.isEmpty()) {
        const QJsonArray flows = m_settings.value(QStringLiteral("flows")).toArray();
        const bool lastPipeline = flows.size() == 1 &&
                                  text(flows.first().toObject(), "id") == flowId;
        menu.addSeparator();
        QAction *remove = menu.addAction(QString::fromUtf8("Delete pipeline\xE2\x80\xA6"),
                                         this, [this, id] { deletePipeline(id); });
        remove->setEnabled(m_canManageSettings && !lastPipeline);
        if (!m_canManageSettings)
            remove->setToolTip(QStringLiteral(
                "Only organization owners and admins can delete pipelines"));
        else if (lastPipeline)
            remove->setToolTip(QStringLiteral("An organization keeps at least one pipeline"));
    } else if (stored) {
        menu.addSeparator();
        menu.addAction(QString::fromUtf8("Delete list\xE2\x80\xA6"), this,
                       [this, id] { deleteList(id); })
            ->setEnabled(sheet.value(QStringLiteral("canManage")).toBool());
    }
    menu.exec(globalPosition);
}

void CrmFeature::userDirectoryChanged()
{
    updateUsersImportAction();
    if (m_userDirectoryListener)
        m_userDirectoryListener();
}

// Shares a list or pipeline with more people: organization members picked
// from a searchable roster, and ForkMesh users outside it by name. A pipeline
// tab no list is bound to becomes a list bound to that pipeline, which is
// where the worker keeps who it is shared with.
void CrmFeature::addSheetMembers(const QString &id)
{
    const QJsonObject sheet = sheetById(id);
    if (sheet.isEmpty() || id == QLatin1String(kNoPipelineTabId))
        return;
    const bool stored = storedSheet(sheet);
    const QString name = text(sheet, "name");
    const QString owner = text(sheet, "owner").toLower();
    QStringList shared;
    for (const QJsonValue &value : sheet.value(QStringLiteral("sharedWith")).toArray())
        shared << value.toString();

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add members"));
    dialog.resize(440, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        QString::fromUtf8("Share \xE2\x80\x9C%1\xE2\x80\x9D with more people. %2")
            .arg(name, text(sheet, "visibility") == QLatin1String("private")
                           ? QStringLiteral("Only the people it is shared with can open it.")
                           : QStringLiteral("Everyone in %1 can already open it; people "
                                            "added here keep it if it is made private.")
                                 .arg(m_organization->currentData().toString())));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *search = lineEdit();
    search->setPlaceholderText(QStringLiteral("Search organization members"));
    layout->addWidget(search);
    auto *roster = new QListWidget;
    roster->setObjectName(QStringLiteral("crmShareRoster"));
    for (const QJsonValue &value : std::as_const(m_members)) {
        const QString member = value.toString();
        if (member.isEmpty() || member == owner || shared.contains(member))
            continue;
        auto *item = new QListWidgetItem(member, roster);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Unchecked);
    }
    layout->addWidget(roster, 1);
    auto *empty = new QLabel(QStringLiteral("Every organization member is already on it."));
    empty->setObjectName(QStringLiteral("mutedLabel"));
    empty->setVisible(roster->count() == 0);
    layout->addWidget(empty);
    connect(search, &QLineEdit::textChanged, &dialog, [roster](const QString &query) {
        for (int row = 0; row < roster->count(); ++row) {
            QListWidgetItem *item = roster->item(row);
            item->setHidden(!item->text().contains(query.trimmed(), Qt::CaseInsensitive));
        }
    });
    connect(roster, &QListWidget::itemActivated, &dialog, [](QListWidgetItem *item) {
        item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
    });
    auto *form = new QFormLayout;
    auto *others = lineEdit();
    others->setObjectName(QStringLiteral("crmShareOthers"));
    others->setPlaceholderText(QStringLiteral("username, another-user"));
    auto *directory = new QStringListModel(&dialog);
    const auto outsiders = [this, owner, shared] {
        QStringList names;
        if (!m_userDirectory)
            return names;
        for (const QString &user : m_userDirectory()) {
            const QString name = user.toLower();
            if (name == owner || shared.contains(name) ||
                m_members.contains(QJsonValue(name)))
                continue;
            names << name;
        }
        return names;
    };
    directory->setStringList(outsiders());
    auto *completer = new ShareNameCompleter(directory, others);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    completer->setMaxVisibleItems(10);
    others->setCompleter(completer);
    m_userDirectoryListener = [directory, outsiders] {
        directory->setStringList(outsiders());
    };
    form->addRow(QStringLiteral("Outside the organization"), others);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->addButton(QStringLiteral("Add"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    const int result = dialog.exec();
    m_userDirectoryListener = nullptr;
    if (result != QDialog::Accepted)
        return;

    QStringList added;
    for (int row = 0; row < roster->count(); ++row) {
        if (roster->item(row)->checkState() == Qt::Checked)
            added << roster->item(row)->text();
    }
    for (QString person :
         others->text().split(QRegularExpression(QStringLiteral("[\\s,;]+")),
                              Qt::SkipEmptyParts)) {
        if (person.startsWith(QLatin1Char('@')))
            person.remove(0, 1);
        person = person.toLower().left(64);
        if (!person.isEmpty() && person != owner && !shared.contains(person) &&
            !added.contains(person))
            added << person;
    }
    if (added.isEmpty())
        return;
    const QStringList wanted = shared + added;
    const auto reply = [this, name, added](bool ok, const QJsonObject &payload,
                                           const QString &error) {
        if (!ok) {
            setStatus(QString::fromUtf8("Members were not added to \xE2\x80\x9C%1\xE2\x80\x9D: %2")
                          .arg(name, error), true);
            loadOrganization();
            return;
        }
        // The worker keeps only names that are ForkMesh accounts. Its copy of
        // the list goes in place of ours rather than through a reload, which
        // would put its own status over the names it dropped.
        const QJsonObject view = payload.value(QStringLiteral("view")).toObject();
        if (!text(view, "id").isEmpty()) {
            qsizetype at = 0;
            while (at < m_views.size() &&
                   text(m_views.at(at).toObject(), "id") != text(view, "id"))
                ++at;
            if (at < m_views.size())
                m_views.replace(at, view);
            else
                m_views.append(view);
            rebuildVirtualSheets();
            refreshContactSnapshot();
        }
        QStringList kept;
        for (const QJsonValue &value : view.value(QStringLiteral("sharedWith")).toArray())
            kept << value.toString();
        QStringList missing;
        for (const QString &person : added) {
            if (!kept.contains(person))
                missing << person;
        }
        const int count = int(added.size() - missing.size());
        QString note = count == 1
            ? QString::fromUtf8("Added 1 member to \xE2\x80\x9C%1\xE2\x80\x9D.").arg(name)
            : QString::fromUtf8("Added %1 members to \xE2\x80\x9C%2\xE2\x80\x9D.")
                  .arg(count).arg(name);
        if (!missing.isEmpty())
            note += QStringLiteral(" No ForkMesh account: %1.")
                        .arg(missing.join(QStringLiteral(", ")));
        setStatus(note, !missing.isEmpty());
    };
    if (stored) {
        request(QByteArrayLiteral("PATCH"), organizationPath(QStringLiteral("/views/") + id),
                QJsonObject{{QStringLiteral("sharedWith"), QJsonArray::fromStringList(wanted)}},
                reply);
        return;
    }
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/views")),
            QJsonObject{{QStringLiteral("name"), name},
                        {QStringLiteral("flow"), text(sheet, "flow")},
                        {QStringLiteral("visibility"), QStringLiteral("organization")},
                        {QStringLiteral("sharedWith"), QJsonArray::fromStringList(wanted)}},
            [this, reply](bool ok, const QJsonObject &payload, const QString &error) {
        if (ok)
            m_pendingSheet = text(payload.value(QStringLiteral("view")).toObject(), "id");
        reply(ok, payload, error);
    });
}

QJsonObject CrmFeature::sheetById(const QString &id) const
{
    if (id.isEmpty())
        return {};
    for (const QJsonValue &value : m_views) {
        const QJsonObject sheet = value.toObject();
        if (text(sheet, "id") == id)
            return sheet;
    }
    for (const QJsonObject &sheet : m_virtualSheets) {
        if (text(sheet, "id") == id)
            return sheet;
    }
    return {};
}

QList<QJsonObject> CrmFeature::tabSheets() const
{
    QList<QJsonObject> sheets;
    for (const QJsonValue &value : m_views)
        sheets << value.toObject();
    for (const QJsonObject &sheet : m_virtualSheets) {
        // "No pipeline" only shows while it has someone to show.
        if (text(sheet, "id") != QLatin1String(kNoPipelineTabId) ||
            sheetContactCount(sheet) > 0)
            sheets << sheet;
    }
    return sheets;
}

// Every pipeline is a tab. One a list is bound to is that list's tab; the rest
// get a client-side tab of their own, which stores nothing on the worker.
void CrmFeature::rebuildVirtualSheets()
{
    QSet<QString> bound;
    for (const QJsonValue &value : std::as_const(m_views))
        bound.insert(text(value.toObject(), "flow"));
    m_virtualSheets.clear();
    for (const QJsonValue &value : m_settings.value(QStringLiteral("flows")).toArray()) {
        const QJsonObject flow = value.toObject();
        const QString flowId = text(flow, "id");
        if (flowId.isEmpty() || bound.contains(flowId))
            continue;
        m_virtualSheets << QJsonObject{
            {QStringLiteral("id"), QLatin1String(kPipelineTabPrefix) + flowId},
            {QStringLiteral("name"), text(flow, "name").isEmpty() ? flowId
                                                                  : text(flow, "name")},
            {QStringLiteral("flow"), flowId},
            {QStringLiteral("virtual"), true}};
    }
    m_virtualSheets << QJsonObject{
        {QStringLiteral("id"), QLatin1String(kNoPipelineTabId)},
        {QStringLiteral("name"), QStringLiteral("No pipeline")},
        {QStringLiteral("virtual"), true}};
}

QJsonObject CrmFeature::activeSheet() const
{
    return m_sheet ? sheetById(m_sheet->currentData().toString()) : QJsonObject();
}

QHash<QString, QStringList> CrmFeature::pipelineLists(const QJsonArray &views)
{
    QHash<QString, QStringList> lists;
    for (const QJsonValue &value : views) {
        const QJsonObject view = value.toObject();
        if (!text(view, "flow").isEmpty())
            lists[text(view, "flow")] << text(view, "id");
    }
    return lists;
}

bool CrmFeature::contactInSheet(const QJsonObject &contact, const QJsonObject &sheet,
                                const QHash<QString, QStringList> &pipelineLists)
{
    const QString id = text(sheet, "id");
    QStringList memberships;
    for (const QJsonValue &member : contact.value(QStringLiteral("views")).toArray())
        memberships << member.toString();
    const QString flow = text(sheet, "flow");
    if (flow.isEmpty())
        return memberships.contains(id);
    if (text(contact, "flow") != flow)
        return false;
    const QStringList lists = pipelineLists.value(flow, QStringList{id});
    for (const QString &list : lists) {
        if (memberships.contains(list))
            return list == id;
    }
    return lists.value(0) == id;
}

bool CrmFeature::contactInSheet(const QJsonObject &contact, const QJsonObject &sheet) const
{
    return contactInSheet(contact, sheet, pipelineLists(m_views));
}

int CrmFeature::sheetContactCount(const QJsonObject &sheet) const
{
    return int(m_snapshot.sheetRows.value(text(sheet, "id")).size());
}

// Everyone a list reaches besides the member who owns it: the names it picks
// out, plus the whole roster while it stays open to the organization.
QStringList CrmFeature::sheetShareNames(const QJsonObject &sheet) const
{
    QStringList names;
    for (const QJsonValue &name : sheet.value(QStringLiteral("sharedWith")).toArray())
        names << name.toString();
    if (text(sheet, "visibility") != QLatin1String("private")) {
        for (const QJsonValue &name : m_members) {
            if (!names.contains(name.toString()))
                names << name.toString();
        }
    }
    const QString owner = text(sheet, "owner").toLower();
    names.removeIf([&owner](const QString &name) {
        return name.isEmpty() || name == owner;
    });
    return names;
}

QString CrmFeature::sheetShareSummary(const QJsonObject &sheet) const
{
    const QStringList names = sheetShareNames(sheet);
    QStringList picked;
    for (const QJsonValue &name : sheet.value(QStringLiteral("sharedWith")).toArray())
        picked << name.toString();
    const QString count = names.size() == 1 ? QStringLiteral("1 person")
                                            : QStringLiteral("%1 people").arg(names.size());
    QStringList lines;
    if (names.isEmpty())
        lines << QStringLiteral("Private to its owner");
    else if (text(sheet, "visibility") == QLatin1String("private"))
        lines << QStringLiteral("Shared with %1").arg(count);
    else
        lines << QStringLiteral("Open to %1 in %2").arg(count, m_organization->currentData().toString());
    for (const QString &name : names)
        lines << (picked.contains(name) ? name
                                        : name + QString::fromUtf8(" \xC2\xB7 member"));
    lines << QStringLiteral("Owner: %1").arg(text(sheet, "owner").isEmpty()
                                                 ? m_organization->currentData().toString()
                                                 : text(sheet, "owner"));
    return lines.join(QLatin1Char('\n'));
}

// The field a column is stored under in a list's layout; empty for the
// computed columns a layout does not carry.
QString CrmFeature::sheetField(const ContactColumnSpec &column) const
{
    switch (column.kind) {
    case ContactColumnSpec::Field:
    case ContactColumnSpec::CustomField:
        return column.key;
    case ContactColumnSpec::LastContacted:
        return QStringLiteral("lastContactedAt");
    default:
        return QString();
    }
}

QString CrmFeature::viewField(const ContactColumnSpec &column) const
{
    switch (column.kind) {
    case ContactColumnSpec::MastodonLatest:
    case ContactColumnSpec::LinkedEmails:
    case ContactColumnSpec::LinkedToots:
        return column.id;
    default:
        return sheetField(column);
    }
}

bool CrmFeature::sheetCarriesField(const QString &field) const
{
    const QJsonObject sheet = activeSheet();
    if (!storedSheet(sheet))
        return true;
    for (const QJsonValue &value : sheet.value(QStringLiteral("columns")).toArray()) {
        if (text(value.toObject(), "field") == field)
            return true;
    }
    return false;
}

bool CrmFeature::columnHiddenInView(const ContactColumnSpec &column) const
{
    if (column.kind == ContactColumnSpec::Actions ||
        column.kind == ContactColumnSpec::AddColumn)
        return false;
    if (allColumnsView())
        return column.kind == ContactColumnSpec::CustomField &&
               !sheetCarriesField(column.key);
    return !tableViewFields().contains(viewField(column));
}

void CrmFeature::applyColumnVisibility()
{
    for (int index = 0; index < m_columns.size(); ++index)
        m_contactsTable->setColumnHidden(index, columnHiddenInView(m_columns.at(index)));
    applyColumnOrder();
}

void CrmFeature::applyColumnOrder()
{
    QHeaderView *header = m_contactsTable->horizontalHeader();
    m_orderingColumns = true;
    if (allColumnsView()) {
        for (int logical = 0; logical < m_columns.size() && logical < header->count(); ++logical)
            header->moveSection(header->visualIndex(logical), logical);
    } else {
        int target = 0;
        for (const QString &field : tableViewFields()) {
            for (int logical = 0; logical < m_columns.size() && logical < header->count();
                 ++logical) {
                if (viewField(m_columns.at(logical)) != field)
                    continue;
                header->moveSection(header->visualIndex(logical), target++);
                break;
            }
        }
    }
    m_orderingColumns = false;
}

QJsonArray CrmFeature::tableViews() const
{
    return m_settings.value(QStringLiteral("tableViews")).toArray();
}

bool CrmFeature::builtinTableView() const
{
    return m_tableViewId == QLatin1String(kDefaultTableView) ||
           m_tableViewId == QLatin1String(kAllTableView);
}

bool CrmFeature::allColumnsView() const
{
    return m_tableViewId == QLatin1String(kAllTableView) && m_tableViewDraft.isEmpty();
}

QStringList CrmFeature::builtinViewFields() const
{
    const auto known = [this](const QString &field) {
        return std::any_of(m_columns.cbegin(), m_columns.cend(),
                           [this, &field](const ContactColumnSpec &column) {
            return viewField(column) == field;
        });
    };
    QStringList fields;
    if (m_tableViewId == QLatin1String(kAllTableView)) {
        for (const ContactColumnSpec &column : m_columns) {
            const QString field = viewField(column);
            if (field.isEmpty() || fields.contains(field) ||
                column.kind == ContactColumnSpec::Actions ||
                column.kind == ContactColumnSpec::AddColumn ||
                (column.kind == ContactColumnSpec::CustomField && !sheetCarriesField(column.key)))
                continue;
            fields << field;
        }
        return fields;
    }
    for (const QString &field : kDefaultViewFields) {
        if (known(field) && !fields.contains(field))
            fields << field;
    }
    return fields;
}

QStringList CrmFeature::tableViewFields() const
{
    if (builtinTableView() && !m_tableViewDraft.isEmpty())
        return m_tableViewDraft;
    for (const QJsonValue &value : tableViews()) {
        const QJsonObject view = value.toObject();
        if (text(view, "id") != m_tableViewId)
            continue;
        QStringList fields;
        for (const QJsonValue &field : view.value(QStringLiteral("columns")).toArray())
            fields << field.toString();
        return fields;
    }
    return kDefaultViewFields;
}

QStringList CrmFeature::shownViewFields() const
{
    const QHeaderView *header = m_contactsTable->horizontalHeader();
    QStringList fields;
    for (int visual = 0; visual < header->count(); ++visual) {
        const int logical = header->logicalIndex(visual);
        if (logical < 0 || logical >= m_columns.size() ||
            m_contactsTable->isColumnHidden(logical))
            continue;
        const QString field = viewField(m_columns.at(logical));
        if (!field.isEmpty() && !fields.contains(field))
            fields << field;
    }
    return fields;
}

void CrmFeature::renderTableViews()
{
    if (!m_tableView)
        return;
    const QSignalBlocker blocker(m_tableView);
    m_tableView->clear();
    m_tableView->addItem(QStringLiteral("Default"), QString::fromLatin1(kDefaultTableView));
    m_tableView->addItem(QStringLiteral("All"), QString::fromLatin1(kAllTableView));
    for (const QJsonValue &value : tableViews()) {
        const QJsonObject view = value.toObject();
        m_tableView->addItem(text(view, "name"), text(view, "id"));
    }
    int index = m_tableView->findData(m_tableViewId);
    if (index < 0) {
        m_tableViewId = QString::fromLatin1(kDefaultTableView);
        m_tableViewDraft.clear();
        index = 0;
    }
    const bool modified = builtinTableView() && !m_tableViewDraft.isEmpty();
    if (modified)
        m_tableView->setItemText(index, QStringLiteral("%1 (modified)")
                                            .arg(m_tableView->itemText(index)));
    m_tableView->setCurrentIndex(index);
    if (m_tableViewSave)
        m_tableViewSave->setVisible(modified);
}

void CrmFeature::selectTableView(const QString &id)
{
    if (id.isEmpty())
        return;
    if (id != m_tableViewId)
        m_tableViewDraft.clear();
    m_tableViewId = id;
    renderTableViews();
    applyColumnVisibility();
}

void CrmFeature::showTableViewMenu()
{
    QMenu menu(this);
    menu.addAction(QString::fromUtf8("New view\xE2\x80\xA6"), this,
                   [this] { createTableView(shownViewFields()); });
    menu.addAction(QString::fromUtf8("Rename view\xE2\x80\xA6"), this,
                   [this] { renameTableView(); })
        ->setEnabled(!builtinTableView());
    menu.addAction(QString::fromUtf8("Delete view\xE2\x80\xA6"), this,
                   [this] { deleteTableView(); })
        ->setEnabled(!builtinTableView());
    menu.exec(m_tableViewMenu->mapToGlobal(QPoint(0, m_tableViewMenu->height())));
}

void CrmFeature::changeViewFields(const QStringList &fields)
{
    if (fields.isEmpty())
        return;
    if (builtinTableView()) {
        const QStringList draft = fields == builtinViewFields() ? QStringList() : fields;
        if (draft == m_tableViewDraft)
            return;
        m_tableViewDraft = draft;
        renderTableViews();
        applyColumnVisibility();
        return;
    }
    QJsonArray views = tableViews();
    for (int at = 0; at < views.size(); ++at) {
        QJsonObject view = views.at(at).toObject();
        if (text(view, "id") != m_tableViewId)
            continue;
        QStringList kept = fields;
        for (const QJsonValue &value : view.value(QStringLiteral("columns")).toArray()) {
            const QString field = value.toString();
            const bool known = std::any_of(m_columns.cbegin(), m_columns.cend(),
                                           [this, &field](const ContactColumnSpec &column) {
                return viewField(column) == field;
            });
            if (!known && !kept.contains(field))
                kept << field;
        }
        if (view.value(QStringLiteral("columns")).toArray() == QJsonArray::fromStringList(kept))
            return;
        view.insert(QStringLiteral("columns"), QJsonArray::fromStringList(kept));
        views.replace(at, view);
    }
    saveTableViews(views, QString());
    applyColumnVisibility();
}

void CrmFeature::createTableView(const QStringList &fields)
{
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("New view"),
        QStringLiteral("Save these columns as a view named"), QLineEdit::Normal,
        QString(), &accepted).trimmed().left(60);
    if (!accepted || name.isEmpty())
        return;
    addTableView(name, fields, QString::fromUtf8("View \xE2\x80\x9C%1\xE2\x80\x9D saved.").arg(name));
}

void CrmFeature::saveTableViewDraft()
{
    if (!builtinTableView() || m_tableViewDraft.isEmpty())
        return;
    QStringList names;
    for (const QJsonValue &value : tableViews())
        names << text(value.toObject(), "name").toCaseFolded();
    QString name;
    for (int number = 1; name.isEmpty() || names.contains(name.toCaseFolded()); ++number)
        name = QStringLiteral("View %1").arg(number);
    const QStringList fields = m_tableViewDraft;
    m_tableViewDraft.clear();
    addTableView(name, fields,
                 QString::fromUtf8("Saved as \xE2\x80\x9C%1\xE2\x80\x9D. Rename it from the "
                                   "\xE2\x80\xA6 menu beside it.").arg(name));
}

void CrmFeature::addTableView(const QString &name, const QStringList &fields, const QString &done)
{
    QJsonArray views = tableViews();
    QStringList taken{QString::fromLatin1(kDefaultTableView), QString::fromLatin1(kAllTableView)};
    for (const QJsonValue &value : views)
        taken << text(value.toObject(), "id");
    QString id;
    for (qint64 stamp = QDateTime::currentMSecsSinceEpoch(); id.isEmpty() || taken.contains(id);
         ++stamp)
        id = QStringLiteral("view-") + QString::number(stamp, 36);
    views.append(QJsonObject{{QStringLiteral("id"), id},
                             {QStringLiteral("name"), name},
                             {QStringLiteral("columns"), QJsonArray::fromStringList(fields)}});
    m_tableViewId = id;
    m_tableViewDraft.clear();
    saveTableViews(views, done);
    applyColumnVisibility();
}

void CrmFeature::renameTableView()
{
    if (builtinTableView())
        return;
    QJsonArray views = tableViews();
    for (int at = 0; at < views.size(); ++at) {
        QJsonObject view = views.at(at).toObject();
        if (text(view, "id") != m_tableViewId)
            continue;
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, QStringLiteral("Rename view"), QStringLiteral("View name"),
            QLineEdit::Normal, text(view, "name"), &accepted).trimmed().left(60);
        if (!accepted || name.isEmpty() || name == text(view, "name"))
            return;
        view.insert(QStringLiteral("name"), name);
        views.replace(at, view);
        saveTableViews(views, QString());
        return;
    }
}

void CrmFeature::deleteTableView()
{
    if (builtinTableView())
        return;
    QJsonArray views = tableViews();
    for (int at = 0; at < views.size(); ++at) {
        const QString name = text(views.at(at).toObject(), "name");
        if (text(views.at(at).toObject(), "id") != m_tableViewId)
            continue;
        if (QMessageBox::question(
                this, QStringLiteral("Delete view"),
                QString::fromUtf8("Delete the view \xE2\x80\x9C%1\xE2\x80\x9D for everyone in "
                                  "the organization? The contacts and their fields stay.")
                    .arg(name)) != QMessageBox::Yes)
            return;
        views.removeAt(at);
        m_tableViewId = QString::fromLatin1(kDefaultTableView);
        saveTableViews(views, QString::fromUtf8("View \xE2\x80\x9C%1\xE2\x80\x9D deleted.").arg(name));
        applyColumnVisibility();
        return;
    }
}

void CrmFeature::saveTableViews(const QJsonArray &views, const QString &done)
{
    m_settings.insert(QStringLiteral("tableViews"), views);
    renderTableViews();
    request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")),
            QJsonObject{{QStringLiteral("tableViews"), views}},
            [this, done](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("The views were not saved: %1").arg(error), true);
            loadOrganization();
            return;
        }
        m_settings.insert(QStringLiteral("tableViews"),
                          payload.value(QStringLiteral("settings")).toObject()
                              .value(QStringLiteral("tableViews")).toArray());
        renderTableViews();
        applyColumnVisibility();
        if (!done.isEmpty())
            setStatus(done);
    });
}

void CrmFeature::setColumnsHidden(const QStringList &ids, bool hidden)
{
    QStringList fields = shownViewFields();
    for (const QString &id : ids) {
        const int index = columnIndexById(id);
        if (index < 0)
            continue;
        const QString field = viewField(m_columns.at(index));
        if (field.isEmpty())
            continue;
        if (hidden)
            fields.removeAll(field);
        else if (!fields.contains(field))
            fields << field;
    }
    if (fields != shownViewFields())
        changeViewFields(fields);
}

void CrmFeature::showColumn(const QString &id)
{
    addColumnToSheet(id);
    applyColumnVisibility();
    const int index = columnIndexById(id);
    if (index < 0 || !m_contactsTable->isColumnHidden(index))
        return;
    if (m_tableViewId == QLatin1String(kDefaultTableView) && m_tableViewDraft.isEmpty())
        selectTableView(QString::fromLatin1(kAllTableView));
    if (m_contactsTable->isColumnHidden(index))
        setColumnsHidden({id}, false);
}

void CrmFeature::addColumnToSheet(const QString &id)
{
    const int index = columnIndexById(id);
    QJsonObject sheet = activeSheet();
    if (index < 0 || !storedSheet(sheet))
        return;
    const ContactColumnSpec spec = m_columns.at(index);
    const QString field = sheetField(spec);
    if (field.isEmpty())
        return;
    QJsonArray layout = sheet.value(QStringLiteral("columns")).toArray();
    bool found = false;
    bool layoutChanged = false;
    for (int at = 0; at < layout.size(); ++at) {
        QJsonObject stored = layout.at(at).toObject();
        if (text(stored, "field") != field)
            continue;
        found = true;
        if (stored.value(QStringLiteral("hidden")).toBool()) {
            stored.insert(QStringLiteral("hidden"), false);
            layout.replace(at, stored);
            layoutChanged = true;
        }
    }
    if (!found) {
        layout.append(QJsonObject{
            {QStringLiteral("field"), field},
            {QStringLiteral("label"), spec.label},
            {QStringLiteral("custom"), spec.kind == ContactColumnSpec::CustomField},
            {QStringLiteral("hidden"), false}});
        layoutChanged = true;
    }
    if (!layoutChanged)
        return;
    const QString sheetId = text(sheet, "id");
    sheet.insert(QStringLiteral("columns"), layout);
    for (int at = 0; at < m_views.size(); ++at) {
        if (text(m_views.at(at).toObject(), "id") == sheetId)
            m_views.replace(at, sheet);
    }
    const QString name = text(sheet, "name");
    request(QByteArrayLiteral("PATCH"), organizationPath(QStringLiteral("/views/") + sheetId),
            QJsonObject{{QStringLiteral("columns"), layout}},
            [this, name](bool ok, const QJsonObject &, const QString &error) {
        if (!ok) {
            setStatus(QString::fromUtf8("The fields on \xE2\x80\x9C%1\xE2\x80\x9D were "
                                        "not saved: %2").arg(name, error), true);
            loadOrganization();
            return;
        }
        renderSheets();
    });
}

void CrmFeature::showSheetMenu(const QPoint &position)
{
    QMenu menu(this);
    const QTableWidgetItem *item = m_sheetsTable->itemAt(position);
    const QTableWidgetItem *idItem = item ? m_sheetsTable->item(item->row(), 0) : nullptr;
    const QJsonObject sheet = idItem ? sheetById(idItem->data(Qt::UserRole).toString())
                                     : QJsonObject();
    if (!sheet.isEmpty()) {
        const QString id = text(sheet, "id");
        int at = -1;
        for (int index = 0; index < m_views.size(); ++index) {
            if (text(m_views.at(index).toObject(), "id") == id)
                at = index;
        }
        menu.addSection(text(sheet, "name"));
        menu.addAction(QStringLiteral("Open its tab"), this,
                       [this, id] { openSheet(id); });
        menu.addAction(QStringLiteral("Copy link"), this, [this, id] { copySheetLink(id); });
        menu.addAction(QString::fromUtf8("Activity history\xE2\x80\xA6"), this,
                       [this, id] { showSheetHistory(id); });
        menu.addAction(QString::fromUtf8("Rename\xE2\x80\xA6"), this,
                       [this, id] { renameSheet(id); });
        menu.addAction(QStringLiteral("Move up"), this, [this, id] { moveSheet(id, -1); })
            ->setEnabled(at > 0);
        menu.addAction(QStringLiteral("Move down"), this, [this, id] { moveSheet(id, 1); })
            ->setEnabled(at >= 0 && at < m_views.size() - 1);
        const QString current = text(sheet, "flow");
        QMenu *type = menu.addMenu(QStringLiteral("Pipeline type"));
        type->setEnabled(m_canManageSettings);
        QAction *plain = type->addAction(plainListIcon(palette().color(QPalette::WindowText)),
                                         QStringLiteral("No pipeline (plain list)"),
                                         this, [this, id] { setSheetFlow(id, QString()); });
        plain->setCheckable(true);
        plain->setChecked(current.isEmpty());
        type->addSeparator();
        for (const QJsonValue &value :
             m_settings.value(QStringLiteral("flows")).toArray()) {
            const QJsonObject flow = value.toObject();
            const QString flowId = text(flow, "id");
            QAction *action = type->addAction(
                flowIcon(flowId), text(flow, "name"), this,
                [this, id, flowId] { setSheetFlow(id, flowId); });
            action->setCheckable(true);
            action->setChecked(flowId == current);
        }
        menu.addSeparator();
    }
    menu.addAction(QString::fromUtf8("Import a CSV\xE2\x80\xA6"), this,
                   [this] { importCsv(); })->setEnabled(!importBusy());
    menu.addAction(QString::fromUtf8("Import Mastodon followers\xE2\x80\xA6"), this,
                   [this] { importMastodonFollowers(); })
        ->setEnabled(!importBusy());
    menu.addAction(QString::fromUtf8("Import GitHub contributors\xE2\x80\xA6"), this,
                   [this] { importGithubContributors(); })
        ->setEnabled(!importBusy());
    if (!m_usersPageButton->isHidden())
        menu.addAction(m_usersPageButton->text(), this,
                       [this] { importForkMeshUsers(); })
            ->setEnabled(!importBusy());
    menu.addAction(QString::fromUtf8("New pipeline\xE2\x80\xA6"), this,
                   [this] { newPipeline(); })->setEnabled(m_canManageSettings);
    menu.addAction(QString::fromUtf8("Manage pipelines\xE2\x80\xA6"), this,
                   [this] { managePipelines(); })->setEnabled(m_canManageSettings);
    menu.exec(m_sheetsTable->viewport()->mapToGlobal(position));
}

void CrmFeature::showContactMenu(const QPoint &position)
{
    const QTableWidgetItem *item = m_contactsTable->itemAt(position);
    int row = item ? item->row() : m_contactsTable->rowAt(position.y());
    if (row < 0)
        return;
    // A right click outside the selection acts on that row alone, the way
    // file managers do; inside it, on every selected contact. Not selectRow():
    // with Shift or Ctrl held it would extend or toggle instead.
    QItemSelectionModel *selection = m_contactsTable->selectionModel();
    if (!selection->isRowSelected(row, QModelIndex())) {
        const QModelIndex index = m_contactsTable->model()->index(row, 0);
        selection->setCurrentIndex(index, QItemSelectionModel::NoUpdate);
        selection->select(index, QItemSelectionModel::ClearAndSelect |
                                     QItemSelectionModel::Rows);
    }
    const QList<QJsonObject> contacts = selectedContacts();
    if (contacts.isEmpty())
        return;
    QStringList ids;
    for (const QJsonObject &contact : contacts)
        ids << text(contact, "id");
    QMenu menu(this);
    menu.addSection(contacts.size() == 1
        ? (text(contacts.first(), "name").isEmpty() ? text(contacts.first(), "email")
                                                    : text(contacts.first(), "name"))
        : QStringLiteral("%1 contacts").arg(contacts.size()));
    // A call or a reply that left nothing to link still counts as contact.
    menu.addAction(QStringLiteral("Mark contacted now"), this,
                   [this, ids] { markContacted(ids, QDateTime::currentMSecsSinceEpoch()); });
    QMenu *pipelines = menu.addMenu(QStringLiteral("Move to pipeline list"));
    QMenu *moveLists = menu.addMenu(QStringLiteral("Move to list"));
    QMenu *lists = menu.addMenu(QStringLiteral("Add to list"));
    const QHash<QString, QStringList> sharedPipelines = pipelineLists(m_views);
    for (const QJsonObject &sheet : tabSheets()) {
        const QString sheetId = text(sheet, "id");
        if (sheetId == QLatin1String(kNoPipelineTabId))
            continue;
        const bool pipeline = !text(sheet, "flow").isEmpty();
        bool allThere = true;
        bool allOnlyThere = true;
        for (const QJsonObject &contact : contacts) {
            const bool there = contactInSheet(contact, sheet);
            allThere = allThere && there;
            const QJsonArray memberships =
                contact.value(QStringLiteral("views")).toArray();
            allOnlyThere = allOnlyThere && memberships.size() == 1
                && memberships.first().toString() == sheetId;
        }
        const bool sharesPipeline =
            pipeline && sharedPipelines.value(text(sheet, "flow")).size() > 1;
        QAction *action = (pipeline ? pipelines : lists)->addAction(
            sharesPipeline ? QString::fromUtf8("%1 \xC2\xB7 %2")
                                 .arg(text(sheet, "name"), flowLabel(text(sheet, "flow")))
                           : text(sheet, "name"), this,
            [this, ids, sheetId] { moveContactsToSheet(ids, sheetId); });
        if (pipeline)
            action->setIcon(flowIcon(text(sheet, "flow")));
        action->setEnabled(!allThere);
        if (allThere)
            action->setToolTip(QStringLiteral("Already in this list"));
        if (pipeline)
            continue;
        QAction *move = moveLists->addAction(
            text(sheet, "name"), this,
            [this, ids, sheetId] { moveContactsToListOnly(ids, sheetId); });
        move->setEnabled(!allOnlyThere);
        if (allOnlyThere)
            move->setToolTip(QStringLiteral("Already in this list and no other"));
    }
    for (QMenu *submenu : {pipelines, moveLists, lists})
        submenu->setToolTipsVisible(true);
    if (pipelines->isEmpty())
        pipelines->addAction(QStringLiteral("No pipelines yet"))->setEnabled(false);
    for (QMenu *submenu : {moveLists, lists}) {
        if (submenu->isEmpty())
            submenu->addAction(QStringLiteral("No lists yet"))->setEnabled(false);
    }
    // Walking the selection along the pipeline it is in. Offered only when
    // every selected contact shares a flow, because a stage means nothing
    // across two different pipelines.
    QString sharedFlow = text(contacts.first(), "flow");
    for (const QJsonObject &contact : contacts) {
        if (text(contact, "flow") != sharedFlow)
            sharedFlow.clear();
    }
    const QStringList stages = flowStages(sharedFlow);
    if (!stages.isEmpty()) {
        QMenu *stageMenu = menu.addMenu(
            QStringLiteral("Set stage in %1").arg(flowLabel(sharedFlow)));
        stageMenu->setToolTipsVisible(true);
        for (const QString &stage : stages) {
            bool allAt = true;
            for (const QJsonObject &contact : contacts)
                allAt = allAt && text(contact, "stage") == stage;
            QAction *action = stageMenu->addAction(
                stage, this, [this, ids, stage] { setContactsStage(ids, stage); });
            action->setEnabled(!allAt);
            if (allAt)
                action->setToolTip(QStringLiteral("Already at this stage"));
        }
    }
    // Taking contacts off the list that is open, which is the only list the
    // menu can name without asking.
    const QJsonObject open = activeSheet();
    if (storedSheet(open) && (text(open, "flow").isEmpty() ||
                              sharedPipelines.value(text(open, "flow")).size() > 1)) {
        const QString openId = text(open, "id");
        menu.addSeparator();
        menu.addAction(QString::fromUtf8("Remove from \xE2\x80\x9C%1\xE2\x80\x9D")
                           .arg(text(open, "name")), this,
                       [this, ids, openId] { removeContactsFromSheet(ids, openId); });
    }
    menu.addSeparator();
    menu.addAction(QString::fromUtf8("New pipeline\xE2\x80\xA6"), this,
                   [this] { newPipeline(); })->setEnabled(m_canManageSettings);
    menu.addAction(QString::fromUtf8("Manage pipelines\xE2\x80\xA6"), this,
                   [this] { managePipelines(); })->setEnabled(m_canManageSettings);
    // Row actions are for one contact: Email and Delete each mean one record,
    // so a multi-row selection gets no separator with nothing under it.
    if (contacts.size() == 1) {
        menu.addSeparator();
        const QString id = ids.first();
        menu.addAction(QString::fromUtf8("Edit\xE2\x80\xA6"), this,
                       [this, id] { editContact(contactById(id)); });
        menu.addAction(QStringLiteral("Email"), this,
                       [this, id] { composeEmail(contactById(id)); })
            ->setEnabled(!text(contacts.first(), "email").isEmpty());
        menu.addAction(QStringLiteral("Copy prompt"), this,
                       [this, id] { copyOutreachPrompt(contactById(id)); });
        menu.addAction(QStringLiteral("Copy link"), this,
                       [this, id] { copyContactLink(contactById(id)); });
        menu.addAction(QString::fromUtf8("Merge into\xE2\x80\xA6"), this,
                       [this, id] { pickMergeTarget(id); })
            ->setEnabled(m_contacts.size() > 1);
        menu.addAction(QStringLiteral("Delete"), this,
                       [this, id] { deleteContact(contactById(id)); });
    } else {
        // Two or more rows made for one person: keep the one picked here and
        // fold the rest of the selection into it.
        menu.addSeparator();
        QMenu *merge = menu.addMenu(QStringLiteral("Merge selected into"));
        for (const QJsonObject &contact : contacts) {
            const QString keep = text(contact, "id");
            QStringList others = ids;
            others.removeAll(keep);
            merge->addAction(contactLabel(contact), this,
                             [this, others, keep] { mergeContacts(others, keep); });
        }
    }
    menu.exec(m_contactsTable->viewport()->mapToGlobal(position));
}

QString CrmFeature::contactLabel(const QJsonObject &contact) const
{
    const QString name = text(contact, "name");
    const QString email = text(contact, "email");
    if (name.isEmpty())
        return email.isEmpty() ? QStringLiteral("(unnamed contact)") : email;
    return email.isEmpty() ? name : QString::fromUtf8("%1 \xE2\x80\x94 %2").arg(name, email);
}

// "Merge into…" on one row: pick the contact it duplicates. Likely matches
// (the same email, name words or company) come first and the best is picked.
void CrmFeature::pickMergeTarget(const QString &duplicateId)
{
    const QJsonObject duplicate = contactById(duplicateId);
    if (duplicate.isEmpty())
        return;
    const auto words = [](const QString &value) {
        QSet<QString> found;
        for (const QString &word : value.toLower().split(
                 QRegularExpression(QStringLiteral("[^\\w]+")), Qt::SkipEmptyParts)) {
            if (word.size() > 1)
                found.insert(word);
        }
        return found;
    };
    const QSet<QString> names = words(text(duplicate, "name"));
    const QString email = text(duplicate, "email").toLower();
    const QString company = text(duplicate, "company").toLower();
    struct Candidate { int score; QString label; QString id; QString detail; };
    QList<Candidate> candidates;
    for (const QJsonValue &value : std::as_const(m_contacts)) {
        const QJsonObject contact = value.toObject();
        const QString id = text(contact, "id");
        if (id.isEmpty() || id == duplicateId)
            continue;
        int score = 0;
        if (!email.isEmpty() && text(contact, "email").toLower() == email)
            score += 4;
        score += 2 * int(words(text(contact, "name")).intersect(names).size());
        if (!company.isEmpty() && text(contact, "company").toLower() == company)
            score += 1;
        const QString detail = QStringList{text(contact, "company"),
                                           flowLabel(text(contact, "flow"))}
                                   .filter(QRegularExpression(QStringLiteral("\\S")))
                                   .join(QString::fromUtf8(" \xC2\xB7 "));
        candidates.append({score, contactLabel(contact), id, detail});
    }
    if (candidates.isEmpty())
        return;
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate &a, const Candidate &b) {
        return a.score != b.score ? a.score > b.score
                                  : a.label.compare(b.label, Qt::CaseInsensitive) < 0;
    });

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Merge contact"));
    dialog.resize(460, 520);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(QString::fromUtf8(
        "Fold \xE2\x80\x9C%1\xE2\x80\x9D into the contact it duplicates. The one you pick "
        "keeps its details; this one fills its blanks, adds its tags, lists, notes and "
        "email history, and is then deleted.").arg(contactLabel(duplicate)));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *search = lineEdit();
    search->setPlaceholderText(QStringLiteral("Search contacts"));
    layout->addWidget(search);
    auto *list = new QListWidget;
    list->setObjectName(QStringLiteral("crmMergeTargets"));
    for (const Candidate &candidate : std::as_const(candidates)) {
        auto *item = new QListWidgetItem(
            candidate.detail.isEmpty()
                ? candidate.label
                : QString::fromUtf8("%1  \xC2\xB7  %2").arg(candidate.label, candidate.detail),
            list);
        item->setData(Qt::UserRole, candidate.id);
    }
    if (candidates.first().score > 0)
        list->setCurrentRow(0);
    layout->addWidget(list, 1);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton *mergeButton =
        buttons->addButton(QStringLiteral("Merge"), QDialogButtonBox::AcceptRole);
    mergeButton->setEnabled(list->currentItem() != nullptr);
    connect(list, &QListWidget::currentItemChanged, &dialog,
            [mergeButton](QListWidgetItem *current) {
        mergeButton->setEnabled(current && !current->isHidden());
    });
    connect(search, &QLineEdit::textChanged, &dialog, [list, mergeButton](const QString &query) {
        for (int row = 0; row < list->count(); ++row) {
            QListWidgetItem *item = list->item(row);
            item->setHidden(!item->text().contains(query.trimmed(), Qt::CaseInsensitive));
        }
        const QListWidgetItem *current = list->currentItem();
        mergeButton->setEnabled(current && !current->isHidden());
    });
    connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    search->setFocus();
    if (dialog.exec() != QDialog::Accepted || !list->currentItem() ||
        list->currentItem()->isHidden())
        return;
    mergeContacts({duplicateId}, list->currentItem()->data(Qt::UserRole).toString());
}

// One POST per duplicate, one after another: each folds into the record the
// previous merge already wrote, so none of them races another for the same row.
void CrmFeature::mergeContacts(const QStringList &duplicateIds, const QString &keepId)
{
    const QJsonObject keep = contactById(keepId);
    QStringList pending;
    QStringList names;
    for (const QString &id : duplicateIds) {
        const QJsonObject contact = contactById(id);
        if (id != keepId && !contact.isEmpty()) {
            pending << id;
            names << QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D").arg(contactLabel(contact));
        }
    }
    if (keep.isEmpty() || pending.isEmpty())
        return;
    const QString kept = QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D").arg(contactLabel(keep));
    if (QMessageBox::question(
            this, QStringLiteral("Merge CRM contacts"),
            QStringLiteral("Merge %1 into %2?\n\n%2 keeps what it has, fills its blanks from "
                           "%1 and gains %4 tags, lists, notes and email history. %1 %3 "
                           "then deleted.")
                .arg(names.join(QStringLiteral(", ")), kept,
                     pending.size() == 1 ? QStringLiteral("is") : QStringLiteral("are"),
                     pending.size() == 1 ? QStringLiteral("its") : QStringLiteral("their")))
        != QMessageBox::Yes)
        return;
    setStatus(QString::fromUtf8("Merging %1 into %2\xE2\x80\xA6")
                  .arg(names.join(QStringLiteral(", ")), kept));
    // Only the request in flight owns the chain, so it is freed once the last
    // reply (or the first failure) has been handled.
    const auto next = std::make_shared<std::function<void(int)>>();
    *next = [this, weak = std::weak_ptr(next), pending, keepId, kept](int at) {
        if (at >= pending.size()) {
            setStatus(QStringLiteral("Merged %1 %2 into %3.")
                          .arg(pending.size())
                          .arg(pending.size() == 1 ? QStringLiteral("contact")
                                                   : QStringLiteral("contacts"), kept));
            loadOrganization();
            return;
        }
        request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/contacts/merge")),
                QJsonObject{{QStringLiteral("from"), pending.at(at)},
                            {QStringLiteral("into"), keepId}},
                [this, next = weak.lock(), at, kept](bool ok, const QJsonObject &,
                                                     const QString &error) {
            if (!ok) {
                setStatus(QStringLiteral("Contacts were not merged into %1: %2")
                              .arg(kept, error), true);
                loadOrganization();
                return;
            }
            (*next)(at + 1);
        });
    };
    (*next)(0);
}

void CrmFeature::moveContactsToSheet(const QStringList &ids, const QString &sheetId)
{
    const QJsonObject sheet = sheetById(sheetId);
    if (sheet.isEmpty() || ids.isEmpty())
        return;
    const QString flowId = text(sheet, "flow");
    const QStringList stages = flowStages(flowId);
    QStringList siblings = pipelineLists(m_views).value(flowId);
    siblings.removeAll(sheetId);
    QList<QPair<QString, QJsonObject>> edits;
    for (const QString &id : ids) {
        const QJsonObject contact = contactById(id);
        if (contact.isEmpty())
            continue;
        QJsonObject body;
        if (!flowId.isEmpty()) {
            if (contactInSheet(contact, sheet))
                continue;
            if (text(contact, "flow") != flowId) {
                body.insert(QStringLiteral("flow"), flowId);
                const QString stage = text(contact, "stage");
                body.insert(QStringLiteral("stage"),
                            stages.contains(stage) || stages.isEmpty() ? stage
                                                                       : stages.first());
            }
            if (storedSheet(sheet)) {
                body.insert(QStringLiteral("views"), QJsonArray{sheetId});
                if (!siblings.isEmpty())
                    body.insert(QStringLiteral("removeViews"),
                                QJsonArray::fromStringList(siblings));
            }
        } else {
            if (contactInSheet(contact, sheet))
                continue;
            body.insert(QStringLiteral("views"), QJsonArray{sheetId});
        }
        edits.append({id, body});
    }
    const QString target = QString::fromUtf8("to \xE2\x80\x9C%1\xE2\x80\x9D")
                               .arg(text(sheet, "name"));
    if (edits.isEmpty()) {
        setStatus(QStringLiteral("Every selected contact is already in %1.")
                      .arg(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D")
                               .arg(text(sheet, "name"))));
        return;
    }
    const bool pipeline = !flowId.isEmpty();
    patchContacts(edits, pipeline ? QStringLiteral("Moving") : QStringLiteral("Adding"),
                  pipeline ? QStringLiteral("moved") : QStringLiteral("added"), target);
}

// "Move to list": the contacts end up on this list and no other. Belonging to
// a pipeline list is having its flow rather than a membership, so a pipeline
// is never a move target - moveContactsToSheet handles those.
void CrmFeature::moveContactsToListOnly(const QStringList &ids, const QString &sheetId)
{
    const QJsonObject sheet = sheetById(sheetId);
    if (sheet.isEmpty() || ids.isEmpty() || !text(sheet, "flow").isEmpty())
        return;
    QList<QPair<QString, QJsonObject>> edits;
    for (const QString &id : ids) {
        const QJsonObject contact = contactById(id);
        if (contact.isEmpty())
            continue;
        const QJsonArray memberships = contact.value(QStringLiteral("views")).toArray();
        if (memberships.size() == 1 && memberships.first().toString() == sheetId)
            continue;
        edits.append({id, QJsonObject{{QStringLiteral("setViews"),
                                       QJsonArray{sheetId}}}});
    }
    const QString name = text(sheet, "name");
    if (edits.isEmpty()) {
        setStatus(QStringLiteral("Every selected contact is already only in %1.")
                      .arg(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D").arg(name)));
        return;
    }
    patchContacts(edits, QStringLiteral("Moving"), QStringLiteral("moved"),
                  QString::fromUtf8("to \xE2\x80\x9C%1\xE2\x80\x9D only").arg(name));
}

// Taking a contact off a list leaves it in the organization, on every other
// list it is on: this is not a delete.
void CrmFeature::removeContactsFromSheet(const QStringList &ids, const QString &sheetId)
{
    const QJsonObject sheet = sheetById(sheetId);
    if (sheet.isEmpty() || ids.isEmpty())
        return;
    QList<QPair<QString, QJsonObject>> edits;
    for (const QString &id : ids) {
        const QJsonObject contact = contactById(id);
        if (contact.isEmpty())
            continue;
        bool member = false;
        for (const QJsonValue &value : contact.value(QStringLiteral("views")).toArray())
            member = member || value.toString() == sheetId;
        if (!member)
            continue;
        edits.append({id, QJsonObject{{QStringLiteral("removeViews"),
                                       QJsonArray{sheetId}}}});
    }
    const QString name = text(sheet, "name");
    if (edits.isEmpty()) {
        setStatus(QStringLiteral("No selected contact is on %1.")
                      .arg(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D").arg(name)));
        return;
    }
    patchContacts(edits, QStringLiteral("Removing"), QStringLiteral("removed"),
                  QString::fromUtf8("from \xE2\x80\x9C%1\xE2\x80\x9D").arg(name));
}

// Walking the selection along the pipeline it is already in - the gesture a
// CRM is used for all day, and the one the table made you open a dialog for.
void CrmFeature::setContactsStage(const QStringList &ids, const QString &stage)
{
    QList<QPair<QString, QJsonObject>> edits;
    for (const QString &id : ids) {
        const QJsonObject contact = contactById(id);
        if (contact.isEmpty() || text(contact, "stage") == stage)
            continue;
        edits.append({id, QJsonObject{{QStringLiteral("stage"), stage}}});
    }
    if (edits.isEmpty()) {
        setStatus(QStringLiteral("Every selected contact is already at %1.")
                      .arg(QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D").arg(stage)));
        return;
    }
    patchContacts(edits, QStringLiteral("Moving"), QStringLiteral("moved"),
                  QString::fromUtf8("to the \xE2\x80\x9C%1\xE2\x80\x9D stage")
                      .arg(stage));
}

// A bulk edit happens on the table first - the rows change at once - and then
// goes to the worker as one request, not a PATCH per contact, which a large
// selection turned into a burst the edge rate-limited. The reply's copies
// replace the local ones; a failure reloads the organization to put back what
// the worker still has. The shared refresh stands down meanwhile so it cannot
// repaint the old rows mid-edit.
void CrmFeature::patchContacts(const QList<QPair<QString, QJsonObject>> &edits,
                               const QString &gerund, const QString &past,
                               const QString &target)
{
    if (edits.isEmpty())
        return;
    QHash<QString, QJsonObject> byId;
    QJsonArray payload;
    for (const auto &[id, body] : std::as_const(edits)) {
        byId.insert(id, body);
        QJsonObject edit = body;
        edit.insert(QStringLiteral("id"), id);
        payload.append(edit);
    }
    for (qsizetype at = 0; at < m_contacts.size(); ++at) {
        const QJsonObject contact = m_contacts.at(at).toObject();
        const auto edit = byId.constFind(text(contact, "id"));
        if (edit != byId.constEnd())
            m_contacts.replace(at, withContactEdit(contact, *edit));
    }
    refreshContactSnapshot();
    const int count = int(edits.size());
    const QString noun = count == 1 ? QStringLiteral("contact") : QStringLiteral("contacts");
    setStatus(QString::fromUtf8("%1 %2 %3 %4\xE2\x80\xA6")
                  .arg(gerund).arg(count).arg(noun, target));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/contacts/bulk")),
            QJsonObject{{QStringLiteral("edits"), payload}},
            [this, count, noun, past, target](bool ok, const QJsonObject &reply,
                                              const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("%1 %2 were not %3 %4: %5")
                          .arg(count).arg(noun, past, target, error), true);
            loadOrganization();
            return;
        }
        QHash<QString, QJsonObject> saved;
        for (const QJsonValue &value : reply.value(QStringLiteral("contacts")).toArray())
            saved.insert(text(value.toObject(), "id"), value.toObject());
        for (qsizetype at = 0; at < m_contacts.size(); ++at) {
            const auto copy = saved.constFind(text(m_contacts.at(at).toObject(), "id"));
            if (copy != saved.constEnd())
                m_contacts.replace(at, *copy);
        }
        refreshContactSnapshot();
        const int missing = int(reply.value(QStringLiteral("missing")).toArray().size());
        if (missing > 0) {
            // Someone deleted them since the table loaded.
            setStatus(QStringLiteral("%1 of %2 contacts were not %3 %4: they no longer exist.")
                          .arg(missing).arg(count).arg(past, target), true);
            loadOrganization();
            return;
        }
        setStatus(QStringLiteral("%1 %2 %3 %4.").arg(count).arg(noun, past, target));
    });
}

// Last contacted set by hand: the cell's date picker, its Now button, or the
// row menu's "Mark contacted now" over a whole selection.
void CrmFeature::markContacted(const QStringList &ids, qint64 stamp)
{
    QList<QPair<QString, QJsonObject>> edits;
    for (const QString &id : ids) {
        if (!id.isEmpty())
            edits.append({id, QJsonObject{{QStringLiteral("lastContactedAt"), QJsonValue(stamp)}}});
    }
    patchContacts(edits, QStringLiteral("Marking"), QStringLiteral("marked"),
                  QStringLiteral("as last contacted %1").arg(timestamp(stamp)));
}

void CrmFeature::openSheet(const QString &id)
{
    const int index = sheetTabIndex(id);
    if (index >= 0)
        m_tabs->setCurrentIndex(index);
}

void CrmFeature::renameSheet(const QString &id)
{
    const QJsonObject target = sheetById(id);
    const QString previous = text(target, "name");
    if (previous.isEmpty() || id == QLatin1String(kNoPipelineTabId))
        return;
    // A pipeline tab no list is bound to is the flow itself, so renaming it
    // renames the pipeline in the shared settings, which owners and admins keep.
    const bool pipeline = !storedSheet(target);
    if (pipeline && !m_canManageSettings) {
        setStatus(QStringLiteral("Only organization owners and admins can rename pipelines."),
                  true);
        return;
    }
    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, pipeline ? QStringLiteral("Rename pipeline") : QStringLiteral("Rename list"),
        QString::fromUtf8("Rename the \xE2\x80\x9C%1\xE2\x80\x9D %2")
            .arg(previous, pipeline ? QStringLiteral("pipeline") : QStringLiteral("list")),
        QLineEdit::Normal, previous, &accepted).trimmed().left(60);
    if (!accepted || name.isEmpty() || name == previous)
        return;
    if (pipeline) {
        QJsonArray flows = m_settings.value(QStringLiteral("flows")).toArray();
        for (qsizetype at = 0; at < flows.size(); ++at) {
            QJsonObject flow = flows.at(at).toObject();
            if (text(flow, "id") != text(target, "flow"))
                continue;
            flow.insert(QStringLiteral("name"), name);
            flows.replace(at, flow);
        }
        m_settings.insert(QStringLiteral("flows"), flows);
        rebuildVirtualSheets();
        renderSheetPicker();
        saveFlows(flows, QString::fromUtf8("Pipeline renamed to \xE2\x80\x9C%1\xE2\x80\x9D.")
                             .arg(name));
        return;
    }
    for (int at = 0; at < m_views.size(); ++at) {
        QJsonObject sheet = m_views.at(at).toObject();
        if (text(sheet, "id") != id)
            continue;
        sheet.insert(QStringLiteral("name"), name);
        m_views.replace(at, sheet);
    }
    renderSheets();
    renderSheetPicker();
    request(QByteArrayLiteral("PATCH"), organizationPath(QStringLiteral("/views/") + id),
            QJsonObject{{QStringLiteral("name"), name}},
            [this, name](bool ok, const QJsonObject &, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("The list was not renamed: %1").arg(error), true);
            loadOrganization();
            return;
        }
        setStatus(QString::fromUtf8("List renamed to \xE2\x80\x9C%1\xE2\x80\x9D.").arg(name));
    });
}

// List order is the organization's, not this window's: a move is the order
// every member then sees, here and on the web page's tabs.
void CrmFeature::moveSheet(const QString &id, int offset)
{
    QStringList order;
    for (const QJsonValue &value : m_views)
        order << text(value.toObject(), "id");
    const qsizetype from = order.indexOf(id);
    const qsizetype to = from + offset;
    if (from < 0 || to < 0 || to >= order.size())
        return;
    order.move(from, to);
    QJsonArray reordered;
    for (const QString &sheetId : order)
        reordered.append(sheetById(sheetId));
    m_views = reordered;
    renderSheets();
    m_sheetsTable->selectRow(int(to));
    renderSheetPicker();
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/views/order")),
            QJsonObject{{QStringLiteral("order"), QJsonArray::fromStringList(order)}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("The list was not moved: %1").arg(error), true);
            loadOrganization();
            return;
        }
        if (payload.value(QStringLiteral("views")).isArray()) {
            m_views = payload.value(QStringLiteral("views")).toArray();
            renderSheets();
            renderSheetPicker();
        }
    });
}

// Deleting a pipeline is for the whole organization: its flow leaves the
// shared settings and every list bound to it is deleted, so its tab goes for
// everyone. The contacts in it stay, under "No pipeline", until they are moved
// into another one.
void CrmFeature::deletePipeline(const QString &id)
{
    const QString flowId = text(sheetById(id), "flow");
    if (flowId.isEmpty())
        return;
    if (!m_canManageSettings) {
        setStatus(QStringLiteral("Only organization owners and admins can delete pipelines."),
                  true);
        return;
    }
    const QJsonArray flows = m_settings.value(QStringLiteral("flows")).toArray();
    QJsonArray remaining;
    for (const QJsonValue &value : flows) {
        if (text(value.toObject(), "id") != flowId)
            remaining.append(value);
    }
    // The worker answers an empty set of pipelines with its four defaults.
    if (remaining.isEmpty()) {
        setStatus(QStringLiteral("An organization keeps at least one pipeline. Add "
                                 "another before deleting this one."), true);
        return;
    }
    QStringList lists;
    for (const QJsonValue &value : std::as_const(m_views)) {
        if (text(value.toObject(), "flow") == flowId)
            lists << text(value.toObject(), "id");
    }
    const QString name = flowLabel(flowId);
    const int contacts = flowContactCount(flowId);
    const QString people = contacts == 0
        ? QStringLiteral("No contact is in it.")
        : contacts == 1
        ? QStringLiteral("Its contact stays in the organization, under No pipeline, "
                         "until it is moved into another pipeline.")
        : QStringLiteral("Its %1 contacts stay in the organization, under No pipeline, "
                         "until they are moved into another pipeline.").arg(contacts);
    const QString bound = lists.isEmpty() ? QString()
        : lists.size() == 1 ? QStringLiteral(" The list bound to it is deleted too.")
        : QStringLiteral(" The %1 lists bound to it are deleted too.").arg(lists.size());
    if (QMessageBox::question(
            this, QStringLiteral("Delete pipeline"),
            QString::fromUtf8("Delete the \xE2\x80\x9C%1\xE2\x80\x9D pipeline for everyone "
                              "in %2?\n\n%3%4")
                .arg(name, m_organization->currentData().toString(), people, bound),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    // The tab goes at once; a failure reloads what the worker still has.
    const bool inSettings = remaining.size() != flows.size();
    m_settings.insert(QStringLiteral("flows"), remaining);
    for (qsizetype at = m_views.size() - 1; at >= 0; --at) {
        if (lists.contains(text(m_views.at(at).toObject(), "id")))
            m_views.removeAt(at);
    }
    rebuildVirtualSheets();
    renderSheets();
    refreshContactSnapshot();
    setStatus(QString::fromUtf8("Deleting the \xE2\x80\x9C%1\xE2\x80\x9D pipeline\xE2\x80\xA6")
                  .arg(name));
    const auto deleteLists = [this, lists, name] {
        if (lists.isEmpty()) {
            setStatus(QString::fromUtf8("Pipeline \xE2\x80\x9C%1\xE2\x80\x9D deleted.").arg(name));
            loadOrganization();
            return;
        }
        const auto left = std::make_shared<int>(int(lists.size()));
        const auto failure = std::make_shared<QString>();
        for (const QString &listId : lists) {
            request(QByteArrayLiteral("DELETE"),
                    organizationPath(QStringLiteral("/views/") + listId), {},
                    [this, left, failure, name](bool ok, const QJsonObject &,
                                                const QString &error) {
                if (!ok && failure->isEmpty())
                    *failure = error;
                if (--*left > 0)
                    return;
                if (failure->isEmpty())
                    setStatus(QString::fromUtf8("Pipeline \xE2\x80\x9C%1\xE2\x80\x9D deleted.")
                                  .arg(name));
                else
                    setStatus(QString::fromUtf8(
                        "The \xE2\x80\x9C%1\xE2\x80\x9D pipeline was deleted, but a list "
                        "bound to it was not: %2").arg(name, *failure), true);
                loadOrganization();
            });
        }
    };
    if (!inSettings) {
        deleteLists();
        return;
    }
    request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")),
            QJsonObject{{QStringLiteral("flows"), remaining}},
            [this, name, deleteLists](bool ok, const QJsonObject &, const QString &error) {
        if (!ok) {
            setStatus(QString::fromUtf8("The \xE2\x80\x9C%1\xE2\x80\x9D pipeline was not "
                                        "deleted: %2").arg(name, error), true);
            loadOrganization();
            return;
        }
        deleteLists();
    });
}

// A deleted list's tab goes for everyone it reaches; the contacts on it stay
// in the organization and on their pipelines.
void CrmFeature::deleteList(const QString &id)
{
    const QJsonObject sheet = sheetById(id);
    if (!storedSheet(sheet))
        return;
    const QString name = text(sheet, "name");
    const QStringList siblings = pipelineLists(m_views).value(text(sheet, "flow"));
    if (!text(sheet, "flow").isEmpty() && siblings.size() < 2)
        return;
    if (QMessageBox::question(
            this, QStringLiteral("Delete list"),
            QString::fromUtf8("Delete the \xE2\x80\x9C%1\xE2\x80\x9D list? Its contacts stay "
                              "in the organization and on their pipelines.").arg(name),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    for (qsizetype at = m_views.size() - 1; at >= 0; --at) {
        if (text(m_views.at(at).toObject(), "id") == id)
            m_views.removeAt(at);
    }
    rebuildVirtualSheets();
    renderSheets();
    refreshContactSnapshot();
    request(QByteArrayLiteral("DELETE"), organizationPath(QStringLiteral("/views/") + id), {},
            [this, name](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            setStatus(QString::fromUtf8("The \xE2\x80\x9C%1\xE2\x80\x9D list was not deleted: %2")
                          .arg(name, error), true);
        else
            setStatus(QString::fromUtf8("List \xE2\x80\x9C%1\xE2\x80\x9D deleted.").arg(name));
        loadOrganization();
    });
}

// A pipeline is a new flow in the shared settings plus a list bound to it, so
// it opens as its own list of every contact in the flow.
void CrmFeature::newPipeline()
{
    if (!m_canManageSettings) {
        setStatus(QStringLiteral("Only organization owners and admins can add pipelines."),
                  true);
        return;
    }
    const QJsonArray flows = m_settings.value(QStringLiteral("flows")).toArray();
    if (flows.size() >= kMaxFlows) {
        setStatus(QStringLiteral("An organization can have %1 pipelines. Remove one in "
                                 "CRM settings first.").arg(kMaxFlows), true);
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("New pipeline"));
    dialog.resize(480, 380);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(QStringLiteral(
        "A pipeline is a flow with ordered stages. It opens as its own list "
        "showing every contact in it, and the browser extension can add "
        "contacts to it from Gmail or Zoho Mail."));
    intro->setObjectName(QStringLiteral("mutedLabel"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    auto *name = lineEdit();
    name->setMaxLength(60);
    name->setPlaceholderText(QString::fromUtf8("Investors, Hiring, Partnerships\xE2\x80\xA6"));
    auto *type = new QComboBox;
    for (const FlowIconKind &kind : kFlowIconKinds)
        type->addItem(flowTypeIcon(QLatin1String(kind.id)), QLatin1String(kind.label),
                      QLatin1String(kind.id));
    auto *stages = new QPlainTextEdit(QStringLiteral("new\nin-progress\ncomplete"));
    stages->setMinimumHeight(120);
    auto *stagesHint = new QLabel(QStringLiteral(
        "One line per stage. Rename or reorder them later in Manage pipelines."));
    stagesHint->setObjectName(QStringLiteral("mutedLabel"));
    stagesHint->setWordWrap(true);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Type"), type);
    form->addRow(QStringLiteral("Ordered stages"), stages);
    form->addRow(QString(), stagesHint);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton *create = buttons->button(QDialogButtonBox::Ok);
    create->setText(QStringLiteral("Create pipeline"));
    create->setEnabled(false);
    connect(name, &QLineEdit::textChanged, &dialog, [create](const QString &value) {
        create->setEnabled(!value.trimmed().isEmpty());
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    name->setFocus();
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QString label = name->text().trimmed();
    const QString icon = type->currentData().toString();
    QStringList stageNames;
    for (const QString &stage :
         stages->toPlainText().split(QRegularExpression(QStringLiteral("[\\n,]+")))) {
        if (!stage.trimmed().isEmpty())
            stageNames << stage.trimmed();
    }
    if (stageNames.isEmpty())
        stageNames << QStringLiteral("lead");
    const QString base = flowSlug(label);
    QStringList taken;
    for (const QJsonValue &flow : flows)
        taken << text(flow.toObject(), "id");
    QString flowId = base;
    for (int suffix = 2; taken.contains(flowId); ++suffix)
        flowId = QStringLiteral("%1-%2").arg(base).arg(suffix);
    QJsonArray next = flows;
    next.append(QJsonObject{{QStringLiteral("id"), flowId},
                            {QStringLiteral("name"), label},
                            {QStringLiteral("icon"), icon},
                            {QStringLiteral("stages"), QJsonArray::fromStringList(stageNames)}});
    m_pipelineButton->setEnabled(false);
    setStatus(QString::fromUtf8("Creating the \xE2\x80\x9C%1\xE2\x80\x9D pipeline\xE2\x80\xA6")
                  .arg(label));
    request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")),
            QJsonObject{{QStringLiteral("flows"), next}},
            [this, flowId, label](bool ok, const QJsonObject &, const QString &error) {
        if (!ok) {
            m_pipelineButton->setEnabled(m_canManageSettings);
            setStatus(QStringLiteral("The pipeline was not created: %1").arg(error), true);
            return;
        }
        request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/views")),
                QJsonObject{{QStringLiteral("name"), label},
                            {QStringLiteral("flow"), flowId}},
                [this, label](bool created, const QJsonObject &payload,
                              const QString &viewError) {
            m_pipelineButton->setEnabled(m_canManageSettings);
            if (!created) {
                setStatus(QString::fromUtf8(
                    "\xE2\x80\x9C%1\xE2\x80\x9D was added to the flows, but its list "
                    "was not created: %2").arg(label, viewError), true);
                loadOrganization();
                return;
            }
            m_pendingSheet = text(payload.value(QStringLiteral("view")).toObject(), "id");
            setStatus(QString::fromUtf8("Pipeline \xE2\x80\x9C%1\xE2\x80\x9D created.")
                          .arg(label));
            loadOrganization();
        });
    });
}

QStringList CrmFeature::flowStages(const QString &flowId) const
{
    if (flowId.isEmpty())
        return {};
    for (const QJsonValue &value : m_settings.value(QStringLiteral("flows")).toArray()) {
        const QJsonObject flow = value.toObject();
        if (text(flow, "id") != flowId)
            continue;
        QStringList stages;
        for (const QJsonValue &stage : flow.value(QStringLiteral("stages")).toArray())
            stages << stage.toString();
        return stages;
    }
    return {};
}

QString CrmFeature::flowLabel(const QString &flowId) const
{
    for (const QJsonValue &value : m_settings.value(QStringLiteral("flows")).toArray()) {
        const QJsonObject flow = value.toObject();
        if (text(flow, "id") == flowId)
            return text(flow, "name");
    }
    return flowId;
}

QString CrmFeature::flowIconId(const QString &flowId) const
{
    for (const QJsonValue &value : m_settings.value(QStringLiteral("flows")).toArray()) {
        const QJsonObject flow = value.toObject();
        if (text(flow, "id") == flowId)
            return QLatin1String(flowIconKind(text(flow, "icon")).id);
    }
    return QLatin1String(kFlowIconKinds[0].id);
}

QIcon CrmFeature::flowIcon(const QString &flowId) const
{
    return flowTypeIcon(flowIconId(flowId));
}

void CrmFeature::addEditStagesAction(QMenu *menu, const QString &flowId)
{
    QAction *edit = menu->addAction(
        flowIcon(flowId),
        QString::fromUtf8("Edit \xE2\x80\x9C%1\xE2\x80\x9D stages\xE2\x80\xA6")
            .arg(flowLabel(flowId)),
        this, [this, flowId] { managePipelines(flowId); });
    edit->setEnabled(m_canManageSettings);
    if (!m_canManageSettings)
        edit->setToolTip(QStringLiteral(
            "Only organization owners and admins can edit pipelines"));
}

int CrmFeature::flowContactCount(const QString &flowId) const
{
    int count = 0;
    for (const QJsonValue &value : m_contacts) {
        if (text(value.toObject(), "flow") == flowId)
            ++count;
    }
    return count;
}

// Binding a list to a pipeline makes it show every contact in that flow, not
// only the ones put on it; clearing the binding makes it a plain list again
// and leaves those memberships untouched.
void CrmFeature::setSheetFlow(const QString &sheetId, const QString &flowId)
{
    const QJsonObject sheet = sheetById(sheetId);
    if (sheet.isEmpty() || text(sheet, "flow") == flowId)
        return;
    const QString name = text(sheet, "name");
    request(QByteArrayLiteral("PATCH"),
            organizationPath(QStringLiteral("/views/") + sheetId),
            QJsonObject{{QStringLiteral("flow"), flowId}},
            [this, name, flowId](bool ok, const QJsonObject &, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("The pipeline type was not changed: %1").arg(error),
                      true);
            loadOrganization();
            return;
        }
        setStatus(flowId.isEmpty()
            ? QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D is a plain list again; it "
                                "shows only the contacts added to it.").arg(name)
            : QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D now shows the "
                                "\xE2\x80\x9C%2\xE2\x80\x9D pipeline.")
                  .arg(name, flowLabel(flowId)));
        loadOrganization();
    });
}

void CrmFeature::loadRankedGrants(const QString &sheetId)
{
    const QJsonObject sheet = sheetById(sheetId);
    const QString flowId = text(sheet, "flow");
    if (flowId.isEmpty() || importBusy())
        return;
    m_grantsLoading = true;
    setImportButtonsEnabled(false);
    setStatus(QString::fromUtf8("Loading the ranked grants\xE2\x80\xA6"));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/contacts/grants")),
            QJsonObject{{QStringLiteral("flow"), flowId},
                        {QStringLiteral("viewId"),
                         storedSheet(sheet) ? sheetId : QString()}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_grantsLoading = false;
        setImportButtonsEnabled(true);
        if (!ok) {
            setStatus(QStringLiteral("The grants were not loaded: %1").arg(error), true);
            loadOrganization();
            return;
        }
        const int created = payload.value(QStringLiteral("created")).toInt();
        const QJsonObject view = payload.value(QStringLiteral("view")).toObject();
        if (!text(view, "id").isEmpty())
            m_pendingSheet = text(view, "id");
        setStatus(created
            ? QStringLiteral("%1: %2 grants loaded.")
                  .arg(text(view, "name").isEmpty() ? QStringLiteral("Grants")
                                                    : text(view, "name"))
                  .arg(created)
            : QStringLiteral("Every ranked grant is already on this pipeline."));
        loadOrganization();
    });
}

void CrmFeature::saveFlows(const QJsonArray &flows, const QString &note)
{
    request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")),
            QJsonObject{{QStringLiteral("flows"), flows}},
            [this, note](bool ok, const QJsonObject &, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("Pipelines were not saved: %1").arg(error), true);
            loadOrganization();
            return;
        }
        setStatus(note);
        loadOrganization();
    });
}

// Every pipeline the organization has, edited in one place: its name, the
// type its icon shows, its ordered stages, and on the Lists page which
// pipeline each list shows. A renamed stage takes the contacts at it along,
// so nobody is left at a stage the pipeline no longer has.
void CrmFeature::managePipelines(const QString &focusFlow)
{
    if (!m_canManageSettings) {
        setStatus(QStringLiteral("Only organization owners and admins can edit "
                                 "pipelines."), true);
        return;
    }
    struct DraftStage
    {
        QString original; // the stored slug; empty for a stage added here
        QString label;
    };
    struct DraftFlow
    {
        QString key; // stable while the dialog is open, even for a new pipeline
        QString id;  // empty until saving mints one from the name
        QString name;
        QString icon;
        QList<DraftStage> stages;
    };
    const auto drafts = std::make_shared<QList<DraftFlow>>();
    for (const QJsonValue &value : m_settings.value(QStringLiteral("flows")).toArray()) {
        const QJsonObject flow = value.toObject();
        DraftFlow draft{QUuid::createUuid().toString(QUuid::WithoutBraces), text(flow, "id"),
                        text(flow, "name"), flowIconId(text(flow, "id")), {}};
        for (const QString &stage : flowStages(draft.id))
            draft.stages.append({stage, CrmStageStrip::stageLabel(stage)});
        drafts->append(draft);
    }

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Pipelines"));
    dialog.resize(860, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(QStringLiteral(
        "A pipeline is an ordered set of stages contacts move through, and its type "
        "is the icon on its tab. A list bound to a pipeline shows every contact in "
        "it. Removing a pipeline leaves the contacts in it without one."));
    intro->setObjectName(QStringLiteral("mutedLabel"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *pages = new QTabWidget;
    layout->addWidget(pages, 1);

    // Pipelines and stages: the pipelines on the left, the picked one's name,
    // type and stages on the right.
    auto *pipelinesPage = new QWidget;
    auto *pipelinesLayout = new QHBoxLayout(pipelinesPage);
    auto *left = new QVBoxLayout;
    auto *list = new QListWidget;
    list->setObjectName(QStringLiteral("crmPipelineList"));
    list->setIconSize(QSize(16, 16));
    left->addWidget(list, 1);
    auto *listButtons = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add"));
    auto *up = new QPushButton(QStringLiteral("Up"));
    auto *down = new QPushButton(QStringLiteral("Down"));
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    remove->setObjectName(QStringLiteral("dangerButton"));
    for (QPushButton *button : {add, up, down, remove})
        listButtons->addWidget(button);
    left->addLayout(listButtons);
    pipelinesLayout->addLayout(left, 2);

    auto *editor = new QWidget;
    auto *form = new QFormLayout(editor);
    form->setContentsMargins(0, 0, 0, 0);
    auto *name = lineEdit();
    name->setMaxLength(60);
    auto *type = new QComboBox;
    type->setObjectName(QStringLiteral("crmPipelineType"));
    for (const FlowIconKind &kind : kFlowIconKinds)
        type->addItem(flowTypeIcon(QLatin1String(kind.id)), QLatin1String(kind.label),
                      QLatin1String(kind.id));
    auto *stages = new QListWidget;
    stages->setObjectName(QStringLiteral("crmPipelineStages"));
    stages->setDragDropMode(QAbstractItemView::InternalMove);
    stages->setDefaultDropAction(Qt::MoveAction);
    stages->setEditTriggers(QAbstractItemView::DoubleClicked |
                            QAbstractItemView::EditKeyPressed);
    auto *stageButtons = new QHBoxLayout;
    auto *addStage = new QPushButton(QStringLiteral("Add stage"));
    auto *renameStage = new QPushButton(QStringLiteral("Rename"));
    auto *stageUp = new QPushButton(QStringLiteral("Up"));
    auto *stageDown = new QPushButton(QStringLiteral("Down"));
    auto *removeStage = new QPushButton(QStringLiteral("Remove"));
    removeStage->setObjectName(QStringLiteral("dangerButton"));
    for (QPushButton *button : {addStage, renameStage, stageUp, stageDown, removeStage})
        stageButtons->addWidget(button);
    stageButtons->addStretch(1);
    auto *stageBox = new QVBoxLayout;
    stageBox->setContentsMargins(0, 0, 0, 0);
    stageBox->addWidget(stages, 1);
    stageBox->addLayout(stageButtons);
    auto *stageHint = new QLabel(QStringLiteral(
        "In order, first to last. Double-click a stage to rename it or drag it to "
        "reorder; contacts at a renamed stage move with it."));
    stageHint->setObjectName(QStringLiteral("mutedLabel"));
    stageHint->setWordWrap(true);
    auto *boundLabel = new QLabel;
    boundLabel->setObjectName(QStringLiteral("mutedLabel"));
    boundLabel->setWordWrap(true);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Type"), type);
    form->addRow(QStringLiteral("Stages"), stageBox);
    form->addRow(QString(), stageHint);
    form->addRow(QStringLiteral("Lists"), boundLabel);
    pipelinesLayout->addWidget(editor, 3);
    pages->addTab(pipelinesPage, QStringLiteral("Pipelines and stages"));

    // Lists: which pipeline each of the organization's lists shows.
    auto *listsPage = new QWidget;
    auto *listsLayout = new QVBoxLayout(listsPage);
    auto *listsNote = new QLabel(QStringLiteral(
        "Pick the pipeline each list shows. A list bound to a pipeline shows every "
        "contact in it; a plain list shows only the contacts added to it."));
    listsNote->setObjectName(QStringLiteral("mutedLabel"));
    listsNote->setWordWrap(true);
    listsLayout->addWidget(listsNote);
    auto *lists = new QTableWidget(0, 3);
    lists->setObjectName(QStringLiteral("crmPipelineLists"));
    lists->setHorizontalHeaderLabels({QStringLiteral("List"), QStringLiteral("Owner"),
                                      QStringLiteral("Pipeline")});
    lists->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    lists->setColumnWidth(1, 140);
    lists->setColumnWidth(2, 260);
    lists->verticalHeader()->setVisible(false);
    lists->setSelectionMode(QAbstractItemView::NoSelection);
    lists->setEditTriggers(QAbstractItemView::NoEditTriggers);
    listsLayout->addWidget(lists, 1);
    pages->addTab(listsPage, QStringLiteral("Lists"));
    const QIcon listIcon = plainListIcon(palette().color(QPalette::WindowText));
    for (const QJsonValue &value : std::as_const(m_views)) {
        const QJsonObject view = value.toObject();
        QString key;
        for (const DraftFlow &draft : std::as_const(*drafts)) {
            if (!draft.id.isEmpty() && draft.id == text(view, "flow"))
                key = draft.key;
        }
        const int row = lists->rowCount();
        lists->insertRow(row);
        auto *listName = new QTableWidgetItem(text(view, "name"));
        listName->setData(Qt::UserRole, text(view, "id"));
        lists->setItem(row, 0, listName);
        lists->setItem(row, 1, new QTableWidgetItem(text(view, "owner")));
        auto *picker = new QComboBox;
        picker->setProperty("crmInitialKey", key);
        picker->addItem(QString(), key);
        lists->setCellWidget(row, 2, picker);
    }
    if (lists->rowCount() == 0) {
        listsNote->setText(listsNote->text() + QStringLiteral(
            "\n\nThe organization has no lists yet; each pipeline gets its own tab."));
    }

    const auto shown = std::make_shared<int>(-1);
    const auto loading = std::make_shared<bool>(false);
    // The pickers on the Lists page follow the pipelines as they are added,
    // renamed, retyped and removed here, keeping each list's choice.
    const auto refreshListPickers = [lists, drafts, listIcon] {
        for (int row = 0; row < lists->rowCount(); ++row) {
            auto *picker = qobject_cast<QComboBox *>(lists->cellWidget(row, 2));
            if (!picker)
                continue;
            const QSignalBlocker blocker(picker);
            const QString key = picker->currentData().toString();
            picker->clear();
            picker->addItem(listIcon, QStringLiteral("No pipeline (plain list)"), QString());
            for (const DraftFlow &draft : std::as_const(*drafts))
                picker->addItem(flowTypeIcon(draft.icon), draft.name, draft.key);
            picker->setCurrentIndex(qMax(0, picker->findData(key)));
        }
    };
    const auto boundNames = [lists](const QString &key) {
        QStringList names;
        for (int row = 0; row < lists->rowCount(); ++row) {
            auto *picker = qobject_cast<QComboBox *>(lists->cellWidget(row, 2));
            if (picker && picker->currentData().toString() == key)
                names << lists->item(row, 0)->text();
        }
        return names;
    };
    const auto syncStages = [drafts, shown, loading, stages] {
        if (*loading || *shown < 0 || *shown >= drafts->size())
            return;
        QList<DraftStage> next;
        for (int row = 0; row < stages->count(); ++row) {
            const QListWidgetItem *item = stages->item(row);
            next.append({item->data(Qt::UserRole).toString(), item->text().trimmed()});
        }
        (*drafts)[*shown].stages = next;
    };
    const auto stageItem = [](const DraftStage &stage) {
        auto *item = new QListWidgetItem(stage.label);
        item->setData(Qt::UserRole, stage.original);
        item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled);
        return item;
    };
    const auto showDraft = [this, drafts, shown, loading, editor, name, type, stages,
                            boundLabel, boundNames, stageItem](int row) {
        *loading = true;
        *shown = row >= 0 && row < drafts->size() ? row : -1;
        editor->setEnabled(*shown >= 0);
        stages->clear();
        if (*shown < 0) {
            name->clear();
            boundLabel->clear();
            *loading = false;
            return;
        }
        const DraftFlow &draft = drafts->at(*shown);
        name->setText(draft.name);
        type->setCurrentIndex(qMax(0, type->findData(draft.icon)));
        for (const DraftStage &stage : draft.stages)
            stages->addItem(stageItem(stage));
        const QStringList bound = boundNames(draft.key);
        const int contacts = draft.id.isEmpty() ? 0 : flowContactCount(draft.id);
        const QString people = contacts == 1 ? QStringLiteral("1 contact")
                                             : QStringLiteral("%1 contacts").arg(contacts);
        boundLabel->setText(bound.isEmpty()
            ? QString::fromUtf8("%1 \xC2\xB7 no list is bound to it, so it has a tab of "
                                "its own. Bind lists on the Lists page.").arg(people)
            : QString::fromUtf8("%1 \xC2\xB7 shown on %2").arg(people, bound.join(QStringLiteral(", "))));
        *loading = false;
    };
    for (const DraftFlow &draft : std::as_const(*drafts))
        list->addItem(new QListWidgetItem(flowTypeIcon(draft.icon), draft.name));
    refreshListPickers();

    connect(list, &QListWidget::currentRowChanged, &dialog, showDraft);
    connect(pages, &QTabWidget::currentChanged, &dialog,
            [list, showDraft, refreshListPickers](int page) {
        if (page == 1)
            refreshListPickers();
        else
            showDraft(list->currentRow());
    });
    connect(name, &QLineEdit::textChanged, &dialog,
            [drafts, shown, loading, list](const QString &value) {
        if (*loading || *shown < 0)
            return;
        (*drafts)[*shown].name = value.trimmed();
        list->item(*shown)->setText(value.trimmed().isEmpty() ? QStringLiteral("(unnamed)")
                                                              : value.trimmed());
    });
    connect(type, &QComboBox::currentIndexChanged, &dialog,
            [drafts, shown, loading, list, type] {
        if (*loading || *shown < 0)
            return;
        (*drafts)[*shown].icon = type->currentData().toString();
        list->item(*shown)->setIcon(flowTypeIcon(type->currentData().toString()));
    });
    connect(stages, &QListWidget::itemChanged, &dialog, [syncStages] { syncStages(); });
    connect(stages->model(), &QAbstractItemModel::rowsMoved, &dialog,
            [syncStages] { syncStages(); });

    connect(add, &QPushButton::clicked, &dialog, [&dialog, drafts, list, name] {
        if (drafts->size() >= kMaxFlows) {
            QMessageBox::information(
                &dialog, QStringLiteral("Pipelines"),
                QStringLiteral("An organization can have %1 pipelines. Remove one "
                               "first.").arg(kMaxFlows));
            return;
        }
        DraftFlow draft{QUuid::createUuid().toString(QUuid::WithoutBraces), QString(),
                        QStringLiteral("New pipeline"), QStringLiteral("pipeline"),
                        {{QString(), QStringLiteral("New")},
                         {QString(), QStringLiteral("In progress")},
                         {QString(), QStringLiteral("Complete")}}};
        drafts->append(draft);
        list->addItem(new QListWidgetItem(flowTypeIcon(draft.icon), draft.name));
        list->setCurrentRow(list->count() - 1);
        name->setFocus();
        name->selectAll();
    });
    const auto movePipeline = [drafts, list, showDraft](int offset) {
        const int from = list->currentRow();
        const int to = from + offset;
        if (from < 0 || to < 0 || to >= list->count())
            return;
        drafts->swapItemsAt(from, to);
        QListWidgetItem *item = list->takeItem(from);
        list->insertItem(to, item);
        list->setCurrentRow(to);
        showDraft(to);
    };
    connect(up, &QPushButton::clicked, &dialog, [movePipeline] { movePipeline(-1); });
    connect(down, &QPushButton::clicked, &dialog, [movePipeline] { movePipeline(1); });
    connect(remove, &QPushButton::clicked, &dialog,
            [this, &dialog, drafts, list, shown, showDraft, boundNames, refreshListPickers] {
        const int at = list->currentRow();
        if (at < 0 || at >= drafts->size())
            return;
        if (drafts->size() == 1) {
            QMessageBox::information(&dialog, QStringLiteral("Pipelines"),
                                     QStringLiteral("An organization keeps at least one "
                                                    "pipeline. Add another first."));
            return;
        }
        const DraftFlow draft = drafts->at(at);
        const int contacts = draft.id.isEmpty() ? 0 : flowContactCount(draft.id);
        const int bound = int(boundNames(draft.key).size());
        if ((contacts > 0 || bound > 0) && QMessageBox::question(
                &dialog, QStringLiteral("Remove pipeline"),
                QString::fromUtf8(
                    "%1 contacts are in \xE2\x80\x9C%2\xE2\x80\x9D and %3 lists show it. "
                    "Removing it leaves those contacts in the organization without a "
                    "pipeline, and the lists become plain lists. Remove it?")
                    .arg(contacts).arg(draft.name).arg(bound)) != QMessageBox::Yes)
            return;
        drafts->removeAt(at);
        *shown = -1;
        delete list->takeItem(at);
        refreshListPickers();
        showDraft(list->currentRow());
    });

    connect(addStage, &QPushButton::clicked, &dialog, [stages, stageItem, syncStages] {
        QListWidgetItem *item = stageItem({QString(), QStringLiteral("New stage")});
        stages->addItem(item);
        syncStages();
        stages->setCurrentItem(item);
        stages->editItem(item);
    });
    connect(renameStage, &QPushButton::clicked, &dialog, [stages] {
        if (QListWidgetItem *item = stages->currentItem())
            stages->editItem(item);
    });
    const auto moveStage = [stages, syncStages](int offset) {
        const int from = stages->currentRow();
        const int to = from + offset;
        if (from < 0 || to < 0 || to >= stages->count())
            return;
        QListWidgetItem *item = stages->takeItem(from);
        stages->insertItem(to, item);
        stages->setCurrentRow(to);
        syncStages();
    };
    connect(stageUp, &QPushButton::clicked, &dialog, [moveStage] { moveStage(-1); });
    connect(stageDown, &QPushButton::clicked, &dialog, [moveStage] { moveStage(1); });
    connect(removeStage, &QPushButton::clicked, &dialog,
            [this, &dialog, drafts, shown, stages, syncStages] {
        QListWidgetItem *item = stages->currentItem();
        if (!item || *shown < 0)
            return;
        const QString flowId = drafts->at(*shown).id;
        const QString original = item->data(Qt::UserRole).toString();
        int contacts = 0;
        if (!flowId.isEmpty() && !original.isEmpty()) {
            for (const QJsonValue &value : std::as_const(m_contacts)) {
                const QJsonObject contact = value.toObject();
                if (text(contact, "flow") == flowId && text(contact, "stage") == original)
                    ++contacts;
            }
        }
        if (contacts > 0 && QMessageBox::question(
                &dialog, QStringLiteral("Remove stage"),
                QString::fromUtf8(
                    "%1 contacts are at \xE2\x80\x9C%2\xE2\x80\x9D. They keep that stage "
                    "until they are moved to another one. Remove it?")
                    .arg(contacts).arg(item->text())) != QMessageBox::Yes)
            return;
        delete item;
        syncStages();
    });

    int focusRow = 0;
    for (int row = 0; row < drafts->size(); ++row) {
        if (!focusFlow.isEmpty() && drafts->at(row).id == focusFlow)
            focusRow = row;
    }
    list->setCurrentRow(drafts->isEmpty() ? -1 : focusRow);
    showDraft(list->currentRow());
    if (!focusFlow.isEmpty())
        stages->setFocus();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save |
                                         QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    syncStages();

    // Ids stay what contacts and lists are stored against; a new pipeline
    // mints one from its name that no kept pipeline already has.
    QStringList taken;
    for (const DraftFlow &draft : std::as_const(*drafts)) {
        if (!draft.id.isEmpty() && !draft.name.isEmpty())
            taken << draft.id;
    }
    static const QRegularExpression stageShape(QStringLiteral("^[a-z][a-z0-9-]*$"));
    QJsonArray flows;
    QHash<QString, QString> idByKey;
    QHash<QString, QHash<QString, QString>> renamedStages;
    for (const DraftFlow &draft : std::as_const(*drafts)) {
        const QString label = draft.name.trimmed().left(60);
        if (label.isEmpty())
            continue;
        QString id = draft.id;
        if (id.isEmpty()) {
            const QString base = flowSlug(label);
            id = base;
            for (int suffix = 2; taken.contains(id); ++suffix)
                id = QStringLiteral("%1-%2").arg(base).arg(suffix);
            taken << id;
        }
        idByKey.insert(draft.key, id);
        QStringList slugs;
        QHash<QString, QString> renames;
        for (const DraftStage &stage : draft.stages) {
            const QString slug = stageSlug(stage.label);
            if (!stageShape.match(slug).hasMatch() || slugs.contains(slug))
                continue;
            slugs << slug;
            if (!draft.id.isEmpty() && !stage.original.isEmpty() && stage.original != slug)
                renames.insert(stage.original, slug);
        }
        if (slugs.isEmpty())
            slugs << QStringLiteral("lead");
        // A stage renamed to a name the pipeline still has under its old one
        // is ambiguous; its contacts stay where they are.
        for (auto rename = renames.begin(); rename != renames.end();) {
            if (slugs.contains(rename.key()))
                rename = renames.erase(rename);
            else
                ++rename;
        }
        if (!renames.isEmpty())
            renamedStages.insert(id, renames);
        flows.append(QJsonObject{{QStringLiteral("id"), id},
                                 {QStringLiteral("name"), label},
                                 {QStringLiteral("icon"), draft.icon},
                                 {QStringLiteral("stages"), QJsonArray::fromStringList(slugs)}});
    }
    if (flows.isEmpty()) {
        setStatus(QStringLiteral("Keep at least one pipeline, with a name."), true);
        return;
    }
    QJsonArray stageEdits;
    for (const QJsonValue &value : std::as_const(m_contacts)) {
        const QJsonObject contact = value.toObject();
        const QString to = renamedStages.value(text(contact, "flow"))
                               .value(text(contact, "stage"));
        if (!to.isEmpty())
            stageEdits.append(QJsonObject{{QStringLiteral("id"), text(contact, "id")},
                                          {QStringLiteral("stage"), to}});
    }
    QList<QPair<QString, QString>> bindings;
    for (int row = 0; row < lists->rowCount(); ++row) {
        auto *picker = qobject_cast<QComboBox *>(lists->cellWidget(row, 2));
        if (!picker)
            continue;
        const QString key = picker->currentData().toString();
        if (key == picker->property("crmInitialKey").toString())
            continue;
        if (!key.isEmpty() && !idByKey.contains(key))
            continue;
        bindings.append({lists->item(row, 0)->data(Qt::UserRole).toString(),
                         idByKey.value(key)});
    }

    const int saved = int(flows.size());
    const int moved = int(stageEdits.size());
    const int rebound = int(bindings.size());
    setStatus(QString::fromUtf8("Saving pipelines\xE2\x80\xA6"));
    request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")),
            QJsonObject{{QStringLiteral("flows"), flows}},
            [this, saved, moved, rebound, stageEdits, bindings](
                bool ok, const QJsonObject &, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("Pipelines were not saved: %1").arg(error), true);
            loadOrganization();
            return;
        }
        const auto failure = std::make_shared<QString>();
        const auto finish = [this, failure, saved, moved, rebound] {
            if (!failure->isEmpty()) {
                setStatus(QStringLiteral("Pipelines were saved, but %1").arg(*failure), true);
            } else {
                QStringList parts{saved == 1 ? QStringLiteral("1 pipeline saved")
                                             : QStringLiteral("%1 pipelines saved").arg(saved)};
                if (moved > 0)
                    parts << QStringLiteral("%1 contacts moved to renamed stages").arg(moved);
                if (rebound > 0)
                    parts << QStringLiteral("%1 lists given a new pipeline").arg(rebound);
                setStatus(parts.join(QStringLiteral("; ")) + QLatin1Char('.'));
            }
            loadOrganization();
        };
        // Lists are bound once the flows they name exist on the worker.
        const auto bindLists = [this, bindings, failure, finish] {
            if (bindings.isEmpty()) {
                finish();
                return;
            }
            const auto left = std::make_shared<int>(int(bindings.size()));
            for (const auto &[listId, flowId] : bindings) {
                request(QByteArrayLiteral("PATCH"),
                        organizationPath(QStringLiteral("/views/") + listId),
                        QJsonObject{{QStringLiteral("flow"), flowId}},
                        [left, failure, finish](bool bound, const QJsonObject &,
                                                const QString &bindError) {
                    if (!bound && failure->isEmpty())
                        *failure = QStringLiteral("a list's pipeline was not changed: %1")
                                       .arg(bindError);
                    if (--*left == 0)
                        finish();
                });
            }
        };
        if (stageEdits.isEmpty()) {
            bindLists();
            return;
        }
        request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/contacts/bulk")),
                QJsonObject{{QStringLiteral("edits"), stageEdits}},
                [failure, bindLists](bool movedOk, const QJsonObject &,
                                     const QString &moveError) {
            if (!movedOk)
                *failure = QStringLiteral("the contacts at renamed stages were not moved: %1")
                               .arg(moveError);
            bindLists();
        });
    });
}

void CrmFeature::openMailbox(bool gmail)
{
    QDesktopServices::openUrl(gmail ? QUrl(QStringLiteral("https://mail.google.com/mail/"))
                                    : QUrl(QStringLiteral("https://") + zohoMailHost() +
                                           QLatin1Char('/')));
}

// A desktop app cannot open chrome://extensions, so the button opens the
// extension's own page and hands over that address for the address bar.
void CrmFeature::openBrowserExtension()
{
    QDesktopServices::openUrl(QUrl(QLatin1String(kExtensionPage)));
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(QStringLiteral("chrome://extensions"));
    setStatus(QString::fromUtf8(
        "Opened the ForkMesh browser extension\xE2\x80\x99s page. Download it there, unzip "
        "it, paste chrome://extensions (copied) into Chrome\xE2\x80\x99s address bar, turn "
        "on Developer mode and choose Load unpacked on the unzipped browser-extension folder "
        "(from a source checkout: marketplace/packages/browser-extension/content/extension)."));
}

void CrmFeature::filterContacts()
{
    const int rows = qMin(m_contactsTable->rowCount(), int(m_tableRows.size()));
    for (int row = 0; row < rows; ++row)
        m_contactsTable->setRowHidden(
            row, !contactRowShown(m_fillSource.at(m_tableRows.at(row))));
    renderPeople();
}

void CrmFeature::setHostSearchQuery(const QString &query)
{
    const QString trimmed = query.trimmed();
    if (m_query == trimmed)
        return;
    m_query = trimmed;
    m_filterNote->setText(trimmed.isEmpty()
        ? QStringLiteral("Filter with the search box at the top of the window.")
        : QString::fromUtf8("Showing contacts matching \xE2\x80\x9C%1\xE2\x80\x9D.")
              .arg(trimmed));
    filterContacts();
}

QList<forkmesh::market::NativePackageSearchHit>
CrmFeature::searchHits(const QString &query, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString needle = query.trimmed();
    if (needle.isEmpty() || !m_installed)
        return hits;
    static const char *const kSearchedFields[] = {
        "name", "email", "company", "title", "phone", "twitter", "mastodon",
        "bluesky", "linkedin", "github", "website", "location", "owner",
        "source", "notes",
    };
    for (const QJsonValue &value : m_contacts) {
        const QJsonObject contact = value.toObject();
        bool matches = false;
        for (const char *field : kSearchedFields) {
            if (text(contact, field).contains(needle, Qt::CaseInsensitive)) {
                matches = true;
                break;
            }
        }
        for (const QJsonValue &tag : contact.value(QStringLiteral("tags")).toArray()) {
            if (matches)
                break;
            matches = tag.toString().contains(needle, Qt::CaseInsensitive);
        }
        if (!matches)
            continue;
        forkmesh::market::NativePackageSearchHit hit;
        hit.id = text(contact, "id");
        // A contact imported without a name or an email is known by its company.
        hit.title = !text(contact, "name").isEmpty() ? text(contact, "name")
            : !text(contact, "email").isEmpty()      ? text(contact, "email")
                                                     : text(contact, "company");
        QStringList detail;
        for (const char *field : {"company", "title", "email"}) {
            const QString part = text(contact, field);
            if (!part.isEmpty() && part != hit.title)
                detail << part;
        }
        hit.detail = detail.join(QString::fromUtf8(" \xC2\xB7 "));
        hits.append(hit);
        if (limit > 0 && hits.size() >= limit)
            break;
    }
    return hits;
}

void CrmFeature::activateSearchHit(const QString &id)
{
    const QJsonObject contact = contactById(id);
    if (contact.isEmpty())
        return;
    // There is no every-contact tab to land on: the contact opens on its
    // pipeline's tab, or failing that the first list (or "No pipeline") with it.
    const QList<QJsonObject> sheets = tabSheets();
    QString tab;
    for (const QJsonObject &sheet : sheets) {
        if (!text(sheet, "flow").isEmpty() && text(sheet, "flow") == text(contact, "flow")) {
            tab = text(sheet, "id");
            break;
        }
    }
    for (const QJsonObject &sheet : sheets) {
        if (!tab.isEmpty())
            break;
        const QString sheetId = text(sheet, "id");
        if (sheetId == QLatin1String(kNoPipelineTabId) || contactInSheet(contact, sheet))
            tab = sheetId;
    }
    const int index = sheetTabIndex(tab);
    if (index < 0)
        return;
    m_focusContact = id;
    m_tabs->setCurrentIndex(index);
    const QString stage = m_stage->currentData().toString();
    if (!stage.isEmpty() && stage != text(contact, "stage"))
        m_stage->setCurrentIndex(qMax(0, m_stage->findData(text(contact, "stage"))));
    focusContactRow();
}

QString CrmFeature::mastodonHandleKey(const QString &value)
{
    QString handle = value.trimmed();
    if (handle.isEmpty())
        return QString();
    QString user, host;
    if (handle.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ||
        handle.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        const QUrl url(handle);
        host = url.host();
        const QStringList segments =
            url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (segments.isEmpty())
            return QString();
        if (segments.first().startsWith(QLatin1Char('@')))
            user = segments.first().mid(1);
        else if (segments.first() == QLatin1String("users") && segments.size() > 1)
            user = segments.at(1);
    } else {
        if (handle.startsWith(QLatin1Char('@')))
            handle.remove(0, 1);
        const QStringList parts = handle.split(QLatin1Char('@'), Qt::SkipEmptyParts);
        if (parts.size() != 2)
            return QString();
        user = parts.at(0);
        host = parts.at(1);
    }
    user = user.trimmed().toLower();
    host = host.trimmed().toLower();
    if (user.isEmpty() || host.isEmpty() || !host.contains(QLatin1Char('.')) ||
        host.contains(QLatin1Char('/')))
        return QString();
    return user + QLatin1Char('@') + host;
}

// The profile a handle names lives on its own instance: "@grace@mastodon.social"
// opens https://mastodon.social/@grace. A profile URL, or its path typed
// without the scheme (mastodon.social/@grace), opens as typed over https.
QUrl CrmFeature::mastodonProfileUrl(const QString &value)
{
    QString handle = value.trimmed();
    if (!handle.contains(QLatin1String("://")) && handle.contains(QLatin1String("/@")))
        handle.prepend(QLatin1String("https://"));
    if (handle.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
        const QUrl url(handle);
        return url.isValid() && !url.host().isEmpty() && url.host().contains(QLatin1Char('.'))
                   ? url : QUrl();
    }
    const QString key = mastodonHandleKey(handle);
    const qsizetype at = key.lastIndexOf(QLatin1Char('@'));
    if (at <= 0)
        return QUrl();
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(key.mid(at + 1));
    url.setPath(QStringLiteral("/@") + key.left(at));
    return url.isValid() ? url : QUrl();
}

void CrmFeature::fillMastodonItem(QTableWidgetItem *item, const QString &key) const
{
    item->setData(Qt::UserRole + 1, QString());
    if (key.isEmpty()) {
        item->setText(QString::fromUtf8("\xE2\x80\x94"));
        item->setToolTip(QStringLiteral(
            "Add a Mastodon handle with its instance (@name@instance) to "
            "follow the contact's newest post."));
        return;
    }
    const auto latest = m_mastodonLatest.constFind(key);
    if (latest == m_mastodonLatest.constEnd()) {
        const bool running = m_mastodonPending.contains(key) ||
                             m_mastodonQueue.contains(key);
        item->setText(running
                          ? QString::fromUtf8("Looking up @%1\xE2\x80\xA6").arg(key)
                          : QStringLiteral("Not read yet"));
        item->setToolTip(running
                             ? QString()
                             : QStringLiteral(
                                   "@%1 has not been read. Use the refresh "
                                   "button on this row to check it now.").arg(key));
        return;
    }
    if (!latest->error.isEmpty()) {
        item->setText(QString::fromUtf8("\xE2\x80\x94"));
        item->setToolTip(QStringLiteral("@%1 could not be read: %2")
                             .arg(key, latest->error));
        return;
    }
    if (latest->postedAt <= 0) {
        item->setText(QStringLiteral("No public posts"));
        item->setToolTip(QStringLiteral("@%1 has no public posts.").arg(key));
        return;
    }
    item->setText(timestamp(latest->postedAt) + QString::fromUtf8(" \xC2\xB7 ") +
                  latest->excerpt);
    item->setToolTip(timestamp(latest->postedAt) + QLatin1Char('\n') +
                     latest->detail +
                     (latest->url.isEmpty()
                          ? QString()
                          : QStringLiteral("\n\nDouble-click to open %1")
                                .arg(latest->url)));
    item->setData(Qt::UserRole + 1, latest->url);
}

void CrmFeature::refreshMastodonUpdates()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList held;
    while (!m_mastodonQueue.isEmpty() &&
           m_mastodonPending.size() < kMastodonInFlight) {
        const QString key = m_mastodonQueue.takeFirst();
        if (key.isEmpty() || m_mastodonPending.contains(key))
            continue;
        // Another row of the same account was read moments ago: one read
        // covers both.
        const auto cached = m_mastodonLatest.constFind(key);
        if (cached != m_mastodonLatest.constEnd() &&
            now - cached->fetchedAt < kMastodonFreshMs)
            continue;
        const QString host = key.mid(key.lastIndexOf(QLatin1Char('@')) + 1);
        const qint64 until = m_mastodonBackoff.value(host);
        if (until > now) {
            MastodonLatest waiting =
                cached != m_mastodonLatest.constEnd() ? *cached : MastodonLatest();
            if (waiting.postedAt <= 0)
                waiting.error = QStringLiteral("%1 asked for fewer requests; it is read again later")
                                    .arg(host);
            waiting.fetchedAt = until - kMastodonFreshMs;
            m_mastodonLatest.insert(key, waiting);
            applyMastodonLatest(key);
            continue;
        }
        int onHost = 0;
        for (const QString &other : std::as_const(m_mastodonPending)) {
            if (other.mid(other.lastIndexOf(QLatin1Char('@')) + 1) == host)
                ++onHost;
        }
        if (onHost >= kMastodonPerHost) {
            held.append(key);
            continue;
        }
        fetchMastodonLatest(key);
    }
    m_mastodonQueue = held + m_mastodonQueue;
    // Each row's button rests while its account is queued or being read.
    const int column = columnIndex(ContactColumnSpec::MastodonLatest);
    if (column < 0)
        return;
    for (int row = 0; row < m_contactsTable->rowCount(); ++row) {
        QWidget *holder = m_contactsTable->cellWidget(row, column);
        auto *refresh = holder ? holder->findChild<QToolButton *>(
                                     QStringLiteral("crmMastodonRefresh"))
                               : nullptr;
        if (!refresh)
            continue;
        const QString key = refresh->property("mastodonKey").toString();
        refresh->setEnabled(!m_mastodonPending.contains(key) &&
                            !m_mastodonQueue.contains(key));
    }
}

void CrmFeature::refreshMastodonNow(const QString &key)
{
    if (key.isEmpty() || m_mastodonPending.contains(key) ||
        m_mastodonQueue.contains(key))
        return;
    const auto latest = m_mastodonLatest.find(key);
    if (latest != m_mastodonLatest.end())
        latest->fetchedAt = 0;
    m_mastodonQueue.append(key);
    refreshMastodonUpdates();
    applyMastodonLatest(key);
}

// Two public, unauthenticated calls against the contact's own instance: the
// account lookup, then its newest original post. Nothing is stored on disk;
// the cache lives with the page and expires after kMastodonFreshMs.
void CrmFeature::fetchMastodonLatest(const QString &key)
{
    const int at = key.lastIndexOf(QLatin1Char('@'));
    if (at <= 0)
        return;
    const QString user = key.left(at);
    const QString host = key.mid(at + 1);
    m_mastodonPending.insert(key);
    const auto finish = [this, key](MastodonLatest latest) {
        latest.fetchedAt = QDateTime::currentMSecsSinceEpoch();
        latest.excerpt = plainExcerpt(latest.text, 140);
        latest.detail = plainExcerpt(latest.text, 2000);
        const auto previous = m_mastodonLatest.constFind(key);
        if (!latest.error.isEmpty() && previous != m_mastodonLatest.constEnd() &&
            previous->postedAt > 0) {
            const qint64 fetchedAt = latest.fetchedAt;
            latest = *previous;
            latest.fetchedAt = fetchedAt;
        }
        m_mastodonLatest.insert(key, latest);
        m_mastodonPending.remove(key);
        applyMastodonLatest(key);
        scheduleNextRender();
        refreshMastodonUpdates();
    };
    const auto statusOf = [](QNetworkReply *reply) {
        return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    };
    const auto slowDown = [this, host](QNetworkReply *reply) {
        const QByteArray retry = reply->rawHeader("Retry-After").trimmed();
        bool seconds = false;
        const qint64 wait = retry.toLongLong(&seconds) * 1000;
        m_mastodonBackoff.insert(host, QDateTime::currentMSecsSinceEpoch() +
                                           qMax(seconds ? wait : 0, kMastodonBackoffMs));
        MastodonLatest latest;
        latest.error = QStringLiteral("the instance asked for fewer requests; it is read again later");
        return latest;
    };
    const auto readStatuses = [this, key, host, finish, statusOf, slowDown](const QString &accountId) {
        QUrl statuses;
        statuses.setScheme(QStringLiteral("https"));
        statuses.setHost(host);
        statuses.setPath(QStringLiteral("/api/v1/accounts/") + accountId +
                         QStringLiteral("/statuses"));
        QUrlQuery statusQuery;
        statusQuery.addQueryItem(QStringLiteral("limit"), QStringLiteral("1"));
        statusQuery.addQueryItem(QStringLiteral("exclude_replies"),
                                 QStringLiteral("true"));
        statusQuery.addQueryItem(QStringLiteral("exclude_reblogs"),
                                 QStringLiteral("true"));
        statuses.setQuery(statusQuery);
        QNetworkRequest statusRequest = instanceRequest(statuses);
        forkmesh::markExpectedHttpStatus(statusRequest, 404);
        forkmesh::markExpectedHttpStatus(statusRequest, 429);
        QNetworkReply *second = m_network->get(statusRequest);
        connect(second, &QNetworkReply::finished, this,
                [this, second, key, finish, statusOf, slowDown] {
            second->deleteLater();
            MastodonLatest latest;
            const int status = statusOf(second);
            if (status == 429) {
                latest = slowDown(second);
            } else if (status == 404) {
                m_mastodonIds.remove(key);
                latest.error = QStringLiteral("the instance knows no such account");
            } else if (second->error() != QNetworkReply::NoError) {
                latest.error = second->errorString();
            } else {
                const QJsonArray items =
                    QJsonDocument::fromJson(second->readAll()).array();
                if (!items.isEmpty()) {
                    const QJsonObject post = items.first().toObject();
                    latest.postedAt = QDateTime::fromString(
                        text(post, "created_at"), Qt::ISODateWithMs)
                        .toMSecsSinceEpoch();
                    latest.text = text(post, "content");
                    latest.url = text(post, "url").isEmpty()
                        ? text(post, "uri") : text(post, "url");
                }
            }
            finish(latest);
        });
    };
    const QString knownId = m_mastodonIds.value(key);
    if (!knownId.isEmpty()) {
        readStatuses(knownId);
        return;
    }
    QUrl lookup;
    lookup.setScheme(QStringLiteral("https"));
    lookup.setHost(host);
    lookup.setPath(QStringLiteral("/api/v1/accounts/lookup"));
    QUrlQuery lookupQuery;
    lookupQuery.addQueryItem(QStringLiteral("acct"), user);
    lookup.setQuery(lookupQuery);
    QNetworkRequest lookupRequest = instanceRequest(lookup);
    forkmesh::markExpectedHttpStatus(lookupRequest, 404);
    forkmesh::markExpectedHttpStatus(lookupRequest, 406);
    forkmesh::markExpectedHttpStatus(lookupRequest, 410);
    forkmesh::markExpectedHttpStatus(lookupRequest, 429);
    QNetworkReply *reply = m_network->get(lookupRequest);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, key, finish, statusOf, slowDown, readStatuses] {
        reply->deleteLater();
        if (statusOf(reply) == 429) {
            finish(slowDown(reply));
            return;
        }
        if (forkmesh::networkReplyStatusIsExpected(reply)) {
            MastodonLatest latest;
            latest.error = QStringLiteral("the instance knows no such account");
            finish(latest);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            MastodonLatest latest;
            latest.error = reply->errorString();
            finish(latest);
            return;
        }
        const QString accountId = QJsonDocument::fromJson(reply->readAll())
            .object().value(QStringLiteral("id")).toString();
        if (accountId.isEmpty()) {
            MastodonLatest latest;
            latest.error = QStringLiteral("the instance knows no such account");
            finish(latest);
            return;
        }
        m_mastodonIds.insert(key, accountId);
        readStatuses(accountId);
    });
}

void CrmFeature::applyMastodonLatest(const QString &key)
{
    const int column = columnIndex(ContactColumnSpec::MastodonLatest);
    if (column < 0)
        return;
    m_rendering = true;
    const int rows = qMin(m_contactsTable->rowCount(), int(m_tableRows.size()));
    for (int row = 0; row < rows; ++row) {
        QTableWidgetItem *item = m_contactsTable->item(row, column);
        if (item && m_fillSource.at(m_tableRows.at(row)).mastodonKey == key)
            fillMastodonItem(item, key);
    }
    m_rendering = false;
}

QStringList CrmFeature::avatarSources(const QJsonObject &contact)
{
    QStringList sources;
    const QString mastodon = mastodonHandleKey(text(contact, "mastodon"));
    if (!mastodon.isEmpty())
        sources << QStringLiteral("mastodon:") + mastodon;
    // A Bluesky handle, a bsky.app profile link, or the account's DID.
    QString bluesky = text(contact, "bluesky");
    static const QRegularExpression blueskyProfile(
        QStringLiteral("bsky\\.app/profile/([^/?#\\s]+)"),
        QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch match = blueskyProfile.match(bluesky); match.hasMatch())
        bluesky = match.captured(1);
    if (bluesky.startsWith(QLatin1Char('@')))
        bluesky.remove(0, 1);
    bluesky = bluesky.toLower();
    static const QRegularExpression blueskyActor(QStringLiteral(
        "^(?:(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\\.)+[a-z][a-z0-9-]*[a-z0-9]"
        "|did:[a-z]+:[a-z0-9._:%-]+)$"));
    if (blueskyActor.match(bluesky).hasMatch())
        sources << QStringLiteral("bluesky:") + bluesky;
    // A GitHub username, "@username", or a github.com profile link.
    QString github = text(contact, "github");
    static const QRegularExpression githubProfile(
        QStringLiteral("github\\.com/([^/?#\\s]+)"), QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch match = githubProfile.match(github); match.hasMatch())
        github = match.captured(1);
    if (github.startsWith(QLatin1Char('@')))
        github.remove(0, 1);
    static const QRegularExpression githubUser(
        QStringLiteral("^[A-Za-z0-9](?:[A-Za-z0-9-]{0,38})$"));
    if (githubUser.match(github).hasMatch())
        sources << QStringLiteral("github:") + github.toLower();
    return sources;
}

QStringList CrmFeature::pictureSources(const QJsonObject &contact)
{
    QStringList sources = avatarSources(contact);
    const QString email = text(contact, "email").trimmed().toLower();
    if (!email.contains(QLatin1Char('@')))
        return sources;
    sources.prepend(QStringLiteral("forkmesh:") + email);
    sources.append(QStringLiteral("gravatar:") + email);
    return sources;
}

void CrmFeature::keepForkMeshAvatars(const QJsonArray &users)
{
    m_forkMeshAvatars.clear();
    for (const QJsonValue &value : users) {
        const QJsonObject user = value.toObject();
        const QString email = text(user, "email").trimmed().toLower();
        const QByteArray png = QByteArray::fromBase64(text(user, "avatarPng").toLatin1());
        if (email.contains(QLatin1Char('@')) && !png.isEmpty())
            m_forkMeshAvatars.insert(email, png);
    }
    m_forkMeshAvatarsReadAt = QDateTime::currentMSecsSinceEpoch();
}

void CrmFeature::readForkMeshAvatars()
{
    if (m_forkMeshAvatarsReading || !m_userRecords || !m_userRecordsAvailable ||
        !m_userRecordsAvailable() ||
        QDateTime::currentMSecsSinceEpoch() - m_forkMeshAvatarsReadAt < kForkMeshAvatarsMs)
        return;
    m_forkMeshAvatarsReading = true;
    QPointer<CrmFeature> self(this);
    m_userRecords([self](const QJsonArray &users, const QString &error) {
        if (!self)
            return;
        self->m_forkMeshAvatarsReading = false;
        if (error.isEmpty())
            self->keepForkMeshAvatars(users);
        else
            self->m_forkMeshAvatarsReadAt = QDateTime::currentMSecsSinceEpoch();
        self->queueAvatars();
        QObject *reads = self->m_detailBody
            ? self->m_detailBody->findChild<QObject *>(QStringLiteral("crmDetailReads"),
                                                        Qt::FindDirectChildrenOnly)
            : nullptr;
        if (reads)
            static_cast<DetailReads *>(reads)->applyPhoto();
    });
}

QImage CrmFeature::forkMeshAvatar(const QString &email) const
{
    const auto png = m_forkMeshAvatars.constFind(email);
    return png == m_forkMeshAvatars.constEnd() ? QImage() : QImage::fromData(*png, "PNG");
}

QString CrmFeature::avatarSource(const ContactRow &row) const
{
    for (const QString &source : row.avatarSources) {
        if (!m_avatarFailed.contains(source))
            return source;
    }
    return QString();
}

QIcon CrmFeature::contactAvatar(const ContactRow &row)
{
    const auto picture = m_avatars.constFind(avatarSource(row));
    if (picture != m_avatars.constEnd())
        return *picture;
    const QColor textColor = m_contactsTable->palette().color(QPalette::Text);
    const QString key = row.initials + QLatin1Char('/') + QString::number(row.avatarTone) +
                        QLatin1Char('/') + QString::number(textColor.rgba());
    auto initials = m_initialsAvatars.constFind(key);
    if (initials == m_initialsAvatars.constEnd())
        initials = m_initialsAvatars.insert(key, initialsIcon(row.initials, row.avatarTone,
                                                              textColor));
    return *initials;
}

void CrmFeature::queueAvatars()
{
    // A snapshot still building would queue the rows it is replacing; its
    // arrival queues the new ones instead.
    if (!m_avatarsWanted || m_forkMeshAvatarsReading ||
        m_snapshot.generation != m_snapshotGeneration)
        return;
    m_avatarsWanted = false;
    for (const ContactRow &row : std::as_const(m_snapshot.rows)) {
        const QString source = avatarSource(row);
        if (source.isEmpty() || m_avatars.contains(source) || m_avatarPending.contains(source))
            continue;
        m_avatarPending.insert(source);
        m_avatarQueue.append(source);
    }
    pumpAvatars();
}

void CrmFeature::pumpAvatars()
{
    while (!m_avatarQueue.isEmpty() && m_avatarInFlight < kAvatarInFlight) {
        ++m_avatarInFlight;
        fetchAvatar(m_avatarQueue.takeFirst());
    }
}

// Public, unauthenticated reads: GitHub serves a picture by username; a
// Mastodon instance and Bluesky's public API name the picture's address in the
// account's profile, read first. A picture read in the last week comes off
// disk instead.
void CrmFeature::fetchAvatar(const QString &source)
{
    const QString kind = source.section(QLatin1Char(':'), 0, 0);
    const QString account = source.section(QLatin1Char(':'), 1);
    if (kind == QLatin1String("forkmesh")) {
        QImage image = forkMeshAvatar(account);
        if (image.width() > kAvatarKeep || image.height() > kAvatarKeep)
            image = image.scaled(kAvatarKeep, kAvatarKeep, Qt::KeepAspectRatioByExpanding,
                                 Qt::SmoothTransformation);
        QTimer::singleShot(0, this, [this, source, image] { finishAvatar(source, image); });
        return;
    }
    const QString cached = avatarCachePath(source);
    const QFileInfo saved(cached);
    if (saved.exists() &&
        saved.lastModified().msecsTo(QDateTime::currentDateTime()) < kAvatarCacheMs) {
        const QImage image = saved.size() > 0 ? QImage(cached) : QImage();
        if (!image.isNull() || saved.size() == 0) {
            // Next turn, so a queue of saved pictures does not recurse
            // through finishAvatar and pumpAvatars.
            QTimer::singleShot(0, this, [this, source, image] { finishAvatar(source, image); });
            return;
        }
    }
    if (kind == QLatin1String("gravatar")) {
        fetchAvatarImage(source, gravatarPicture(account, kAvatarKeep));
        return;
    }
    if (kind == QLatin1String("github")) {
        QUrl url;
        url.setScheme(QStringLiteral("https"));
        url.setHost(QStringLiteral("avatars.githubusercontent.com"));
        url.setPath(QLatin1Char('/') + account);
        url.setQuery(QStringLiteral("s=%1").arg(kAvatarKeep));
        fetchAvatarImage(source, url);
        return;
    }
    QUrl profile;
    profile.setScheme(QStringLiteral("https"));
    QUrlQuery query;
    if (kind == QLatin1String("mastodon")) {
        const qsizetype at = account.lastIndexOf(QLatin1Char('@'));
        profile.setHost(account.mid(at + 1));
        profile.setPath(QStringLiteral("/api/v1/accounts/lookup"));
        query.addQueryItem(QStringLiteral("acct"), account.left(at));
    } else {
        profile.setHost(QStringLiteral("public.api.bsky.app"));
        profile.setPath(QStringLiteral("/xrpc/app.bsky.actor.getProfile"));
        query.addQueryItem(QStringLiteral("actor"), account);
    }
    profile.setQuery(query);
    // An account the service does not know (Mastodon's 404, Bluesky's 400)
    // is an answer: the contact's next source is tried.
    QNetworkRequest request = jsonRequest(profile);
    forkmesh::markExpectedHttpStatus(request, 404);
    forkmesh::markExpectedHttpStatus(request, 400);
    forkmesh::markExpectedHttpStatus(request, 406);
    forkmesh::markExpectedHttpStatus(request, 410);
    if (kind == QLatin1String("mastodon"))
        forkmesh::markExpectedTransportFailure(request);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, source, kind] {
        reply->deleteLater();
        const QJsonObject profile = reply->error() == QNetworkReply::NoError
            ? QJsonDocument::fromJson(reply->readAll()).object()
            : QJsonObject();
        const QUrl picture(text(profile, kind == QLatin1String("mastodon")
                                             ? "avatar_static" : "avatar"));
        // Mastodon hands out its own placeholder for an account without one.
        if (picture.scheme() != QLatin1String("https") ||
            picture.path().endsWith(QLatin1String("/missing.png"))) {
            finishAvatar(source, QImage());
            return;
        }
        fetchAvatarImage(source, picture);
    });
}

void CrmFeature::fetchAvatarImage(const QString &source, const QUrl &url)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("image/*"));
    request.setTransferTimeout(15000);
    forkmesh::markExpectedHttpStatus(request, 404);
    forkmesh::markExpectedTransportFailure(request);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, source] {
        reply->deleteLater();
        QImage image;
        if (reply->error() == QNetworkReply::NoError)
            image.loadFromData(reply->readAll());
        if (forkmesh::networkReplyStatusIsExpected(reply)) {
            QSaveFile file(avatarCachePath(source));
            if (openCacheFile(file))
                file.commit();
        }
        if (!image.isNull()) {
            const int keep = kAvatarKeep;
            if (image.width() > keep || image.height() > keep)
                image = image.scaled(keep, keep, Qt::KeepAspectRatioByExpanding,
                                     Qt::SmoothTransformation);
            QSaveFile file(avatarCachePath(source));
            if (openCacheFile(file) && image.save(&file, "PNG"))
                file.commit();
        }
        finishAvatar(source, image);
    });
}

void CrmFeature::finishAvatar(const QString &source, const QImage &image)
{
    --m_avatarInFlight;
    m_avatarPending.remove(source);
    if (image.isNull()) {
        m_avatarFailed.insert(source);
    } else {
        m_avatars.insert(source, avatarIcon(image));
        m_avatarImages.insert(source, image);
    }
    m_avatarChanged.insert(source);
    // Pictures landing close together update the rows once.
    if (!m_avatarApplyQueued) {
        m_avatarApplyQueued = true;
        QTimer::singleShot(150, this, [this] { applyAvatars(); });
    }
    pumpAvatars();
}

void CrmFeature::applyAvatars()
{
    m_avatarApplyQueued = false;
    const QSet<QString> changed = std::exchange(m_avatarChanged, {});
    const auto touched = [&changed](const ContactRow &row) {
        for (const QString &source : row.avatarSources) {
            if (changed.contains(source))
                return true;
        }
        return false;
    };
    // A contact whose source failed moves on to its next one.
    for (const ContactRow &row : std::as_const(m_snapshot.rows)) {
        if (!touched(row))
            continue;
        const QString source = avatarSource(row);
        if (source.isEmpty() || m_avatars.contains(source) || m_avatarPending.contains(source))
            continue;
        m_avatarPending.insert(source);
        m_avatarQueue.append(source);
    }
    const int column = columnIndex(ContactColumnSpec::Field, QStringLiteral("name"));
    if (column >= 0) {
        m_rendering = true;
        const int rows = qMin(m_contactsTable->rowCount(), int(m_tableRows.size()));
        for (int row = 0; row < rows; ++row) {
            const ContactRow &source = m_fillSource.at(m_tableRows.at(row));
            QTableWidgetItem *item = m_contactsTable->item(row, column);
            if (item && touched(source))
                item->setIcon(contactAvatar(source));
        }
        m_rendering = false;
    }
    if (!m_nextPhotos.isEmpty()) {
        for (const ContactRow &row : std::as_const(m_snapshot.rows)) {
            const QPointer<QLabel> photo = m_nextPhotos.value(row.id);
            if (photo && touched(row))
                photo->setPixmap(contactPicture(row, kNextPhotoSize)
                                     .pixmap(kNextPhotoSize, kNextPhotoSize));
        }
    }
    pumpAvatars();
    renderSheetList();
    if (!m_peopleQueued && m_tabs->currentWidget() == m_peoplePage) {
        m_peopleQueued = true;
        QTimer::singleShot(600, this, [this] {
            m_peopleQueued = false;
            renderPeople();
        });
    }
}

QUrl CrmFeature::mailComposeUrl(const QString &email) const
{
    const QString address = email.trimmed();
    if (text(m_settings, "mailProvider") == QLatin1String("gmail")) {
        QUrl url(QStringLiteral("https://mail.google.com/mail/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("view"), QStringLiteral("cm"));
        query.addQueryItem(QStringLiteral("fs"), QStringLiteral("1"));
        query.addQueryItem(QStringLiteral("to"), address);
        url.setQuery(query);
        return url;
    }
    // Zoho's own mailto handler shape, on the organization's data center.
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(zohoMailHost());
    url.setPath(QStringLiteral("/zm/comp.do"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("ct"), QStringLiteral("mailto:") + address);
    url.setQuery(query);
    return url;
}

// The Zoho Mail host of the data center the organization's connection uses
// (settings → Zoho data center; .com when unset).
QString CrmFeature::zohoMailHost() const
{
    const QString region = text(
        m_settings.value(QStringLiteral("zoho")).toObject(), "region");
    static const QStringList kRegions{
        QStringLiteral("com"), QStringLiteral("eu"), QStringLiteral("in"),
        QStringLiteral("com.au"), QStringLiteral("jp"), QStringLiteral("ca")};
    const QString suffix = kRegions.contains(region) ? region : QStringLiteral("com");
    return suffix == QLatin1String("ca") ? QStringLiteral("mail.zohocloud.ca")
                                         : QStringLiteral("mail.zoho.") + suffix;
}

void CrmFeature::openMailCompose(const QString &email)
{
    const QUrl url = mailComposeUrl(email);
    if (!url.isValid()) {
        setStatus(QStringLiteral("That address cannot be opened."), true);
        return;
    }
    QDesktopServices::openUrl(url);
    setStatus(QString::fromUtf8("Opening a %1 message to %2\xE2\x80\xA6")
                  .arg(text(m_settings, "mailProvider") == QLatin1String("gmail")
                           ? QStringLiteral("Gmail") : QStringLiteral("Zoho Mail"),
                       email.trimmed()));
}

// Through the id index: filtering and selection look contacts up per row, and
// a scan per lookup made those quadratic in the organization's size.
QJsonObject CrmFeature::contactById(const QString &id) const
{
    const auto at = m_contactIndex.constFind(id);
    if (at == m_contactIndex.constEnd() || *at >= m_contacts.size())
        return {};
    const QJsonObject contact = m_contacts.at(*at).toObject();
    return text(contact, "id") == id ? contact : QJsonObject();
}

int CrmFeature::rowOfContact(const QString &id) const
{
    if (id.isEmpty())
        return -1;
    for (int row = 0; row < m_contactsTable->rowCount(); ++row) {
        QTableWidgetItem *item = m_contactsTable->item(row, 0);
        if (item && item->data(Qt::UserRole).toString() == id)
            return row;
    }
    return -1;
}

QJsonObject CrmFeature::selectedContact() const
{
    const int row = m_contactsTable ? m_contactsTable->currentRow() : -1;
    QTableWidgetItem *item = row >= 0 ? m_contactsTable->item(row, 0) : nullptr;
    return item ? contactById(item->data(Qt::UserRole).toString()) : QJsonObject();
}

QList<QJsonObject> CrmFeature::selectedContacts() const
{
    QList<QJsonObject> contacts;
    if (!m_contactsTable)
        return contacts;
    QModelIndexList rows = m_contactsTable->selectionModel()->selectedRows();
    std::sort(rows.begin(), rows.end(),
              [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
    for (const QModelIndex &index : std::as_const(rows)) {
        if (m_contactsTable->isRowHidden(index.row()))
            continue;
        const QTableWidgetItem *item = m_contactsTable->item(index.row(), 0);
        const QJsonObject contact =
            item ? contactById(item->data(Qt::UserRole).toString()) : QJsonObject();
        if (!contact.isEmpty())
            contacts << contact;
    }
    return contacts;
}

QJsonObject CrmFeature::selectedEmail() const
{
    const int row = m_emailsTable ? m_emailsTable->currentRow() : -1;
    QTableWidgetItem *item = row >= 0 ? m_emailsTable->item(row, 0) : nullptr;
    const QString id = item ? item->data(Qt::UserRole).toString() : QString();
    if (id.isEmpty())
        return {};
    for (const QJsonArray *records : {&m_emails, &m_mail}) {
        for (const QJsonValue &value : *records) {
            const QJsonObject record = value.toObject();
            if (text(record, "id") == id)
                return record;
        }
    }
    return {};
}

// Everything the organization knows about one contact, in one window: the
// picture at a size worth looking at, every field the record carries, the
// messages and deck opens tied to it, and the newest posts on the accounts it
// names. The picture and the posts are read from those services while the
// card is open and kept in memory for the session; nothing goes to disk.
void CrmFeature::closeContactDetail()
{
    m_detailContactId.clear();
    m_detailContact = QJsonObject();
    if (m_detailBody) {
        m_detailBody->deleteLater();
        m_detailBody = nullptr;
    }
    m_detailPanel->hide();
}

void CrmFeature::showContactDetail(const QString &contactId, bool reread)
{
    const QJsonObject contact = contactById(contactId);
    if (contact.isEmpty()) {
        if (contactId == m_detailContactId)
            closeContactDetail();
        return;
    }
    if (!reread && contactId == m_detailContactId && contact == m_detailContact &&
        m_detailPanel->isVisible())
        return;
    if (m_detailBody)
        m_detailBody->deleteLater();
    m_detailContactId = contactId;
    m_detailContact = contact;
    auto *panel = new QWidget;
    m_detailBody = panel;
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    m_detailPanel->layout()->addWidget(panel);
    m_detailPanel->show();
    auto *reads = new DetailReads(panel);
    reads->setObjectName(QStringLiteral("crmDetailReads"));

    const QLocale locale;
    const auto fieldValue = [this, &contact, &locale](const QString &key) {
        if (key == QLatin1String("tags")) {
            QStringList tags;
            for (const QJsonValue &tag : contact.value(QStringLiteral("tags")).toArray())
                tags << tag.toString();
            return tags.join(QStringLiteral(", "));
        }
        if (key == QLatin1String("value")) {
            const qint64 amount = qint64(contact.value(QStringLiteral("value")).toDouble());
            return amount > 0 ? locale.toString(amount) : QString();
        }
        if (key == QLatin1String("flow"))
            return flowLabel(text(contact, "flow"));
        return text(contact, key);
    };
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const auto htmlLine = [](const QString &plain, const QUrl &link) {
        const QString shown = plain.toHtmlEscaped();
        return QStringLiteral("<div style=\"margin-bottom:5px\">%1</div>").arg(
            link.isValid() && link.scheme() == QLatin1String("https")
                ? QStringLiteral("<a href=\"%1\">%2</a>")
                      .arg(link.toString().toHtmlEscaped(), shown)
                : shown);
    };

    // --- the head: the picture, who the person is, where they stand -------
    const QStringList sources = avatarSources(contact);
    reads->sources = sources;
    reads->photoSources = pictureSources(contact);
    auto *photo = new QLabel;
    reads->photo = photo;
    photo->setFixedSize(kDetailPhotoSize, kDetailPhotoSize);
    photo->setPixmap(initialsPixmap(contactInitials(contact), contactAvatarTone(contact),
                                    palette().color(QPalette::Text), kDetailPhotoSize,
                                    devicePixelRatioF()));
    photo->setToolTip(reads->photoSources.isEmpty()
        ? QStringLiteral("This record names no account or email address to read a "
                         "picture from")
        : QStringLiteral("Read from the accounts on this record while the card is open"));
    auto *name = new QLabel(text(contact, "name").isEmpty()
        ? (text(contact, "email").isEmpty() ? QStringLiteral("Unnamed contact")
                                            : text(contact, "email"))
        : text(contact, "name"));
    name->setTextFormat(Qt::PlainText);
    name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont nameFont = name->font();
    nameFont.setPointSizeF(nameFont.pointSizeF() * 1.35);
    nameFont.setWeight(QFont::DemiBold);
    name->setFont(nameFont);
    QStringList place;
    for (const char *key : {"title", "company", "location"}) {
        if (!text(contact, key).isEmpty())
            place << text(contact, key);
    }
    auto *subtitle = new QLabel(place.isEmpty()
        ? QStringLiteral("No title or company on this record") : place.join(dot));
    subtitle->setTextFormat(Qt::PlainText);
    subtitle->setWordWrap(true);
    const qint64 contacted = qint64(contact.value(QStringLiteral("lastContactedAt")).toDouble());
    auto *standing = new QLabel(QStringList{
        fieldValue(QStringLiteral("stage")).isEmpty() ? QStringLiteral("No stage")
                                                      : fieldValue(QStringLiteral("stage")),
        fieldValue(QStringLiteral("flow")),
        QStringLiteral("last contacted %1").arg(contacted > 0 ? timestamp(contacted)
                                                              : QStringLiteral("never")),
    }.join(dot));
    standing->setTextFormat(Qt::PlainText);
    auto *heading = new QVBoxLayout;
    heading->setSpacing(2);
    heading->addWidget(name);
    heading->addWidget(subtitle);
    heading->addWidget(standing);
    heading->addStretch(1);
    auto *head = new QHBoxLayout;
    head->setSpacing(14);
    head->addWidget(photo, 0, Qt::AlignTop);
    head->addLayout(heading, 1);
    layout->addLayout(head);

    // --- every field the record carries ----------------------------------
    auto *body = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(0, 8, 0, 0);
    auto *fields = new QFormLayout;
    fields->setLabelAlignment(Qt::AlignRight | Qt::AlignTop);
    fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    const auto valueLabel = [&dash](const QString &value, const QUrl &link) {
        auto *label = new QLabel;
        label->setWordWrap(true);
        if (link.isValid() && link.scheme() == QLatin1String("https")) {
            label->setTextFormat(Qt::RichText);
            label->setText(QStringLiteral("<a href=\"%1\">%2</a>")
                               .arg(link.toString().toHtmlEscaped(), value.toHtmlEscaped()));
            label->setOpenExternalLinks(true);
            label->setTextInteractionFlags(Qt::TextBrowserInteraction);
        } else {
            label->setTextFormat(Qt::PlainText);
            label->setText(value.isEmpty() ? dash : value);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        }
        return label;
    };
    for (const ContactColumn &column : contactColumns()) {
        const QString field = QString::fromLatin1(column.field);
        const QString value = fieldValue(field);
        const QUrl link = field == QLatin1String("email") && !value.isEmpty()
            ? mailComposeUrl(value) : profileUrl(field, value);
        fields->addRow(QString::fromLatin1(column.label), valueLabel(value, link));
    }
    // A custom column only reads as a field where this record fills it in.
    QStringList customKeys = m_customColumns;
    for (const QString &key : contact.value(QStringLiteral("fields")).toObject().keys()) {
        if (!customKeys.contains(key))
            customKeys << key;
    }
    for (const QString &key : std::as_const(customKeys)) {
        const QString value = contact.value(QStringLiteral("fields")).toObject()
                                  .value(key).toString().trimmed();
        if (!value.isEmpty())
            fields->addRow(customColumnLabel(key), valueLabel(value, {}));
    }
    QStringList record;
    const qint64 created = qint64(contact.value(QStringLiteral("createdAt")).toDouble());
    const qint64 updated = qint64(contact.value(QStringLiteral("updatedAt")).toDouble());
    if (created > 0) {
        record << QStringLiteral("added %1%2").arg(timestamp(created),
            text(contact, "createdBy").isEmpty()
                ? QString() : QStringLiteral(" by ") + text(contact, "createdBy"));
    }
    if (updated > 0) {
        record << QStringLiteral("last edited %1%2").arg(timestamp(updated),
            text(contact, "updatedBy").isEmpty()
                ? QString() : QStringLiteral(" by ") + text(contact, "updatedBy"));
    }
    record << QStringLiteral("shared with every member of %1")
                  .arg(text(contact, "organization").isEmpty()
                           ? m_organization->currentData().toString()
                           : text(contact, "organization"));
    const QStringList onLists = contactListNames(contactId);
    fields->addRow(QStringLiteral("Lists"),
                   valueLabel(onLists.isEmpty() ? QStringLiteral("On no list")
                                                : onLists.join(QStringLiteral(", ")), {}));
    fields->addRow(QStringLiteral("Record"), valueLabel(record.join(dot), {}));
    bodyLayout->addLayout(fields);

    // --- what has happened on the record ---------------------------------
    QList<QJsonObject> linked = m_contactEmails.value(contactId);
    linked.append(m_contactToots.value(contactId));
    std::sort(linked.begin(), linked.end(),
              [](const QJsonObject &first, const QJsonObject &second) {
        return emailSentAt(first) > emailSentAt(second);
    });
    QStringList timeline;
    const QJsonObject deck = deckLinkFor(contactId);
    if (!deck.isEmpty())
        timeline << htmlLine(deckActivityLine(deck), QUrl(text(deck, "url")));
    for (const QJsonObject &email : std::as_const(linked))
        timeline << htmlLine(linkedEmailLine(email), QUrl(text(email, "providerUrl")));
    if (timeline.isEmpty())
        timeline << htmlLine(QStringLiteral("Nothing has been linked to this record yet."), {});
    auto *activityHeading = new QLabel(QStringLiteral("Activity"));
    QFont sectionFont = activityHeading->font();
    sectionFont.setWeight(QFont::DemiBold);
    activityHeading->setFont(sectionFont);
    auto *activity = new QLabel(timeline.join(QString()));
    activity->setTextFormat(Qt::RichText);
    activity->setWordWrap(true);
    activity->setOpenExternalLinks(true);
    activity->setTextInteractionFlags(Qt::TextBrowserInteraction);
    bodyLayout->addSpacing(10);
    bodyLayout->addWidget(activityHeading);
    bodyLayout->addWidget(activity);

    // --- the newest posts on the accounts the record names ---------------
    auto *updatesHeading = new QLabel(QStringLiteral("Recent updates"));
    updatesHeading->setFont(sectionFont);
    auto *updatesNote = new QLabel(sources.isEmpty()
        ? QStringLiteral("This record names no Mastodon, Bluesky or GitHub account to read.")
        : QStringLiteral("Read from the accounts on this record when it opens; kept in "
                         "memory for the session and never stored."));
    updatesNote->setWordWrap(true);
    bodyLayout->addSpacing(10);
    bodyLayout->addWidget(updatesHeading);
    bodyLayout->addWidget(updatesNote);

    reads->applyPhoto = [this, reads] {
        if (reads->shown)
            return;
        for (const QString &source : std::as_const(reads->photoSources)) {
            if (source.startsWith(QLatin1String("forkmesh:")) && !m_photos.contains(source)) {
                const QImage image = forkMeshAvatar(source.section(QLatin1Char(':'), 1));
                if (!image.isNull())
                    m_photos.insert(source, image.width() > kDetailPhotoSize * 2
                        ? image.scaled(kDetailPhotoSize * 2, kDetailPhotoSize * 2,
                                       Qt::KeepAspectRatioByExpanding,
                                       Qt::SmoothTransformation)
                        : image);
            }
            const QImage cached = m_photos.value(source);
            if (!cached.isNull()) {
                reads->photo->setPixmap(
                    circlePixmap(cached, kDetailPhotoSize, devicePixelRatioF()));
                reads->shown = true;
                return;
            }
            const QUrl picture = reads->pictures.value(source);
            if (m_photos.contains(source) || !picture.isValid() || reads->asked.contains(source))
                continue;
            reads->asked.insert(source);
            readContactPhoto(source, picture, reads, [reads] { reads->applyPhoto(); });
        }
    };

    for (const QString &source : sources) {
        const QString service = accountServiceLabel(source);
        const QString account = source.section(QLatin1Char(':'), 1);
        const QString field = source.section(QLatin1Char(':'), 0, 0);
        auto *card = new QFrame;
        card->setFrameShape(QFrame::StyledPanel);
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setSpacing(4);
        auto *accountLabel = new QLabel;
        accountLabel->setTextFormat(Qt::RichText);
        accountLabel->setOpenExternalLinks(true);
        accountLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
        accountLabel->setText(htmlLine(
            QString::fromUtf8("%1 \xC2\xB7 %2").arg(service, account),
            profileUrl(field, text(contact, field))));
        auto *posts = new QLabel;
        posts->setWordWrap(true);
        posts->setOpenExternalLinks(true);
        posts->setTextInteractionFlags(Qt::TextBrowserInteraction);
        cardLayout->addWidget(accountLabel);
        cardLayout->addWidget(posts);
        bodyLayout->addWidget(card);
        reads->posts.append(posts);
    }
    for (const QString &source : std::as_const(reads->photoSources))
        reads->pictures.insert(source, accountKnownPicture(source));
    bodyLayout->addStretch(1);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(body);
    layout->addWidget(scroll, 1);

    reads->readAccounts = [this, reads, htmlLine](bool fresh) {
        for (int at = 0; at < reads->sources.size(); ++at) {
            const QString source = reads->sources.at(at);
            const QString service = accountServiceLabel(source);
            QLabel *posts = reads->posts.at(at);
            const auto show = [this, reads, posts, service, source, htmlLine](
                    const QString &error, const QUrl &picture,
                    const QList<AccountUpdate> &updates) {
                m_accountReads.insert(source, AccountRead{error, picture, updates});
                if (picture.isValid()) {
                    reads->pictures.insert(source, picture);
                    reads->applyPhoto();
                }
                if (!error.isEmpty()) {
                    posts->setTextFormat(Qt::PlainText);
                    posts->setText(QStringLiteral("%1 could not be read: %2.")
                                       .arg(service, error));
                    return;
                }
                if (updates.isEmpty()) {
                    posts->setTextFormat(Qt::PlainText);
                    posts->setText(QStringLiteral("No public posts on this account."));
                    return;
                }
                QStringList lines;
                for (const AccountUpdate &update : updates) {
                    lines << htmlLine(update.at > 0
                                          ? QString::fromUtf8("%1 \xC2\xB7 %2")
                                                .arg(update.text, timestamp(update.at))
                                          : update.text,
                                      update.url);
                }
                posts->setTextFormat(Qt::RichText);
                posts->setText(lines.join(QString()));
            };
            const auto cached = m_accountReads.constFind(source);
            if (!fresh && cached != m_accountReads.constEnd()) {
                show(cached->error, cached->picture, cached->updates);
                continue;
            }
            posts->setTextFormat(Qt::PlainText);
            posts->setText(QString::fromUtf8("Reading %1\xE2\x80\xA6").arg(service));
            readAccountUpdates(source, reads, show);
        }
    };
    reads->applyPhoto();
    reads->readAccounts(reread);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(6);
    auto *edit = new QPushButton(QStringLiteral("Edit contact"));
    edit->setObjectName(QStringLiteral("primaryButton"));
    auto *write = new QPushButton(QStringLiteral("Email"));
    write->setEnabled(!text(contact, "email").isEmpty());
    auto *draft = new QPushButton;
    draft->setObjectName(QStringLiteral("crmDraftEmailButton"));
    m_draftButton = draft;
    syncDraftButton();
    auto *prompt = new QPushButton(QStringLiteral("Write with AI"));
    prompt->setToolTip(QStringLiteral("Gather everything about them and us into a prompt for "
                                      "ChatGPT or Claude to write the message"));
    auto *autoSend = new QPushButton(QStringLiteral("Send AI email"));
    autoSend->setToolTip(autoEmailTip());
    autoSend->setEnabled(!text(contact, "email").isEmpty());
    auto *toot = new QPushButton(QStringLiteral("Log toot"));
    toot->setToolTip(QStringLiteral("Paste a toot's link to log it as a Mastodon "
                                    "conversation with this contact"));
    auto *again = new QPushButton(QStringLiteral("Read again"));
    again->setEnabled(!sources.isEmpty());
    for (QPushButton *button : {edit, write, draft, prompt, autoSend, toot, again})
        actions->addWidget(button);
    actions->addStretch(1);
    layout->addLayout(actions);
    connect(autoSend, &QPushButton::clicked, this, [this, contactId] { sendAutoEmail(contactId); });
    connect(toot, &QPushButton::clicked, this, [this, contactId] { logToot(contactId); });
    connect(again, &QPushButton::clicked, reads, [reads] { reads->readAccounts(true); });
    connect(prompt, &QPushButton::clicked, this,
            [this, contactId] { showAiWriter(contactId); });
    connect(write, &QPushButton::clicked, this,
            [this, contactId] { composeEmail(contactById(contactId)); });
    connect(draft, &QPushButton::clicked, this,
            [this, contactId] { draftInvestorEmail(contactById(contactId)); });
    connect(edit, &QPushButton::clicked, this,
            [this, contactId] { editContact(contactById(contactId)); });
}

// One connected account's newest public posts, or its newest public events:
// the same public, unauthenticated reads the pictures come from, made only
// while a card is open. `context` guards the handlers, so a card closed
// mid-read drops whatever comes back.
void CrmFeature::readAccountUpdates(const QString &source, QObject *context,
                                    UpdatesHandler done)
{
    const QString kind = source.section(QLatin1Char(':'), 0, 0);
    const QString account = source.section(QLatin1Char(':'), 1);
    if (kind == QLatin1String("mastodon"))
        readMastodonUpdates(account, context, std::move(done));
    else if (kind == QLatin1String("bluesky"))
        readBlueskyUpdates(account, context, std::move(done));
    else if (kind == QLatin1String("github"))
        readGithubUpdates(account, context, std::move(done));
}

// The account lookup on the contact's own instance, which also names the
// picture, then that account's newest original posts.
void CrmFeature::readMastodonUpdates(const QString &key, QObject *context,
                                     UpdatesHandler done)
{
    const qsizetype at = key.lastIndexOf(QLatin1Char('@'));
    if (at <= 0) {
        done(QStringLiteral("the handle names no instance"), {}, {});
        return;
    }
    const QString user = key.left(at);
    const QString host = key.mid(at + 1);
    QUrl lookup;
    lookup.setScheme(QStringLiteral("https"));
    lookup.setHost(host);
    lookup.setPath(QStringLiteral("/api/v1/accounts/lookup"));
    QUrlQuery lookupQuery;
    lookupQuery.addQueryItem(QStringLiteral("acct"), user);
    lookup.setQuery(lookupQuery);
    // A handle the instance does not know answers 404: that is its answer,
    // not a failed request.
    QNetworkRequest request = instanceRequest(lookup);
    forkmesh::markExpectedHttpStatus(request, 404);
    forkmesh::markExpectedHttpStatus(request, 406);
    forkmesh::markExpectedHttpStatus(request, 410);
    QNetworkReply *reply = m_network->get(request);
    const QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, host, context, guard, done] {
        reply->deleteLater();
        if (!guard)
            return;
        if (forkmesh::networkReplyStatusIsExpected(reply)) {
            done(QStringLiteral("the instance knows no such account"), {}, {});
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            done(reply->errorString(), {}, {});
            return;
        }
        const QJsonObject profile = QJsonDocument::fromJson(reply->readAll()).object();
        // Mastodon hands out its own placeholder for an account without one.
        QUrl picture(text(profile, "avatar_static").isEmpty()
                         ? text(profile, "avatar") : text(profile, "avatar_static"));
        if (picture.scheme() != QLatin1String("https") ||
            picture.path().endsWith(QLatin1String("/missing.png")))
            picture = QUrl();
        const QString accountId = text(profile, "id");
        if (accountId.isEmpty()) {
            done(QStringLiteral("the instance knows no such account"), picture, {});
            return;
        }
        QUrl statuses;
        statuses.setScheme(QStringLiteral("https"));
        statuses.setHost(host);
        statuses.setPath(QStringLiteral("/api/v1/accounts/") + accountId +
                         QStringLiteral("/statuses"));
        QUrlQuery statusQuery;
        statusQuery.addQueryItem(QStringLiteral("limit"),
                                 QString::number(kAccountUpdates));
        statusQuery.addQueryItem(QStringLiteral("exclude_replies"),
                                 QStringLiteral("true"));
        statusQuery.addQueryItem(QStringLiteral("exclude_reblogs"),
                                 QStringLiteral("true"));
        statuses.setQuery(statusQuery);
        QNetworkReply *second = m_network->get(instanceRequest(statuses));
        connect(second, &QNetworkReply::finished, this,
                [second, picture, guard, done] {
            second->deleteLater();
            if (!guard)
                return;
            if (second->error() != QNetworkReply::NoError) {
                done(second->errorString(), picture, {});
                return;
            }
            QList<AccountUpdate> updates;
            const QJsonArray items = QJsonDocument::fromJson(second->readAll()).array();
            for (const QJsonValue &value : items) {
                const QJsonObject status = value.toObject();
                AccountUpdate update;
                update.at = QDateTime::fromString(text(status, "created_at"),
                                                  Qt::ISODateWithMs).toMSecsSinceEpoch();
                update.text = plainExcerpt(text(status, "content"), 220);
                if (update.text.isEmpty())
                    update.text = QStringLiteral("A post with no text");
                const QString address = text(status, "url").isEmpty()
                    ? text(status, "uri") : text(status, "url");
                update.url = QUrl(address);
                updates.append(update);
            }
            done(QString(), picture, updates);
        });
    });
}

// Bluesky's public API: the profile names the picture, the author feed holds
// the posts, without the replies.
void CrmFeature::readBlueskyUpdates(const QString &actor, QObject *context,
                                    UpdatesHandler done)
{
    const auto endpoint = [actor](const QString &method,
                                  const QList<QPair<QString, QString>> &extra) {
        QUrl url;
        url.setScheme(QStringLiteral("https"));
        url.setHost(QStringLiteral("public.api.bsky.app"));
        url.setPath(QStringLiteral("/xrpc/") + method);
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("actor"), actor);
        for (const auto &item : extra)
            query.addQueryItem(item.first, item.second);
        url.setQuery(query);
        return url;
    };
    // An account Bluesky does not know answers 400; that is its answer.
    QNetworkRequest request = jsonRequest(
        endpoint(QStringLiteral("app.bsky.actor.getProfile"), {}));
    forkmesh::markExpectedHttpStatus(request, 400);
    QNetworkReply *reply = m_network->get(request);
    const QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, endpoint, actor, guard, done] {
        reply->deleteLater();
        if (!guard)
            return;
        if (forkmesh::networkReplyStatusIsExpected(reply)) {
            done(QStringLiteral("Bluesky knows no such account"), {}, {});
            return;
        }
        const QJsonObject profile = reply->error() == QNetworkReply::NoError
            ? QJsonDocument::fromJson(reply->readAll()).object() : QJsonObject();
        QUrl picture(text(profile, "avatar"));
        if (picture.scheme() != QLatin1String("https"))
            picture = QUrl();
        const QString handle = text(profile, "handle").isEmpty()
            ? actor : text(profile, "handle");
        QNetworkReply *feed = m_network->get(jsonRequest(
            endpoint(QStringLiteral("app.bsky.feed.getAuthorFeed"),
                     {{QStringLiteral("limit"), QString::number(kAccountUpdates)},
                      {QStringLiteral("filter"), QStringLiteral("posts_no_replies")}})));
        connect(feed, &QNetworkReply::finished, this,
                [feed, picture, handle, guard, done] {
            feed->deleteLater();
            if (!guard)
                return;
            if (feed->error() != QNetworkReply::NoError) {
                done(feed->errorString(), picture, {});
                return;
            }
            QList<AccountUpdate> updates;
            const QJsonArray items = QJsonDocument::fromJson(feed->readAll())
                                         .object().value(QStringLiteral("feed")).toArray();
            for (const QJsonValue &value : items) {
                const QJsonObject post = value.toObject()
                                             .value(QStringLiteral("post")).toObject();
                const QJsonObject record = post.value(QStringLiteral("record")).toObject();
                AccountUpdate update;
                update.at = QDateTime::fromString(text(record, "createdAt"),
                                                  Qt::ISODateWithMs).toMSecsSinceEpoch();
                update.text = plainExcerpt(text(record, "text").toHtmlEscaped(), 220);
                if (update.text.isEmpty())
                    update.text = QStringLiteral("A post with no text");
                // at://did/app.bsky.feed.post/<key> is bsky.app/profile/<handle>/post/<key>.
                const QString key = text(post, "uri").section(QLatin1Char('/'), -1);
                if (!key.isEmpty()) {
                    QUrl url;
                    url.setScheme(QStringLiteral("https"));
                    url.setHost(QStringLiteral("bsky.app"));
                    url.setPath(QStringLiteral("/profile/%1/post/%2").arg(handle, key));
                    update.url = url;
                }
                updates.append(update);
            }
            done(QString(), picture, updates);
        });
    });
}

// GitHub's public activity feed, read unauthenticated: what the account did,
// newest first, as far as the rate limit allows.
void CrmFeature::readGithubUpdates(const QString &user, QObject *context,
                                   UpdatesHandler done)
{
    QUrl events;
    events.setScheme(QStringLiteral("https"));
    events.setHost(QStringLiteral("api.github.com"));
    events.setPath(QStringLiteral("/users/%1/events/public").arg(user));
    events.setQuery(QStringLiteral("per_page=%1").arg(kAccountUpdates * 2));
    // An account GitHub does not know answers 404, and an address it has seen
    // too much of today answers 403; both are answers, not failed requests.
    QNetworkRequest request = jsonRequest(events);
    forkmesh::markExpectedHttpStatus(request, 404);
    forkmesh::markExpectedHttpStatus(request, 403);
    QNetworkReply *reply = m_network->get(request);
    const QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this, [reply, user, guard, done] {
        reply->deleteLater();
        if (!guard)
            return;
        const QUrl picture = accountKnownPicture(QStringLiteral("github:") + user);
        if (forkmesh::networkReplyStatusIsExpected(reply)) {
            done(QStringLiteral("GitHub answered %1")
                     .arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()),
                 picture, {});
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            done(reply->errorString(), picture, {});
            return;
        }
        QList<AccountUpdate> updates;
        const QJsonArray items = QJsonDocument::fromJson(reply->readAll()).array();
        for (const QJsonValue &value : items) {
            if (updates.size() >= kAccountUpdates)
                break;
            const QJsonObject event = value.toObject();
            AccountUpdate update;
            update.at = QDateTime::fromString(text(event, "created_at"),
                                              Qt::ISODateWithMs).toMSecsSinceEpoch();
            update.text = githubEventLine(event);
            const QString repository =
                text(event.value(QStringLiteral("repo")).toObject(), "name");
            if (repository.count(QLatin1Char('/')) == 1) {
                QUrl url;
                url.setScheme(QStringLiteral("https"));
                url.setHost(QStringLiteral("github.com"));
                url.setPath(QLatin1Char('/') + repository);
                update.url = url;
            }
            updates.append(update);
        }
        done(QString(), picture, updates);
    });
}

// The card's picture, read once per source per session: reopening a card, or
// opening one on a contact who shares the account, shows it at once.
void CrmFeature::readContactPhoto(const QString &source, const QUrl &picture,
                                  QObject *context, std::function<void()> done)
{
    QNetworkRequest request(picture);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("image/*"));
    request.setTransferTimeout(15000);
    forkmesh::markExpectedHttpStatus(request, 404);
    forkmesh::markExpectedTransportFailure(request);
    QNetworkReply *reply = m_network->get(request);
    const QPointer<QObject> guard(context);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, source, guard, done] {
        reply->deleteLater();
        QImage image;
        if (reply->error() == QNetworkReply::NoError)
            image.loadFromData(reply->readAll());
        // Kept at the card's own size: the originals an instance serves are
        // several times larger than anything the card shows.
        if (!image.isNull() && image.width() > kDetailPhotoSize * 2) {
            image = image.scaled(kDetailPhotoSize * 2, kDetailPhotoSize * 2,
                                 Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        }
        // A source that answered with nothing is remembered too, so the card
        // asks for it once and falls through to the initials.
        m_photos.insert(source, image);
        if (guard)
            done();
    });
}

void CrmFeature::editContact(const QJsonObject &contact)
{
    if (m_organization->currentData().toString().isEmpty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(contact.isEmpty() ? QStringLiteral("Add contact")
                                             : QStringLiteral("Edit contact"));
    dialog.resize(680, 720);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *name = lineEdit(text(contact, "name"));
    auto *email = lineEdit(text(contact, "email"));
    auto *company = lineEdit(text(contact, "company"));
    auto *title = lineEdit(text(contact, "title"));
    auto *phone = lineEdit(text(contact, "phone"));
    auto *twitter = lineEdit(text(contact, "twitter"));
    twitter->setPlaceholderText(QStringLiteral("@handle or profile URL"));
    auto *mastodon = lineEdit(text(contact, "mastodon"));
    mastodon->setPlaceholderText(QStringLiteral("@name@instance or profile URL"));
    auto *bluesky = lineEdit(text(contact, "bluesky"));
    bluesky->setPlaceholderText(QStringLiteral("name.bsky.social or profile URL"));
    auto *linkedin = lineEdit(text(contact, "linkedin"));
    linkedin->setPlaceholderText(QStringLiteral("linkedin.com/in/name"));
    auto *github = lineEdit(text(contact, "github"));
    github->setPlaceholderText(QStringLiteral("@username or profile URL"));
    auto *website = lineEdit(text(contact, "website"));
    website->setPlaceholderText(QStringLiteral("https://example.com"));
    auto *location = lineEdit(text(contact, "location"));
    location->setPlaceholderText(QStringLiteral("City, country"));
    auto *owner = lineEdit(text(contact, "owner"));
    auto *source = lineEdit(text(contact, "source"));
    auto *tags = lineEdit();
    QStringList tagValues;
    for (const QJsonValue &value : contact.value(QStringLiteral("tags")).toArray())
        tagValues << value.toString();
    tags->setText(tagValues.join(QStringLiteral(", ")));
    auto *flow = new QComboBox;
    const QJsonArray flows = m_settings.value(QStringLiteral("flows")).toArray();
    for (const QJsonValue &value : flows) {
        const QJsonObject item = value.toObject();
        flow->addItem(text(item, "name"), text(item, "id"));
    }
    // A contact added while a pipeline list is open starts in that pipeline.
    const QString sheetFlow = text(activeSheet(), "flow");
    const QString requestedFlow = !text(contact, "flow").isEmpty() ? text(contact, "flow")
        : contact.isEmpty() && !sheetFlow.isEmpty()                ? sheetFlow
                                                                   : QStringLiteral("customers");
    flow->setCurrentIndex(qMax(0, flow->findData(requestedFlow)));
    auto *stage = new QComboBox;
    const auto populateStages = [flows, flow, stage](const QString &selected = QString()) {
        stage->clear();
        for (const QJsonValue &value : flows) {
            const QJsonObject item = value.toObject();
            if (text(item, "id") != flow->currentData().toString())
                continue;
            for (const QJsonValue &stageValue :
                 item.value(QStringLiteral("stages")).toArray()) {
                const QString name = stageValue.toString();
                stage->addItem(name, name);
            }
            break;
        }
        stage->setCurrentIndex(qMax(0, stage->findData(selected)));
    };
    populateStages(text(contact, "stage"));
    makeSearchable(flow);
    makeSearchable(stage);
    connect(flow, &QComboBox::currentIndexChanged, &dialog,
            [populateStages](int) { populateStages(); });
    auto *value = new QSpinBox;
    value->setRange(0, 1000000000);
    value->setGroupSeparatorShown(true);
    value->setSpecialValueText(QStringLiteral("None"));
    value->setValue(int(qMin<qint64>(
        qint64(contact.value(QStringLiteral("value")).toDouble()), 1000000000)));
    auto *nextStep = lineEdit(text(contact, "nextStep"));
    nextStep->setMaxLength(200);
    nextStep->setPlaceholderText(QString::fromUtf8(
        "Send the deck, book a call, chase the contract\xE2\x80\xA6"));
    auto *notes = new QPlainTextEdit(text(contact, "notes"));
    notes->setMinimumHeight(130);
    form->addRow(QStringLiteral("Name"), name);
    form->addRow(QStringLiteral("Email"), email);
    form->addRow(QStringLiteral("Company"), company);
    form->addRow(QStringLiteral("Title"), title);
    form->addRow(QStringLiteral("Phone"), phone);
    form->addRow(QStringLiteral("Twitter"), twitter);
    form->addRow(QStringLiteral("Mastodon"), mastodon);
    form->addRow(QStringLiteral("Bluesky"), bluesky);
    form->addRow(QStringLiteral("LinkedIn"), linkedin);
    form->addRow(QStringLiteral("GitHub"), github);
    form->addRow(QStringLiteral("Website"), website);
    form->addRow(QStringLiteral("Location"), location);
    form->addRow(QStringLiteral("Flow"), flow);
    form->addRow(QStringLiteral("Stage"), stage);
    form->addRow(QStringLiteral("Deal value"), value);
    form->addRow(QStringLiteral("Next step"), nextStep);
    form->addRow(QStringLiteral("Owner"), owner);
    form->addRow(QStringLiteral("Source"), source);
    form->addRow(QStringLiteral("Tags"), tags);
    form->addRow(QStringLiteral("Notes"), notes);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    // Profiles are researched by hand: open a web search seeded with whatever
    // the record already knows, so handles can be pasted back into the form.
    auto *lookup = buttons->addButton(
        QStringLiteral("Search the web for profiles"),
        QDialogButtonBox::ActionRole);
    connect(lookup, &QPushButton::clicked, &dialog, [name, company, email] {
        const QString subject = name->text().trimmed().isEmpty()
            ? (company->text().trimmed().isEmpty() ? email->text().trimmed()
                                                   : company->text().trimmed())
            : QStringLiteral("\"%1\" %2").arg(name->text().trimmed(),
                                              company->text().trimmed()).trimmed();
        if (subject.isEmpty())
            return;
        QUrl url(QStringLiteral("https://duckduckgo.com/"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("q"),
                           subject + QStringLiteral(
                               " twitter OR mastodon OR bluesky OR linkedin"));
        url.setQuery(query);
        QDesktopServices::openUrl(url);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    QJsonObject body{
        {QStringLiteral("name"), name->text()},
        {QStringLiteral("email"), email->text()},
        {QStringLiteral("company"), company->text()},
        {QStringLiteral("title"), title->text()},
        {QStringLiteral("phone"), phone->text()},
        {QStringLiteral("twitter"), twitter->text()},
        {QStringLiteral("mastodon"), mastodon->text()},
        {QStringLiteral("bluesky"), bluesky->text()},
        {QStringLiteral("linkedin"), linkedin->text()},
        {QStringLiteral("github"), github->text()},
        {QStringLiteral("website"), website->text()},
        {QStringLiteral("location"), location->text()},
        {QStringLiteral("flow"), flow->currentData().toString()},
        {QStringLiteral("stage"), stage->currentData().toString()},
        {QStringLiteral("value"), value->value()},
        {QStringLiteral("nextStep"), nextStep->text()},
        {QStringLiteral("owner"), owner->text()},
        {QStringLiteral("source"), source->text()},
        {QStringLiteral("tags"), QJsonArray::fromStringList(
             tags->text().split(QLatin1Char(','), Qt::SkipEmptyParts))},
        {QStringLiteral("notes"), notes->toPlainText()},
    };
    // A contact added while a list is open goes on that list, or it would land
    // on none of the tabs in view.
    const QJsonObject listOpen = activeSheet();
    if (contact.isEmpty() && storedSheet(listOpen) && text(listOpen, "flow").isEmpty())
        body.insert(QStringLiteral("views"), QJsonArray{text(listOpen, "id")});
    const QString id = text(contact, "id");
    request(id.isEmpty() ? QByteArrayLiteral("POST")
                         : QByteArrayLiteral("PATCH"),
            organizationPath(QStringLiteral("/contacts") +
                             (id.isEmpty() ? QString()
                                           : QLatin1Char('/') + id)),
            body, [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok) {
            setStatus(QStringLiteral("Contact was not saved: %1").arg(error), true);
            return;
        }
        setStatus(QStringLiteral("Contact saved."));
        loadOrganization();
    });
}

void CrmFeature::deleteContact(const QJsonObject &contact)
{
    const QJsonObject current = contact.isEmpty() ? selectedContact() : contact;
    const QString id = text(current, "id");
    if (id.isEmpty() || QMessageBox::question(
            this, QStringLiteral("Delete CRM contact"),
            QStringLiteral("Delete %1? Existing email history will remain.")
                .arg(text(current, "name"))) != QMessageBox::Yes)
        return;
    request(QByteArrayLiteral("DELETE"),
            organizationPath(QStringLiteral("/contacts/") + id), {},
            [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Contact was not deleted: %1").arg(error), true);
        setStatus(QStringLiteral("Contact deleted."));
        loadOrganization();
    });
}

// The prompt "Copy prompt" hands to an assistant: what ForkMesh is, who the
// contact is, and a short toot for them. The sheet the row is open in (or the
// contact's pipeline) sets the angle, so a journalist is offered a story and
// an investor the opportunity.
QString CrmFeature::outreachPrompt(const QJsonObject &contact) const
{
    const QString name = text(contact, "name").isEmpty() ? text(contact, "email")
                                                          : text(contact, "name");
    const QString mastodon = text(contact, "mastodon");
    const QString flowId = text(contact, "flow");
    const QString flow = flowId.isEmpty() ? QString() : flowLabel(flowId);
    QString group = text(activeSheet(), "name");
    if (group.isEmpty() || group == QLatin1String("No pipeline"))
        group = flow;
    const QString audience = (group + QLatin1Char(' ') + flow).toLower();
    const auto mentions = [&audience](std::initializer_list<const char *> words) {
        for (const char *word : words) {
            if (audience.contains(QLatin1String(word)))
                return true;
        }
        return false;
    };

    QStringList about;
    if (!mastodon.isEmpty()) {
        const QUrl profile = mastodonProfileUrl(mastodon);
        about << QStringLiteral("Mastodon: %1").arg(mastodon) +
                     (profile.isValid() && profile.toString() != mastodon
                          ? QStringLiteral(" (%1)").arg(profile.toString()) : QString());
    } else {
        about << QStringLiteral("no Mastodon handle saved, so look it up");
    }
    const QString title = text(contact, "title");
    const QString company = text(contact, "company");
    if (!title.isEmpty() && !company.isEmpty())
        about << QStringLiteral("%1 at %2").arg(title, company);
    else if (!title.isEmpty() || !company.isEmpty())
        about << title + company;
    for (const char *field : {"website", "location"}) {
        if (!text(contact, field).isEmpty())
            about << text(contact, field);
    }

    QString angle;
    const QString quoted = QString::fromUtf8("\xE2\x80\x9C%1\xE2\x80\x9D").arg(group);
    if (mentions({"journalist", "press", "media", "reporter", "writer", "editor",
                  "podcast", "newsletter", "blog"})) {
        angle = QStringLiteral(
            "They are on my %1 sheet as a journalist, so pitch a story: why "
            "decentralized code hosting matters now and what is new about "
            "ForkMesh, and offer an interview or a demo. No sales talk.").arg(quoted);
    } else if (mentions({"investor", "angel", "vc", "fund", "capital"})) {
        angle = QStringLiteral(
            "They are on my %1 sheet as an investor, so lead with the "
            "opportunity ForkMesh opens and ask for a short call; state only "
            "facts you were given or looked up, and never invent traction, "
            "numbers or names.").arg(quoted);
    } else if (mentions({"partner", "integration", "alliance"})) {
        angle = QStringLiteral(
            "They are on my %1 sheet as a potential partner, so show how "
            "ForkMesh fits what they build and suggest exploring working "
            "together.").arg(quoted);
    } else if (mentions({"customer", "lead", "user", "developer", "maintainer",
                         "community", "open source"})) {
        angle = QStringLiteral(
            "They are on my %1 sheet as a potential user, so give one concrete "
            "way ForkMesh helps their projects and invite them to try it.").arg(quoted);
    } else if (!group.isEmpty()) {
        angle = QStringLiteral(
            "They are on my %1 sheet, so pitch it the way that suits that group.")
                    .arg(quoted);
    }

    QStringList prompt{
        QStringLiteral("Tell me about ForkMesh (https://forkmesh.com)."),
        QStringLiteral("Then tell me about %1 (%2).").arg(name, about.join(QStringLiteral("; "))),
        QStringLiteral("Then write a short toot (under 500 characters) I can send "
                       "them on Mastodon about ForkMesh.") +
            (angle.isEmpty() ? QString() : QLatin1Char(' ') + angle) +
            QStringLiteral(" Mention them by their handle, make it specific to "
                           "their work, and use at most two hashtags.")};
    return prompt.join(QStringLiteral("\n\n"));
}

void CrmFeature::copyOutreachPrompt(const QJsonObject &contact)
{
    if (contact.isEmpty())
        return;
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(outreachPrompt(contact));
    const QString name = text(contact, "name").isEmpty() ? text(contact, "email")
                                                          : text(contact, "name");
    setStatus(QStringLiteral("Copied a prompt about %1. Paste it into your assistant.")
                  .arg(name));
}

QString CrmFeature::aiBrief() const
{
    const QString saved = text(m_settings, "messageBrief");
    if (!saved.isEmpty())
        return saved;
    return m_organization && m_organization->currentData().toString().toLower() ==
                                 QLatin1String("forkmesh")
        ? kForkMeshBrief.join(QLatin1Char('\n'))
        : QString();
}

QString CrmFeature::aiPrompt(const AiDraft &draft) const
{
    const QJsonObject contact = contactById(draft.contactId);
    const QString name = text(contact, "name").isEmpty()
        ? (text(contact, "email").isEmpty() ? QStringLiteral("Unnamed contact")
                                            : text(contact, "email"))
        : text(contact, "name");
    QString organization = m_organization ? m_organization->currentData().toString() : QString();
    if (organization.isEmpty())
        organization = QStringLiteral("our organization");
    const QString viewer = m_viewerName.isEmpty() ? m_actor : m_viewerName;
    const QString sender = viewer.isEmpty() ? QStringLiteral("the %1 team").arg(organization) : viewer;
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const auto day = [](qint64 at) {
        return at > 0 ? QDateTime::fromMSecsSinceEpoch(at).toLocalTime().toString(
                            QStringLiteral("yyyy-MM-dd"))
                      : QStringLiteral("an unknown date");
    };
    const QString mastodon = mastodonHandleKey(text(contact, "mastodon"));
    QString thread;
    qint64 threadAt = -1;
    for (const QJsonObject &toot : m_contactToots.value(draft.contactId)) {
        const QUrl url(text(toot, "providerUrl"));
        if (url.scheme() == QLatin1String("https") && emailSentAt(toot) > threadAt) {
            threadAt = emailSentAt(toot);
            thread = url.toString();
        }
    }

    QString ask;
    if (draft.format == QLatin1String("application")) {
        ask = QStringLiteral("a complete draft of our grant application to %1. Follow their form: "
                             "answer every question they ask, in their order and under their "
                             "headings, within their word or character limits, with a budget in "
                             "their currency that fits the amount they award.").arg(name);
    } else if (draft.format == QLatin1String("toot")) {
        ask = QStringLiteral("a Mastodon post (toot) to %1%2, at most 500 characters including the "
                             "mention and any link, with no more than two hashtags.%3")
                  .arg(name,
                       mastodon.isEmpty() ? QString()
                                          : QStringLiteral(" that mentions @%1 at the start").arg(mastodon),
                       thread.isEmpty() ? QString()
                                        : QStringLiteral(" Write it as a reply in our existing thread: %1").arg(thread));
    } else if (draft.format == QLatin1String("message")) {
        ask = QStringLiteral("a short direct message to %1 for LinkedIn, X or Bluesky: under 80 "
                             "words, no subject line, signed by %2.").arg(name, sender);
    } else {
        ask = QStringLiteral("an email to %1. Start with a \"Subject:\" line, then the body, signed "
                             "by %2. Keep the body under 150 words.").arg(name, sender);
    }
    const QString goal = draft.format == QLatin1String("application")
        ? QStringLiteral("We are applying to %1 for funding: make the case that funding %2 "
                         "advances their mission, and propose one concrete, fundable piece of %2 "
                         "work with milestones and deliverables.").arg(name, organization)
        : draft.mode == QLatin1String("reach")
        ? QStringLiteral("This is our first contact with %1: introduce %2 in a sentence and give "
                         "them one concrete reason it matters to them in particular.")
              .arg(name, organization)
        : QStringLiteral("We are already in touch with %1: pick up where the conversation below "
                         "left off, answer anything they asked, and do not repeat the first pitch.")
              .arg(name);
    const QString flow = flowLabel(text(contact, "flow"));
    QString group = text(activeSheet(), "name");
    if (group == QLatin1String("No pipeline"))
        group.clear();
    const QString angle = aiAngle(group, flow);

    QStringList facts;
    const auto fact = [&facts](const QString &label, const QString &value) {
        if (!value.trimmed().isEmpty())
            facts << QStringLiteral("- %1: %2").arg(label, value.trimmed());
    };
    fact(QStringLiteral("Title"), text(contact, "title"));
    fact(QStringLiteral("Company"), text(contact, "company"));
    fact(QStringLiteral("Location"), text(contact, "location"));
    QStringList pipeline;
    if (!flow.isEmpty())
        pipeline << flow;
    if (!text(contact, "stage").isEmpty())
        pipeline << CrmStageStrip::stageLabel(text(contact, "stage"));
    fact(QStringLiteral("Pipeline"), pipeline.join(QStringLiteral(", stage ")));
    const qint64 value = qint64(contact.value(QStringLiteral("value")).toDouble());
    fact(QStringLiteral("Deal value"), value > 0 ? QLocale(QLocale::English).toString(value) : QString());
    fact(QStringLiteral("Next step on our record"), text(contact, "nextStep"));
    fact(QStringLiteral("Owner"), text(contact, "owner"));
    fact(QStringLiteral("Source"), text(contact, "source"));
    QStringList tags;
    for (const QJsonValue &tag : contact.value(QStringLiteral("tags")).toArray())
        tags << tag.toString();
    fact(QStringLiteral("Tags"), tags.join(QStringLiteral(", ")));
    const qint64 last = qint64(contact.value(QStringLiteral("lastContactedAt")).toDouble());
    fact(QStringLiteral("Last contacted"), last > 0 ? day(last) : QStringLiteral("never"));
    const qint64 created = qint64(contact.value(QStringLiteral("createdAt")).toDouble());
    if (created > 0)
        fact(QStringLiteral("Added to our CRM"),
             day(created) + (text(contact, "createdBy").isEmpty()
                                 ? QString() : QStringLiteral(" by ") + text(contact, "createdBy")));
    fact(QStringLiteral("Email"), text(contact, "email"));
    for (const char *field : {"twitter", "mastodon", "bluesky", "linkedin", "github", "website"}) {
        const QString raw = text(contact, field);
        const QUrl link = profileUrl(QString::fromLatin1(field), raw);
        QString label;
        for (const ContactColumn &column : contactColumns()) {
            if (qstrcmp(column.field, field) == 0)
                label = QString::fromLatin1(column.label);
        }
        fact(label, link.isValid() ? link.toString() : raw);
    }
    fact(QStringLiteral("Phone"), text(contact, "phone"));
    fact(QStringLiteral("Their investor deck link"), text(deckLinkFor(draft.contactId), "url"));
    const QJsonObject custom = contact.value(QStringLiteral("fields")).toObject();
    int customCount = 0;
    for (auto entry = custom.constBegin(); entry != custom.constEnd() && customCount < kAiFieldLimit;
         ++entry, ++customCount) {
        const QString shown = entry.value().isDouble() ? QString::number(entry.value().toDouble())
                                                       : entry.value().toString().simplified();
        fact(customColumnLabel(entry.key()),
             shown.size() > 200 ? shown.left(199) + QString::fromUtf8("\xE2\x80\xA6") : shown);
    }
    if (facts.isEmpty())
        facts << QStringLiteral("- Nothing beyond the name is on record.");

    QStringList prompt{
        QStringLiteral("You are helping %1 at %2 write %3").arg(sender, organization, ask),
        QString(),
        goal,
    };
    if (!angle.isEmpty())
        prompt << angle;
    prompt << QString() << QStringLiteral("How to write it:");
    if (draft.format == QLatin1String("application")) {
        prompt << QStringLiteral("- If you can browse, open their program page and application "
                                 "form (links below) first, and use their current questions, "
                                 "limits, deadline and criteria; say so if anything on our record "
                                 "is out of date.")
               << QStringLiteral("- Answer each question under its own heading, with its word or "
                                 "character count after it.")
               << QStringLiteral("- Fit the request to their range and to what they fund; break "
                                 "the budget into milestones with dates and deliverables.")
               << QStringLiteral("- Plain, specific words, no hype, no emoji. Use only facts given "
                                 "here or found on the linked pages; leave out anything you cannot "
                                 "back up.")
               << QStringLiteral("- Where the form needs something only we can supply (legal "
                                 "entity, registration numbers, bank details, CVs, letters of "
                                 "support), never invent it: list it at the end instead.")
               << QStringLiteral("- Reply with the finished application, ready to paste, then a "
                                 "checklist of every document and fact we still have to supply, "
                                 "then three ways to make it stronger.");
    } else {
        prompt << QStringLiteral("- Make it personal. Open with something specific to them, taken "
                                 "from their recent posts, work or links below, never a compliment "
                                 "that would fit anyone.")
               << QStringLiteral("- Say plainly who we are, why we are writing to them in "
                                 "particular, and why now.")
               << QStringLiteral("- End with one clear, low-effort ask (a reply, a short call, or "
                                 "trying one thing).")
               << QStringLiteral("- Plain, warm words, short paragraphs, no hype, no emoji, no "
                                 "[placeholders]. Use only facts given here or found on the linked "
                                 "pages; leave out anything you cannot back up.")
               << QStringLiteral("- If you can browse, open their profiles and posts and our site, "
                                 "blog and changelog below first, and use anything newer than what "
                                 "is written here.")
               << QStringLiteral("- Reply with the finished message, ready to paste, then two "
                                 "alternative opening lines.");
    }
    prompt << QString()
           << QStringLiteral("## About us: %1").arg(organization)
           << (draft.brief.trimmed().isEmpty()
                   ? QStringLiteral("(No description saved yet. Look %1 up before writing.)")
                         .arg(organization)
                   : draft.brief.trimmed())
           << QString()
           << QStringLiteral("## About %1").arg(name);
    prompt << facts;
    const QString notes = text(contact, "notes");
    if (!notes.isEmpty()) {
        prompt << QString() << QStringLiteral("Our notes on them:")
               << (notes.size() > kAiNotesChars
                       ? QString::fromUtf8("\xE2\x80\xA6") + notes.right(kAiNotesChars)
                       : notes);
    }
    prompt << QString() << QStringLiteral("## Their recent public posts");
    if (!draft.updatesRead) {
        prompt << QStringLiteral("Not read yet: open their profiles linked above.");
    } else if (draft.updates.isEmpty()) {
        prompt << QStringLiteral("No Mastodon, Bluesky or GitHub account on record; look at the "
                                 "links above.");
    } else {
        for (const auto &[source, read] : draft.updates) {
            const QString field = source.section(QLatin1Char(':'), 0, 0);
            const QString account = source.section(QLatin1Char(':'), 1);
            const QUrl link = profileUrl(field, text(contact, field));
            const QString head = QStringLiteral("%1 %2%3").arg(
                accountServiceLabel(source),
                field == QLatin1String("bluesky") ? account : QStringLiteral("@") + account,
                link.isValid() ? QStringLiteral(" (%1)").arg(link.toString()) : QString());
            if (!read.error.isEmpty()) {
                prompt << QStringLiteral("%1: could not be read (%2); open the profile yourself.")
                              .arg(head, read.error);
            } else if (read.updates.isEmpty()) {
                prompt << QStringLiteral("%1: no public posts.").arg(head);
            } else {
                prompt << head + QLatin1Char(':');
                for (const AccountUpdate &post : read.updates) {
                    prompt << QStringLiteral("- %1: %2%3").arg(
                        day(post.at), post.text,
                        post.url.isValid() ? QStringLiteral(" (%1)").arg(post.url.toString())
                                           : QString());
                }
            }
        }
    }
    prompt << QString() << QStringLiteral("## Our conversation so far (oldest first)");
    QList<QJsonObject> messages = draft.messages;
    std::sort(messages.begin(), messages.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return emailSentAt(a) < emailSentAt(b);
    });
    if (messages.isEmpty())
        prompt << QStringLiteral("Nothing on record yet: this is the first message.");
    for (const QJsonObject &message : std::as_const(messages)) {
        const bool inbound = text(message, "direction") == QLatin1String("inbound");
        const QString status = text(message, "status").toLower();
        QString head = QStringLiteral("- %1%2%3 via %4").arg(
            day(emailSentAt(message)), dot,
            inbound ? QStringLiteral("They wrote")
                    : QStringLiteral("We wrote") + (text(message, "sender").isEmpty()
                                                        ? QString()
                                                        : QStringLiteral(" (%1)").arg(text(message, "sender"))),
            aiProviderLabel(message));
        if (!inbound && kUnsentStatuses.contains(status))
            head += QStringLiteral(" [%1, not delivered]").arg(status);
        if (!text(message, "subject").isEmpty())
            head += dot + QStringLiteral("Subject: ") + text(message, "subject");
        const QUrl url(text(message, "providerUrl"));
        if (url.scheme() == QLatin1String("https"))
            head += dot + url.toString();
        prompt << head;
        const QString body = aiMessageText(text(message, "text"));
        for (const QString &line : body.isEmpty() ? QStringList() : body.split(QLatin1Char('\n')))
            prompt << QStringLiteral("  > ") + line;
    }
    if (!draft.reason.isEmpty())
        prompt << QString() << QStringLiteral("## Why now") << draft.reason;
    return prompt.join(QLatin1Char('\n'));
}

void CrmFeature::showAiWriter(const QString &contactId, const QString &mode,
                              const QString &reason)
{
    const QJsonObject contact = contactById(contactId);
    if (contact.isEmpty())
        return;
    const QString name = text(contact, "name").isEmpty()
        ? (text(contact, "email").isEmpty() ? QStringLiteral("Unnamed contact")
                                            : text(contact, "email"))
        : text(contact, "name");
    auto draft = std::make_shared<AiDraft>();
    draft->contactId = contactId;
    draft->reason = reason;
    draft->brief = aiBrief();
    QList<QJsonObject> linked = m_contactEmails.value(contactId);
    linked.append(m_contactToots.value(contactId));
    std::sort(linked.begin(), linked.end(), [](const QJsonObject &a, const QJsonObject &b) {
        return emailSentAt(a) > emailSentAt(b);
    });
    draft->messages = linked.mid(0, kAiMessageLimit);
    bool talked = qint64(contact.value(QStringLiteral("lastContactedAt")).toDouble()) > 0;
    for (const QJsonObject &message : std::as_const(draft->messages)) {
        if (text(message, "direction") == QLatin1String("inbound") ||
            !kUnsentStatuses.contains(text(message, "status").toLower()))
            talked = true;
    }
    draft->mode = mode.isEmpty() ? (talked ? QStringLiteral("follow") : QStringLiteral("reach"))
                                 : mode;
    const QString email = text(contact, "email");
    draft->format = flowIconId(text(contact, "flow")) == QLatin1String("grants")
        ? QStringLiteral("application")
        : email.contains(QLatin1Char('@')) ? QStringLiteral("email")
        : !mastodonHandleKey(text(contact, "mastodon")).isEmpty() ? QStringLiteral("toot")
                                                                  : QStringLiteral("message");
    const QStringList sources = avatarSources(contact);
    draft->updatesRead = sources.isEmpty();

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("crmAiWriter"));
    dialog->setWindowTitle(QStringLiteral("Write to %1 with AI").arg(name));
    dialog->resize(760, 680);
    auto *layout = new QVBoxLayout(dialog);
    auto *intro = new QLabel(QStringLiteral(
        "One prompt holds who we are, everything on their record, their recent public posts and "
        "every message so far. ChatGPT or Claude opens in your browser with it filled in and "
        "writes the message there; nothing is sent from here."));
    intro->setWordWrap(true);
    intro->setObjectName(QStringLiteral("mutedLabel"));
    layout->addWidget(intro);
    auto *formatRow = new QHBoxLayout;
    auto *format = new QComboBox;
    format->addItem(QStringLiteral("Email"), QStringLiteral("email"));
    format->addItem(QStringLiteral("Direct message"), QStringLiteral("message"));
    format->addItem(QStringLiteral("Toot"), QStringLiteral("toot"));
    format->addItem(QStringLiteral("Grant application"), QStringLiteral("application"));
    format->setCurrentIndex(format->findData(draft->format));
    formatRow->addWidget(new QLabel(QStringLiteral("Write")));
    formatRow->addWidget(format);
    formatRow->addStretch(1);
    layout->addLayout(formatRow);
    layout->addWidget(new QLabel(QStringLiteral("About us: the brief every prompt starts from")));
    auto *brief = new QPlainTextEdit(draft->brief);
    brief->setMaximumHeight(130);
    brief->setPlaceholderText(QStringLiteral("What %1 does, who it is for, links, recent news")
                                  .arg(m_organization->currentData().toString()));
    layout->addWidget(brief);
    auto *saveBrief = new QPushButton(QStringLiteral("Save for the organization"));
    saveBrief->setVisible(m_canManageSettings);
    auto *briefRow = new QHBoxLayout;
    briefRow->addStretch(1);
    briefRow->addWidget(saveBrief);
    layout->addLayout(briefRow);
    layout->addWidget(new QLabel(QStringLiteral("Prompt (edit anything before opening)")));
    auto *prompt = new QPlainTextEdit;
    prompt->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    prompt->setAccessibleName(QStringLiteral("Prompt"));
    layout->addWidget(prompt, 1);
    auto *status = new QLabel;
    status->setWordWrap(true);
    status->setObjectName(QStringLiteral("mutedLabel"));
    layout->addWidget(status);
    auto *buttons = new QHBoxLayout;
    auto *copy = new QPushButton(QStringLiteral("Copy prompt"));
    auto *claude = new QPushButton(QStringLiteral("Open in Claude"));
    auto *chatgpt = new QPushButton(QStringLiteral("Open in ChatGPT"));
    chatgpt->setObjectName(QStringLiteral("primaryButton"));
    buttons->addStretch(1);
    for (QPushButton *button : {copy, claude, chatgpt})
        buttons->addWidget(button);
    layout->addLayout(buttons);

    auto edited = std::make_shared<bool>(false);
    const auto render = [this, draft, prompt, edited](bool force) {
        if (*edited && !force)
            return;
        const QSignalBlocker blocker(prompt);
        prompt->setPlainText(aiPrompt(*draft));
        *edited = false;
    };
    render(true);
    connect(prompt, &QPlainTextEdit::textChanged, dialog, [edited] { *edited = true; });
    connect(format, &QComboBox::currentIndexChanged, dialog, [draft, format, render] {
        draft->format = format->currentData().toString();
        render(true);
    });
    connect(brief, &QPlainTextEdit::textChanged, dialog, [draft, brief, render] {
        draft->brief = brief->toPlainText();
        render(true);
    });
    connect(copy, &QPushButton::clicked, dialog, [prompt, status] {
        if (QClipboard *clipboard = QGuiApplication::clipboard())
            clipboard->setText(prompt->toPlainText());
        status->setText(QStringLiteral("Prompt copied."));
    });
    const auto open = [prompt, status](const QString &label, const char *base) {
        const QString text = prompt->toPlainText();
        if (QClipboard *clipboard = QGuiApplication::clipboard())
            clipboard->setText(text);
        const QByteArray filled =
            QByteArray(base) + "?q=" + QUrl::toPercentEncoding(text);
        const bool fits = filled.size() <= kAiUrlLimit;
        QDesktopServices::openUrl(fits ? QUrl::fromEncoded(filled)
                                       : QUrl(QString::fromLatin1(base)));
        status->setText(fits
            ? QStringLiteral("Opened %1 with the prompt filled in. It is on your clipboard too.")
                  .arg(label)
            : QStringLiteral("The prompt is too long for a link, so it is on your clipboard: "
                             "paste it into %1.").arg(label));
    };
    connect(claude, &QPushButton::clicked, dialog,
            [open] { open(QStringLiteral("Claude"), kAiClaudeUrl); });
    connect(chatgpt, &QPushButton::clicked, dialog,
            [open] { open(QStringLiteral("ChatGPT"), kAiChatGptUrl); });
    connect(saveBrief, &QPushButton::clicked, dialog, [this, brief, saveBrief, status] {
        const QPointer<QPushButton> button(saveBrief);
        const QPointer<QLabel> label(status);
        saveBrief->setEnabled(false);
        request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")),
                QJsonObject{{QStringLiteral("messageBrief"),
                             brief->toPlainText().trimmed().left(kAiBriefLimit)}},
                [this, button, label](bool ok, const QJsonObject &payload, const QString &error) {
            if (button)
                button->setEnabled(true);
            if (ok)
                m_settings = payload.value(QStringLiteral("settings")).toObject();
            if (label)
                label->setText(ok ? QStringLiteral("Saved as %1's brief for every member.")
                                        .arg(m_organization->currentData().toString())
                                  : QStringLiteral("Could not save the brief: %1").arg(error));
        });
    });

    auto pending = std::make_shared<int>(0);
    const auto settle = [draft, pending, status, render, edited] {
        if (--*pending > 0)
            return;
        int posts = 0;
        for (const auto &update : std::as_const(draft->updates))
            posts += int(update.second.updates.size());
        draft->updatesRead = true;
        status->setText(QStringLiteral("%1: %2 past message%3 and %4 recent post%5 are in the prompt.")
                            .arg(*edited ? QStringLiteral("Read, but your edits to the prompt were kept")
                                         : QStringLiteral("Ready"))
                            .arg(draft->messages.size())
                            .arg(draft->messages.size() == 1 ? QString() : QStringLiteral("s"))
                            .arg(posts)
                            .arg(posts == 1 ? QString() : QStringLiteral("s")));
        render(false);
    };
    *pending = int(draft->messages.size() + sources.size());
    if (*pending == 0) {
        status->setText(QStringLiteral("Nothing to read: the prompt holds the record, our notes "
                                       "and the brief."));
    } else {
        status->setText(QString::fromUtf8("Reading %1 past message%2 and %3 public account%4\xE2\x80\xA6")
                            .arg(draft->messages.size())
                            .arg(draft->messages.size() == 1 ? QString() : QStringLiteral("s"))
                            .arg(sources.size())
                            .arg(sources.size() == 1 ? QString() : QStringLiteral("s")));
    }
    const QPointer<QDialog> guard(dialog);
    for (int at = 0; at < draft->messages.size(); ++at) {
        const QString id = text(draft->messages.at(at), "id");
        request(QByteArrayLiteral("GET"), organizationPath(QStringLiteral("/emails/") + id), {},
                [guard, draft, at, settle](bool ok, const QJsonObject &payload, const QString &) {
            if (!guard)
                return;
            const QJsonObject full = payload.value(QStringLiteral("email")).toObject();
            if (ok && at < draft->messages.size()) {
                const QString plain = text(full, "text").isEmpty()
                    ? QTextDocumentFragment::fromHtml(text(full, "html")).toPlainText()
                    : full.value(QStringLiteral("text")).toString();
                draft->messages[at].insert(QStringLiteral("text"), plain);
            }
            settle();
        });
    }
    for (const QString &source : sources) {
        draft->updates.append({source, AccountRead{}});
        const int at = int(draft->updates.size()) - 1;
        const auto cached = m_accountReads.constFind(source);
        if (cached != m_accountReads.constEnd()) {
            draft->updates[at].second = *cached;
            QTimer::singleShot(0, dialog, settle);
            continue;
        }
        readAccountUpdates(source, dialog,
                           [this, draft, at, source, settle](const QString &error, const QUrl &picture,
                                                             const QList<AccountUpdate> &updates) {
            m_accountReads.insert(source, AccountRead{error, picture, updates});
            draft->updates[at].second = AccountRead{error, picture, updates};
            settle();
        });
    }
    dialog->show();
}

QJsonObject CrmFeature::deckLinkFor(const QString &contactId) const
{
    for (const QJsonValue &value : m_decks) {
        const QJsonObject link = value.toObject();
        if (text(link, "contactId") == contactId &&
            link.value(QStringLiteral("active")).toBool())
            return link;
    }
    return {};
}

QString CrmFeature::deckActivityLine(const QJsonObject &link) const
{
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const int views = link.value(QStringLiteral("views")).toInt();
    if (views <= 0) {
        return QStringLiteral("Investor deck") + dot +
               QStringLiteral("link made %1, not opened yet").arg(
                   timestamp(qint64(link.value(QStringLiteral("createdAt")).toDouble())));
    }
    const int seconds = link.value(QStringLiteral("seconds")).toInt();
    return QStringList{
        QStringLiteral("Investor deck"),
        views == 1 ? QStringLiteral("opened once")
                   : QStringLiteral("opened %1 times").arg(views),
        QStringLiteral("%1 of %2 pages read").arg(
            link.value(QStringLiteral("pagesViewed")).toInt())
            .arg(link.value(QStringLiteral("pageCount")).toInt()),
        seconds >= 60 ? QStringLiteral("%1 min reading").arg(seconds / 60)
                      : QStringLiteral("%1 s reading").arg(seconds),
        QStringLiteral("last opened %1").arg(
            timestamp(qint64(link.value(QStringLiteral("lastViewedAt")).toDouble()))),
    }.join(dot);
}

QString CrmFeature::investorEmailPrompt(const QJsonObject &contact,
                                        const QString &deckUrl) const
{
    const QString contactId = text(contact, "id");
    QStringList record;
    for (const ContactColumn &column : contactColumns()) {
        const QString field = QString::fromLatin1(column.field);
        QString value;
        if (field == QLatin1String("tags")) {
            QStringList tags;
            for (const QJsonValue &tag : contact.value(QStringLiteral("tags")).toArray())
                tags << tag.toString();
            value = tags.join(QStringLiteral(", "));
        } else if (field == QLatin1String("flow")) {
            value = text(contact, "flow").isEmpty() ? QString() : flowLabel(text(contact, "flow"));
        } else if (field == QLatin1String("value")) {
            const qint64 amount = qint64(contact.value(QStringLiteral("value")).toDouble());
            value = amount > 0 ? QString::number(amount) : QString();
        } else {
            value = text(contact, field);
        }
        if (!value.isEmpty())
            record << QStringLiteral("- %1: %2").arg(QString::fromLatin1(column.label), value);
    }
    const QJsonObject custom = contact.value(QStringLiteral("fields")).toObject();
    for (auto it = custom.constBegin(); it != custom.constEnd(); ++it) {
        const QString value = it.value().toVariant().toString().trimmed();
        if (!value.isEmpty())
            record << QStringLiteral("- %1: %2").arg(customColumnLabel(it.key()), value);
    }
    const QStringList lists = contactListNames(contactId);
    if (!lists.isEmpty())
        record << QStringLiteral("- Lists: %1").arg(lists.join(QStringLiteral(", ")));
    const qint64 contacted = qint64(contact.value(QStringLiteral("lastContactedAt")).toDouble());
    record << QStringLiteral("- Last contacted: %1").arg(
        contacted > 0 ? timestamp(contacted) : QStringLiteral("never"));

    QList<QJsonObject> linked = m_contactEmails.value(contactId);
    linked.append(m_contactToots.value(contactId));
    std::sort(linked.begin(), linked.end(),
              [](const QJsonObject &first, const QJsonObject &second) {
        return emailSentAt(first) > emailSentAt(second);
    });
    QStringList history;
    for (const QJsonObject &email : std::as_const(linked)) {
        if (history.size() >= 15)
            break;
        history << QStringLiteral("- ") + linkedEmailLine(email);
    }
    const QJsonObject deck = deckLinkFor(contactId);
    if (!deck.isEmpty())
        history << QStringLiteral("- ") + deckActivityLine(deck);

    QStringList posts;
    for (const QString &source : avatarSources(contact)) {
        const auto read = m_accountReads.constFind(source);
        if (read == m_accountReads.constEnd())
            continue;
        int shown = 0;
        for (const AccountUpdate &update : read->updates) {
            if (++shown > 5)
                break;
            posts << QStringLiteral("- %1, %2: %3").arg(
                accountServiceLabel(source),
                update.at > 0 ? timestamp(update.at) : QStringLiteral("undated"),
                update.text.left(400).simplified());
        }
    }

    const QString organization = m_organization->currentData().toString();
    const QString senderName = text(m_settings, "fromName").isEmpty()
        ? m_viewerName : text(m_settings, "fromName");
    const QStringList brief{
        QStringLiteral("- ForkMesh (https://forkmesh.com) turns Claude Code and Codex into "
                       "attributable contributors on a local-first Git platform teams "
                       "control. Tagline: Code with agents. Own the forge."),
        QStringLiteral("- Problem: coding is moving from autocomplete to delegated agent "
                       "work, but tasks, transcripts, branches and reviews are scattered, "
                       "accountability for agent changes is shallow, and code, identity "
                       "and collaboration stay rented from a central platform."),
        QStringLiteral("- Product: an issue or prompt starts a Claude Code or Codex session "
                       "in its own worktree and branch; sessions run in parallel and can "
                       "be streamed, steered, stopped and resumed; the patch and session "
                       "metadata (model, cost, tool use) become a signed pull request that "
                       "a human reviews and merges. Desktop app and web."),
        QStringLiteral("- Proof (Git history and live network, 13 September 2026): ForkMesh "
                       "builds ForkMesh. 7,416 agent branches merged into main, 51,457 "
                       "commits in 90 days, 24 human and agent contributors, 63 community "
                       "pull requests, 54 repositories on the live network."),
        QStringLiteral("- Open source and peer-to-peer: repositories mirror across "
                       "independent nodes; the protocol is MIT-licensed; any community "
                       "incentives are optional, non-custodial and never guaranteed."),
        QStringLiteral("- Customers: 20 to 200 person software teams using several coding "
                       "agents on private code; buyer is the CTO, VP Engineering or "
                       "security lead. Land one agent workflow, then expand."),
        QStringLiteral("- Business model: Community (free, open source), Agent team "
                       "(subscription plus usage), Enterprise (annual contract: policy, "
                       "RBAC, audit, self-hosted and federated). $25K blended ACV "
                       "hypothesis."),
        QStringLiteral("- Why now: 180M+ developers on GitHub, 4.7M paid Copilot "
                       "subscribers growing 75% a year; teams use several agents and "
                       "need provenance, policy and trusted merges."),
        QStringLiteral("- Competition: hosted AI forges (Copilot, GitLab Duo) keep the code "
                       "and control plane; coding agents (Claude, Codex, Cursor) are not a "
                       "shared system of record; Radicle is peer-to-peer Git without agent "
                       "orchestration. ForkMesh owns the intersection."),
        QStringLiteral("- Team: Jett Vale, CEO and founder, built and exited a prior "
                       "company; Md. Kaif, CTO and co-founder, distributed systems and Git."),
        QStringLiteral("- The round: raising $1.0M for an 18-month plan to reach 20 paid "
                       "organizations, $500K ARR and Series A readiness by Q4 2027. Funds: "
                       "55% product and engineering, 20% infrastructure and security, 15% "
                       "go-to-market, 10% legal, ops and contingency."),
        QStringLiteral("- Contact: founders@forkmesh.com."),
    };

    QStringList prompt{
        QStringLiteral("Write a first-person outreach email from a ForkMesh founder to "
                       "the investor described below. Work only from these facts and "
                       "https://forkmesh.com; never invent traction, numbers, names or "
                       "shared history. If the working folder is the ForkMesh source you "
                       "may read README.md and docs/ for current facts. Do not edit, "
                       "create or run anything."),
        QStringLiteral("About ForkMesh (from its investor deck):\n") + brief.join(QLatin1Char('\n')),
        QStringLiteral("The sender:\n- Name: %1\n- Sending address: %2\n- Organization: %3")
            .arg(senderName.isEmpty() ? QStringLiteral("a ForkMesh founder") : senderName,
                 text(m_settings, "fromEmail").isEmpty() ? QStringLiteral("founders@forkmesh.com")
                                                         : text(m_settings, "fromEmail"),
                 organization),
        QStringLiteral("The investor, as the CRM record has them:\n") + record.join(QLatin1Char('\n')),
    };
    if (!history.isEmpty())
        prompt << QStringLiteral("Earlier contact with them, newest first:\n") +
                      history.join(QLatin1Char('\n'));
    if (!posts.isEmpty())
        prompt << QStringLiteral("Their recent public posts:\n") + posts.join(QLatin1Char('\n'));
    prompt << QStringLiteral("Their personal deck link: %1\nIt is unique to them and every "
                             "open and page read is recorded against their record, so put "
                             "it in the email exactly as written, on its own line, once. "
                             "Do not shorten, rewrite or add anything to it.").arg(deckUrl)
           << QStringLiteral("Write the email: plain text, no markdown, under 180 words. "
                             "Give one specific reason this investor fits, drawn from "
                             "their focus, notes, portfolio or posts above. Make one clear "
                             "ask: a 20-minute call. If there was earlier contact, write a "
                             "follow-up rather than an introduction, and respect their "
                             "stage and next step. Sign with the sender's name.")
           << QStringLiteral("Answer with only a JSON object and nothing else: "
                             "{\"subject\": \"...\", \"body\": \"...\"}");
    return prompt.join(QStringLiteral("\n\n"));
}

bool CrmFeature::parseEmailDraft(const QString &reply, const QString &deckUrl,
                                 QString *subject, QString *body)
{
    QString subjectText;
    QString bodyText;
    const qsizetype open = reply.indexOf(QLatin1Char('{'));
    const qsizetype close = reply.lastIndexOf(QLatin1Char('}'));
    if (open >= 0 && close > open) {
        const QJsonObject parsed = QJsonDocument::fromJson(
            reply.mid(open, close - open + 1).toUtf8()).object();
        subjectText = text(parsed, "subject");
        bodyText = parsed.value(QStringLiteral("body")).toString().trimmed();
    }
    if (bodyText.isEmpty()) {
        QStringList lines = reply.trimmed().split(QLatin1Char('\n'));
        if (!lines.isEmpty() &&
            lines.first().startsWith(QLatin1String("Subject:"), Qt::CaseInsensitive))
            subjectText = lines.takeFirst().mid(8).trimmed();
        bodyText = lines.join(QLatin1Char('\n')).trimmed();
    }
    if (bodyText.isEmpty())
        return false;
    if (!deckUrl.isEmpty() && !bodyText.contains(deckUrl))
        bodyText += QStringLiteral("\n\nThe deck: ") + deckUrl;
    if (subject)
        *subject = subjectText;
    if (body)
        *body = bodyText;
    return true;
}

void CrmFeature::syncDraftButton()
{
    if (!m_draftButton)
        return;
    const QString agent = m_agentName ? m_agentName() : QString();
    const bool busy = m_drafting.contains(m_detailContactId);
    m_draftButton->setText(busy ? QString::fromUtf8("Writing\xE2\x80\xA6")
                                : agent.isEmpty() ? QStringLiteral("Write email with agent")
                                                  : QStringLiteral("Write email with %1").arg(agent));
    m_draftButton->setEnabled(!busy && m_askAgent && !agent.isEmpty() &&
                              !text(m_detailContact, "email").isEmpty());
    m_draftButton->setToolTip(agent.isEmpty()
        ? QStringLiteral("Pick Claude Code or Codex as the default agent in Settings to "
                         "write emails here.")
        : QStringLiteral("%1 writes an email from ForkMesh's investor brief and everything "
                         "this record holds, with the contact's own tracked deck link. "
                         "You get a ping when they open it.").arg(agent));
}

void CrmFeature::draftInvestorEmail(const QJsonObject &contact)
{
    const QString id = text(contact, "id");
    if (id.isEmpty() || !m_askAgent || m_drafting.contains(id) ||
        m_organization->currentData().toString().isEmpty())
        return;
    const QString flowId = text(contact, "flow");
    const bool investor = flowIconId(flowId) == QLatin1String("investors") ||
                          flowId == QLatin1String("investors") ||
                          flowId == QLatin1String("investment");
    if (!investor) {
        setStatus(QStringLiteral("%1 is in %2, not an investor pipeline. Move them to an "
                                 "investor pipeline first if they should get the deck.")
                      .arg(contactLabel(contact),
                           flowId.isEmpty() ? QStringLiteral("no pipeline") : flowLabel(flowId)),
                  true);
        return;
    }
    m_drafting.insert(id);
    syncDraftButton();
    const QJsonObject link = deckLinkFor(id);
    if (!link.isEmpty()) {
        askAgentForEmail(id, text(link, "url"));
        return;
    }
    setStatus(QString::fromUtf8("Making %1's deck link\xE2\x80\xA6").arg(contactLabel(contact)));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/decks")),
            QJsonObject{{QStringLiteral("contactId"), id}},
            [this, id](bool ok, const QJsonObject &payload, const QString &error) {
        const QJsonObject made = payload.value(QStringLiteral("link")).toObject();
        if (!ok || text(made, "url").isEmpty()) {
            m_drafting.remove(id);
            syncDraftButton();
            setStatus(QStringLiteral("The deck link was not made: %1")
                          .arg(error.isEmpty() ? QStringLiteral("no link came back") : error),
                      true);
            return;
        }
        m_decks.prepend(made);
        askAgentForEmail(id, text(made, "url"));
    });
}

void CrmFeature::askAgentForEmail(const QString &contactId, const QString &deckUrl)
{
    const QJsonObject contact = contactById(contactId);
    QString agent = m_agentName ? m_agentName() : QString();
    if (agent.isEmpty())
        agent = QStringLiteral("The agent");
    setStatus(QString::fromUtf8("%1 is writing an email to %2\xE2\x80\xA6")
                  .arg(agent, contactLabel(contact)));
    QPointer<CrmFeature> self(this);
    m_askAgent(investorEmailPrompt(contact, deckUrl),
               [self, contactId, deckUrl, agent](const QString &reply, const QString &error) {
        if (!self)
            return;
        self->m_drafting.remove(contactId);
        self->syncDraftButton();
        QString subject;
        QString body;
        if (!error.isEmpty() || !parseEmailDraft(reply, deckUrl, &subject, &body)) {
            self->setStatus(QStringLiteral("%1 did not write the email: %2")
                                .arg(agent, error.isEmpty()
                                                ? QStringLiteral("the reply was empty")
                                                : error),
                            true);
            return;
        }
        const QJsonObject current = self->contactById(contactId);
        if (current.isEmpty()) {
            self->setStatus(QStringLiteral("The contact was deleted while the email was "
                                           "being written."), true);
            return;
        }
        self->setStatus(QStringLiteral("%1 wrote the email. Review it before sending; "
                                       "opens of the deck link are tracked and pinged.")
                            .arg(agent));
        self->composeEmail(current, subject, body);
    });
}

void CrmFeature::sendAutoEmail(const QString &contactId, const QString &mode,
                               const QString &reason, const QJsonObject &draft)
{
    const QJsonObject contact = contactById(contactId);
    const QString email = text(contact, "email");
    if (contactId.isEmpty() || email.isEmpty() || m_autoSending.contains(contactId) ||
        m_organization->currentData().toString().isEmpty())
        return;
    if (draft.isEmpty()) {
        const QString flowId = text(contact, "flow");
        const bool investor = flowIconId(flowId) == QLatin1String("investors") ||
                              flowId == QLatin1String("investors") ||
                              flowId == QLatin1String("investment");
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Send AI email"),
            QStringLiteral("Write and send an email to %1 <%2> now?\n\nA model writes it from "
                           "their record and from what our past emails got opened or clicked "
                           "for, with %3. It goes out straight away from %4.")
                .arg(contactLabel(contact), email,
                     investor ? QStringLiteral("their tracked deck link")
                              : QStringLiteral("your referral link"),
                     m_settings.value(QStringLiteral("zoho")).toObject()
                             .value(QStringLiteral("connected")).toBool()
                         ? QStringLiteral("your connected Zoho Mail mailbox, so it lands in "
                                          "its Sent folder")
                         : QStringLiteral("the outreach sender")));
        if (answer != QMessageBox::Yes)
            return;
    }
    m_autoSending.insert(contactId);
    setStatus(QString::fromUtf8("Writing an email to %1\xE2\x80\xA6").arg(contactLabel(contact)));
    QJsonObject body{{QStringLiteral("contactId"), contactId},
                     {QStringLiteral("mode"), mode},
                     {QStringLiteral("reason"), reason},
                     {QStringLiteral("list"), text(activeSheet(), "name")}};
    for (auto item = draft.constBegin(); item != draft.constEnd(); ++item)
        body.insert(item.key(), item.value());
    const bool written = !draft.isEmpty();
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/outreach/auto")), body,
            [this, contactId, mode, reason, written](bool ok, const QJsonObject &payload,
                                                     const QString &error) {
        m_autoSending.remove(contactId);
        const QString label = contactLabel(contactById(contactId));
        const QString code = text(payload, "error");
        if (ok) {
            const QJsonObject learning = payload.value(QStringLiteral("learning")).toObject();
            const int past = learning.value(QStringLiteral("sent")).toInt();
            const QString via = text(payload, "via") == QLatin1String("zoho")
                ? QStringLiteral(" from %1 through Zoho Mail")
                      .arg(text(payload, "fromEmail").isEmpty() ? QStringLiteral("your mailbox")
                                                                : text(payload, "fromEmail"))
                : QString();
            setStatus(QString::fromUtf8("Sent \xE2\x80\x9C%1\xE2\x80\x9D to %2. You get a ping when "
                                        "they open it or click.%3")
                          .arg(text(payload, "subject"), label + via,
                               past ? QStringLiteral(" Learned from %1 past emails (%2% opened, "
                                                     "%3% clicked).")
                                          .arg(past)
                                          .arg(learning.value(QStringLiteral("openRate")).toInt())
                                          .arg(learning.value(QStringLiteral("clickRate")).toInt())
                                    : QString()));
            loadOrganization();
            return;
        }
        if (!written && !text(payload, "prompt").isEmpty() && m_askAgent && m_agentName &&
            !m_agentName().isEmpty() &&
            (code == QLatin1String("ai_unavailable") || code == QLatin1String("draft_unusable"))) {
            autoEmailWithAgent(contactId, mode, reason, payload);
            return;
        }
        setStatus(QStringLiteral("Nothing was sent to %1: %2").arg(label, autoEmailError(code, error)),
                  true);
    });
}

void CrmFeature::autoEmailWithAgent(const QString &contactId, const QString &mode,
                                    const QString &reason, const QJsonObject &payload)
{
    const QString agent = m_agentName();
    m_autoSending.insert(contactId);
    setStatus(QString::fromUtf8("The host has no model for this, so %1 is writing the email to "
                                "%2\xE2\x80\xA6")
                  .arg(agent, contactLabel(contactById(contactId))));
    QPointer<CrmFeature> self(this);
    m_askAgent(text(payload, "prompt"),
               [self, contactId, mode, reason, agent](const QString &reply, const QString &error) {
        if (!self)
            return;
        self->m_autoSending.remove(contactId);
        QString subject;
        QString body;
        if (!error.isEmpty() || !parseEmailDraft(reply, QString(), &subject, &body)) {
            self->setStatus(QStringLiteral("%1 did not write the email: %2")
                                .arg(agent, error.isEmpty() ? QStringLiteral("the reply was empty")
                                                            : error),
                            true);
            return;
        }
        self->sendAutoEmail(contactId, mode, reason,
                            QJsonObject{{QStringLiteral("subject"), subject},
                                        {QStringLiteral("text"), body},
                                        {QStringLiteral("model"), agent}});
    });
}

void CrmFeature::copyContactLink(const QJsonObject &contact)
{
    const QString organization = m_organization->currentData().toString();
    const QString id = text(contact, "id");
    if (organization.isEmpty() || id.isEmpty())
        return;
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(crmPageLink(QStringLiteral("contact"), id));
    setStatus(QStringLiteral("Copied a link to %1. It opens for members of %2.")
                  .arg(contactLabel(contact), organization));
}

void CrmFeature::copySheetLink(const QString &id)
{
    const QJsonObject sheet = sheetById(id);
    const QString organization = m_organization->currentData().toString();
    if (!storedSheet(sheet) || organization.isEmpty())
        return;
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(crmPageLink(QStringLiteral("list"), id));
    setStatus(QString::fromUtf8("Copied a link to \xE2\x80\x9C%1\xE2\x80\x9D. "
                                "Who it opens for: %2.")
                  .arg(text(sheet, "name"),
                       sheetShareSummary(sheet).section(QLatin1Char('\n'), 0, 0)));
}

void CrmFeature::noteSheetViewed()
{
    const QJsonObject sheet = activeSheet();
    const bool stored = storedSheet(sheet);
    if (m_historyButton)
        m_historyButton->setEnabled(stored);
    const QString id = text(sheet, "id");
    if (!stored || id.isEmpty() || !isVisible() || !m_request ||
        m_organization->currentData().toString().isEmpty())
        return;
    const QString key = m_organization->currentData().toString() + QLatin1Char('/') + id;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kViewGapMs = 30 * 60 * 1000;
    if (now - m_sheetViewedAt.value(key) < kViewGapMs)
        return;
    m_sheetViewedAt.insert(key, now);
    m_request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/views/activity")),
              QJsonObject{{QStringLiteral("view"), id}},
              [](bool, const QJsonObject &, const QString &) {});
}

void CrmFeature::showSheetHistory(const QString &id)
{
    const QJsonObject sheet = sheetById(id);
    if (!storedSheet(sheet))
        return;
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QString::fromUtf8("Activity history \xC2\xB7 %1")
                               .arg(text(sheet, "name")));
    dialog->resize(760, 540);
    auto *layout = new QVBoxLayout(dialog);
    auto *note = new QLabel(QString::fromUtf8("Loading\xE2\x80\xA6"));
    note->setObjectName(QStringLiteral("mutedLabel"));
    note->setWordWrap(true);
    auto *table = new QTableWidget(0, 3);
    table->setObjectName(QStringLiteral("crmListHistory"));
    table->setHorizontalHeaderLabels(
        {QStringLiteral("When"), QStringLiteral("Who"), QStringLiteral("What")});
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setWordWrap(true);
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(0, 140);
    table->setColumnWidth(1, 130);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QPushButton *reload = buttons->addButton(QStringLiteral("Refresh"),
                                             QDialogButtonBox::ActionRole);
    layout->addWidget(note);
    layout->addWidget(table, 1);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    const QPointer<QDialog> guard(dialog);
    const QString path = organizationPath(QStringLiteral("/views/activity?view=") + id);
    const auto load = [this, guard, table, note, reload, path] {
        reload->setEnabled(false);
        request(QByteArrayLiteral("GET"), path, {},
                [guard, table, note, reload](bool ok, const QJsonObject &payload,
                                             const QString &error) {
            if (!guard)
                return;
            reload->setEnabled(true);
            if (!ok) {
                note->setText(error.isEmpty()
                                  ? QStringLiteral("The list's history could not be read.")
                                  : error);
                return;
            }
            const QJsonArray entries = payload.value(QStringLiteral("entries")).toArray();
            table->setRowCount(0);
            table->setRowCount(entries.size());
            QSet<QString> viewers;
            for (int row = 0; row < entries.size(); ++row) {
                const QJsonObject entry = entries.at(row).toObject();
                const qint64 at = static_cast<qint64>(
                    entry.value(QStringLiteral("at")).toDouble());
                const QString actor = text(entry, "actor");
                if (text(entry, "action") == QLatin1String("list.viewed"))
                    viewers.insert(actor);
                auto *when = new QTableWidgetItem(timestamp(at));
                when->setToolTip(QLocale().toString(
                    QDateTime::fromMSecsSinceEpoch(at).toLocalTime(), QLocale::LongFormat));
                table->setItem(row, 0, when);
                table->setItem(row, 1, new QTableWidgetItem(actor));
                auto *what = new QTableWidgetItem(text(entry, "text"));
                what->setToolTip(text(entry, "text"));
                table->setItem(row, 2, what);
            }
            table->resizeRowsToContents();
            if (entries.isEmpty()) {
                note->setText(QStringLiteral("Nothing has happened on this list yet."));
                return;
            }
            QStringList viewerNames(viewers.cbegin(), viewers.cend());
            viewerNames.sort(Qt::CaseInsensitive);
            note->setText(QStringLiteral("%1 event%2, newest first; %3 opened it. "
                                         "Opens count once per person every 30 minutes; "
                                         "history is kept for 90 days.%4")
                              .arg(entries.size())
                              .arg(entries.size() == 1 ? QString() : QStringLiteral("s"))
                              .arg(viewerNames.isEmpty()
                                       ? QStringLiteral("no one")
                                       : viewerNames.join(QStringLiteral(", ")))
                              .arg(payload.value(QStringLiteral("more")).toBool()
                                       ? QStringLiteral(" Older events are not shown.")
                                       : QString()));
        });
    };
    connect(reload, &QPushButton::clicked, dialog, load);
    dialog->show();
    load();
}

void CrmFeature::composeEmail(const QJsonObject &contact, const QString &subjectText,
                              const QString &draft, std::function<void()> sent)
{
    if (m_organization->currentData().toString().isEmpty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Send one-off email with Mailtrap"));
    dialog.resize(680, 560);
    auto *layout = new QVBoxLayout(&dialog);
    auto *notice = new QLabel(QStringLiteral(
        "The Mailtrap key is used for this request only and is never saved."));
    notice->setObjectName(QStringLiteral("mutedLabel"));
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *form = new QFormLayout;
    auto *to = lineEdit(text(contact, "email"));
    auto *key = lineEdit();
    key->setEchoMode(QLineEdit::Password);
    key->setPlaceholderText(QStringLiteral("Leave blank to use the server key"));
    auto *fromName = lineEdit(text(m_settings, "fromName"));
    auto *fromEmail = lineEdit(text(m_settings, "fromEmail"));
    auto *subject = lineEdit(subjectText);
    auto *message = new QPlainTextEdit;
    message->setMinimumHeight(220);
    message->setPlainText(draft);
    form->addRow(QStringLiteral("To"), to);
    form->addRow(QStringLiteral("Mailtrap key"), key);
    form->addRow(QStringLiteral("From name"), fromName);
    form->addRow(QStringLiteral("From email"), fromEmail);
    form->addRow(QStringLiteral("Subject"), subject);
    form->addRow(QStringLiteral("Message"), message);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Send"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QJsonObject body{
        {QStringLiteral("contactId"), text(contact, "id")},
        {QStringLiteral("to"), to->text()},
        {QStringLiteral("mailtrapKey"), key->text()},
        {QStringLiteral("fromName"), fromName->text()},
        {QStringLiteral("fromEmail"), fromEmail->text()},
        {QStringLiteral("subject"), subject->text()},
        {QStringLiteral("text"), message->toPlainText()},
    };
    key->clear();
    setStatus(QStringLiteral("Sending one-off email…"));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/emails/send")),
            body, [this, sent](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("Mailtrap did not accept the email: %1")
                                 .arg(error), true);
        setStatus(QStringLiteral("Mailtrap accepted the email."));
        if (sent)
            sent();
        loadOrganization();
    });
}

void CrmFeature::linkExternalEmail()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Link external email send"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *to = lineEdit();
    auto *subject = lineEdit();
    auto *externalId = lineEdit();
    auto *url = lineEdit();
    url->setPlaceholderText(QStringLiteral("https://provider.example/sends/id"));
    auto *status = new QComboBox;
    for (const QString &value :
         {QStringLiteral("external"), QStringLiteral("accepted"),
          QStringLiteral("delivered"), QStringLiteral("open"),
          QStringLiteral("click"), QStringLiteral("failed")})
        status->addItem(value, value);
    form->addRow(QStringLiteral("Recipient"), to);
    form->addRow(QStringLiteral("Subject"), subject);
    form->addRow(QStringLiteral("Provider id"), externalId);
    form->addRow(QStringLiteral("Status"), status);
    form->addRow(QStringLiteral("Provider URL"), url);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QJsonObject body{
        {QStringLiteral("to"), to->text()},
        {QStringLiteral("subject"), subject->text()},
        {QStringLiteral("externalId"), externalId->text()},
        {QStringLiteral("status"), status->currentData().toString()},
        {QStringLiteral("providerUrl"), url->text()},
    };
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/emails")),
            body, [this](bool ok, const QJsonObject &, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("External send was not linked: %1")
                                 .arg(error), true);
        setStatus(QStringLiteral("External email send linked."));
        loadOrganization();
    });
}

void CrmFeature::editSettings()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("CRM settings"));
    dialog.resize(760, 760);
    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    auto *flowEditor = new QPlainTextEdit;
    QStringList flowLines;
    for (const QJsonValue &value :
         m_settings.value(QStringLiteral("flows")).toArray()) {
        const QJsonObject flow = value.toObject();
        QStringList stages;
        for (const QJsonValue &stage : flow.value(QStringLiteral("stages")).toArray())
            stages << stage.toString();
        flowLines << text(flow, "name") + QStringLiteral(" | ") +
                     text(flow, "id") + QStringLiteral(" | ") +
                     stages.join(QStringLiteral(", "));
    }
    flowEditor->setPlainText(flowLines.join(QLatin1Char('\n')));
    flowEditor->setPlaceholderText(QStringLiteral(
        "Flow name | flow-id | first-stage, second-stage, complete"));
    flowEditor->setMinimumHeight(150);
    QStringList boxNames;
    for (const QJsonValue &box :
         m_settings.value(QStringLiteral("mailBoxes")).toArray())
        boxNames << box.toString();
    auto *boxes = lineEdit(boxNames.join(QStringLiteral(", ")));
    auto *provider = lineEdit(text(m_settings, "providerName"));
    auto *url = lineEdit(text(m_settings, "providerUrlTemplate"));
    url->setPlaceholderText(QStringLiteral("https://provider.example/sends/{id}"));
    auto *fromName = lineEdit(text(m_settings, "fromName"));
    auto *fromEmail = lineEdit(text(m_settings, "fromEmail"));
    // Where a click on a contact's address in the table composes a message.
    auto *mailProvider = new QComboBox;
    mailProvider->addItem(QStringLiteral("Zoho Mail"), QStringLiteral("zoho"));
    mailProvider->addItem(QStringLiteral("Gmail"), QStringLiteral("gmail"));
    mailProvider->setCurrentIndex(
        qMax(0, mailProvider->findData(text(m_settings, "mailProvider"))));
    const QJsonObject currentZoho =
        m_settings.value(QStringLiteral("zoho")).toObject();
    auto *region = new QComboBox;
    for (const auto &item : {
             qMakePair(QStringLiteral("United States (.com)"), QStringLiteral("com")),
             qMakePair(QStringLiteral("Europe (.eu)"), QStringLiteral("eu")),
             qMakePair(QStringLiteral("India (.in)"), QStringLiteral("in")),
             qMakePair(QStringLiteral("Australia (.com.au)"), QStringLiteral("com.au")),
             qMakePair(QStringLiteral("Japan (.jp)"), QStringLiteral("jp")),
             qMakePair(QStringLiteral("Canada (.ca)"), QStringLiteral("ca"))})
        region->addItem(item.first, item.second);
    region->setCurrentIndex(qMax(
        0, region->findData(text(currentZoho, "region"))));
    auto *zohoAccount = lineEdit(text(currentZoho, "accountId"));
    auto *zohoClient = lineEdit();
    auto *zohoSecret = lineEdit();
    auto *zohoRefresh = lineEdit();
    auto *zohoCode = lineEdit();
    zohoSecret->setEchoMode(QLineEdit::Password);
    zohoRefresh->setEchoMode(QLineEdit::Password);
    zohoCode->setEchoMode(QLineEdit::Password);
    zohoCode->setPlaceholderText(
        QStringLiteral("Self Client code: Save redeems it and finds the account"));
    zohoClient->setPlaceholderText(QStringLiteral("Unchanged when blank"));
    zohoSecret->setPlaceholderText(QStringLiteral("Unchanged when blank"));
    zohoRefresh->setPlaceholderText(QStringLiteral("Unchanged when blank"));
    form->addRow(QStringLiteral("Flows (name | id | stages)"), flowEditor);
    form->addRow(QStringLiteral("Shared mail boxes"), boxes);
    form->addRow(QStringLiteral("Zoho data center"), region);
    form->addRow(QStringLiteral("Zoho account id"), zohoAccount);
    form->addRow(QStringLiteral("Zoho OAuth client id"), zohoClient);
    form->addRow(QStringLiteral("Zoho OAuth client secret"), zohoSecret);
    form->addRow(QStringLiteral("Zoho Self Client code"), zohoCode);
    form->addRow(QStringLiteral("Zoho OAuth refresh token"), zohoRefresh);
    form->addRow(QStringLiteral("Provider"), provider);
    form->addRow(QStringLiteral("Send URL template"), url);
    form->addRow(QStringLiteral("From name"), fromName);
    form->addRow(QStringLiteral("From email"), fromEmail);
    form->addRow(QStringLiteral("Email addresses open in"), mailProvider);
    const QJsonObject currentMastodon =
        m_settings.value(QStringLiteral("mastodon")).toObject();
    auto *mastodonAccount = lineEdit(text(currentMastodon, "account").isEmpty()
        ? QString() : QStringLiteral("@") + text(currentMastodon, "account"));
    mastodonAccount->setPlaceholderText(QStringLiteral("@forkmesh@mastodon.social"));
    mastodonAccount->setProperty("saved", mastodonAccount->text());
    auto *mastodonToken = lineEdit();
    mastodonToken->setEchoMode(QLineEdit::Password);
    mastodonToken->setPlaceholderText(currentMastodon.value(QStringLiteral("connected")).toBool()
        ? QStringLiteral("Unchanged when blank") : QStringLiteral("Access token"));
    auto *mastodonState = new QLabel(mastodonStateLine(currentMastodon));
    mastodonState->setObjectName(QStringLiteral("crmMastodonState"));
    mastodonState->setTextFormat(Qt::PlainText);
    mastodonState->setWordWrap(true);
    auto *mastodonHelp = new QLabel(QStringLiteral(
        "Toots from this account that mention a contact are logged to that contact "
        "when the CRM loads. The access token needs the read:statuses scope: create it "
        "under Preferences → Development on the account's instance."));
    mastodonHelp->setObjectName(QStringLiteral("mutedLabel"));
    mastodonHelp->setWordWrap(true);
    auto *mastodonSave = new QPushButton(QStringLiteral("Save"));
    auto *mastodonDisconnect = new QPushButton(QStringLiteral("Disconnect"));
    auto *mastodonCheck = new QPushButton(QStringLiteral("Check toots now"));
    mastodonAccount->setEnabled(m_canManageSettings);
    mastodonToken->setEnabled(m_canManageSettings);
    mastodonSave->setEnabled(m_canManageSettings);
    mastodonDisconnect->setEnabled(m_canManageSettings &&
                                   !text(currentMastodon, "account").isEmpty());
    mastodonCheck->setEnabled(currentMastodon.value(QStringLiteral("connected")).toBool());
    auto *mastodonButtons = new QHBoxLayout;
    mastodonButtons->setSpacing(6);
    for (QPushButton *button : {mastodonSave, mastodonDisconnect, mastodonCheck})
        mastodonButtons->addWidget(button);
    mastodonButtons->addStretch(1);
    form->addRow(QStringLiteral("Mastodon account"), mastodonAccount);
    form->addRow(QStringLiteral("Mastodon access token"), mastodonToken);
    form->addRow(QString(), mastodonState);
    form->addRow(QString(), mastodonButtons);
    form->addRow(QString(), mastodonHelp);
    const QPointer<QDialog> mastodonDialog(&dialog);
    const auto showMastodon = [mastodonDialog, mastodonAccount, mastodonToken, mastodonState,
                               mastodonDisconnect, mastodonCheck,
                               canManage = m_canManageSettings](const QJsonObject &mastodon) {
        if (!mastodonDialog)
            return;
        const QString account = text(mastodon, "account").isEmpty()
            ? QString() : QStringLiteral("@") + text(mastodon, "account");
        const bool connected = mastodon.value(QStringLiteral("connected")).toBool();
        mastodonAccount->setText(account);
        mastodonAccount->setProperty("saved", account);
        mastodonToken->clear();
        mastodonToken->setPlaceholderText(connected ? QStringLiteral("Unchanged when blank")
                                                    : QStringLiteral("Access token"));
        mastodonState->setText(mastodonStateLine(mastodon));
        mastodonDisconnect->setEnabled(canManage && !account.isEmpty());
        mastodonCheck->setEnabled(connected);
    };
    const auto putMastodon = [this, mastodonDialog, mastodonState, showMastodon](
                                 const QJsonObject &body) {
        request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")), body,
                [this, mastodonDialog, mastodonState, showMastodon](
                    bool ok, const QJsonObject &payload, const QString &error) {
            if (!ok) {
                const QString reason = mastodonErrorText(text(payload, "error"), error);
                if (mastodonDialog)
                    mastodonState->setText(
                        QStringLiteral("Not saved: %1").arg(reason));
                return setStatus(QStringLiteral("The Mastodon account was not saved: %1")
                                     .arg(reason), true);
            }
            m_settings = payload.value(QStringLiteral("settings")).toObject();
            showMastodon(m_settings.value(QStringLiteral("mastodon")).toObject());
            setStatus(QStringLiteral("Mastodon account saved."));
        });
    };
    connect(mastodonSave, &QPushButton::clicked, &dialog,
            [mastodonAccount, mastodonToken, putMastodon] {
        putMastodon(QJsonObject{{QStringLiteral("mastodon"), QJsonObject{
            {QStringLiteral("account"), mastodonAccount->text().trimmed()},
            {QStringLiteral("token"), mastodonToken->text().trimmed()},
        }}});
    });
    connect(mastodonDisconnect, &QPushButton::clicked, &dialog, [putMastodon] {
        putMastodon(QJsonObject{{QStringLiteral("mastodonDisconnect"), true}});
    });
    connect(mastodonCheck, &QPushButton::clicked, &dialog, [this, mastodonCheck, showMastodon] {
        if (m_tootSyncRunning)
            return;
        mastodonCheck->setEnabled(false);
        syncToots(false, showMastodon);
    });
    layout->addLayout(form);
    auto *notice = new QLabel(m_canManageSettings
        ? QStringLiteral(
              "Mailtrap keys remain request-only. Zoho refresh credentials are "
              "encrypted for organization-wide sync and never shown again.")
        : QStringLiteral("Only organization owners and admins can change these settings."));
    notice->setObjectName(QStringLiteral("mutedLabel"));
    notice->setWordWrap(true);
    layout->addWidget(notice);
    // Plugin data lifecycle: export everything, or uninstall for the whole
    // organization (download first, purge, reverse schema on last install).
    auto *pluginNote = new QLabel(QStringLiteral(
        "Plugin data: export everything as JSON, or uninstall the plugin for "
        "this organization. Uninstalling downloads the data first, then "
        "deletes every contact, email record, and setting; once no "
        "organization keeps the plugin installed, its tables are removed "
        "as well."));
    pluginNote->setObjectName(QStringLiteral("mutedLabel"));
    pluginNote->setWordWrap(true);
    layout->addWidget(pluginNote);
    auto *pluginRow = new QHBoxLayout;
    auto *exportAllButton = new QPushButton(QStringLiteral("Export all data…"));
    auto *uninstallButton = new QPushButton(
        QStringLiteral("Uninstall from organization…"));
    uninstallButton->setObjectName(QStringLiteral("dangerButton"));
    exportAllButton->setEnabled(m_canManageSettings);
    uninstallButton->setEnabled(m_canManageSettings);
    pluginRow->addWidget(exportAllButton);
    pluginRow->addWidget(uninstallButton);
    pluginRow->addStretch();
    layout->addLayout(pluginRow);
    // Run lifecycle actions after the modal closes so their own dialogs never
    // nest inside this exec() loop.
    enum class PluginAction { None, ExportAll, Uninstall };
    PluginAction pluginAction = PluginAction::None;
    connect(exportAllButton, &QPushButton::clicked, &dialog,
            [&pluginAction, &dialog] {
                pluginAction = PluginAction::ExportAll;
                dialog.reject();
            });
    connect(uninstallButton, &QPushButton::clicked, &dialog,
            [&pluginAction, &dialog] {
                pluginAction = PluginAction::Uninstall;
                dialog.reject();
            });
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Save)->setEnabled(m_canManageSettings);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted) {
        if (pluginAction == PluginAction::ExportAll)
            exportAllData();
        else if (pluginAction == PluginAction::Uninstall)
            uninstallPlugin();
        return;
    }
    QJsonArray flows;
    for (const QString &line :
         flowEditor->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList parts = line.split(QLatin1Char('|'));
        if (parts.size() < 3)
            continue;
        flows.append(QJsonObject{
            {QStringLiteral("name"), parts.at(0).trimmed()},
            {QStringLiteral("id"), parts.at(1).trimmed()},
            {QStringLiteral("stages"), QJsonArray::fromStringList(
                parts.mid(2).join(QLatin1Char('|')).split(
                    QLatin1Char(','), Qt::SkipEmptyParts))},
        });
    }
    QJsonObject body{
        {QStringLiteral("providerName"), provider->text()},
        {QStringLiteral("providerUrlTemplate"), url->text()},
        {QStringLiteral("fromName"), fromName->text()},
        {QStringLiteral("fromEmail"), fromEmail->text()},
        {QStringLiteral("mailProvider"), mailProvider->currentData().toString()},
        {QStringLiteral("flows"), flows},
        {QStringLiteral("mailBoxes"), QJsonArray::fromStringList(
             boxes->text().split(QLatin1Char(','), Qt::SkipEmptyParts))},
    };
    const QString codeText = zohoCode->text().trimmed();
    const QJsonObject exchange = codeText.isEmpty() ? QJsonObject{} : QJsonObject{
        {QStringLiteral("code"), codeText},
        {QStringLiteral("region"), region->currentData().toString()},
        {QStringLiteral("clientId"), zohoClient->text()},
        {QStringLiteral("clientSecret"), zohoSecret->text()},
    };
    if (exchange.isEmpty() &&
        (currentZoho.value(QStringLiteral("connected")).toBool() ||
         !zohoAccount->text().trimmed().isEmpty() ||
         !zohoClient->text().trimmed().isEmpty() ||
         !zohoSecret->text().isEmpty() || !zohoRefresh->text().isEmpty())) {
        body.insert(QStringLiteral("zoho"), QJsonObject{
            {QStringLiteral("region"), region->currentData().toString()},
            {QStringLiteral("accountId"), zohoAccount->text()},
            {QStringLiteral("clientId"), zohoClient->text()},
            {QStringLiteral("clientSecret"), zohoSecret->text()},
            {QStringLiteral("refreshToken"), zohoRefresh->text()},
        });
    }
    if (!mastodonToken->text().trimmed().isEmpty() ||
        mastodonAccount->text().trimmed() != mastodonAccount->property("saved").toString()) {
        body.insert(QStringLiteral("mastodon"), QJsonObject{
            {QStringLiteral("account"), mastodonAccount->text().trimmed()},
            {QStringLiteral("token"), mastodonToken->text().trimmed()},
        });
    }
    mastodonToken->clear();
    zohoSecret->clear();
    zohoRefresh->clear();
    zohoCode->clear();
    request(QByteArrayLiteral("PUT"), organizationPath(QStringLiteral("/settings")),
            body, [this, exchange](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("CRM settings were not saved: %1")
                                 .arg(mastodonErrorText(text(payload, "error"), error)), true);
        if (!exchange.isEmpty())
            return exchangeZohoCode(exchange);
        setStatus(QStringLiteral("CRM settings saved."));
        loadOrganization();
    });
}

void CrmFeature::exchangeZohoCode(const QJsonObject &body)
{
    setStatus(QStringLiteral("Redeeming the Zoho code in every data center…"));
    request(QByteArrayLiteral("POST"),
            organizationPath(QStringLiteral("/zoho/exchange")), body,
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            const QString code = text(payload, "error");
            const QString reason = text(payload, "zohoError");
            if (code == QLatin1String("zoho_code_rejected"))
                return setStatus(QStringLiteral(
                    "Zoho rejected the code in every data center (%1). Codes work "
                    "once and expire in minutes: generate a new one and check the "
                    "client ID and secret are the Self Client's.")
                    .arg(reason.isEmpty() ? QStringLiteral("no reason given") : reason),
                    true);
            if (code == QLatin1String("zoho_code_incomplete"))
                return setStatus(QStringLiteral(
                    "Enter the Self Client's client ID, its client secret and the code."),
                    true);
            if (code == QLatin1String("zoho_account_missing"))
                return setStatus(QStringLiteral(
                    "Zoho accepted the code, but that login has no Zoho Mail account."),
                    true);
            return setStatus(QStringLiteral("Could not connect with the code: %1")
                                 .arg(error), true);
        }
        const QJsonObject zoho = payload.value(QStringLiteral("settings")).toObject()
                                     .value(QStringLiteral("zoho")).toObject();
        setStatus(QStringLiteral(
            "Zoho Mail connected to account %1 (%2). Click Sync Zoho Mail.")
            .arg(text(zoho, "accountId"), text(zoho, "region")));
        loadOrganization();
    });
}

void CrmFeature::logToot(const QString &contactId)
{
    const QJsonObject contact = contactById(contactId);
    if (contact.isEmpty())
        return;
    bool accepted = false;
    const QString url = QInputDialog::getText(
        this, QStringLiteral("Log toot"),
        QStringLiteral("Link to the toot (https://mastodon.social/@name/id)"),
        QLineEdit::Normal, QString(), &accepted).trimmed();
    if (!accepted || url.isEmpty())
        return;
    setStatus(QString::fromUtf8("Logging the toot\xE2\x80\xA6"));
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/emails/toot")),
            QJsonObject{{QStringLiteral("url"), url},
                        {QStringLiteral("contactId"), contactId}},
            [this, contactId](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("The toot was not logged: %1")
                                 .arg(mastodonErrorText(text(payload, "error"), error)),
                             true);
        const QString name = contactLabel(contactById(contactId));
        setStatus(payload.value(QStringLiteral("relinked")).toBool()
                      ? QStringLiteral("That toot was already logged for %1.").arg(name)
                      : QStringLiteral("Toot logged for %1.").arg(name));
        if (contactId == m_detailContactId)
            m_detailContact = QJsonObject();
        loadOrganization();
    });
}

void CrmFeature::syncToots(bool automatic, std::function<void(const QJsonObject &)> done)
{
    if (m_tootSyncRunning)
        return;
    if (automatic && !m_settings.value(QStringLiteral("mastodon")).toObject()
                          .value(QStringLiteral("connected")).toBool())
        return;
    m_tootSyncRunning = true;
    if (!automatic)
        setStatus(QString::fromUtf8("Checking the Mastodon account's toots\xE2\x80\xA6"));
    const QJsonObject body = automatic
        ? QJsonObject{{QStringLiteral("auto"), true}} : QJsonObject{};
    const QString revision = m_revision;
    request(QByteArrayLiteral("POST"), organizationPath(QStringLiteral("/emails/toots")),
            body, [this, automatic, done = std::move(done)](
                      bool ok, const QJsonObject &payload, const QString &error) {
        m_tootSyncRunning = false;
        const int logged = payload.value(QStringLiteral("logged")).toInt();
        if (ok && payload.contains(QStringLiteral("mastodon"))) {
            QJsonObject settings = m_settings;
            settings.insert(QStringLiteral("mastodon"),
                            payload.value(QStringLiteral("mastodon")));
            m_settings = settings;
        }
        if (done) {
            QJsonObject mastodon = m_settings.value(QStringLiteral("mastodon")).toObject();
            if (!ok && !text(payload, "error").isEmpty())
                mastodon.insert(QStringLiteral("lastError"), text(payload, "error"));
            done(mastodon);
        }
        if (!ok) {
            if (!automatic)
                setStatus(QStringLiteral("Toots were not checked: %1")
                              .arg(mastodonErrorText(text(payload, "error"), error)),
                          true);
            return;
        }
        if (!automatic)
            setStatus(logged > 0
                ? QStringLiteral("Logged %1 new toot%2 to contacts.")
                      .arg(logged).arg(logged == 1 ? QString() : QStringLiteral("s"))
                : QStringLiteral("Checked %1 toots: no new mentions of a contact.")
                      .arg(payload.value(QStringLiteral("checked")).toInt()));
        if (logged > 0) {
            m_detailContact = QJsonObject();
            m_revision.clear();
            loadOrganization();
        }
    });
    if (automatic && m_revision.isEmpty())
        m_revision = revision;
}

void CrmFeature::connectZohoMail()
{
    setStatus(QStringLiteral("Opening Zoho to connect the organization's mail…"));
    request(QByteArrayLiteral("POST"),
            organizationPath(QStringLiteral("/zoho/connect")), QJsonObject{},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        const QUrl url(payload.value(QStringLiteral("authorizationUrl")).toString());
        if (!ok || url.scheme() != QLatin1String("https") || url.host().isEmpty()) {
            setStatus(QStringLiteral("Could not start the Zoho connection: %1")
                          .arg(ok ? QStringLiteral("no sign-in link") : error), true);
            return;
        }
        QDesktopServices::openUrl(url);
        setStatus(QStringLiteral(
            "Finish in the browser: allow access in Zoho, then refresh the CRM here."));
    });
}

namespace {

constexpr char kNoteDraftMark[] = "DRAFT, not sent.";
constexpr char kZohoApiConsole[] = "https://api-console.zoho.com/";
constexpr char kZohoCallbackUri[] = "https://app.forkmesh.com/api/crm/-/zoho/callback";
constexpr char kZohoScopes[] =
    "ZohoMail.messages.READ,ZohoMail.messages.CREATE,ZohoMail.folders.READ,ZohoMail.accounts.READ";

qsizetype noteDraftAt(const QString &notes)
{
    const QString mark = QLatin1String(kNoteDraftMark);
    qsizetype at = notes.indexOf(mark);
    while (at > 0 && notes.at(at - 1) != QLatin1Char('\n'))
        at = notes.indexOf(mark, at + 1);
    return at;
}

QString zohoSecretLines()
{
    return QStringLiteral("PACKAGE_CRM_ZOHO_OAUTH_CLIENT_ID=<client id>\n"
                          "PACKAGE_CRM_ZOHO_OAUTH_CLIENT_SECRET=<client secret>\n"
                          "PACKAGE_CRM_ZOHO_OAUTH_REDIRECT_URI=%1\n")
        .arg(QLatin1String(kZohoCallbackUri));
}

QString zohoGuideHtml(const QString &state)
{
    const QString callback = QLatin1String(kZohoCallbackUri);
    const QString console = QLatin1String(kZohoApiConsole);
    const QString code = QStringLiteral("<pre>%1</pre>");
    return QStringLiteral(
        "<p><b>%1</b></p>"
        "<h3>A. One-click Connect Zoho Mail for every organization</h3>"
        "<p>Done once by the server operator. Afterwards an owner or admin of any "
        "organization only clicks <b>Connect Zoho Mail</b>.</p>"
        "<ol>"
        "<li>Open the <a href=\"%2\">Zoho API Console</a>, sign in with the Zoho account "
        "that owns the company mail, and choose <b>Add Client</b> &rarr; "
        "<b>Server-based Applications</b>.</li>"
        "<li>Client name <b>ForkMesh CRM</b>, homepage URL <b>https://forkmesh.com</b>, "
        "Authorized Redirect URI exactly:%3Click <b>Create</b>.</li>"
        "<li>Open the client's <b>Settings</b> tab, turn on <b>Multi-DC</b> (so accounts "
        "in the EU, IN, AU, JP and CA data centers can sign in) and save.</li>"
        "<li>On the <b>Client Secret</b> tab copy the <b>Client ID</b> and the "
        "<b>Client Secret</b>.</li>"
        "<li>On the operator machine add these lines to "
        "<code>~/projects/forkmesh/config/fleet/secrets.env</code> with the two values "
        "filled in:%4The redirect line lets the desktop start the sign-in; a desktop "
        "request has no browser origin.</li>"
        "<li>Add the three names to <code>optional_secrets</code> in "
        "<code>deploy/fleet.toml</code>, run <code>tools/next_vault.py sync</code> to "
        "check the plan, then <code>tools/next_vault.py sync --execute</code>.</li>"
        "<li>Click <b>Refresh</b> in the CRM. The line at the top of this guide then "
        "says one-click is on. Click <b>Connect Zoho Mail</b>, sign in to Zoho and press "
        "<b>Accept</b>. The browser lands on the web CRM; click <b>Refresh</b> here to "
        "pick the connection up, then <b>Sync Zoho Mail</b>.</li>"
        "</ol>"
        "<h3>B. This organization only, without a server change</h3>"
        "<ol>"
        "<li>In the <a href=\"%2\">Zoho API Console</a> choose <b>Add Client</b> &rarr; "
        "<b>Self Client</b> and click <b>Create</b>. Copy its Client ID and Client "
        "Secret.</li>"
        "<li>On its <b>Generate Code</b> tab enter the scope%5pick 10 minutes, click "
        "<b>Create</b> and copy the code.</li>"
        "<li>Within those 10 minutes click <b>Enter in Settings</b> below, paste the "
        "client ID, the client secret and the code into <b>Zoho Self Client code</b>, "
        "and save. ForkMesh redeems the code itself, tries every Zoho data center and "
        "reads the mail account id. No <code>curl</code> needed. Then click "
        "<b>Sync Zoho Mail</b>.</li>"
        "</ol>"
        "<p>The CRM only reads mail: it asks for %6. The client secret and refresh "
        "token are stored encrypted and are never shown again.</p>")
        .arg(state.toHtmlEscaped(), console, code.arg(callback.toHtmlEscaped()),
             code.arg(zohoSecretLines().toHtmlEscaped()),
             code.arg(QLatin1String(kZohoScopes)),
             QStringLiteral("<code>%1</code>").arg(QLatin1String(kZohoScopes)));
}

} // namespace

void CrmFeature::showZohoGuide()
{
    const QJsonObject zoho = m_settings.value(QStringLiteral("zoho")).toObject();
    const bool connected = zoho.value(QStringLiteral("connected")).toBool();
    const bool oneClick = zoho.value(QStringLiteral("oneClick")).toBool();
    const QString state = connected
        ? QStringLiteral("Zoho Mail is already connected for this organization: use "
                         "Sync Zoho Mail. This guide is only needed to set it up again.")
        : oneClick
        ? QStringLiteral("One-click connect is on for this server: skip to step A7.")
        : QStringLiteral("One-click connect is off: the server has no ForkMesh Zoho "
                         "OAuth client yet. Follow A (server operator) or B (this "
                         "organization only).");
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Set up Zoho OAuth"));
    dialog.resize(720, 640);
    auto *layout = new QVBoxLayout(&dialog);
    auto *guide = new QTextBrowser;
    guide->setOpenExternalLinks(true);
    guide->setHtml(zohoGuideHtml(state));
    layout->addWidget(guide, 1);
    auto *row = new QHBoxLayout;
    const auto action = [row](const QString &label) {
        auto *button = new QPushButton(label);
        button->setAutoDefault(false);
        row->addWidget(button);
        return button;
    };
    auto *openConsole = action(QStringLiteral("Open Zoho API Console"));
    auto *copyRedirect = action(QStringLiteral("Copy redirect URI"));
    auto *copySecrets = action(QStringLiteral("Copy secrets.env lines"));
    auto *manual = action(QStringLiteral("Enter in Settings"));
    auto *connectNow = action(QStringLiteral("Connect Zoho Mail"));
    row->addStretch();
    auto *close = action(QStringLiteral("Close"));
    layout->addLayout(row);
    manual->setEnabled(m_canManageSettings);
    connectNow->setEnabled(m_canManageSettings && oneClick && !connected);
    connectNow->setToolTip(oneClick ? QString()
                                    : QStringLiteral("Available once step A is done"));
    connect(openConsole, &QPushButton::clicked, &dialog, [] {
        QDesktopServices::openUrl(QUrl(QLatin1String(kZohoApiConsole)));
    });
    connect(copyRedirect, &QPushButton::clicked, &dialog, [this] {
        QGuiApplication::clipboard()->setText(QLatin1String(kZohoCallbackUri));
        setStatus(QStringLiteral("Zoho redirect URI copied."));
    });
    connect(copySecrets, &QPushButton::clicked, &dialog, [this] {
        QGuiApplication::clipboard()->setText(zohoSecretLines());
        setStatus(QStringLiteral("secrets.env lines copied: fill in the client id and secret."));
    });
    connect(manual, &QPushButton::clicked, &dialog, [this, &dialog] {
        dialog.accept();
        editSettings();
    });
    connect(connectNow, &QPushButton::clicked, &dialog, [this, &dialog] {
        dialog.accept();
        connectZohoMail();
    });
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    dialog.exec();
}

QList<NoteDraft> CrmFeature::noteDrafts() const
{
    static const QRegularExpression status(
        QStringLiteral("https://[A-Za-z0-9.-]+/@[A-Za-z0-9_.]+/[0-9]{6,}"));
    QList<NoteDraft> drafts;
    for (const QJsonValue &value : m_contacts) {
        const QJsonObject contact = value.toObject();
        const QString notes = text(contact, "notes");
        const qsizetype at = noteDraftAt(notes);
        if (at < 0)
            continue;
        const qsizetype end = notes.indexOf(QLatin1String("\n\n"), at);
        QStringList lines = notes.mid(at, end < 0 ? -1 : end - at).split(QLatin1Char('\n'));
        NoteDraft draft;
        draft.contactId = text(contact, "id");
        draft.name = text(contact, "name");
        draft.mastodon = lines.takeFirst().contains(QLatin1String("Mastodon"),
                                                    Qt::CaseInsensitive);
        if (!lines.isEmpty() && lines.first().startsWith(QLatin1String("Subject:")))
            draft.subject = lines.takeFirst().mid(8).trimmed();
        draft.body = lines.join(QLatin1Char('\n')).trimmed();
        if (draft.body.isEmpty())
            continue;
        if (draft.mastodon) {
            draft.to = text(contact, "mastodon");
            const QList<QJsonObject> toots = m_contactToots.value(draft.contactId);
            draft.thread = toots.isEmpty() ? QString() : text(toots.first(), "providerUrl");
            if (draft.thread.isEmpty())
                draft.thread = status.match(notes).captured(0);
        } else {
            draft.to = text(contact, "email");
        }
        if (draft.name.isEmpty())
            draft.name = draft.to;
        drafts.append(draft);
    }
    return drafts;
}

void CrmFeature::syncNoteDrafts()
{
    if (!m_noteDraftsButton)
        return;
    const int count = int(noteDrafts().size());
    m_noteDraftsButton->setText(count > 0 ? QStringLiteral("Drafts (%1)").arg(count)
                                          : QStringLiteral("Drafts"));
    m_noteDraftsButton->setAccessibleName(m_noteDraftsButton->text());
    if (m_noteDraftsTable)
        fillNoteDrafts();
}

void CrmFeature::showNoteDrafts()
{
    if (m_noteDraftsTable) {
        m_noteDraftsTable->window()->raise();
        m_noteDraftsTable->window()->activateWindow();
        return;
    }
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Unsent drafts"));
    dialog->resize(900, 420);
    auto *layout = new QVBoxLayout(dialog);
    m_noteDraftsCount = new QLabel;
    m_noteDraftsCount->setWordWrap(true);
    layout->addWidget(m_noteDraftsCount);
    m_noteDraftsTable = new QTableWidget(0, 5);
    m_noteDraftsTable->setHorizontalHeaderLabels(
        {QStringLiteral("Contact"), QStringLiteral("Channel"), QStringLiteral("To"),
         QStringLiteral("Message"), QString()});
    m_noteDraftsTable->setAccessibleName(QStringLiteral("Unsent drafts"));
    m_noteDraftsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_noteDraftsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_noteDraftsTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_noteDraftsTable->setWordWrap(false);
    m_noteDraftsTable->verticalHeader()->hide();
    m_noteDraftsTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    layout->addWidget(m_noteDraftsTable, 1);
    connect(m_noteDraftsTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (QTableWidgetItem *item = m_noteDraftsTable ? m_noteDraftsTable->item(row, 0)
                                                       : nullptr)
            showContactDetail(item->data(Qt::UserRole).toString());
    });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);
    fillNoteDrafts();
    dialog->show();
}

void CrmFeature::fillNoteDrafts()
{
    if (!m_noteDraftsTable)
        return;
    const QList<NoteDraft> drafts = noteDrafts();
    const auto mastodon = std::count_if(drafts.cbegin(), drafts.cend(),
                                        [](const NoteDraft &draft) { return draft.mastodon; });
    if (m_noteDraftsCount)
        m_noteDraftsCount->setText(drafts.isEmpty()
            ? QStringLiteral("No unsent drafts. A draft is a contact note paragraph that "
                             "starts with \"DRAFT, not sent.\"")
            : QStringLiteral("%1 unsent %2: %3 by email, %4 as Mastodon replies. Click the "
                             "button on a row to send it; double-click a row to open the "
                             "contact.")
                  .arg(drafts.size())
                  .arg(drafts.size() == 1 ? QStringLiteral("draft") : QStringLiteral("drafts"))
                  .arg(drafts.size() - mastodon)
                  .arg(mastodon));
    m_noteDraftsTable->setRowCount(int(drafts.size()));
    for (int row = 0; row < drafts.size(); ++row) {
        const NoteDraft &draft = drafts.at(row);
        auto *name = new QTableWidgetItem(draft.name);
        name->setData(Qt::UserRole, draft.contactId);
        m_noteDraftsTable->setItem(row, 0, name);
        m_noteDraftsTable->setItem(row, 1, new QTableWidgetItem(
            draft.mastodon ? QStringLiteral("Mastodon reply") : QStringLiteral("Email")));
        m_noteDraftsTable->setItem(row, 2, new QTableWidgetItem(draft.to));
        const QString message = draft.subject.isEmpty()
            ? draft.body : QString::fromUtf8("%1 \xE2\x80\x94 %2").arg(draft.subject, draft.body);
        auto *preview = new QTableWidgetItem(QString(message).replace(QLatin1Char('\n'),
                                                                      QLatin1Char(' ')));
        preview->setToolTip(message);
        m_noteDraftsTable->setItem(row, 3, preview);
        auto *send = new QPushButton(draft.mastodon ? QStringLiteral("Reply on Mastodon")
                                                    : QStringLiteral("Send"));
        send->setEnabled(draft.mastodon ? !draft.thread.isEmpty() : !draft.to.isEmpty());
        send->setToolTip(draft.mastodon
            ? (draft.thread.isEmpty() ? QStringLiteral("No outreach toot is linked to this contact")
                                      : QStringLiteral("Copy the reply and open %1").arg(draft.thread))
            : (draft.to.isEmpty() ? QStringLiteral("The contact has no email address")
                                  : QStringLiteral("Open the compose form filled in for %1")
                                        .arg(draft.to)));
        connect(send, &QPushButton::clicked, this, [this, draft] { sendNoteDraft(draft); });
        m_noteDraftsTable->setCellWidget(row, 4, send);
    }
    m_noteDraftsTable->resizeColumnToContents(0);
    m_noteDraftsTable->resizeColumnToContents(1);
    m_noteDraftsTable->resizeColumnToContents(2);
    m_noteDraftsTable->resizeColumnToContents(4);
}

void CrmFeature::sendNoteDraft(const NoteDraft &draft)
{
    if (!draft.mastodon) {
        QJsonObject contact;
        for (const QJsonValue &value : std::as_const(m_contacts)) {
            if (text(value.toObject(), "id") == draft.contactId)
                contact = value.toObject();
        }
        if (contact.isEmpty())
            return;
        const QString id = draft.contactId;
        composeEmail(contact, draft.subject, draft.body,
                     [this, id] { markNoteDraftSent(id, false); });
        return;
    }
    QGuiApplication::clipboard()->setText(draft.body);
    QDesktopServices::openUrl(QUrl(draft.thread));
    QWidget *parent = m_noteDraftsTable ? m_noteDraftsTable->window() : this;
    const auto answer = QMessageBox::question(
        parent, QStringLiteral("Reply on Mastodon"),
        QStringLiteral("The reply to %1 is on the clipboard and the thread is open in the "
                       "browser. Reply there, paste it and post it.\n\n"
                       "Mark this draft as sent?").arg(draft.name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes)
        markNoteDraftSent(draft.contactId, true);
}

void CrmFeature::markNoteDraftSent(const QString &contactId, bool stampContacted)
{
    for (qsizetype at = 0; at < m_contacts.size(); ++at) {
        QJsonObject contact = m_contacts.at(at).toObject();
        if (text(contact, "id") != contactId)
            continue;
        QString notes = text(contact, "notes");
        const qsizetype where = noteDraftAt(notes);
        if (where < 0)
            return;
        const QString by = m_viewerName.isEmpty() ? QString()
                                                  : QStringLiteral(" by ") + m_viewerName;
        notes.replace(where, QLatin1String(kNoteDraftMark).size(),
                      QStringLiteral("DRAFT, sent %1%2.")
                          .arg(QDate::currentDate().toString(Qt::ISODate), by));
        QJsonObject body{{QStringLiteral("notes"), notes}};
        if (stampContacted)
            body.insert(QStringLiteral("lastContactedAt"),
                        QJsonValue(QDateTime::currentMSecsSinceEpoch()));
        for (auto field = body.constBegin(); field != body.constEnd(); ++field)
            contact.insert(field.key(), field.value());
        m_contacts.replace(at, contact);
        refreshContactSnapshot();
        request(QByteArrayLiteral("PATCH"),
                organizationPath(QStringLiteral("/contacts/") + contactId), body,
                [this](bool ok, const QJsonObject &, const QString &error) {
            if (ok)
                setStatus(QStringLiteral("Draft marked sent in the contact's notes."));
            else
                setStatus(QStringLiteral("The draft was not marked sent: %1").arg(error), true);
            loadOrganization();
        });
        return;
    }
}

void CrmFeature::syncZohoMail()
{
    if (!m_settings.value(QStringLiteral("zoho")).toObject()
             .value(QStringLiteral("connected")).toBool())
        return;
    m_zohoSyncButton->setEnabled(false);
    setStatus(QStringLiteral("Importing Zoho Mail into shared CRM boxes…"));
    request(QByteArrayLiteral("POST"),
            organizationPath(QStringLiteral("/mail/import")),
            QJsonObject{{QStringLiteral("limit"), 200}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        m_zohoSyncButton->setEnabled(true);
        if (!ok) {
            setStatus(QStringLiteral("Zoho Mail sync failed: %1").arg(error), true);
            return;
        }
        setStatus(QStringLiteral("Zoho Mail synced: %1 new, %2 refreshed, %3 sent emails recorded.")
                      .arg(payload.value(QStringLiteral("created")).toInt())
                      .arg(payload.value(QStringLiteral("updated")).toInt())
                      .arg(payload.value(QStringLiteral("outboundRecorded")).toInt()));
        loadOrganization();
    });
}

QStringList CrmFeature::csvRecords(const QString &text)
{
    QStringList records;
    qsizetype start = 0;
    bool quoted = false;
    const qsizetype size = text.size();
    for (qsizetype index = 0; index <= size; ++index) {
        const bool end = index == size;
        if (!end && text.at(index) == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (end || (text.at(index) == QLatin1Char('\n') && !quoted)) {
            QString record = text.mid(start, index - start);
            if (record.endsWith(QLatin1Char('\r')))
                record.chop(1);
            if (!record.isEmpty())
                records.append(record);
            start = index + 1;
        }
    }
    return records;
}

void CrmFeature::importCsv()
{
    if (importBusy())
        return;
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Import CRM contacts"), QString(),
        QStringLiteral("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1536 * 1024) {
        setStatus(QStringLiteral("Choose a readable CSV no larger than 1.5 MB."), true);
        return;
    }
    QString csv = QString::fromUtf8(file.readAll());
    if (csv.startsWith(QChar(0xFEFF)))
        csv.remove(0, 1);
    // Every part goes to the organization the import started in, even if the
    // picker moves on while it runs.
    startCsvImport(organizationPath(QStringLiteral("/contacts/import")),
                   QFileInfo(path).fileName(), csv, QString());
}

void CrmFeature::startCsvImport(const QString &path, const QString &fileName,
                                const QString &csv, const QString &sheetName,
                                const QString &viewId, const QJsonArray &mapping)
{
    QStringList records = csvRecords(csv);
    m_import = CsvImportJob{};
    m_import.active = true;
    m_import.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_import.path = path;
    m_import.fileName = fileName;
    m_import.sheetName = sheetName;
    if (!records.isEmpty())
        m_import.header = records.takeFirst();
    m_import.rows = records;
    m_import.parts = qMax(
        1, int((records.size() + kImportPartRows - 1) / kImportPartRows));

    m_importErrors->clear();
    m_importErrors->hide();
    setImportButtonsEnabled(false);
    if (!mapping.isEmpty()) {
        m_import.viewId = viewId;
        m_import.mapping = mapping;
        m_import.delimiter = QStringLiteral(",");
        beginImportUpload();
        return;
    }
    setStatus(QString::fromUtf8("Reading the columns of %1\xE2\x80\xA6").arg(m_import.fileName));
    // The import step: the worker reads the header and the first rows and
    // guesses what each column is. Nothing is written until the member
    // confirms the mapping, which then travels with every part.
    QString preview = m_import.header + QStringLiteral("\r\n");
    for (const QString &row : records.mid(0, kImportPreviewRows))
        preview += row + QStringLiteral("\r\n");
    request(QByteArrayLiteral("POST"), m_import.path,
            QJsonObject{{QStringLiteral("preview"), true},
                        {QStringLiteral("csv"), preview}},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok) {
            m_import = CsvImportJob{};
            setImportButtonsEnabled(true);
            setStatus(QStringLiteral("CSV import failed: %1").arg(error), true);
            return;
        }
        const QString delimiter = payload.value(QStringLiteral("delimiter")).toString();
        if (!delimiter.isEmpty())
            m_import.delimiter = delimiter;
        if (!mapImportColumns(payload.value(QStringLiteral("columns")).toArray())) {
            m_import = CsvImportJob{};
            setImportButtonsEnabled(true);
            setStatus(QStringLiteral("CSV import cancelled."));
            return;
        }
        beginImportUpload();
    });
}

void CrmFeature::beginImportUpload()
{
    m_importProgress->setRange(0, qMax(1, int(m_import.rows.size())));
    m_importProgress->setValue(0);
    m_importProgress->setFormat(m_import.rows.isEmpty()
                                    ? QStringLiteral("%p%")
                                    : QStringLiteral("%v of %m rows · %p%"));
    m_importProgress->show();
    setStatus(QStringLiteral("Importing %1…").arg(m_import.fileName));
    sendImportPart();
}

namespace {

QString csvCell(const QString &value)
{
    if (!value.contains(QRegularExpression(QStringLiteral("[\",\r\n]"))))
        return value;
    QString quoted = value;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

// The rel="next" page of a Mastodon Link header, kept on the same instance.
QUrl mastodonNextPage(const QByteArray &link, const QString &host)
{
    static const QRegularExpression next(
        QStringLiteral("<([^>]+)>\\s*;\\s*rel=\"?next\"?"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = next.match(QString::fromUtf8(link));
    if (!match.hasMatch())
        return QUrl();
    const QUrl url(match.captured(1));
    return url.scheme() == QLatin1String("https") &&
                   url.host().compare(host, Qt::CaseInsensitive) == 0
               ? url : QUrl();
}

} // namespace

void CrmFeature::importMastodonFollowers()
{
    if (importBusy())
        return;
    if (organizationPath().isEmpty()) {
        setStatus(QStringLiteral("Select an organization first."), true);
        return;
    }
    bool ok = false;
    QString typed = QInputDialog::getText(
        this, QStringLiteral("Import Mastodon followers"),
        QStringLiteral("The account whose public followers become a list, as a "
                       "handle (@name@mastodon.social) or a profile link.\n"
                       "Importing the same account again updates the followers it "
                       "added before. Accounts that hide their followers list none."),
        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || typed.isEmpty())
        return;
    if (!typed.contains(QLatin1String("://")) && typed.contains(QLatin1String("/@")))
        typed.prepend(QLatin1String("https://"));
    const QString key = mastodonHandleKey(typed);
    qsizetype at = key.lastIndexOf(QLatin1Char('@'));
    QString user = key.left(at);
    QString host = key.mid(at + 1);
    // A profile link to a remote account (host/@name@home) is read on the
    // account's own instance, where its full follower list lives.
    at = user.indexOf(QLatin1Char('@'));
    if (at > 0) {
        host = user.mid(at + 1);
        user = user.left(at);
    }
    if (key.isEmpty() || user.isEmpty() || !host.contains(QLatin1Char('.'))) {
        setStatus(QStringLiteral("Enter a handle like @name@mastodon.social or a "
                                 "profile link."), true);
        return;
    }
    m_followers = MastodonFollowersJob{};
    m_followers.active = true;
    m_followers.path = organizationPath(QStringLiteral("/contacts/import"));
    m_followers.host = host;
    m_followers.handle = QLatin1Char('@') + user + QLatin1Char('@') + host;
    setImportButtonsEnabled(false);
    setStatus(QString::fromUtf8("Reading the followers of %1\xE2\x80\xA6")
                  .arg(m_followers.handle));
    QUrl lookup;
    lookup.setScheme(QStringLiteral("https"));
    lookup.setHost(host);
    lookup.setPath(QStringLiteral("/api/v1/accounts/lookup"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("acct"), user);
    lookup.setQuery(query);
    QNetworkRequest lookupRequest = instanceRequest(lookup);
    forkmesh::markExpectedHttpStatus(lookupRequest, 404);
    forkmesh::markExpectedHttpStatus(lookupRequest, 406);
    forkmesh::markExpectedHttpStatus(lookupRequest, 410);
    QNetworkReply *reply = m_network->get(lookupRequest);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (forkmesh::networkReplyStatusIsExpected(reply))
            return finishMastodonFollowers(QStringLiteral("the instance knows no such account"));
        if (reply->error() != QNetworkReply::NoError)
            return finishMastodonFollowers(reply->errorString());
        const QJsonObject account = QJsonDocument::fromJson(reply->readAll()).object();
        const QString id = account.value(QStringLiteral("id")).toString();
        if (!QRegularExpression(QStringLiteral("^[\\w-]{1,64}$")).match(id).hasMatch())
            return finishMastodonFollowers(QStringLiteral("the instance knows no such account"));
        m_followers.total = account.value(QStringLiteral("followers_count")).toInt();
        QUrl followers;
        followers.setScheme(QStringLiteral("https"));
        followers.setHost(m_followers.host);
        followers.setPath(QStringLiteral("/api/v1/accounts/%1/followers").arg(id));
        QUrlQuery page;
        page.addQueryItem(QStringLiteral("limit"),
                          QString::number(kMastodonFollowersPage));
        followers.setQuery(page);
        fetchMastodonFollowersPage(followers);
    });
}

void CrmFeature::fetchMastodonFollowersPage(const QUrl &url)
{
    QNetworkReply *reply = m_network->get(instanceRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return finishMastodonFollowers(reply->errorString());
        static const QRegularExpression shortcode(QStringLiteral(":[\\w+-]+:"));
        const QJsonArray accounts = QJsonDocument::fromJson(reply->readAll()).array();
        for (const QJsonValue &value : accounts) {
            if (m_followers.rows.size() >= kMastodonFollowersMax)
                break;
            const QJsonObject account = value.toObject();
            const QString acct = text(account, "acct");
            const QString id = text(account, "id");
            if (acct.isEmpty() || m_followers.seen.contains(id + acct))
                continue;
            m_followers.seen.insert(id + acct);
            // A local follower's acct has no instance, so the instance the
            // list is read from makes every handle full.
            const QString handle = acct.contains(QLatin1Char('@'))
                ? QLatin1Char('@') + acct
                : QLatin1Char('@') + acct + QLatin1Char('@') + m_followers.host;
            QString name = text(account, "display_name");
            name = name.remove(shortcode).simplified();
            if (name.isEmpty())
                name = text(account, "username");
            m_followers.rows << QStringList{
                csvCell(name.isEmpty() ? acct : name), csvCell(handle),
                csvCell(QTextDocumentFragment::fromHtml(text(account, "note"))
                            .toPlainText().simplified())}
                .join(QLatin1Char(','));
        }
        setStatus(QString::fromUtf8("Reading the followers of %1\xE2\x80\xA6 %2%3")
                      .arg(m_followers.handle)
                      .arg(m_followers.rows.size())
                      .arg(m_followers.total
                               ? QStringLiteral(" of %1").arg(m_followers.total)
                               : QString()));
        const QUrl next = accounts.isEmpty()
            ? QUrl()
            : mastodonNextPage(reply->rawHeader(QByteArrayLiteral("Link")),
                               m_followers.host);
        if (next.isValid() && m_followers.rows.size() < kMastodonFollowersMax)
            return fetchMastodonFollowersPage(next);
        finishMastodonFollowers();
    });
}

void CrmFeature::finishMastodonFollowers(const QString &error)
{
    const MastodonFollowersJob job = m_followers;
    m_followers = MastodonFollowersJob{};
    setImportButtonsEnabled(true);
    if (!error.isEmpty()) {
        setStatus(QStringLiteral("Could not read the followers of %1: %2.")
                      .arg(job.handle, error), true);
        return;
    }
    if (job.rows.isEmpty()) {
        setStatus(job.total
                      ? QStringLiteral("%1 hides its followers, so there is nothing "
                                       "to import.").arg(job.handle)
                      : QStringLiteral("%1 has no followers yet.").arg(job.handle),
                  true);
        return;
    }
    QString csv = QStringLiteral("Name,Mastodon,Notes\r\n");
    for (const QString &row : job.rows)
        csv += row + QStringLiteral("\r\n");
    startCsvImport(job.path,
                   QStringLiteral("mastodon-followers-%1.csv").arg(job.handle.mid(1)),
                   csv, QStringLiteral("Followers of %1").arg(job.handle).left(60));
    if (job.rows.size() >= kMastodonFollowersMax && job.total > job.rows.size() &&
        m_import.active)
        setStatus(QStringLiteral("Importing the %1 most recent of %2's %3 followers; "
                                 "one import takes at most %1.")
                      .arg(kMastodonFollowersMax).arg(job.handle).arg(job.total));
}

namespace {

constexpr int kGithubContributorsMax = 2000;
constexpr int kGithubContributorsTimeoutMs = 10 * 60 * 1000;

struct GitContributor
{
    QString name;
    QString email;
    QString last;
    int commits = 0;
};

} // namespace

QString CrmFeature::githubRepositoryPath(const QString &value)
{
    static const QRegularExpression repository(
        QStringLiteral("^(?:(?:https?://)?(?:www\\.)?github\\.com/|git@github\\.com:)?"
                       "([A-Za-z0-9](?:[A-Za-z0-9-]{0,38}))/"
                       "([A-Za-z0-9._-]{1,100}?)(?:\\.git)?/?(?:[/?#].*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = repository.match(value.trimmed());
    if (!match.hasMatch())
        return QString();
    const QString name = match.captured(2);
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return QString();
    return match.captured(1) + QLatin1Char('/') + name;
}

bool CrmFeature::maskedGitEmail(const QString &email)
{
    static const QRegularExpression masked(
        QStringLiteral("(^|[.@+_-])(do-?not-?|no-?)reply([.@+_-]|$)"),
        QRegularExpression::CaseInsensitiveOption);
    return masked.match(email).hasMatch();
}

QString CrmFeature::githubContributorsCsv(const QByteArray &log, int limit,
                                          int *found)
{
    static const QRegularExpression address(
        QStringLiteral("^[^@\\s<>\"(),;:]+@[a-z0-9-]+(\\.[a-z0-9-]+)+$"));
    static const QRegularExpression unreachable(
        QStringLiteral("\\.(local|localdomain|lan|home|internal|invalid|example)$"));
    static const QRegularExpression coauthor(
        QStringLiteral("^(.*?)\\s*<([^<>]+)>$"));
    QHash<QString, GitContributor> people;
    for (const QByteArray &record : log.split('\0')) {
        const QStringList fields =
            QString::fromUtf8(record.trimmed()).split(QChar(0x1f));
        if (fields.size() < 6)
            continue;
        QList<std::pair<QString, QString>> signers{{fields.at(0), fields.at(1)},
                                                   {fields.at(2), fields.at(3)}};
        for (const QString &line :
             fields.at(4).split(QChar(0x1d), Qt::SkipEmptyParts)) {
            const QRegularExpressionMatch match = coauthor.match(line.trimmed());
            if (match.hasMatch())
                signers.append({match.captured(1), match.captured(2)});
        }
        QSet<QString> counted;
        for (const auto &[rawName, rawEmail] : signers) {
            const QString email = rawEmail.trimmed().toLower();
            const QString name = rawName.simplified();
            if (counted.contains(email) || !address.match(email).hasMatch() ||
                unreachable.match(email).hasMatch() || maskedGitEmail(email) ||
                name.endsWith(QLatin1String("[bot]"), Qt::CaseInsensitive) ||
                email.section(QLatin1Char('@'), 0, 0).endsWith(QLatin1String("[bot]")))
                continue;
            counted.insert(email);
            GitContributor &person = people[email];
            if (person.email.isEmpty()) {
                person.email = email;
                person.name = name;
                person.last = fields.at(5).trimmed();
            } else if (person.name.isEmpty()) {
                person.name = name;
            }
            ++person.commits;
        }
    }
    QList<GitContributor> ranked = people.values();
    std::sort(ranked.begin(), ranked.end(),
              [](const GitContributor &a, const GitContributor &b) {
        if (a.commits != b.commits)
            return a.commits > b.commits;
        if (a.last != b.last)
            return a.last > b.last;
        return a.email < b.email;
    });
    if (found)
        *found = int(ranked.size());
    QString csv = QStringLiteral("Name,Email,Commits,Last commit\r\n");
    for (const GitContributor &person : ranked.mid(0, limit))
        csv += QStringList{csvCell(person.name), csvCell(person.email),
                           QString::number(person.commits), csvCell(person.last)}
                   .join(QLatin1Char(',')) +
               QStringLiteral("\r\n");
    return csv;
}

void CrmFeature::importGithubContributors()
{
    if (importBusy())
        return;
    if (organizationPath().isEmpty()) {
        setStatus(QStringLiteral("Select an organization first."), true);
        return;
    }
    bool ok = false;
    const QString typed = QInputDialog::getText(
        this, QStringLiteral("Import GitHub contributors"),
        QStringLiteral("The public GitHub repository whose contributors become a "
                       "list, as owner/name or a github.com link.\n"
                       "Its history is cloned without any files into a temporary "
                       "folder that is deleted afterwards. Every author, committer "
                       "and co-author email is imported; GitHub's masked addresses "
                       "(users.noreply.github.com) and other no-reply addresses "
                       "are left out.\n"
                       "Importing the same repository again updates the "
                       "contributors it added before."),
        QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || typed.isEmpty())
        return;
    const QString repository = githubRepositoryPath(typed);
    if (repository.isEmpty()) {
        setStatus(QStringLiteral("Enter a repository like owner/name or "
                                 "https://github.com/owner/name."), true);
        return;
    }
    auto checkout = std::make_shared<QTemporaryDir>(
        QDir::temp().filePath(QStringLiteral("forkmesh-crm-contributors-XXXXXX")));
    if (!checkout->isValid()) {
        setStatus(QStringLiteral("Could not create a temporary folder to clone "
                                 "%1 into.").arg(repository), true);
        return;
    }
    m_contributors = GithubContributorsJob{};
    m_contributors.active = true;
    m_contributors.path = organizationPath(QStringLiteral("/contacts/import"));
    m_contributors.repository = repository;
    m_contributors.checkout = checkout;
    setImportButtonsEnabled(false);
    setStatus(QString::fromUtf8("Cloning %1\xE2\x80\xA6").arg(repository));
    runContributorsGit(
        {QStringLiteral("-c"), QStringLiteral("credential.helper="),
         QStringLiteral("clone"), QStringLiteral("--bare"), QStringLiteral("--quiet"),
         QStringLiteral("--no-tags"), QStringLiteral("--filter=tree:0"),
         QStringLiteral("https://github.com/%1.git").arg(repository),
         QStringLiteral("repository.git")},
        [this](const QByteArray &) {
        setStatus(QString::fromUtf8("Reading the contributors of %1\xE2\x80\xA6")
                      .arg(m_contributors.repository));
        runContributorsGit(
            {QStringLiteral("-C"), QStringLiteral("repository.git"),
             QStringLiteral("-c"), QStringLiteral("log.mailmap=false"),
             QStringLiteral("log"), QStringLiteral("--all"), QStringLiteral("-z"),
             QStringLiteral("--date=short"),
             QStringLiteral("--format=%an%x1f%ae%x1f%cn%x1f%ce%x1f"
                            "%(trailers:key=Co-authored-by,valueonly,unfold,"
                            "separator=%x1d)%x1f%ad")},
            [this](const QByteArray &log) { finishGithubContributors(QString(), log); });
    });
}

void CrmFeature::runContributorsGit(const QStringList &arguments,
                                    std::function<void(const QByteArray &)> done)
{
    auto *process = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GCM_INTERACTIVE"), QStringLiteral("never"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process->setProcessEnvironment(environment);
    process->setWorkingDirectory(m_contributors.checkout->path());
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    connect(process, &QProcess::finished, this,
            [this, process, done](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (!m_contributors.active)
            return;
        if (status == QProcess::NormalExit && code == 0)
            return done(process->readAllStandardOutput());
        const QString failure = QString::fromUtf8(process->readAllStandardError())
                                    .trimmed().section(QLatin1Char('\n'), -1)
                                    .remove(QRegularExpression(QStringLiteral("^fatal:\\s*")));
        if (failure.contains(QLatin1String("not found")) ||
            failure.contains(QLatin1String("could not read Username")))
            return finishGithubContributors(
                QStringLiteral("GitHub has no public repository by that name"));
        finishGithubContributors(status == QProcess::NormalExit && !failure.isEmpty()
                                     ? failure
                                     : QStringLiteral("git stopped before it finished"));
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        if (m_contributors.active)
            finishGithubContributors(QStringLiteral("Git is not installed"));
    });
    timeout->start(kGithubContributorsTimeoutMs);
    process->start(QStringLiteral("git"), arguments);
}

void CrmFeature::finishGithubContributors(const QString &error, const QByteArray &log)
{
    const GithubContributorsJob job = m_contributors;
    m_contributors = GithubContributorsJob{};
    setImportButtonsEnabled(true);
    if (!error.isEmpty()) {
        setStatus(QStringLiteral("Could not read the contributors of %1: %2.")
                      .arg(job.repository, error), true);
        return;
    }
    int found = 0;
    const QString csv = githubContributorsCsv(log, kGithubContributorsMax, &found);
    if (!found) {
        setStatus(QStringLiteral("No contributor of %1 has a public email "
                                 "address to import.").arg(job.repository), true);
        return;
    }
    startCsvImport(job.path,
                   QStringLiteral("github-contributors-%1.csv")
                       .arg(QString(job.repository).replace(QLatin1Char('/'),
                                                            QLatin1Char('-'))),
                   csv, QStringLiteral("Contributors of %1").arg(job.repository).left(60));
    if (found > kGithubContributorsMax && m_import.active)
        setStatus(QStringLiteral("Importing the %1 most active of %2's %3 "
                                 "contributors; one import takes at most %1.")
                      .arg(kGithubContributorsMax).arg(job.repository).arg(found));
}

namespace {

constexpr int kForkMeshUsersMax = 2000;
const char kForkMeshUsersFile[] = "forkmesh-users.csv";

} // namespace

QString CrmFeature::forkMeshUsersCsv(const QJsonArray &users, int limit, int *found,
                                     int *withoutEmail)
{
    static const QRegularExpression address(
        QStringLiteral("^[^@\\s<>\"(),;:]+@[a-z0-9-]+(\\.[a-z0-9-]+)+$"));
    QList<QJsonObject> kept;
    QSet<QString> seen;
    int missing = 0;
    for (const QJsonValue &value : users) {
        const QJsonObject user = value.toObject();
        const QString kind = text(user, "kind");
        const QString status = text(user, "status");
        if ((!kind.isEmpty() && kind != QLatin1String("user")) ||
            (!status.isEmpty() && status != QLatin1String("active")) ||
            text(user, "name").isEmpty())
            continue;
        const QString email = text(user, "email").toLower();
        if (!address.match(email).hasMatch()) {
            ++missing;
            continue;
        }
        if (seen.contains(email))
            continue;
        seen.insert(email);
        QJsonObject row = user;
        row.insert(QStringLiteral("email"), email);
        kept.append(row);
    }
    std::stable_sort(kept.begin(), kept.end(),
                     [](const QJsonObject &a, const QJsonObject &b) {
        return a.value(QStringLiteral("createdAt")).toDouble() >
               b.value(QStringLiteral("createdAt")).toDouble();
    });
    if (found)
        *found = int(kept.size());
    if (withoutEmail)
        *withoutEmail = missing;
    QString csv = QStringLiteral("Name,Email,Signed up,Email notifications\r\n");
    for (const QJsonObject &user : kept.mid(0, limit)) {
        const qint64 created =
            static_cast<qint64>(user.value(QStringLiteral("createdAt")).toDouble());
        csv += QStringList{
                   csvCell(text(user, "name")), csvCell(text(user, "email")),
                   created > 0 ? QDateTime::fromMSecsSinceEpoch(created).toUTC()
                                     .toString(QStringLiteral("yyyy-MM-dd"))
                               : QString(),
                   user.value(QStringLiteral("emailNotifications")).toBool(true)
                       ? QStringLiteral("On")
                       : QStringLiteral("Off")}
                   .join(QLatin1Char(',')) +
               QStringLiteral("\r\n");
    }
    return csv;
}

QString CrmFeature::forkMeshUsersSheetId(const QJsonArray &views)
{
    for (const QJsonValue &value : views) {
        const QJsonObject view = value.toObject();
        if (text(view, "fileName") == QLatin1String(kForkMeshUsersFile) &&
            view.value(QStringLiteral("canManage")).toBool())
            return text(view, "id");
    }
    return QString();
}

bool CrmFeature::importBusy() const
{
    return m_import.active || m_followers.active || m_contributors.active ||
           m_usersReading || m_grantsLoading;
}

void CrmFeature::setImportButtonsEnabled(bool enabled)
{
    m_importButton->setEnabled(enabled);
    m_githubButton->setEnabled(enabled);
    m_usersButton->setEnabled(enabled);
    m_usersPageButton->setEnabled(enabled);
}

void CrmFeature::updateUsersImportAction()
{
    const bool available = m_userRecords && m_userRecordsAvailable &&
                           m_userRecordsAvailable();
    m_usersButton->setVisible(available && m_installed);
    m_usersPageButton->setVisible(available);
    const bool refreshes = !forkMeshUsersSheetId(m_views).isEmpty();
    m_usersButton->setText(refreshes ? QStringLiteral("Refresh users")
                                     : QStringLiteral("Import users"));
    m_usersButton->setAccessibleName(m_usersButton->text());
    m_usersPageButton->setText(refreshes ? QStringLiteral("Refresh ForkMesh users")
                                         : QStringLiteral("Import ForkMesh users"));
    const QString tip = refreshes
        ? QStringLiteral("Read the ForkMesh user directory again: new signups join "
                         "the ForkMesh users list and the users already on it are "
                         "updated, matched by email")
        : QStringLiteral("Import every active ForkMesh user with an email address "
                         "as a new list, ForkMesh users, that Refresh users keeps "
                         "up to date");
    m_usersButton->setToolTip(tip);
    m_usersPageButton->setToolTip(tip);
}

void CrmFeature::importForkMeshUsers()
{
    if (importBusy())
        return;
    if (organizationPath().isEmpty()) {
        setStatus(QStringLiteral("Select an organization first."), true);
        return;
    }
    if (!m_userRecords || !m_userRecordsAvailable || !m_userRecordsAvailable()) {
        setStatus(QStringLiteral("Only ForkMesh administrators can import the "
                                 "user directory."), true);
        return;
    }
    m_usersReading = true;
    setImportButtonsEnabled(false);
    setStatus(QString::fromUtf8("Reading the ForkMesh user directory\xE2\x80\xA6"));
    QPointer<CrmFeature> self(this);
    m_userRecords([self, path = organizationPath(QStringLiteral("/contacts/import"))](
                      const QJsonArray &users, const QString &error) {
        if (self)
            self->finishForkMeshUsers(path, users, error);
    });
}

void CrmFeature::finishForkMeshUsers(const QString &path, const QJsonArray &users,
                                     const QString &error)
{
    m_usersReading = false;
    setImportButtonsEnabled(true);
    if (!error.isEmpty()) {
        setStatus(QStringLiteral("Could not read the ForkMesh user directory: %1.")
                      .arg(error), true);
        return;
    }
    keepForkMeshAvatars(users);
    int found = 0;
    int withoutEmail = 0;
    const QString csv =
        forkMeshUsersCsv(users, kForkMeshUsersMax, &found, &withoutEmail);
    if (!found) {
        setStatus(QStringLiteral("No active ForkMesh user has an email address "
                                 "to import."), true);
        return;
    }
    startCsvImport(path, QString::fromLatin1(kForkMeshUsersFile), csv,
                   QStringLiteral("ForkMesh users"), forkMeshUsersSheetId(m_views),
                   QJsonArray{QStringLiteral("name"), QStringLiteral("email"),
                              QStringLiteral("custom"), QStringLiteral("custom")});
    if (!m_import.active)
        return;
    if (found > kForkMeshUsersMax)
        setStatus(QStringLiteral("Importing the %1 newest of %2 ForkMesh users; one "
                                 "import takes at most %1.")
                      .arg(kForkMeshUsersMax).arg(found));
    else if (withoutEmail)
        setStatus(QStringLiteral("Importing %1 ForkMesh users; %2 without an email "
                                 "address are left out.")
                      .arg(found).arg(withoutEmail));
}

bool CrmFeature::mapImportColumns(const QJsonArray &columns)
{
    // What a column can be imported as: its own column, nothing, or a contact
    // column, with a split name's two halves right after the name.
    QList<std::pair<QString, QString>> targets;
    for (const ContactColumn &column : contactColumns()) {
        targets.append({QString::fromLatin1(column.label), QString::fromLatin1(column.field)});
        if (qstrcmp(column.field, "name") == 0) {
            targets.append({QStringLiteral("First name"), QStringLiteral("first_name")});
            targets.append({QStringLiteral("Last name"), QStringLiteral("last_name")});
        }
    }
    targets.append({QStringLiteral("Last contacted"), QStringLiteral("lastContactedAt")});

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Map columns: %1").arg(m_import.fileName));
    dialog.resize(780, 540);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(QStringLiteral(
        "Each column starts on a best guess from its header and its values; "
        "change any that are wrong. An Email column is optional: rows without "
        "one are added by name, or by company when they have no name."));
    intro->setObjectName(QStringLiteral("mutedLabel"));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    // The list opens as its own tab; it is named here, starting from the
    // file's name.
    auto *nameRow = new QFormLayout;
    QLineEdit *sheetName = lineEdit(m_import.sheetName.isEmpty()
                                        ? sheetNameFromFile(m_import.fileName)
                                        : m_import.sheetName);
    sheetName->setObjectName(QStringLiteral("crmImportSheetName"));
    sheetName->setAccessibleName(QStringLiteral("Tab name"));
    sheetName->setMaxLength(60);
    nameRow->addRow(QStringLiteral("Tab name"), sheetName);
    layout->addLayout(nameRow);
    auto *table = new QTableWidget(int(columns.size()), 3, &dialog);
    table->setObjectName(QStringLiteral("crmImportMapTable"));
    table->setHorizontalHeaderLabels({QStringLiteral("CSV column"),
                                      QStringLiteral("Sample values"),
                                      QStringLiteral("Import as")});
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(36);
    table->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionMode(QAbstractItemView::NoSelection);
    table->setWordWrap(false);
    QHeaderView *header = table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(0, QHeaderView::Interactive);
    header->setSectionResizeMode(1, QHeaderView::Stretch);
    // The pickers are cell widgets, which column autosizing never measures.
    header->setSectionResizeMode(2, QHeaderView::Interactive);
    table->setColumnWidth(0, 180);
    table->setColumnWidth(2, 250);
    QList<QComboBox *> pickers;
    for (int row = 0; row < columns.size(); ++row) {
        const QJsonObject column = columns.at(row).toObject();
        const QString name = text(column, "header").isEmpty()
            ? QStringLiteral("Column %1").arg(column.value(QStringLiteral("index")).toInt() + 1)
            : text(column, "header");
        QStringList samples;
        for (const QJsonValue &sample : column.value(QStringLiteral("samples")).toArray())
            samples << sample.toString();
        const QString sampleText = samples.isEmpty()
            ? QString::fromUtf8("\xE2\x80\x94") : samples.join(QString::fromUtf8(" \xC2\xB7 "));
        auto *nameItem = new QTableWidgetItem(name);
        nameItem->setToolTip(name);
        auto *sampleItem = new QTableWidgetItem(sampleText);
        sampleItem->setToolTip(sampleText);
        table->setItem(row, 0, nameItem);
        table->setItem(row, 1, sampleItem);
        auto *picker = new QComboBox;
        picker->setAccessibleName(QStringLiteral("Import %1 as").arg(name));
        picker->addItem(QStringLiteral("Its own column: %1").arg(name), QStringLiteral("custom"));
        picker->addItem(QStringLiteral("Don't import"), QString());
        for (const auto &[label, field] : targets)
            picker->addItem(label, field);
        const int guess = picker->findData(column.value(QStringLiteral("field")).toString());
        picker->setCurrentIndex(guess >= 0 ? guess : 0);
        table->setCellWidget(row, 2, picker);
        pickers << picker;
    }
    layout->addWidget(table, 1);
    auto *note = new QLabel;
    note->setObjectName(QStringLiteral("mutedLabel"));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton *importButton = buttons->button(QDialogButtonBox::Ok);
    importButton->setText(m_import.rows.size() == 1
                              ? QStringLiteral("Import 1 row")
                              : QStringLiteral("Import %1 rows").arg(m_import.rows.size()));
    // Email is optional, but a row still needs something to be known by, and
    // a file without emails cannot be matched to the contacts it made before.
    const auto refreshNote = [pickers, note, importButton, sheetName] {
        static const QStringList kIdentity{
            QStringLiteral("name"), QStringLiteral("first_name"),
            QStringLiteral("last_name"), QStringLiteral("email"),
            QStringLiteral("company")};
        bool identified = false;
        bool email = false;
        bool mastodon = false;
        for (const QComboBox *picker : pickers) {
            const QString field = picker->currentData().toString();
            identified = identified || kIdentity.contains(field);
            email = email || field == QLatin1String("email");
            mastodon = mastodon || field == QLatin1String("mastodon");
        }
        const bool named = !sheetName->text().trimmed().isEmpty();
        note->setText(!identified
            ? QStringLiteral("Import a column as Name, Email or Company, or every row "
                             "will be skipped.")
            : !named ? QStringLiteral("Name the tab this list opens in.")
            : email ? QString()
            : mastodon ? QStringLiteral("No Email column: rows are matched by their "
                                        "Mastodon handle, so importing this file "
                                        "again updates them.")
            : QStringLiteral("No Email column: every row is added as a new contact, so "
                             "importing this file again adds its rows again."));
        note->setVisible(!note->text().isEmpty());
        note->setProperty("error", !identified || !named);
        note->style()->unpolish(note);
        note->style()->polish(note);
        importButton->setEnabled(identified && named);
    };
    for (QComboBox *picker : pickers)
        connect(picker, &QComboBox::currentIndexChanged, &dialog, refreshNote);
    connect(sheetName, &QLineEdit::textChanged, &dialog, refreshNote);
    refreshNote();
    sheetName->selectAll();
    sheetName->setFocus();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return false;
    QJsonArray mapping;
    for (const QComboBox *picker : pickers)
        mapping.append(picker->currentData().toString());
    m_import.mapping = mapping;
    m_import.sheetName = sheetName->text().trimmed().left(60);
    return true;
}

QJsonObject CrmFeature::importEnvelope() const
{
    // The worker's finish ping quotes the first few skipped rows of the whole
    // file, so the earliest ones travel with every later part.
    QJsonArray earlier;
    for (const QJsonValue &value : m_import.errors) {
        if (earlier.size() == 3)
            break;
        const QJsonObject item = value.toObject();
        earlier.append(QJsonObject{
            {QStringLiteral("row"), item.value(QStringLiteral("row"))},
            {QStringLiteral("error"), item.value(QStringLiteral("error"))}});
    }
    return QJsonObject{
        {QStringLiteral("id"), m_import.id},
        {QStringLiteral("part"), m_import.part},
        {QStringLiteral("parts"), m_import.parts},
        {QStringLiteral("offset"), m_import.offset},
        {QStringLiteral("rows"), int(m_import.rows.size())},
        {QStringLiteral("delimiter"), m_import.delimiter},
        {QStringLiteral("created"), m_import.created},
        {QStringLiteral("updated"), m_import.updated},
        {QStringLiteral("skipped"), m_import.skipped},
        {QStringLiteral("errors"), earlier},
    };
}

void CrmFeature::sendImportPart()
{
    const QStringList slice = m_import.rows.mid(m_import.offset, kImportPartRows);
    QString csv = m_import.header + QStringLiteral("\r\n");
    for (const QString &row : slice)
        csv += row + QStringLiteral("\r\n");
    request(QByteArrayLiteral("POST"), m_import.path,
            QJsonObject{{QStringLiteral("csv"), csv},
                        {QStringLiteral("fileName"), m_import.fileName},
                        {QStringLiteral("sheetName"), m_import.sheetName},
                        {QStringLiteral("viewId"), m_import.viewId},
                        {QStringLiteral("mapping"), m_import.mapping},
                        {QStringLiteral("import"), importEnvelope()}},
            [this, sent = int(slice.size())](
                bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return failImport(error, payload);
        // Later parts extend the sheet the first part opened, read with the
        // delimiter the first part detected.
        const QString viewId = text(payload, "viewId");
        if (!viewId.isEmpty())
            m_import.viewId = viewId;
        const QString delimiter =
            payload.value(QStringLiteral("delimiter")).toString();
        if (!delimiter.isEmpty())
            m_import.delimiter = delimiter;
        m_import.created += payload.value(QStringLiteral("created")).toInt();
        m_import.updated += payload.value(QStringLiteral("updated")).toInt();
        m_import.skipped += payload.value(QStringLiteral("skipped")).toInt();
        for (const QJsonValue &value :
             payload.value(QStringLiteral("errors")).toArray())
            m_import.errors.append(value);
        m_import.offset += sent;
        m_importProgress->setValue(m_import.rows.isEmpty()
                                       ? m_importProgress->maximum()
                                       : m_import.offset);
        showImportErrors();
        if (++m_import.part < m_import.parts)
            return sendImportPart();
        finishImport();
        // Land on the tab of the list the import just filled, like the web
        // page does. No list comes back when every row was skipped.
        if (!m_import.viewId.isEmpty() && !text(payload, "viewId").isEmpty())
            m_pendingSheet = m_import.viewId;
        const QString sheet = payload.value(QStringLiteral("view")).toObject()
                                  .value(QStringLiteral("name")).toString();
        setStatus(QStringLiteral("CSV imported into %1: %2 created, %3 updated, "
                                 "%4 skipped.")
                      .arg(sheet.isEmpty() ? QStringLiteral("the CRM") : sheet)
                      .arg(m_import.created)
                      .arg(m_import.updated)
                      .arg(m_import.skipped));
        loadOrganization();
    });
}

void CrmFeature::failImport(const QString &error, const QJsonObject &payload)
{
    // A dropped connection or a failed part never reached the worker's own
    // failure ping, so close the import's "started" ping from here.
    if (!workerClosedImport(payload)) {
        QJsonObject envelope = importEnvelope();
        envelope.insert(QStringLiteral("failed"), error);
        request(QByteArrayLiteral("POST"), m_import.path,
                QJsonObject{{QStringLiteral("fileName"), m_import.fileName},
                            {QStringLiteral("viewId"), m_import.viewId},
                            {QStringLiteral("import"), envelope}},
                [](bool, const QJsonObject &, const QString &) {});
    }
    finishImport();
    setStatus(m_import.offset > 0
                  ? QStringLiteral("CSV import failed after %1 of %2 rows: %3")
                        .arg(m_import.offset)
                        .arg(m_import.rows.size())
                        .arg(error)
                  : QStringLiteral("CSV import failed: %1").arg(error),
              true);
    if (m_import.created + m_import.updated > 0)
        loadOrganization();
}

void CrmFeature::finishImport()
{
    m_import.active = false;
    m_importProgress->hide();
    setImportButtonsEnabled(true);
}

void CrmFeature::showImportErrors()
{
    if (m_import.errors.isEmpty()) {
        m_importErrors->hide();
        return;
    }
    QStringList lines{m_import.skipped == 1
                          ? QStringLiteral("1 row skipped:")
                          : QStringLiteral("%1 rows skipped:").arg(m_import.skipped)};
    for (const QJsonValue &value : m_import.errors) {
        const QJsonObject item = value.toObject();
        const QString message = text(item, "message");
        lines.append(message.isEmpty()
                         ? QStringLiteral("Row %1: %2")
                               .arg(item.value(QStringLiteral("row")).toInt())
                               .arg(text(item, "error").replace(
                                   QLatin1Char('_'), QLatin1Char(' ')))
                         : message);
    }
    if (m_import.skipped > m_import.errors.size())
        lines.append(QStringLiteral("…and %1 more.")
                         .arg(m_import.skipped - m_import.errors.size()));
    m_importErrors->setPlainText(lines.join(QLatin1Char('\n')));
    m_importErrors->show();
}

void CrmFeature::exportCsv()
{
    request(QByteArrayLiteral("GET"),
            organizationPath(QStringLiteral("/contacts/export")), {},
            [this](bool ok, const QJsonObject &payload, const QString &error) {
        if (!ok)
            return setStatus(QStringLiteral("CSV export failed: %1").arg(error), true);
        const QString suggested = text(payload, "filename").isEmpty()
            ? QStringLiteral("contacts.csv") : text(payload, "filename");
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Export CRM contacts"), suggested,
            QStringLiteral("CSV files (*.csv)"));
        if (path.isEmpty())
            return;
        QSaveFile file(path);
        const QByteArray bytes = payload.value(QStringLiteral("csv")).toString().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
            !file.commit()) {
            setStatus(QStringLiteral("The CSV file could not be written."), true);
            return;
        }
        setStatus(QStringLiteral("Exported %1 contacts to %2.")
                      .arg(m_contacts.size()).arg(path));
    });
}

void CrmFeature::openProviderSend()
{
    const QUrl url(text(selectedEmail(), "providerUrl"));
    if (!url.isValid() || url.scheme() != QLatin1String("https")) {
        setStatus(QStringLiteral("This email has no valid HTTPS provider link."), true);
        return;
    }
    QDesktopServices::openUrl(url);
}

void CrmFeature::setStatus(const QString &value, bool error)
{
    m_status->setText(value);
    m_status->setProperty("error", error);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
}

void CrmFeature::updateActions()
{
    m_providerOpen->setEnabled(
        QUrl(text(selectedEmail(), "providerUrl")).scheme() == QLatin1String("https"));
    m_settingsButton->setEnabled(!m_organization->currentData().toString().isEmpty());
}

} // namespace forkmesh::crm

namespace {

const forkmesh::market::NativePackageRegistration crmRegistration({
    QStringLiteral("builtin:crm"),
    {QStringLiteral("extension.native")},
    QStringLiteral("CRM"),
    QStringLiteral("people"),
    QStringLiteral("/api/crm"),
    QStringLiteral("forkmesh-crm-request-v1"),
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new forkmesh::crm::CrmFeature(host, parent);
    },
});

} // namespace
