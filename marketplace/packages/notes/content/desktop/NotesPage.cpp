#include "NotesPage.h"

#include "AppPaths.h"
#include "ClientErrorReports.h"
#include "DesignTokens.h"
#include "MarkdownEditor.h"
#include "OcticonIcons.h"
#include "Theme.h"
#include "NoteCommentsPanel.h"
#include "NoteListEntry.h"
#include "NotePresenceFaces.h"
#include "NoteRequestProof.h"
#include "NoteTools.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHideEvent>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>
#include <QFile>
#include <QFontInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QInputDialog>
#include <QAbstractItemView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QSet>
#include <QSignalBlocker>

#include <memory>
#include <utility>

using namespace forkmesh::ui;

namespace forkmesh::notes {

namespace {
constexpr int kNoteIdRole = Qt::UserRole;
constexpr int kNoteStorageRole = Qt::UserRole + 1;
constexpr int kNoteRowTitleRole = Qt::UserRole + 2;
constexpr int kNoteRowStorageRole = Qt::UserRole + 3;
constexpr int kNoteRowVisibilityRole = Qt::UserRole + 4;
constexpr int kNoteRowPublicRole = Qt::UserRole + 5;
constexpr int kNoteRowDetailRole = Qt::UserRole + 6;
constexpr int kNoteRowUnreadRole = Qt::UserRole + 7;
constexpr int kNoteRowPeopleRole = Qt::UserRole + 8;
constexpr int kNoteRowCommentsRole = Qt::UserRole + 9;
constexpr int kNoteRowCloudIdRole = Qt::UserRole + 10;
constexpr int kNoteRowRoleRole = Qt::UserRole + 11;
constexpr int kNoteRowHeaderRole = Qt::UserRole + 12;
constexpr int kShareSuggestionNameRole = Qt::UserRole + 20;
constexpr int kShareSuggestionTypeRole = Qt::UserRole + 21;
constexpr int kNoteAutosaveMs = 1500;
constexpr int kShareSuggestionDebounceMs = 150;
constexpr int kNoteSearchDebounceMs = 250;

constexpr int kNoteRowPadX = 10;
constexpr int kNoteRowPadY = 10;
constexpr int kNoteRowLineGap = 6;
constexpr int kNoteRowRadius = 8;
constexpr int kNoteRowHintWidth = 120;
constexpr int kNoteRowDetailMinWidth = 24;
constexpr int kNoteRowPartialMinWidth = 72;
constexpr int kNotePillPadX = 6;
constexpr int kNotePillPadY = 2;
constexpr int kNotePillGap = 6;
constexpr int kNotePillRadius = 6;
constexpr int kNotePillMinWidth = 28;
constexpr int kNoteRowAvatar = 16;
constexpr int kNoteRowAvatarStep = 12;
constexpr int kNoteRowAvatarGap = 6;
constexpr int kNoteRowDot = 6;
constexpr int kNoteRowCommentIcon = 12;

QString notesFilePath()
{
    const QString directory =
        forkmesh::paths::dataPath(QStringLiteral("notes"));
    QDir().mkpath(directory);
    return directory + QStringLiteral("/notes.json");
}

QJsonArray readLocalNotes()
{
    QFile file(notesFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isArray() ? document.array() : QJsonArray();
}

bool writeLocalNotes(const QJsonArray &notes)
{
    QSaveFile file(notesFilePath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(notes).toJson(QJsonDocument::Indented));
    return file.commit();
}

QString newNoteId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces)
        .remove(QLatin1Char('-')).toLower();
}

QString noteError(const QJsonObject &payload, const QString &fallback)
{
    QString value = payload.value(QStringLiteral("error")).toString();
    if (value.isEmpty())
        return fallback;
    value.replace(QLatin1Char('_'), QLatin1Char(' '));
    return value;
}

int localNoteIndex(const QJsonArray &notes, const QString &id)
{
    for (int i = 0; i < notes.size(); ++i) {
        const QJsonObject note = notes.at(i).toObject();
        if (note.value(QStringLiteral("id")).toString() == id ||
            note.value(QStringLiteral("cloudId")).toString() == id)
            return i;
    }
    return -1;
}

QString noteTimestampLabel(const QJsonObject &note)
{
    const qint64 stamp = static_cast<qint64>(
        note.value(QStringLiteral("updatedAt")).toDouble());
    if (stamp <= 0)
        return QString();
    return QStringLiteral("edited %1").arg(
        QDateTime::fromMSecsSinceEpoch(stamp).toLocalTime()
            .toString(QStringLiteral("MMM d, HH:mm")));
}

struct NoteRowFonts {
    QFont title;
    QFont pill;
    QFont detail;
};

NoteRowFonts noteRowFonts(const QFont &base)
{
    const int px = base.pixelSize() > 0 ? base.pixelSize()
                                        : QFontInfo(base).pixelSize();
    NoteRowFonts fonts{base, base, base};
    fonts.title.setWeight(QFont::Medium);
    fonts.pill.setPixelSize(qMax(9, px - 3));
    fonts.pill.setWeight(QFont::Medium);
    fonts.detail.setPixelSize(qMax(10, px - 2));
    fonts.detail.setWeight(QFont::Normal);
    return fonts;
}

int noteRowMetaHeight(const NoteRowFonts &fonts)
{
    return qMax(QFontMetrics(fonts.pill).height() + 2 * kNotePillPadY,
                QFontMetrics(fonts.detail).height());
}

int paintNotePill(QPainter *painter, const QFont &font, const QString &text,
                  bool accent, int x, const QRect &line)
{
    const int room = line.right() + 1 - x;
    if (text.isEmpty() || room < kNotePillMinWidth)
        return x;
    const QFontMetrics metrics(font);
    const int natural = metrics.horizontalAdvance(text) + 2 * kNotePillPadX;
    const int width = qMin(room, natural);
    const int height = metrics.height() + 2 * kNotePillPadY;
    const QRect pill(x, line.top() + (line.height() - height) / 2, width,
                     height);
    const QColor fill = Theme::color(Theme::Role::AccentSubtle);
    painter->setPen(accent ? fill : Theme::color(Theme::Role::BorderStrong));
    painter->setBrush(accent ? QBrush(fill) : QBrush(Qt::NoBrush));
    painter->drawRoundedRect(QRectF(pill).adjusted(0.5, 0.5, -0.5, -0.5),
                             kNotePillRadius, kNotePillRadius);
    painter->setFont(font);
    painter->setPen(Theme::color(accent ? Theme::Role::AccentFg
                                        : Theme::Role::FgMuted));
    painter->drawText(
        pill, Qt::AlignCenter | Qt::TextSingleLine | Qt::TextDontClip,
        width < natural
            ? metrics.elidedText(text, Qt::ElideRight,
                                 width - 2 * kNotePillPadX)
            : text);
    return x + width + kNotePillGap;
}

QString fittedNoteDetail(const QStringList &parts, const QFontMetrics &metrics,
                         int room)
{
    QString text;
    for (const QString &part : parts) {
        const QString next =
            text.isEmpty() ? part : text + QStringLiteral(" · ") + part;
        if (metrics.horizontalAdvance(next) <= room) {
            text = next;
            continue;
        }
        if (text.isEmpty() || room - metrics.horizontalAdvance(text) >=
                                  kNoteRowPartialMinWidth)
            return metrics.elidedText(next, Qt::ElideRight, room);
        break;
    }
    return text;
}

int noteRowAvatarSlots(const QStringList &people)
{
    const int faces = qMin(int(people.size()), forkmesh::notes::kNoteRowAvatars);
    return faces + (forkmesh::notes::noteShareOverflow(int(people.size())).isEmpty()
                        ? 0 : 1);
}

int noteRowAvatarsWidth(const QStringList &people)
{
    const int slots_ = noteRowAvatarSlots(people);
    return slots_ ? kNoteRowAvatar + (slots_ - 1) * kNoteRowAvatarStep : 0;
}

void paintNoteRowAvatars(QPainter *painter, const QStringList &people,
                         const QRect &line, const QColor &ring)
{
    const int faces = qMin(int(people.size()), forkmesh::notes::kNoteRowAvatars);
    const int count = noteRowAvatarSlots(people);
    const int top = line.top() + (line.height() - kNoteRowAvatar) / 2;
    const int left = line.right() + 1 - noteRowAvatarsWidth(people);
    QFont font = painter->font();
    font.setPixelSize(qMax(7, kNoteRowAvatar * 9 / 16));
    font.setWeight(QFont::DemiBold);
    painter->setFont(font);
    for (int slot = 0; slot < count; ++slot) {
        const QRectF box(left + slot * kNoteRowAvatarStep, top, kNoteRowAvatar,
                         kNoteRowAvatar);
        const bool overflow = slot >= faces;
        const QString entry = overflow ? QString() : people.at(slot);
        const bool organization = entry.startsWith(QLatin1String("o:"));
        const QString name = entry.mid(2);
        QColor fill = Theme::color(Theme::Role::SurfaceActive);
        if (!overflow)
            fill = QColor(QString::fromLatin1(Theme::kSenderPalette[
                forkmesh::notes::noteAvatarIndex(name.toLower(),
                                                 Theme::kSenderPaletteSize)]));
        painter->setPen(QPen(ring, 1.5));
        painter->setBrush(fill);
        if (organization)
            painter->drawRoundedRect(box, 4, 4);
        else
            painter->drawEllipse(box);
        painter->setPen(overflow ? Theme::color(Theme::Role::FgSecondary)
                                 : QColor(Qt::white));
        painter->drawText(
            box, Qt::AlignCenter,
            overflow ? forkmesh::notes::noteShareOverflow(int(people.size()))
                     : forkmesh::notes::noteInitials(name));
    }
}

class NoteRowDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        if (!Theme::isNext())
            return QStyledItemDelegate::sizeHint(option, index);
        const NoteRowFonts fonts = noteRowFonts(option.font);
        return QSize(kNoteRowHintWidth,
                     2 * kNoteRowPadY + QFontMetrics(fonts.title).height() +
                         kNoteRowLineGap + noteRowMetaHeight(fonts));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!Theme::isNext()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        const NoteRowFonts fonts = noteRowFonts(option.font);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool hovered = option.state.testFlag(QStyle::State_MouseOver);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        if (selected || hovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(selected
                                               ? Theme::Role::SurfaceActive
                                               : Theme::Role::SurfaceRaised));
            painter->drawRoundedRect(QRectF(option.rect), kNoteRowRadius,
                                     kNoteRowRadius);
        }

        const QRect content = option.rect.adjusted(
            kNoteRowPadX, kNoteRowPadY, -kNoteRowPadX, -kNoteRowPadY);
        QString title = index.data(kNoteRowTitleRole).toString().simplified();
        const bool untitled = title.isEmpty();
        if (untitled)
            title = QStringLiteral("Untitled note");
        const bool unread = index.data(kNoteRowUnreadRole).toBool();
        const QStringList people = index.data(kNoteRowPeopleRole).toStringList();
        QFont titleFont = fonts.title;
        if (unread)
            titleFont.setWeight(QFont::Bold);
        const QFontMetrics titleMetrics(titleFont);
        const QRect titleLine(content.left(), content.top(), content.width(),
                              titleMetrics.height());
        const int avatars = noteRowAvatarsWidth(people);
        if (avatars)
            paintNoteRowAvatars(
                painter, people, titleLine,
                selected  ? Theme::color(Theme::Role::SurfaceActive)
                : hovered ? Theme::color(Theme::Role::SurfaceRaised)
                          : option.palette.color(QPalette::Base));
        if (unread) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(Theme::color(Theme::Role::Accent));
            painter->drawEllipse(QRectF(
                option.rect.left() + (kNoteRowPadX - kNoteRowDot) / 2.0,
                titleLine.center().y() - kNoteRowDot / 2.0 + 0.5, kNoteRowDot,
                kNoteRowDot));
        }
        const QRect titleRect(
            titleLine.left(), titleLine.top(),
            qMax(0, titleLine.width() - (avatars ? avatars + kNoteRowAvatarGap : 0)),
            titleLine.height());
        painter->setFont(titleFont);
        painter->setPen(Theme::color(untitled ? Theme::Role::FgMuted
                                              : Theme::Role::Fg));
        painter->drawText(
            titleRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
            titleMetrics.elidedText(title, Qt::ElideRight, titleRect.width()));

        const QRect meta(content.left(),
                         titleRect.bottom() + 1 + kNoteRowLineGap,
                         content.width(), noteRowMetaHeight(fonts));
        int x = paintNotePill(painter, fonts.pill,
                              index.data(kNoteRowStorageRole).toString(),
                              false, meta.left(), meta);
        x = paintNotePill(painter, fonts.pill,
                          index.data(kNoteRowVisibilityRole).toString(),
                          index.data(kNoteRowPublicRole).toBool(), x, meta);
        const int comments = index.data(kNoteRowCommentsRole).toInt();
        if (comments > 0) {
            const QString number = QString::number(comments);
            const QFontMetrics detailMetrics(fonts.detail);
            const int width = kNoteRowCommentIcon + 3 +
                              detailMetrics.horizontalAdvance(number);
            if (meta.right() + 1 - x >= width + kNoteRowDetailMinWidth) {
                const QColor muted = Theme::color(Theme::Role::FgMuted);
                painter->drawPixmap(
                    x, meta.top() + (meta.height() - kNoteRowCommentIcon) / 2,
                    themedOcticon(QStringLiteral("comment"), muted,
                                  kNoteRowCommentIcon)
                        .pixmap(kNoteRowCommentIcon, kNoteRowCommentIcon));
                painter->setFont(fonts.detail);
                painter->setPen(muted);
                painter->drawText(
                    QRect(x + kNoteRowCommentIcon + 3, meta.top(),
                          width - kNoteRowCommentIcon - 3, meta.height()),
                    Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                    number);
                x += width + kNotePillGap;
            }
        }
        const QStringList detail =
            index.data(kNoteRowDetailRole).toStringList();
        const int room = meta.right() + 1 - x;
        if (!detail.isEmpty() && room >= kNoteRowDetailMinWidth) {
            painter->setFont(fonts.detail);
            painter->setPen(Theme::color(Theme::Role::FgMuted));
            painter->drawText(
                QRect(x, meta.top(), room, meta.height()),
                Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                fittedNoteDetail(detail, QFontMetrics(fonts.detail), room));
        }
        painter->restore();
    }
};

QListWidgetItem *noteListItem(const QJsonObject &note,
                              const QJsonObject &cloud, const QString &mode,
                              bool synced, const QString &me)
{
    const bool local = mode == QLatin1String("local");
    const QJsonObject &status = cloud.isEmpty() ? note : cloud;
    const qint64 stamp =
        qint64(status.value(QStringLiteral("updatedAt")).toDouble());
    const QString editor =
        local ? QString() : cloud.value(QStringLiteral("lastEditedBy")).toString();
    const QStringList lines =
        NoteListEntry::lines(note, cloud, mode, synced, noteTimestampLabel(status));
    const bool unread = forkmesh::notes::noteUnread(cloud, mode);
    const int comments =
        local ? 0 : cloud.value(QStringLiteral("openComments")).toInt();
    const QString storage = forkmesh::notes::noteStorageBadge(mode);
    const QString visibility =
        local ? QStringLiteral("Private")
              : forkmesh::notes::noteVisibilityBadge(status, synced);
    const QString edited = forkmesh::notes::noteLastEditLabel(
        stamp, editor, me, QDateTime::currentDateTime());

    QStringList text{(unread ? QStringLiteral("● ") : QString()) + lines.value(0),
                     QStringLiteral("%1 · %2").arg(storage, visibility)};
    if (!edited.isEmpty())
        text << edited;
    auto *item = new QListWidgetItem(text.join(QLatin1Char('\n')));
    item->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

    QStringList people;
    if (!local) {
        for (const forkmesh::notes::NoteSharePerson &person :
             forkmesh::notes::noteSharePeople(
                 cloud.value(QStringLiteral("shares")).toArray()))
            people << (person.organization ? QStringLiteral("o:")
                                           : QStringLiteral("u:")) +
                          person.name;
    }
    const QString role = local ? QStringLiteral("owner")
                               : status.value(QStringLiteral("role"))
                                     .toString(QStringLiteral("owner"));
    const QStringList header = forkmesh::notes::noteAccessHeader(
        role, local ? QJsonArray() : cloud.value(QStringLiteral("shares")).toArray(),
        visibility, storage, stamp, editor, me);
    item->setToolTip(header.join(QLatin1Char('\n')));

    QStringList detail;
    if (!edited.isEmpty())
        detail << edited;
    if (!local && !synced)
        detail << QStringLiteral("not in the cloud yet");
    item->setData(kNoteRowTitleRole, lines.value(0));
    item->setData(kNoteRowStorageRole, storage);
    item->setData(kNoteRowVisibilityRole, visibility);
    item->setData(kNoteRowPublicRole, visibility == QLatin1String("Public"));
    item->setData(kNoteRowDetailRole, detail);
    item->setData(kNoteRowUnreadRole, unread);
    item->setData(kNoteRowPeopleRole, people);
    item->setData(kNoteRowCommentsRole, comments);
    item->setData(kNoteRowRoleRole, role);
    item->setData(kNoteRowHeaderRole, header);
    item->setData(kNoteRowCloudIdRole,
                  local || !synced
                      ? QString()
                      : note.value(QStringLiteral("cloudId"))
                            .toString(note.value(QStringLiteral("id")).toString()));
    return item;
}

QJsonObject localSnapshot(QJsonObject note, const QString &title,
                          const QString &markdown, const QString &mode)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int version = qMax(0, note.value(QStringLiteral("version")).toInt()) + 1;
    QJsonArray versions = note.value(QStringLiteral("versions")).toArray();
    versions.prepend(QJsonObject{
        {QStringLiteral("version"), version},
        {QStringLiteral("createdAt"), static_cast<double>(now)},
        {QStringLiteral("title"), title},
        {QStringLiteral("markdown"), markdown},
    });
    while (versions.size() > 100)
        versions.removeLast();
    note.insert(QStringLiteral("id"),
                note.value(QStringLiteral("id")).toString(newNoteId()));
    note.insert(QStringLiteral("title"), title);
    note.insert(QStringLiteral("markdown"), markdown);
    note.insert(QStringLiteral("storage"), mode);
    note.insert(QStringLiteral("role"), QStringLiteral("owner"));
    note.insert(QStringLiteral("version"), version);
    note.insert(QStringLiteral("updatedAt"), static_cast<double>(now));
    note.insert(QStringLiteral("versions"), versions);
    return note;
}
}

NotesPage::NotesPage(const forkmesh::market::NativePackageHost &host,
                     QWidget *parent)
    : forkmesh::market::NativePackageWidget(parent), m_host(host),
      m_searchIndexChanged(host.searchIndexChanged),
      m_presenceClientId(newNoteId()), m_presencePaused(true)
{
    buildNotesSection();
}

NotesPage::~NotesPage()
{
    m_presencePaused = true;
    leaveNotePresence();
}

void NotesPage::showEvent(QShowEvent *event)
{
    forkmesh::market::NativePackageWidget::showEvent(event);
    if (event->spontaneous() || !m_presencePaused)
        return;
    m_presencePaused = false;
    schedulePresencePoll(0);
}

void NotesPage::hideEvent(QHideEvent *event)
{
    forkmesh::market::NativePackageWidget::hideEvent(event);
    if (event->spontaneous() || m_presencePaused)
        return;
    m_presencePaused = true;
    if (m_presenceTimer)
        m_presenceTimer->stop();
    leaveNotePresence();
    hideNotePresence();
}

void NotesPage::refresh()
{
    refreshNotes();
}

QUrl NotesPage::catalogApiUrl() const
{
    QUrl url = m_host.accountApiUrl
        ? m_host.accountApiUrl(QStringLiteral("/api/notes")) : QUrl();
    url.setPath(QString());
    url.setQuery(QString());
    return url;
}

void NotesPage::flashMessage(const QString &text, bool error, const QString &,
                             const QString &kind)
{
    if (error && kind.isEmpty() && m_host.reportError)
        m_host.reportError(text);
    else if (m_host.flashMessage)
        m_host.flashMessage(text, error);
}

void NotesPage::openReferenceLink(const QString &href)
{
    if (m_host.openReference)
        m_host.openReference(href);
}

void NotesPage::searchIndexChanged()
{
    if (m_searchIndexChanged)
        m_searchIndexChanged();
}

QList<forkmesh::market::NativePackageSearchHit>
NotesPage::searchHits(const QString &rawQuery, int limit) const
{
    QList<forkmesh::market::NativePackageSearchHit> hits;
    const QString query = rawQuery.trimmed();
    if (query.isEmpty())
        return hits;
    const auto text = [](const QJsonObject &note, const char *field) {
        return note.value(QLatin1String(field)).toString().trimmed();
    };
    QSet<QString> mirrored;
    for (const QJsonValue &value : m_localNotes)
        mirrored.insert(text(value.toObject(), "cloudId"));
    for (const bool local : {true, false}) {
        for (const QJsonValue &value : local ? m_localNotes : m_cloudNotes) {
            if (hits.size() >= limit)
                return hits;
            const QJsonObject note = value.toObject();
            const QString id = text(note, "id");
            if (id.isEmpty() || (!local && mirrored.contains(id)))
                continue;
            const bool inTitle =
                note.value(QStringLiteral("title")).toString().contains(
                    query, Qt::CaseInsensitive);
            const QString markdown = note.value(QStringLiteral("markdown")).toString();
            const int at = inTitle ? -1 : int(markdown.indexOf(query, 0, Qt::CaseInsensitive));
            if (!inTitle && at < 0)
                continue;
            QString title = text(note, "title");
            if (title.isEmpty())
                title = QStringLiteral("Untitled note");
            QString detail;
            if (!inTitle) {
                const int from = qMax(0, at - 30);
                detail = markdown.mid(from, int(query.size()) + 60).simplified();
                if (from > 0)
                    detail.prepend(QString::fromUtf8("\xE2\x80\xA6"));
            }
            hits.append({(local ? QStringLiteral("local:") : QStringLiteral("cloud:")) + id,
                         title, detail});
        }
    }
    return hits;
}

void NotesPage::activateSearchHit(const QString &hitId)
{
    const int colon = hitId.indexOf(QLatin1Char(':'));
    if (colon > 0)
        openNoteFromSearch(hitId.mid(colon + 1), hitId.left(colon));
}

bool NotesPage::acceptsHostCommand(const QString &command) const
{
    return command == QLatin1String("note.add") ||
           command == QLatin1String("note.openLink") ||
           command == QLatin1String("note.openSystemLink");
}

bool NotesPage::hostCommand(const QString &command, const QJsonObject &args)
{
    if (command == QLatin1String("note.add")) {
        QStringList attachments;
        for (const QJsonValue &value : args.value(QStringLiteral("attachments")).toArray())
            attachments << value.toString();
        return takeQuickAddNote(args.value(QStringLiteral("text")).toString(),
                                attachments);
    }
    if (command == QLatin1String("note.openLink"))
        return openNoteLink(args.value(QStringLiteral("href")).toString());
    if (command == QLatin1String("note.openSystemLink")) {
        openNoteFromSystemLink(args.value(QStringLiteral("target")).toString());
        return true;
    }
    return false;
}

void NotesPage::buildNotesSection()
{
    QWidget *page = this;
    page->setObjectName(QStringLiteral("notesSection"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(20, 18, 20, 20);
    outer->setSpacing(10);

    auto *heading = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Notes"));
    title->setObjectName(QStringLiteral("pageTitle"));
    heading->addWidget(title);
    m_notesStatus = new QLabel(QStringLiteral(
        "Local notes never leave this computer. Cloud notes use your account, "
        "and publishing one puts it on the web."));
    m_notesStatus->setObjectName(QStringLiteral("mutedLabel"));
    m_notesStatus->setWordWrap(true);
    QSizePolicy statusPolicy = m_notesStatus->sizePolicy();
    statusPolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    m_notesStatus->setSizePolicy(statusPolicy);
    heading->addStretch();
    auto *refresh = new QPushButton(QStringLiteral("Refresh"));
    refresh->setObjectName(QStringLiteral("ghostButton"));
    refresh->setCursor(Qt::PointingHandCursor);
    refresh->setToolTip(QStringLiteral(
        "Reload notes, including versions a collaborator saved"));
    setOcticon(refresh, "sync", 14);
    connect(refresh, &QPushButton::clicked, this, [this] {
        if (m_noteDirty) {
            m_notesStatus->setText(QStringLiteral(
                "Save this note before refreshing, or the reload would "
                "replace your unsaved edits."));
            return;
        }
        m_cloudNotes = QJsonArray();
        refreshNotes();
    });
    heading->addWidget(refresh);
    auto *versions = new QPushButton(QStringLiteral("Versions"));
    versions->setObjectName(QStringLiteral("ghostButton"));
    setOcticon(versions, "history", 14);
    connect(versions, &QPushButton::clicked, this,
            &NotesPage::showNoteVersions);
    heading->addWidget(versions);
    auto *share = new QPushButton(QStringLiteral("Share"));
    share->setObjectName(QStringLiteral("ghostButton"));
    setOcticon(share, "people", 14);
    connect(share, &QPushButton::clicked, this, &NotesPage::shareNote);
    heading->addWidget(share);
    auto *attach = new QPushButton(QStringLiteral("Links"));
    attach->setObjectName(QStringLiteral("ghostButton"));
    setOcticon(attach, "link", 14);
    attach->setToolTip(QStringLiteral(
        "Issues, pull requests and discussions linked to this note"));
    connect(attach, &QPushButton::clicked, this, &NotesPage::showNoteLinks);
    heading->addWidget(attach);
    m_commentsButton = new QPushButton(commentsButtonLabel(0));
    m_commentsButton->setObjectName(QStringLiteral("ghostButton"));
    m_commentsButton->setProperty("noteCommentsButton", true);
    m_commentsButton->setCursor(Qt::PointingHandCursor);
    m_commentsButton->setCheckable(true);
    m_commentsButton->setToolTip(QStringLiteral(
        "Comments on this note; the number counts open threads"));
    setOcticon(m_commentsButton, "comment", 14);
    m_commentsButton->hide();
    connect(m_commentsButton, &QPushButton::clicked, this, [this] {
        setCommentsPanelOpen(m_commentsPanel && !m_commentsPanel->isVisible());
    });
    heading->addWidget(m_commentsButton);
    auto *convert = new QPushButton(QStringLiteral("Convert"));
    convert->setObjectName(QStringLiteral("ghostButton"));
    convert->setCursor(Qt::PointingHandCursor);
    setOcticon(convert, "issue-opened", 14);
    convert->setToolTip(QStringLiteral(
        "Open a task or an issue prefilled with this note. The note itself "
        "stays here, unchanged."));
    auto *convertMenu = new QMenu(convert);
    convertMenu->addAction(QStringLiteral("To an organization task…"), this,
                           &NotesPage::convertNoteToTask);
    convertMenu->addAction(QStringLiteral("To a repository issue…"), this,
                           &NotesPage::convertNoteToIssue);
    convert->setMenu(convertMenu);
    heading->addWidget(convert);
    auto *more = new QPushButton;
    more->setObjectName(QStringLiteral("ghostButton"));
    more->setCursor(Qt::PointingHandCursor);
    more->setToolTip(QStringLiteral("More actions for this note"));
    setOcticon(more, "kebab-horizontal", 14);
    auto *moreMenu = new QMenu(more);
    moreMenu->setObjectName(QStringLiteral("noteMoreMenu"));
    QAction *copyLink = moreMenu->addAction(QStringLiteral("Copy link"), this,
                                            &NotesPage::copyNoteLink);
    QAction *copyPublic = moreMenu->addAction(
        QStringLiteral("Copy public link"), this,
        &NotesPage::copyNotePublicLink);
    QAction *openPublic =
        moreMenu->addAction(QStringLiteral("Open published page"), this, [this] {
            const QString address = selectedNotePublicUrl();
            if (!address.isEmpty())
                QDesktopServices::openUrl(QUrl(address));
        });
    moreMenu->addSeparator();
    moreMenu->addAction(QStringLiteral("Export Markdown…"), this,
                        &NotesPage::exportNote);
    connect(moreMenu, &QMenu::aboutToShow, this,
            [this, copyLink, copyPublic, openPublic] {
        const bool cloud = !selectedNoteCloudId().isEmpty();
        const bool published = !selectedNotePublicUrl().isEmpty();
        copyLink->setEnabled(cloud);
        copyPublic->setEnabled(published);
        openPublic->setEnabled(published);
    });
    more->setMenu(moreMenu);
    heading->addWidget(more);
    auto *remove = new QPushButton(QStringLiteral("Delete"));
    remove->setObjectName(QStringLiteral("dangerButton"));
    setOcticon(remove, "trash", 14);
    connect(remove, &QPushButton::clicked, this, &NotesPage::deleteNote);
    heading->addWidget(remove);
    auto *add = new QPushButton(QStringLiteral("New note"));
    add->setObjectName(QStringLiteral("primaryButton"));
    setOcticon(add, "plus", 14);
    connect(add, &QPushButton::clicked, this, &NotesPage::createNote);
    heading->addWidget(add);
    auto *header = new QVBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(2);
    header->addLayout(heading);
    header->addWidget(m_notesStatus);
    outer->addLayout(header);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
    m_noteSplitter = splitter;
    auto *listColumn = new QWidget;
    auto *listLayout = new QVBoxLayout(listColumn);
    listLayout->setContentsMargins(0, 0, 0, 0);
    listLayout->setSpacing(6);
    m_noteSearch = new QLineEdit;
    m_noteSearch->setObjectName(QStringLiteral("noteSearch"));
    m_noteSearch->setPlaceholderText(QStringLiteral("Search notes"));
    m_noteSearch->setClearButtonEnabled(true);
    m_noteSearch->setToolTip(QStringLiteral(
        "Matches note titles, and the text of notes this computer has read"));
    m_noteSearchTimer = new QTimer(page);
    m_noteSearchTimer->setSingleShot(true);
    m_noteSearchTimer->setInterval(kNoteSearchDebounceMs);
    connect(m_noteSearchTimer, &QTimer::timeout, this,
            &NotesPage::searchCloudNotes);
    connect(m_noteSearch, &QLineEdit::textChanged, this, [this] {
        renderNotesList(m_selectedNote.value(QStringLiteral("id")).toString(),
                        false);
        m_noteSearchTimer->start();
    });
    listLayout->addWidget(m_noteSearch);
    auto *filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(0, 0, 0, 0);
    filterRow->setSpacing(2);
    auto *filterGroup = new QButtonGroup(listColumn);
    filterGroup->setExclusive(true);
    m_noteFilterButtons.clear();
    for (const QString &filter : forkmesh::notes::noteFilters()) {
        auto *button =
            new QPushButton(forkmesh::notes::noteFilterLabel(filter, 0));
        button->setObjectName(QStringLiteral("repoTab"));
        button->setProperty("noteFilter", filter);
        button->setCheckable(true);
        button->setChecked(filter == m_noteFilter);
        button->setCursor(Qt::PointingHandCursor);
        filterGroup->addButton(button);
        connect(button, &QPushButton::clicked, this, [this, filter] {
            m_noteFilter = filter;
            renderNotesList(
                m_selectedNote.value(QStringLiteral("id")).toString(), false);
        });
        m_noteFilterButtons << button;
        filterRow->addWidget(button);
    }
    filterRow->addStretch();
    listLayout->addLayout(filterRow);
    m_notesList = new QListWidget;
    m_notesList->setObjectName(QStringLiteral("notesList"));
    m_notesList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_notesList->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_notesList->setSpacing(2);
    m_notesList->setAlternatingRowColors(false);
    m_notesList->setUniformItemSizes(false);
    m_notesList->setMinimumWidth(220);
    m_notesList->setItemDelegate(new NoteRowDelegate(m_notesList));
    m_notesList->viewport()->setAttribute(Qt::WA_Hover, true);
    connect(m_notesList, &QListWidget::itemSelectionChanged, this, [this] {
        QListWidgetItem *item = m_notesList->currentItem();
        if (item && !item->data(kNoteIdRole).toString().isEmpty())
            selectNote(item->data(kNoteIdRole).toString(),
                       item->data(kNoteStorageRole).toString());
    });
    m_notesList->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_notesList, &QWidget::customContextMenuRequested, this,
            &NotesPage::showNoteRowMenu);
    listLayout->addWidget(m_notesList, 1);
    splitter->addWidget(listColumn);

    auto *editorHost = new QWidget;
    auto *editorLayout = new QVBoxLayout(editorHost);
    editorLayout->setContentsMargins(12, 0, 0, 0);
    auto *toolbar = new QHBoxLayout;
    m_noteTitle = new QLineEdit;
    m_noteTitle->setObjectName(QStringLiteral("noteTitleEdit"));
    m_noteTitle->setPlaceholderText(QStringLiteral("Untitled note"));
    m_noteTitle->setMaxLength(200);
    toolbar->addWidget(m_noteTitle, 1);
    m_presenceBox = new QWidget;
    m_presenceBox->setObjectName(QStringLiteral("notePresence"));
    auto *presenceLayout = new QHBoxLayout(m_presenceBox);
    presenceLayout->setContentsMargins(0, 0, 0, 0);
    presenceLayout->setSpacing(6);
    m_presenceFaces = new NotePresenceFaces;
    m_presenceFaces->picture = m_host.accountPicture;
    m_presenceFaces->openProfile = m_host.openUserProfile;
    presenceLayout->addWidget(m_presenceFaces);
    m_presenceLine = new QLabel;
    m_presenceLine->setObjectName(QStringLiteral("mutedLabel"));
    m_presenceLine->setProperty("notePresenceLine", true);
    presenceLayout->addWidget(m_presenceLine);
    m_presenceBox->hide();
    toolbar->addWidget(m_presenceBox);
    m_noteStorageMode = new QComboBox;
    m_noteStorageMode->setObjectName(QStringLiteral("noteStorageMode"));
    m_noteStorageMode->addItem(QStringLiteral("Local only"),
                               QStringLiteral("local"));
    m_noteStorageMode->addItem(QStringLiteral("Cloud only"),
                               QStringLiteral("cloud"));
    m_noteStorageMode->addItem(QStringLiteral("Local + cloud"),
                               QStringLiteral("both"));
    m_noteStorageMode->setToolTip(QStringLiteral(
        "Choose exactly where this note is saved. Local-only notes never contact the relay."));
    toolbar->addWidget(m_noteStorageMode);
    m_notePublic = new QCheckBox(QStringLiteral("Publish publicly"));
    m_notePublic->setToolTip(QStringLiteral(
        "Publish this note at a public web address. Needs a cloud storage "
        "mode, because a local-only note never reaches the web."));
    toolbar->addWidget(m_notePublic);
    m_notePublicLink = new QLabel;
    m_notePublicLink->setObjectName(QStringLiteral("mutedLabel"));
    m_notePublicLink->setTextFormat(Qt::RichText);
    m_notePublicLink->setOpenExternalLinks(true);
    m_notePublicLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_notePublicLink->hide();
    toolbar->addWidget(m_notePublicLink);
    m_notePublicCopy = new QPushButton(QStringLiteral("Copy public link"));
    m_notePublicCopy->setObjectName(QStringLiteral("ghostButton"));
    m_notePublicCopy->setCursor(Qt::PointingHandCursor);
    setOcticon(m_notePublicCopy, "copy", 14);
    m_notePublicCopy->hide();
    connect(m_notePublicCopy, &QPushButton::clicked, this,
            &NotesPage::copyNotePublicLink);
    toolbar->addWidget(m_notePublicCopy);
    m_commentButton = new QPushButton(QStringLiteral("Comment"));
    m_commentButton->setObjectName(QStringLiteral("ghostButton"));
    m_commentButton->setProperty("noteCommentSelection", true);
    m_commentButton->setCursor(Qt::PointingHandCursor);
    m_commentButton->setToolTip(QStringLiteral(
        "Comment on the selected text. Select a passage first."));
    setOcticon(m_commentButton, "comment", 14);
    m_commentButton->hide();
    connect(m_commentButton, &QPushButton::clicked, this,
            &NotesPage::startNoteComment);
    toolbar->addWidget(m_commentButton);
    m_noteSaveState = new QLabel;
    m_noteSaveState->setObjectName(QStringLiteral("mutedLabel"));
    m_noteSaveState->setProperty("noteSaveState", true);
    toolbar->addWidget(m_noteSaveState);
    auto *save = new QPushButton(QStringLiteral("Save"));
    save->setObjectName(QStringLiteral("primaryButton"));
    setOcticon(save, "check-circle", 14);
    connect(save, &QPushButton::clicked, this, &NotesPage::saveNote);
    toolbar->addWidget(save);
    editorLayout->addLayout(toolbar);
    m_noteConflictBar = new QWidget;
    m_noteConflictBar->setObjectName(QStringLiteral("noteConflictBar"));
    auto *conflictLayout = new QHBoxLayout(m_noteConflictBar);
    conflictLayout->setContentsMargins(0, 4, 0, 4);
    auto *conflictText = new QLabel(QStringLiteral(
        "Someone else saved this note while you were editing. Keep your "
        "version, or take theirs and drop your changes?"));
    conflictText->setWordWrap(true);
    conflictLayout->addWidget(conflictText, 1);
    auto *keepMine = new QPushButton(QStringLiteral("Keep mine"));
    keepMine->setObjectName(QStringLiteral("ghostButton"));
    connect(keepMine, &QPushButton::clicked, this,
            &NotesPage::keepMyNoteVersion);
    conflictLayout->addWidget(keepMine);
    auto *takeTheirs = new QPushButton(QStringLiteral("Take theirs"));
    takeTheirs->setObjectName(QStringLiteral("ghostButton"));
    connect(takeTheirs, &QPushButton::clicked, this,
            &NotesPage::takeTheirNoteVersion);
    conflictLayout->addWidget(takeTheirs);
    m_noteConflictBar->hide();
    editorLayout->addWidget(m_noteConflictBar);
    m_noteEditor = new MarkdownEditor;
    m_noteEditor->setPlaceholderText(QStringLiteral(
        "Write Markdown. Preview it with the Preview tab."));
    editorLayout->addWidget(m_noteEditor, 1);
    m_noteLinkedItems = new QLabel;
    m_noteLinkedItems->setObjectName(QStringLiteral("mutedLabel"));
    m_noteLinkedItems->setTextFormat(Qt::RichText);
    m_noteLinkedItems->setWordWrap(true);
    m_noteLinkedItems->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_noteLinkedItems->hide();
    connect(m_noteLinkedItems, &QLabel::linkActivated, this,
            [this](const QString &href) { openReferenceLink(href); });
    editorLayout->addWidget(m_noteLinkedItems);
    splitter->addWidget(editorHost);
    NoteCommentsPanel::Hooks commentHooks;
    commentHooks.request = [this](const QByteArray &method, const QString &path,
                                  const QJsonObject &body,
                                  NoteCommentsPanel::Reply reply) {
        requestNotes(method, path, body, std::move(reply));
    };
    commentHooks.markdown = [this] { return m_noteEditor->markdown(); };
    commentHooks.me = [this] {
        return m_host.accountName ? m_host.accountName() : QString();
    };
    commentHooks.noteOwner = [this] {
        return selectedNoteRole() == QLatin1String("owner");
    };
    commentHooks.showPassage = [this](const CommentThread *thread) {
        showCommentPassage(thread);
    };
    commentHooks.countChanged = [this] { updateCommentControls(); };
    commentHooks.newComment = [this] { startNoteComment(); };
    commentHooks.closed = [this] { setCommentsPanelOpen(false); };
    m_commentsPanel = new NoteCommentsPanel(std::move(commentHooks));
    m_commentsPanel->hide();
    splitter->addWidget(m_commentsPanel);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 4);
    splitter->setStretchFactor(2, 0);
    outer->addWidget(splitter, 1);

    m_presenceTimer = new QTimer(page);
    m_presenceTimer->setSingleShot(true);
    connect(m_presenceTimer, &QTimer::timeout, this,
            &NotesPage::pollNotePresence);
    QPlainTextEdit *source = m_noteEditor->sourceEdit();
    connect(source, &QPlainTextEdit::textChanged, this, [this, source] {
        if (source->hasFocus())
            noteTyped();
        if (m_commentsPanel)
            m_commentsPanel->markdownChanged();
    });
    connect(m_noteTitle, &QLineEdit::textEdited, this, [this] { noteTyped(); });
    connect(source, &QPlainTextEdit::selectionChanged, this,
            [this] { updateCommentControls(); });
    source->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(source, &QWidget::customContextMenuRequested, this,
            [this, source](const QPoint &pos) { showNoteContextMenu(source, pos); });
    if (auto *preview = m_noteEditor->findChild<QTextBrowser *>(
            QStringLiteral("markdownPreview"))) {
        connect(preview, &QTextEdit::selectionChanged, this,
                [this] { updateCommentControls(); });
        preview->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(preview, &QWidget::customContextMenuRequested, this,
                [this, preview](const QPoint &pos) {
            showNoteContextMenu(preview, pos);
        });
    }
    if (auto *stack = m_noteEditor->findChild<QStackedWidget *>(
            QStringLiteral("markdownStack")))
        connect(stack, &QStackedWidget::currentChanged, this,
                [this] { updateCommentControls(); });

    m_noteAutosaveTimer = new QTimer(page);
    m_noteAutosaveTimer->setSingleShot(true);
    m_noteAutosaveTimer->setInterval(kNoteAutosaveMs);
    connect(m_noteAutosaveTimer, &QTimer::timeout, this,
            &NotesPage::autosaveNote);

    connect(m_noteTitle, &QLineEdit::textChanged, this,
            [this] { scheduleNoteAutosave(); });
    connect(m_noteEditor->sourceEdit(), &QPlainTextEdit::textChanged, this,
            [this] { scheduleNoteAutosave(); });
    connect(m_noteStorageMode, &QComboBox::currentIndexChanged, this, [this] {
        m_noteDirty = true;
        updateNotePublishState();
        setNoteSaveState(QStringLiteral("Unsaved changes"));
    });
    connect(m_notePublic, &QCheckBox::toggled, this, [this] {
        updateNotePublishState();
        scheduleNoteAutosave();
    });
}

QString NotesPage::selectedNoteCloudId() const
{
    return m_selectedNote.value(QStringLiteral("cloudId"))
        .toString(m_selectedNoteStorage == QLatin1String("cloud")
                      ? m_selectedNote.value(QStringLiteral("id")).toString()
                      : QString());
}

QString NotesPage::selectedNotePublicUrl() const
{
    const QString cloudId = selectedNoteCloudId();
    if (cloudId.isEmpty() ||
        m_selectedNote.value(QStringLiteral("visibility")).toString() !=
            QLatin1String("public"))
        return QString();
    return forkmesh::notes::notePublicLink(catalogApiUrl(), cloudId);
}

QString NotesPage::selectedNoteAccessLink() const
{
    return forkmesh::notes::noteAccessLink(catalogApiUrl(),
                                           selectedNoteCloudId());
}

QString NotesPage::selectedNoteRole() const
{
    return m_selectedNote.value(QStringLiteral("role"))
        .toString(QStringLiteral("owner"));
}

QStringList NotesPage::noteRelayHosts() const
{
    const QString host = catalogApiUrl().host().toLower();
    return host.isEmpty() ? QStringList() : QStringList{host};
}

void NotesPage::updateNotePublishState()
{
    if (!m_notePublic || !m_noteStorageMode)
        return;
    const QString mode = m_noteStorageMode->currentData().toString();
    const QString role = selectedNoteRole();
    const bool publishable = mode != QLatin1String("local") &&
                             role == QLatin1String("owner");
    m_notePublic->setEnabled(publishable);
    if (!publishable)
        m_notePublic->setChecked(false);
    if (!m_notePublicLink)
        return;
    const QString address = selectedNotePublicUrl();
    if (m_notePublicCopy)
        m_notePublicCopy->setVisible(!address.isEmpty());
    if (address.isEmpty()) {
        m_notePublicLink->clear();
        m_notePublicLink->hide();
        if (m_notePublic->isChecked() && m_notePublic->isEnabled()) {
            m_notePublicLink->setText(QStringLiteral(
                "<span>Save to publish</span>"));
            m_notePublicLink->show();
        }
        return;
    }
    m_notePublicLink->setText(
        QStringLiteral("<a href=\"%1\">Live on the web</a>").arg(address));
    m_notePublicLink->setToolTip(address);
    m_notePublicLink->show();
}

void NotesPage::requestNotes(const QByteArray &method, const QString &path,
                             const QJsonObject &body,
                             NoteReplyHandler handler, QObject *context)
{
    const qsizetype mark = path.indexOf(QLatin1Char('?'));
    const QString cleanPath = mark < 0 ? path : path.left(mark);
    QUrlQuery query(mark < 0 ? QString() : path.mid(mark + 1));
    QUrl url = m_host.accountApiUrl ? m_host.accountApiUrl(cleanPath) : QUrl();
    url.setQuery(query);
    const QString token =
        m_host.accountSessionToken && url.isValid() ? m_host.accountSessionToken(url)
                                                    : QString();
    const QByteArray encoded = body.isEmpty()
        ? QByteArray()
        : QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (!m_host.network || !url.isValid()) {
        handler(false, {}, QStringLiteral(
            "Cloud notes are unavailable; local notes remain available."));
        return;
    }
    if (token.isEmpty()) {
        QString node, ts, sig;
        if (!m_host.signProof ||
            !m_host.signProof(
                forkmesh::notes::kNotesRequestProof,
                forkmesh::notes::noteRequestResource(method, cleanPath, encoded),
                &node, &ts, &sig)) {
            handler(false, {}, QStringLiteral(
                "Sign in to your ForkMesh account on this device to load "
                "cloud notes; local notes remain available."));
            return;
        }
        query.addQueryItem(QStringLiteral("node"), node);
        query.addQueryItem(QStringLiteral("ts"), ts);
        query.addQueryItem(QStringLiteral("sig"), sig);
        url.setQuery(query);
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    if (!token.isEmpty())
        request.setRawHeader(QByteArrayLiteral("Authorization"),
                             QByteArrayLiteral("Bearer ") + token.toUtf8());
    if (!encoded.isEmpty())
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
    QNetworkReply *reply = method == QByteArrayLiteral("GET")
        ? m_host.network->get(request)
        : method == QByteArrayLiteral("POST")
        ? m_host.network->post(request, encoded)
        : m_host.network->sendCustomRequest(request, method, encoded);
    connect(reply, &QNetworkReply::finished, context ? context : this,
            [reply, handler = std::move(handler)]() mutable {
        const QJsonObject payload =
            QJsonDocument::fromJson(reply->readAll()).object();
        const int status = reply->attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError &&
                        status >= 200 && status < 300;
        const QString error = ok ? QString() : noteError(
            payload, status ? QStringLiteral("HTTP %1").arg(status)
                            : reply->errorString());
        reply->deleteLater();
        handler(ok, payload, error);
    });
}

void NotesPage::refreshNotes()
{
    if (!m_notesList || m_notesLoading)
        return;
    m_localNotes = readLocalNotes();
    m_notesLoading = true;
    requestNotes(QByteArrayLiteral("GET"), QStringLiteral("/api/notes"), {},
                 [this](bool ok, const QJsonObject &payload,
                        const QString &error) {
        m_notesLoading = false;
        m_cloudNotes = ok
            ? payload.value(QStringLiteral("notes")).toArray()
            : QJsonArray();
        m_notesStatus->setText(ok
            ? QStringLiteral("Local and cloud notes refreshed.")
            : QStringLiteral("Local notes loaded. %1").arg(error));
        renderNotesList(m_selectedNote.value(QStringLiteral("id")).toString());
        searchIndexChanged();
    });
}

QJsonObject NotesPage::cloudNoteFor(const QJsonObject &note) const
{
    const QString cloudId = note.value(QStringLiteral("cloudId")).toString();
    if (cloudId.isEmpty())
        return {};
    for (const QJsonValue &value : m_cloudNotes) {
        const QJsonObject cloud = value.toObject();
        if (cloud.value(QStringLiteral("id")).toString() == cloudId)
            return cloud;
    }
    return note.contains(QStringLiteral("shares")) ? note : QJsonObject();
}

void NotesPage::renderNotesList(const QString &selectId, bool reselect)
{
    if (!m_notesList)
        return;
    const QString query = m_noteSearch ? m_noteSearch->text() : QString();
    const QString me = m_host.accountName ? m_host.accountName() : QString();
    QHash<QString, int> counts;
    const auto consider = [this, &counts, &query](
                              QListWidgetItem *item, const QString &role,
                              const QString &visibility, const QString &title,
                              const QString &markdown) {
        if (!forkmesh::notes::noteMatchesSearch(query, title, markdown)) {
            delete item;
            return false;
        }
        for (const QString &filter : forkmesh::notes::noteFilters()) {
            if (forkmesh::notes::noteMatchesFilter(filter, role, visibility))
                ++counts[filter];
        }
        if (!forkmesh::notes::noteMatchesFilter(m_noteFilter, role,
                                                visibility)) {
            delete item;
            return false;
        }
        m_notesList->addItem(item);
        return true;
    };
    m_notesList->blockSignals(true);
    m_notesList->clear();
    QSet<QString> cloudIds;
    for (const QJsonValue &value : m_localNotes) {
        const QJsonObject note = value.toObject();
        const QString cloudId = note.value(QStringLiteral("cloudId")).toString();
        if (!cloudId.isEmpty())
            cloudIds.insert(cloudId);
        const QJsonObject cloud = cloudNoteFor(note);
        QListWidgetItem *item = noteListItem(
            note, cloud,
            note.value(QStringLiteral("storage")).toString(
                QStringLiteral("local")),
            !cloudId.isEmpty(), me);
        item->setData(kNoteIdRole,
                      note.value(QStringLiteral("id")).toString());
        item->setData(kNoteStorageRole, QStringLiteral("local"));
        const QJsonObject &status = cloud.isEmpty() ? note : cloud;
        if (!consider(item,
                      status.value(QStringLiteral("role"))
                          .toString(QStringLiteral("owner")),
                      cloudId.isEmpty()
                          ? QString()
                          : status.value(QStringLiteral("visibility")).toString(),
                      note.value(QStringLiteral("title")).toString(),
                      note.value(QStringLiteral("markdown")).toString()))
            continue;
        if (item->data(kNoteIdRole).toString() == selectId)
            m_notesList->setCurrentItem(item);
    }
    for (const QJsonValue &value : m_cloudNotes) {
        const QJsonObject note = value.toObject();
        const QString id = note.value(QStringLiteral("id")).toString();
        if (cloudIds.contains(id))
            continue;
        QListWidgetItem *item =
            noteListItem(note, note, QStringLiteral("cloud"), true, me);
        item->setData(kNoteIdRole, id);
        item->setData(kNoteStorageRole, QStringLiteral("cloud"));
        QString markdown =
            m_selectedNoteStorage == QLatin1String("cloud") &&
                    m_selectedNote.value(QStringLiteral("id")).toString() == id
                ? m_selectedNote.value(QStringLiteral("markdown")).toString()
                : note.value(QStringLiteral("markdown")).toString();
        if (m_noteSearchCloudIds.contains(id) &&
            m_noteSearchCloudQuery == forkmesh::notes::noteSearchQuery(query))
            markdown = query;
        if (!consider(item,
                      note.value(QStringLiteral("role"))
                          .toString(QStringLiteral("owner")),
                      note.value(QStringLiteral("visibility")).toString(),
                      note.value(QStringLiteral("title")).toString(),
                      markdown))
            continue;
        if (id == selectId)
            m_notesList->setCurrentItem(item);
    }
    for (QPushButton *button : std::as_const(m_noteFilterButtons)) {
        const QString filter = button->property("noteFilter").toString();
        button->setText(
            forkmesh::notes::noteFilterLabel(filter, counts.value(filter)));
        button->setChecked(filter == m_noteFilter);
    }
    if (!m_notesList->count()) {
        auto *empty = new QListWidgetItem(
            forkmesh::notes::noteEmptyListText(m_noteFilter, query));
        empty->setFlags(Qt::NoItemFlags);
        empty->setData(kNoteRowTitleRole, empty->text());
        m_notesList->addItem(empty);
    }
    m_notesList->blockSignals(false);
    if (!reselect)
        return;
    if (!m_notesList->currentItem() && selectId.isEmpty() &&
        m_notesList->count() &&
        !m_notesList->item(0)->data(kNoteIdRole).toString().isEmpty())
        m_notesList->setCurrentRow(0);
    if (m_notesList->currentItem() &&
        !m_notesList->currentItem()->data(kNoteIdRole).toString().isEmpty())
        selectNote(m_notesList->currentItem()->data(kNoteIdRole).toString(),
                   m_notesList->currentItem()->data(kNoteStorageRole).toString());
}

void NotesPage::selectNote(const QString &id, const QString &storage)
{
    if (m_noteDirty && !m_selectedNote.isEmpty() && noteAutosaveApplies() &&
        !m_noteSaveInFlight && m_noteConflict.isEmpty()) {
        autosaveNote();
    } else if (m_noteDirty && !m_selectedNote.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Unsaved note"),
            QStringLiteral("Discard the unsaved changes to this note?"));
        if (answer != QMessageBox::Yes)
            return;
    }
    const bool opening =
        id != m_selectedNote.value(QStringLiteral("id")).toString();
    if (opening)
        clearNoteConflict();
    if (storage == QLatin1String("local")) {
        const int index = localNoteIndex(m_localNotes, id);
        if (index < 0)
            return;
        m_selectedNote = m_localNotes.at(index).toObject();
        m_selectedNoteStorage = QStringLiteral("local");
        m_noteTitle->setText(m_selectedNote.value(QStringLiteral("title")).toString());
        m_noteEditor->setMarkdown(m_selectedNote.value(QStringLiteral("markdown")).toString());
        const QString mode = m_selectedNote.value(QStringLiteral("storage"))
                                 .toString(QStringLiteral("local"));
        m_noteStorageMode->setCurrentIndex(
            qMax(0, m_noteStorageMode->findData(mode)));
        m_notePublic->setChecked(
            m_selectedNote.value(QStringLiteral("visibility")).toString() ==
            QLatin1String("public"));
        updateNotePublishState();
        markNoteClean();
        renderNoteLinkedItems();
        if (opening)
            showOpenedNoteView();
        syncNoteCollab();
        runPendingRowAction();
        return;
    }
    requestNotes(QByteArrayLiteral("GET"),
                 QStringLiteral("/api/notes/%1").arg(id), {},
                 [this, opening](bool ok, const QJsonObject &payload,
                                 const QString &error) {
        if (!ok) {
            m_notesStatus->setText(QStringLiteral("Note unavailable: %1").arg(error));
            flashMessage(QStringLiteral("Could not open that note: %1")
                             .arg(error),
                         true, QString(), forkmesh::kLocalOperationNoticeKind);
            return;
        }
        m_selectedNote = payload.value(QStringLiteral("note")).toObject();
        m_selectedNoteStorage = QStringLiteral("cloud");
        m_noteTitle->setText(m_selectedNote.value(QStringLiteral("title")).toString());
        m_noteEditor->setMarkdown(m_selectedNote.value(QStringLiteral("markdown")).toString());
        m_noteStorageMode->setCurrentIndex(
            m_noteStorageMode->findData(QStringLiteral("cloud")));
        m_notePublic->setChecked(
            m_selectedNote.value(QStringLiteral("visibility")).toString() ==
            QLatin1String("public"));
        updateNotePublishState();
        markNoteClean();
        renderNoteLinkedItems();
        if (opening)
            showOpenedNoteView();
        if (m_notesList && !m_notesList->currentItem()) {
            const QSignalBlocker blocker(m_notesList);
            for (int row = 0; row < m_notesList->count(); ++row) {
                if (m_notesList->item(row)->data(kNoteIdRole).toString() ==
                    m_selectedNote.value(QStringLiteral("id")).toString())
                    m_notesList->setCurrentRow(row);
            }
        }
        syncNoteCollab();
        runPendingRowAction();
    });
}

void NotesPage::showOpenedNoteView()
{
    if (m_noteEditor->markdown().trimmed().isEmpty())
        m_noteEditor->showWriteArea();
    else
        m_noteEditor->showPreviewArea();
}

void NotesPage::openNoteFromSearch(const QString &id, const QString &storage)
{
    if (m_host.showPage)
        m_host.showPage();
    if (!m_notesList || id.isEmpty())
        return;
    for (int row = 0; row < m_notesList->count(); ++row) {
        QListWidgetItem *item = m_notesList->item(row);
        if (item->data(kNoteIdRole).toString() != id ||
            item->data(kNoteStorageRole).toString() != storage)
            continue;
        const QSignalBlocker blocker(m_notesList);
        m_notesList->setCurrentItem(item);
        break;
    }
    selectNote(id, storage);
}

void NotesPage::createNote()
{
    m_selectedNote = QJsonObject{
        {QStringLiteral("id"), newNoteId()},
        {QStringLiteral("title"), QStringLiteral("Untitled note")},
        {QStringLiteral("markdown"), QString()},
        {QStringLiteral("storage"), QStringLiteral("local")},
        {QStringLiteral("version"), 0},
    };
    m_selectedNoteStorage = QStringLiteral("local");
    m_noteTitle->setText(QStringLiteral("Untitled note"));
    m_noteEditor->setMarkdown(QString());
    m_noteEditor->showWriteArea();
    m_noteStorageMode->setCurrentIndex(0);
    m_notePublic->setChecked(false);
    updateNotePublishState();
    clearNoteConflict();
    m_noteAutosaveTimer->stop();
    m_noteDirty = true;
    setNoteSaveState(QStringLiteral("Unsaved changes"));
    renderNoteLinkedItems();
    syncNoteCollab();
    m_noteTitle->selectAll();
    m_noteTitle->setFocus();
}

bool NotesPage::takeQuickAddNote(const QString &text,
                                 const QStringList &attachments)
{
    QString markdown = text.trimmed();
    for (const QString &attachment : attachments) {
        if (!markdown.isEmpty())
            markdown += QStringLiteral("\n\n");
        markdown += QStringLiteral("![%1](%2)")
                        .arg(QFileInfo(attachment).fileName(),
                             QUrl::fromLocalFile(attachment).toString());
    }
    if (markdown.isEmpty()) {
        flashMessage(QStringLiteral("Type something before taking a note."), true,
                     QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    QString title = text.trimmed().section(QLatin1Char('\n'), 0, 0).simplified();
    if (title.isEmpty())
        title = QStringLiteral("Untitled note");
    if (title.size() > 120)
        title = title.left(119).trimmed() + QString::fromUtf8("\xE2\x80\xA6");
    QJsonArray notes = readLocalNotes();
    const QJsonObject note = localSnapshot(
        QJsonObject{{QStringLiteral("id"), newNoteId()}}, title, markdown,
        QStringLiteral("local"));
    notes.prepend(note);
    if (!writeLocalNotes(notes)) {
        flashMessage(QStringLiteral("Could not save the note to %1")
                         .arg(QDir::toNativeSeparators(notesFilePath())),
                     true);
        return false;
    }
    m_localNotes = notes;
    if (m_notesList)
        renderNotesList(m_selectedNote.value(QStringLiteral("id")).toString());
    searchIndexChanged();
    flashMessage(QStringLiteral("Note saved on this computer: %1").arg(title),
                 false, QString(), forkmesh::kLocalOperationNoticeKind);
    return true;
}

void NotesPage::saveNote()
{
    if (m_selectedNote.isEmpty())
        return createNote();
    if (m_noteSaveInFlight) {
        m_noteSaveQueued = true;
        setNoteSaveState(QStringLiteral("Saving…"));
        return;
    }
    if (m_noteAutosaveTimer)
        m_noteAutosaveTimer->stop();
    const QString title = m_noteTitle->text().trimmed().isEmpty()
        ? QStringLiteral("Untitled note") : m_noteTitle->text().trimmed();
    const QString body = m_noteEditor->markdown();
    const QString mode = m_noteStorageMode->currentData().toString();
    const bool keepLocal = mode != QLatin1String("cloud");
    const bool keepCloud = mode != QLatin1String("local");
    auto saveLocal = [this, title, body, mode](QJsonObject note) {
        note = localSnapshot(note, title, body, mode);
        const int index = localNoteIndex(
            m_localNotes, note.value(QStringLiteral("id")).toString());
        if (index >= 0)
            m_localNotes.replace(index, note);
        else
            m_localNotes.prepend(note);
        if (!writeLocalNotes(m_localNotes))
            return false;
        m_selectedNote = note;
        m_selectedNoteStorage = QStringLiteral("local");
        markNoteClean();
        renderNotesList(note.value(QStringLiteral("id")).toString());
        return true;
    };
    auto announce = [this, title, mode, keepLocal, keepCloud](
                        bool savedLocal, bool savedCloud,
                        const QJsonObject &cloud, const QString &cloudError) {
        const QString text = NoteListEntry::savedAlert(
            title, mode, savedLocal, savedCloud, cloud, cloudError);
        m_notesStatus->setText(text);
        m_notesStatus->setToolTip(text);
        const bool missed = (keepLocal && !savedLocal) ||
                            (keepCloud && !savedCloud);
        flashMessage(text, missed, QString(),
                     savedLocal || savedCloud
                         ? forkmesh::kLocalOperationNoticeKind : QString());
    };
    if (!keepCloud) {
        announce(saveLocal(m_selectedNote), false, {}, QString());
        return;
    }
    const QString cloudId = selectedNoteCloudId();
    QJsonObject requestBody{
        {QStringLiteral("title"), title},
        {QStringLiteral("markdown"), body},
    };
    if (cloudId.isEmpty() || selectedNoteRole() == QLatin1String("owner"))
        requestBody.insert(QStringLiteral("visibility"),
                           m_notePublic->isChecked() ? QStringLiteral("public")
                                                     : QStringLiteral("private"));
    QByteArray method = QByteArrayLiteral("POST");
    QString path = QStringLiteral("/api/notes");
    if (!cloudId.isEmpty()) {
        method = QByteArrayLiteral("PATCH");
        path += QStringLiteral("/%1").arg(cloudId);
        requestBody.insert(QStringLiteral("baseVersion"),
            m_selectedNote.value(QStringLiteral("cloudVersion"))
                .toInt(m_selectedNote.value(QStringLiteral("version")).toInt()));
    }
    QJsonObject pending = m_selectedNote;
    if (!cloudId.isEmpty())
        pending.insert(QStringLiteral("cloudId"), cloudId);
    const QString sentTitle = m_noteTitle->text();
    m_noteSaveInFlight = true;
    setNoteSaveState(QStringLiteral("Saving…"));
    requestNotes(method, path, requestBody,
                 [this, keepLocal, saveLocal, announce, pending, sentTitle,
                  body](bool ok, const QJsonObject &payload,
                        const QString &error) mutable {
        m_noteSaveInFlight = false;
        const bool queued = std::exchange(m_noteSaveQueued, false);
        const QString pendingId = pending.value(QStringLiteral("id")).toString();
        const bool still =
            m_selectedNote.value(QStringLiteral("id")).toString() == pendingId;
        if (!ok && forkmesh::notes::isVersionConflict(payload)) {
            const QString reason = QStringLiteral(
                "someone else saved a newer version; choose Keep mine or "
                "Take theirs");
            const bool savedLocal = keepLocal && still && saveLocal(pending);
            announce(savedLocal, false, {}, reason);
            if (still) {
                m_noteDirty = true;
                showNoteConflict(payload.value(QStringLiteral("note")).toObject());
            }
            return;
        }
        if (!ok) {
            announce(saveLocal(pending), false, {}, error);
            return;
        }
        QJsonObject cloud = payload.value(QStringLiteral("note")).toObject();
        storeCloudNote(cloud, m_host.accountName ? m_host.accountName() : QString());
        clearNoteConflict();
        if (keepLocal) {
            QJsonObject local = pending;
            local.insert(QStringLiteral("cloudId"),
                         cloud.value(QStringLiteral("id")));
            local.insert(QStringLiteral("cloudVersion"),
                         cloud.value(QStringLiteral("version")));
            local.insert(QStringLiteral("visibility"),
                         cloud.value(QStringLiteral("visibility")));
            local.insert(QStringLiteral("shares"),
                         cloud.value(QStringLiteral("shares")));
            local.insert(QStringLiteral("views"),
                         cloud.value(QStringLiteral("views")));
            local.insert(QStringLiteral("readers"),
                         cloud.value(QStringLiteral("readers")));
            const bool savedLocal = saveLocal(local);
            updateNotePublishState();
            announce(savedLocal, true, cloud, QString());
            if (savedLocal)
                setNoteSaveState(QStringLiteral("Saved"));
            syncNoteCollab();
        } else {
            const int localIndex = localNoteIndex(m_localNotes, pendingId);
            if (localIndex >= 0) {
                m_localNotes.removeAt(localIndex);
                writeLocalNotes(m_localNotes);
            }
            const QString id = cloud.value(QStringLiteral("id")).toString();
            if (!still) {
                renderNotesList(m_selectedNote.value(QStringLiteral("id")).toString(),
                                false);
                announce(false, true, cloud, QString());
                return;
            }
            const bool changedSince = m_noteTitle->text() != sentTitle ||
                                      m_noteEditor->markdown() != body;
            m_selectedNote = cloud;
            m_selectedNoteStorage = QStringLiteral("cloud");
            updateNotePublishState();
            renderNotesList(id, false);
            renderNoteLinkedItems();
            syncNoteCollab();
            announce(false, true, cloud, QString());
            if (changedSince && queued)
                saveNote();
            else if (changedSince)
                scheduleNoteAutosave();
            else
                markNoteClean(QStringLiteral("Saved"));
        }
    });
}

void NotesPage::storeCloudNote(const QJsonObject &cloud, const QString &editor)
{
    const QString id = cloud.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;
    for (int i = 0; i < m_cloudNotes.size(); ++i) {
        const QJsonObject previous = m_cloudNotes.at(i).toObject();
        if (previous.value(QStringLiteral("id")).toString() == id) {
            m_cloudNotes.replace(
                i, forkmesh::notes::noteKeepFreshness(cloud, previous, editor));
            return;
        }
    }
    m_cloudNotes.prepend(
        forkmesh::notes::noteKeepFreshness(cloud, QJsonObject(), editor));
}

void NotesPage::recordNoteView(const QString &cloudId)
{
    if (!forkmesh::notes::isNoteId(cloudId))
        return;
    requestNotes(QByteArrayLiteral("POST"),
                 QStringLiteral("/api/notes/%1/activity").arg(cloudId),
                 QJsonObject{{QStringLiteral("kind"), QStringLiteral("view")}},
                 [](bool, const QJsonObject &, const QString &) {});
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < m_cloudNotes.size(); ++i) {
        const QJsonObject cloud = m_cloudNotes.at(i).toObject();
        if (cloud.value(QStringLiteral("id")).toString() == cloudId)
            m_cloudNotes.replace(i, forkmesh::notes::noteMarkedRead(cloud, now));
    }
    if (m_selectedNoteStorage == QLatin1String("cloud") &&
        m_selectedNote.value(QStringLiteral("id")).toString() == cloudId)
        m_selectedNote = forkmesh::notes::noteMarkedRead(m_selectedNote, now);
    if (!m_notesList)
        return;
    for (int row = 0; row < m_notesList->count(); ++row) {
        QListWidgetItem *item = m_notesList->item(row);
        if (item->data(kNoteRowCloudIdRole).toString() != cloudId ||
            !item->data(kNoteRowUnreadRole).toBool())
            continue;
        item->setData(kNoteRowUnreadRole, false);
        QString text = item->text();
        if (text.startsWith(QStringLiteral("● ")))
            item->setText(text.mid(2));
    }
}

bool NotesPage::noteAutosaveApplies() const
{
    return m_noteStorageMode &&
           m_selectedNoteStorage == QLatin1String("cloud") &&
           m_noteStorageMode->currentData().toString() ==
               QLatin1String("cloud") &&
           !selectedNoteCloudId().isEmpty() &&
           forkmesh::notes::noteCanEdit(selectedNoteRole());
}

void NotesPage::setNoteSaveState(const QString &text)
{
    if (m_noteSaveState)
        m_noteSaveState->setText(text);
}

void NotesPage::markNoteClean(const QString &state)
{
    m_noteDirty = false;
    if (m_noteAutosaveTimer)
        m_noteAutosaveTimer->stop();
    setNoteSaveState(m_noteConflict.isEmpty()
                         ? state
                         : QStringLiteral("Not saved — resolve the conflict"));
}

void NotesPage::scheduleNoteAutosave()
{
    m_noteDirty = true;
    if (!m_noteConflict.isEmpty()) {
        setNoteSaveState(QStringLiteral("Not saved — resolve the conflict"));
        return;
    }
    setNoteSaveState(QStringLiteral("Unsaved changes"));
    if (m_noteAutosaveTimer && noteAutosaveApplies())
        m_noteAutosaveTimer->start();
}

void NotesPage::autosaveNote()
{
    if (!m_noteDirty || !noteAutosaveApplies() || !m_noteConflict.isEmpty())
        return;
    if (m_noteSaveInFlight) {
        m_noteAutosaveTimer->start();
        return;
    }
    const QString cloudId = selectedNoteCloudId();
    const QString sentTitle = m_noteTitle->text();
    const QString body = m_noteEditor->markdown();
    QJsonObject request{
        {QStringLiteral("title"), sentTitle.trimmed().isEmpty()
                                      ? QStringLiteral("Untitled note")
                                      : sentTitle.trimmed()},
        {QStringLiteral("markdown"), body},
        {QStringLiteral("baseVersion"),
         m_selectedNote.value(QStringLiteral("version")).toInt()},
    };
    if (selectedNoteRole() == QLatin1String("owner"))
        request.insert(QStringLiteral("visibility"),
                       m_notePublic->isChecked() ? QStringLiteral("public")
                                                 : QStringLiteral("private"));
    m_noteSaveInFlight = true;
    setNoteSaveState(QStringLiteral("Saving…"));
    requestNotes(QByteArrayLiteral("PATCH"),
                 QStringLiteral("/api/notes/%1").arg(cloudId), request,
                 [this, cloudId, sentTitle, body](bool ok,
                                                  const QJsonObject &payload,
                                                  const QString &error) {
        m_noteSaveInFlight = false;
        const bool queued = std::exchange(m_noteSaveQueued, false);
        const bool still = m_selectedNoteStorage == QLatin1String("cloud") &&
                           selectedNoteCloudId() == cloudId;
        if (!ok) {
            const bool conflict =
                still && forkmesh::notes::isVersionConflict(payload);
            if (conflict) {
                showNoteConflict(payload.value(QStringLiteral("note")).toObject());
            } else if (still) {
                setNoteSaveState(QStringLiteral("Not saved: %1").arg(error));
            } else {
                flashMessage(QStringLiteral("Your last edits to a note you "
                                            "left were not saved: %1")
                                 .arg(error),
                             true, QString(),
                             forkmesh::kLocalOperationNoticeKind);
            }
            if (queued && !conflict)
                saveNote();
            return;
        }
        const QJsonObject cloud = payload.value(QStringLiteral("note")).toObject();
        storeCloudNote(cloud, m_host.accountName ? m_host.accountName() : QString());
        if (!still) {
            renderNotesList(m_selectedNote.value(QStringLiteral("id")).toString(),
                            false);
            if (queued)
                saveNote();
            return;
        }
        const bool changedSince = m_noteTitle->text() != sentTitle ||
                                  m_noteEditor->markdown() != body;
        m_selectedNote = cloud;
        clearNoteConflict();
        updateNotePublishState();
        renderNotesList(cloudId, false);
        if (changedSince && queued)
            saveNote();
        else if (changedSince)
            scheduleNoteAutosave();
        else if (queued) {
            markNoteClean(QStringLiteral("Saved"));
            flashMessage(NoteListEntry::savedAlert(cloud.value(QStringLiteral("title")).toString(),
                                                   QStringLiteral("cloud"), false, true, cloud),
                         false, QString(), forkmesh::kLocalOperationNoticeKind);
        } else
            markNoteClean(QStringLiteral("Saved"));
    });
}

void NotesPage::showNoteConflict(const QJsonObject &theirs)
{
    m_noteConflict = theirs;
    if (m_noteAutosaveTimer)
        m_noteAutosaveTimer->stop();
    if (m_noteConflictBar)
        m_noteConflictBar->show();
    setNoteSaveState(QStringLiteral("Not saved — resolve the conflict"));
}

void NotesPage::clearNoteConflict()
{
    m_noteConflict = QJsonObject();
    if (m_noteConflictBar)
        m_noteConflictBar->hide();
}

void NotesPage::keepMyNoteVersion()
{
    const QJsonObject theirs = m_noteConflict;
    if (theirs.isEmpty())
        return;
    const int version = theirs.value(QStringLiteral("version")).toInt();
    if (m_selectedNoteStorage == QLatin1String("cloud"))
        m_selectedNote.insert(QStringLiteral("version"), version);
    else
        m_selectedNote.insert(QStringLiteral("cloudVersion"), version);
    clearNoteConflict();
    m_noteDirty = true;
    saveNote();
}

void NotesPage::takeTheirNoteVersion()
{
    const QJsonObject theirs = m_noteConflict;
    if (theirs.isEmpty())
        return;
    clearNoteConflict();
    const QString title = theirs.value(QStringLiteral("title")).toString();
    const QString markdown = theirs.value(QStringLiteral("markdown")).toString();
    if (m_selectedNoteStorage == QLatin1String("cloud")) {
        m_selectedNote = theirs;
        storeCloudNote(theirs);
    } else {
        QJsonObject local = m_selectedNote;
        local.insert(QStringLiteral("cloudVersion"),
                     theirs.value(QStringLiteral("version")));
        local.insert(QStringLiteral("visibility"),
                     theirs.value(QStringLiteral("visibility")));
        local.insert(QStringLiteral("shares"),
                     theirs.value(QStringLiteral("shares")));
        local = localSnapshot(local, title, markdown,
                              m_noteStorageMode->currentData().toString());
        const int index = localNoteIndex(
            m_localNotes, local.value(QStringLiteral("id")).toString());
        if (index >= 0)
            m_localNotes.replace(index, local);
        else
            m_localNotes.prepend(local);
        if (!writeLocalNotes(m_localNotes))
            flashMessage(QStringLiteral("Could not write their version to %1")
                             .arg(QDir::toNativeSeparators(notesFilePath())),
                         true);
        m_selectedNote = local;
        storeCloudNote(theirs);
    }
    m_noteTitle->setText(title);
    m_noteEditor->setMarkdown(markdown);
    m_notePublic->setChecked(theirs.value(QStringLiteral("visibility")).toString() ==
                             QLatin1String("public"));
    updateNotePublishState();
    markNoteClean(QStringLiteral("Saved"));
    renderNotesList(m_selectedNote.value(QStringLiteral("id")).toString(), false);
    renderNoteLinkedItems();
}

void NotesPage::copyNoteLink()
{
    const QString link = selectedNoteAccessLink();
    if (link.isEmpty()) {
        flashMessage(QStringLiteral("Save this note to the cloud to get a link "
                                    "to it."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    QGuiApplication::clipboard()->setText(link);
    flashMessage(QStringLiteral("Link copied. It opens this note for anyone "
                                "who has access."),
                 false, QString(), forkmesh::kLocalOperationNoticeKind);
}

void NotesPage::copyNotePublicLink()
{
    const QString address = selectedNotePublicUrl();
    if (address.isEmpty()) {
        flashMessage(QStringLiteral("This note is not published."), true,
                     QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    QGuiApplication::clipboard()->setText(address);
    flashMessage(QStringLiteral("Public link copied."), false, QString(),
                 forkmesh::kLocalOperationNoticeKind);
}

void NotesPage::exportNote()
{
    if (m_selectedNote.isEmpty() || !m_noteTitle || !m_noteEditor) {
        flashMessage(QStringLiteral("Select a note to export."), true,
                     QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    const QString title = m_noteTitle->text();
    const QString folder = QStandardPaths::writableLocation(
        QStandardPaths::DocumentsLocation);
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export note as Markdown"),
        QDir(folder).filePath(forkmesh::notes::exportFileName(title)),
        QStringLiteral("Markdown (*.md);;All files (*)"));
    if (path.isEmpty())
        return;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(forkmesh::notes::exportMarkdown(title, m_noteEditor->markdown())
                       .toUtf8()) < 0 ||
        !file.commit()) {
        flashMessage(QStringLiteral("Could not write %1")
                         .arg(QDir::toNativeSeparators(path)),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    flashMessage(QStringLiteral("Exported to %1")
                     .arg(QDir::toNativeSeparators(path)),
                 false, QString(), forkmesh::kLocalOperationNoticeKind);
}

void NotesPage::deleteNote()
{
    if (m_selectedNote.isEmpty())
        return;
    const QString localId = m_selectedNote.value(QStringLiteral("id")).toString();
    const QString cloudId = selectedNoteCloudId();
    const bool hasLocalCopy = localNoteIndex(m_localNotes, localId) >= 0;
    const QString location = hasLocalCopy && !cloudId.isEmpty()
        ? QStringLiteral("this computer and the cloud")
        : hasLocalCopy ? QStringLiteral("this computer")
        : QStringLiteral("the cloud");
    if (QMessageBox::question(
            this, QStringLiteral("Delete note"),
            QStringLiteral("Delete this note from %1?").arg(location)) !=
        QMessageBox::Yes)
        return;

    auto removeLocal = [this, localId] {
        const int index = localNoteIndex(m_localNotes, localId);
        if (index < 0)
            return true;
        QJsonArray remaining = m_localNotes;
        remaining.removeAt(index);
        if (!writeLocalNotes(remaining))
            return false;
        m_localNotes = std::move(remaining);
        return true;
    };
    auto finish = [this, cloudId] {
        if (!cloudId.isEmpty()) {
            for (int i = m_cloudNotes.size() - 1; i >= 0; --i) {
                if (m_cloudNotes.at(i).toObject().value(
                        QStringLiteral("id")).toString() == cloudId)
                    m_cloudNotes.removeAt(i);
            }
        }
        m_selectedNote = {};
        m_selectedNoteStorage.clear();
        m_noteDirty = false;
        syncNoteCollab();
        renderNotesList();
        refreshNotes();
    };
    if (cloudId.isEmpty()) {
        if (!removeLocal())
            QMessageBox::warning(
                this, QStringLiteral("Delete note"),
                QStringLiteral("The local copy could not be removed."));
        else
            finish();
        return;
    }
    requestNotes(QByteArrayLiteral("DELETE"),
                 QStringLiteral("/api/notes/%1").arg(cloudId), {},
                 [this, finish, removeLocal](bool ok, const QJsonObject &,
                                             const QString &error) {
        if (!ok) {
            QMessageBox::warning(
                this, QStringLiteral("Cloud delete failed"),
                QStringLiteral("The cloud copy was not deleted, so the local "
                               "copy was kept. %1").arg(error));
            return;
        }
        if (!removeLocal()) {
            QMessageBox::warning(
                this, QStringLiteral("Local delete failed"),
                QStringLiteral("The cloud copy was deleted, but the local "
                               "copy could not be removed."));
        }
        finish();
    });
}

void NotesPage::applyNoteShares(const QString &cloudId,
                                 const QJsonArray &shares)
{
    if (selectedNoteCloudId() == cloudId)
        m_selectedNote.insert(QStringLiteral("shares"), shares);
    for (int i = 0; i < m_cloudNotes.size(); ++i) {
        QJsonObject cloud = m_cloudNotes.at(i).toObject();
        if (cloud.value(QStringLiteral("id")).toString() != cloudId)
            continue;
        cloud.insert(QStringLiteral("shares"), shares);
        m_cloudNotes.replace(i, cloud);
        break;
    }
    const int local = localNoteIndex(m_localNotes, cloudId);
    if (local >= 0) {
        QJsonObject note = m_localNotes.at(local).toObject();
        note.insert(QStringLiteral("shares"), shares);
        m_localNotes.replace(local, note);
        writeLocalNotes(m_localNotes);
    }
    renderNotesList(m_selectedNote.value(QStringLiteral("id")).toString(),
                    false);
}

void NotesPage::shareNote()
{
    const QString cloudId = selectedNoteCloudId();
    if (cloudId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Share note"),
            QStringLiteral("Choose Cloud only or Local + cloud and save before sharing."));
        return;
    }
    if (selectedNoteRole() != QLatin1String("owner")) {
        QMessageBox::information(
            this, QStringLiteral("Share note"),
            QStringLiteral("Only the note's owner can change who it is shared "
                           "with. Use Copy link in the ⋯ menu to send it to "
                           "someone who already has access."));
        return;
    }
    struct ShareState {
        QJsonArray shares;
        int sequence = 0;
        QString asked;
    };
    auto state = std::make_shared<ShareState>();
    state->shares = m_selectedNote.value(QStringLiteral("shares")).toArray();
    const QString accessLink = selectedNoteAccessLink();

    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("noteShareDialog"));
    dialog->setWindowTitle(QStringLiteral("Share note"));
    dialog->setMinimumWidth(520);
    auto *layout = new QVBoxLayout(dialog);
    layout->setSpacing(8);
    auto *heading = new QLabel(QStringLiteral("Share “%1”").arg(
        m_noteTitle->text().trimmed().isEmpty()
            ? QStringLiteral("Untitled note")
            : m_noteTitle->text().trimmed()));
    heading->setStyleSheet(
        Theme::remapSheet(QStringLiteral("font-weight:600;")));
    layout->addWidget(heading);

    auto *addRow = new QHBoxLayout;
    auto *type = new QComboBox;
    type->setObjectName(QStringLiteral("noteShareType"));
    type->addItem(QStringLiteral("User"), QStringLiteral("user"));
    type->addItem(QStringLiteral("Organization"),
                  QStringLiteral("organization"));
    auto *name = new QLineEdit;
    name->setObjectName(QStringLiteral("noteShareName"));
    name->setPlaceholderText(QStringLiteral("Name of a person or organization"));
    name->setClearButtonEnabled(true);
    auto *role = new QComboBox;
    role->setObjectName(QStringLiteral("noteShareRole"));
    role->addItem(QStringLiteral("Can edit"), QStringLiteral("editor"));
    role->addItem(QStringLiteral("Can view"), QStringLiteral("viewer"));
    auto *add = new QPushButton(QStringLiteral("Share"));
    add->setObjectName(QStringLiteral("primaryButton"));
    add->setDefault(true);
    setOcticon(add, "people", 14);
    addRow->addWidget(type);
    addRow->addWidget(name, 1);
    addRow->addWidget(role);
    addRow->addWidget(add);
    layout->addLayout(addRow);

    auto *model = new QStandardItemModel(dialog);
    auto *completer = new QCompleter(model, dialog);
    completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    completer->setCompletionRole(kShareSuggestionNameRole);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setWidget(name);

    auto *notice = new QLabel;
    notice->setObjectName(QStringLiteral("mutedLabel"));
    notice->setWordWrap(true);
    notice->hide();
    layout->addWidget(notice);

    auto *peopleTitle = new QLabel(QStringLiteral("People with access"));
    peopleTitle->setStyleSheet(
        Theme::remapSheet(QStringLiteral("font-weight:600;")));
    layout->addWidget(peopleTitle);
    auto *people = new QWidget;
    people->setObjectName(QStringLiteral("noteSharePeople"));
    auto *peopleLayout = new QVBoxLayout(people);
    peopleLayout->setContentsMargins(0, 0, 0, 0);
    peopleLayout->setSpacing(4);
    layout->addWidget(people);

    auto *linkBox = new QWidget;
    linkBox->setObjectName(QStringLiteral("noteShareLinkBox"));
    auto *linkLayout = new QVBoxLayout(linkBox);
    linkLayout->setContentsMargins(0, 6, 0, 0);
    linkLayout->setSpacing(4);
    auto *linkTitle = new QLabel(QStringLiteral("Link for people with access"));
    linkTitle->setStyleSheet(
        Theme::remapSheet(QStringLiteral("font-weight:600;")));
    linkLayout->addWidget(linkTitle);
    auto *linkRow = new QHBoxLayout;
    auto *linkEdit = new QLineEdit(accessLink);
    linkEdit->setObjectName(QStringLiteral("noteShareLink"));
    linkEdit->setReadOnly(true);
    linkEdit->setCursorPosition(0);
    auto *copy = new QPushButton(QStringLiteral("Copy link"));
    copy->setObjectName(QStringLiteral("ghostButton"));
    setOcticon(copy, "copy", 14);
    linkRow->addWidget(linkEdit, 1);
    linkRow->addWidget(copy);
    linkLayout->addLayout(linkRow);
    auto *linkHint = new QLabel(QStringLiteral(
        "Only people listed above can open it. Anyone else is asked to sign "
        "in and is refused."));
    linkHint->setObjectName(QStringLiteral("mutedLabel"));
    linkHint->setWordWrap(true);
    linkLayout->addWidget(linkHint);
    layout->addWidget(linkBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);

    QPointer<QDialog> guard(dialog);
    auto say = [notice](const QString &text) {
        notice->setText(text);
        notice->setVisible(!text.isEmpty());
    };
    auto render = std::make_shared<std::function<void()>>();
    connect(dialog, &QObject::destroyed, dialog, [render] {});
    const std::weak_ptr<std::function<void()>> weakRender = render;
    auto send = [this, guard, cloudId, state, say, weakRender](
                    const QByteArray &method, const QJsonObject &principal,
                    const QString &success,
                    const std::function<void()> &after = {}) {
        requestNotes(method,
                     QStringLiteral("/api/notes/%1/shares").arg(cloudId),
                     principal,
                     [this, guard, cloudId, state, say, weakRender, principal,
                      success, after](bool ok, const QJsonObject &payload,
                                      const QString &error) {
            if (!ok) {
                if (guard)
                    say(forkmesh::notes::shareError(
                        payload.value(QStringLiteral("error")).toString(),
                        principal.value(QStringLiteral("name")).toString(),
                        QStringLiteral("Sharing failed: %1").arg(error)));
                return;
            }
            const QJsonArray shares =
                payload.value(QStringLiteral("shares")).toArray();
            applyNoteShares(cloudId, shares);
            if (!guard)
                return;
            state->shares = shares;
            if (const auto again = weakRender.lock())
                (*again)();
            say(success);
            if (after)
                after();
        });
    };
    *render = [this, people, peopleLayout, linkBox, state, send] {
        while (QLayoutItem *item = peopleLayout->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        if (state->shares.isEmpty()) {
            auto *empty = new QLabel(QStringLiteral(
                "Only you can open this note."));
            empty->setObjectName(QStringLiteral("mutedLabel"));
            peopleLayout->addWidget(empty);
        }
        for (const QJsonValue &value : std::as_const(state->shares)) {
            const QJsonObject share = value.toObject();
            const QString shareType = share.value(QStringLiteral("type")).toString();
            const QString shareName = share.value(QStringLiteral("name")).toString();
            const QString shareRole =
                share.value(QStringLiteral("role")).toString(QStringLiteral("viewer"));
            auto *row = new QWidget(people);
            row->setObjectName(QStringLiteral("noteSharePerson"));
            row->setProperty("shareName", shareName);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            auto *who = new QLabel(
                shareType == QLatin1String("organization")
                    ? QStringLiteral("@%1").arg(shareName).toHtmlEscaped()
                    : QStringLiteral("<a href=\"%1\">@%2</a>")
                          .arg(QString::fromLatin1(QUrl::toPercentEncoding(shareName)),
                               shareName.toHtmlEscaped()));
            who->setTextFormat(Qt::RichText);
            who->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
            who->setOpenExternalLinks(false);
            connect(who, &QLabel::linkActivated, this, [this](const QString &href) {
                if (m_host.openUserProfile)
                    m_host.openUserProfile(QUrl::fromPercentEncoding(href.toUtf8()));
            });
            auto *kind = new QLabel(forkmesh::notes::principalTypeLabel(shareType));
            kind->setObjectName(QStringLiteral("mutedLabel"));
            auto *access = new QComboBox;
            access->setObjectName(QStringLiteral("noteSharePersonRole"));
            access->addItem(QStringLiteral("Can edit"), QStringLiteral("editor"));
            access->addItem(QStringLiteral("Can view"), QStringLiteral("viewer"));
            access->setCurrentIndex(qMax(0, access->findData(shareRole)));
            auto *remove = new QPushButton(QStringLiteral("Remove"));
            remove->setObjectName(QStringLiteral("ghostButton"));
            remove->setToolTip(QStringLiteral("Stop sharing with @%1").arg(shareName));
            rowLayout->addWidget(who, 1);
            rowLayout->addWidget(kind);
            rowLayout->addWidget(access);
            rowLayout->addWidget(remove);
            peopleLayout->addWidget(row);
            const QJsonObject principal{{QStringLiteral("type"), shareType},
                                        {QStringLiteral("name"), shareName}};
            connect(access, &QComboBox::activated, row,
                    [send, principal, access, shareName] {
                QJsonObject body = principal;
                body.insert(QStringLiteral("role"), access->currentData().toString());
                send(QByteArrayLiteral("POST"), body,
                     QStringLiteral("@%1 now %2 this note.")
                         .arg(shareName,
                              access->currentData().toString() ==
                                      QLatin1String("editor")
                                  ? QStringLiteral("can edit")
                                  : QStringLiteral("can view")));
            });
            connect(remove, &QPushButton::clicked, row,
                    [send, principal, shareName, remove] {
                remove->setEnabled(false);
                send(QByteArrayLiteral("DELETE"), principal,
                     QStringLiteral("Stopped sharing with @%1.").arg(shareName));
            });
        }
        linkBox->setVisible(!state->shares.isEmpty());
    };
    (*render)();

    auto *debounce = new QTimer(dialog);
    debounce->setSingleShot(true);
    debounce->setInterval(kShareSuggestionDebounceMs);
    connect(name, &QLineEdit::textEdited, dialog, [model, completer, debounce] {
        model->clear();
        completer->popup()->hide();
        debounce->start();
    });
    connect(debounce, &QTimer::timeout, dialog,
            [this, guard, cloudId, state, name, model, completer] {
        const QString query = forkmesh::notes::shareQuery(name->text());
        if (!forkmesh::notes::shareQueryWorthAsking(query))
            return;
        const int sequence = ++state->sequence;
        state->asked = query;
        requestNotes(
            QByteArrayLiteral("GET"),
            QStringLiteral("/api/notes/%1/shares?query=%2")
                .arg(cloudId, QString::fromLatin1(QUrl::toPercentEncoding(query))),
            {},
            [guard, state, sequence, query, name, model, completer](
                bool ok, const QJsonObject &payload, const QString &) {
            if (!guard || !ok || sequence != state->sequence ||
                forkmesh::notes::shareQuery(name->text()) != query)
                return;
            model->clear();
            for (const QJsonValue &value :
                 payload.value(QStringLiteral("suggestions")).toArray()) {
                const QJsonObject suggestion = value.toObject();
                const QString suggestedName =
                    suggestion.value(QStringLiteral("name")).toString();
                const QString suggestedType =
                    suggestion.value(QStringLiteral("type")).toString();
                if (suggestedName.isEmpty())
                    continue;
                auto *item = new QStandardItem(QStringLiteral("%1 · %2").arg(
                    suggestedName,
                    forkmesh::notes::principalTypeLabel(suggestedType)));
                item->setData(suggestedName, kShareSuggestionNameRole);
                item->setData(suggestedType, kShareSuggestionTypeRole);
                item->setEditable(false);
                model->appendRow(item);
            }
            if (model->rowCount() && name->hasFocus()) {
                completer->setCompletionPrefix(QString());
                completer->complete();
            }
        });
    });
    connect(completer, qOverload<const QModelIndex &>(&QCompleter::activated),
            dialog, [name, type](const QModelIndex &index) {
        name->setText(index.data(kShareSuggestionNameRole).toString());
        const int found = type->findData(
            index.data(kShareSuggestionTypeRole).toString());
        if (found >= 0)
            type->setCurrentIndex(found);
    });

    connect(add, &QPushButton::clicked, dialog,
            [name, type, role, send, say, debounce, state] {
        const QString who = forkmesh::notes::shareQuery(name->text());
        if (who.isEmpty()) {
            say(QStringLiteral("Type the name of an account or organization."));
            name->setFocus();
            return;
        }
        debounce->stop();
        ++state->sequence;
        send(QByteArrayLiteral("POST"),
             QJsonObject{{QStringLiteral("type"), type->currentData().toString()},
                         {QStringLiteral("name"), who},
                         {QStringLiteral("role"), role->currentData().toString()}},
             forkmesh::notes::sharedNotice(who), [name] { name->clear(); });
    });
    connect(copy, &QPushButton::clicked, dialog, [accessLink, say] {
        QGuiApplication::clipboard()->setText(accessLink);
        say(QStringLiteral("Link copied."));
    });

    dialog->open();
    name->setFocus();
}

void NotesPage::applyNoteLinks(const QString &cloudId, const QJsonArray &links)
{
    if (selectedNoteCloudId() == cloudId)
        m_selectedNote.insert(QStringLiteral("links"), links);
    renderNoteLinkedItems();
}

void NotesPage::renderNoteLinkedItems()
{
    if (!m_noteLinkedItems)
        return;
    const QJsonArray links = m_selectedNote.value(QStringLiteral("links")).toArray();
    if (links.isEmpty()) {
        m_noteLinkedItems->clear();
        m_noteLinkedItems->hide();
        return;
    }
    QStringList parts;
    for (const QJsonValue &value : links) {
        const QJsonObject link = value.toObject();
        parts << QStringLiteral("<a href=\"%1\">%2</a>")
                     .arg(forkmesh::notes::conversationHref(link, catalogApiUrl())
                              .toHtmlEscaped(),
                          forkmesh::notes::conversationLabel(link).toHtmlEscaped());
    }
    m_noteLinkedItems->setText(QStringLiteral("Linked: ") +
                               parts.join(QStringLiteral(" · ")));
    m_noteLinkedItems->show();
}

void NotesPage::showNoteLinks()
{
    if (m_selectedNote.isEmpty()) {
        flashMessage(QStringLiteral("Select a note first."), true, QString(),
                     forkmesh::kLocalOperationNoticeKind);
        return;
    }
    const QString cloudId = selectedNoteCloudId();
    const bool canEdit = forkmesh::notes::noteCanEdit(selectedNoteRole());
    auto *dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("noteLinksDialog"));
    dialog->setWindowTitle(QStringLiteral("Linked issues, pull requests and "
                                          "discussions"));
    dialog->setMinimumWidth(520);
    auto *layout = new QVBoxLayout(dialog);
    auto *list = new QListWidget;
    list->setObjectName(QStringLiteral("noteLinksList"));
    layout->addWidget(list, 1);
    auto *notice = new QLabel;
    notice->setObjectName(QStringLiteral("mutedLabel"));
    notice->setWordWrap(true);
    layout->addWidget(notice);
    auto *addRow = new QHBoxLayout;
    auto *input = new QLineEdit;
    input->setObjectName(QStringLiteral("noteLinkInput"));
    input->setPlaceholderText(QStringLiteral(
        "Paste an issue, pull request or discussion URL, or owner/repo#123"));
    auto *kind = new QComboBox;
    kind->addItem(QStringLiteral("Issue"), QStringLiteral("issue"));
    kind->addItem(QStringLiteral("Pull request"), QStringLiteral("pull"));
    kind->addItem(QStringLiteral("Discussion"), QStringLiteral("discussion"));
    kind->setToolTip(QStringLiteral("What owner/repo#123 refers to"));
    auto *add = new QPushButton(QStringLiteral("Link"));
    add->setObjectName(QStringLiteral("primaryButton"));
    add->setDefault(true);
    addRow->addWidget(input, 1);
    addRow->addWidget(kind);
    addRow->addWidget(add);
    layout->addLayout(addRow);
    auto *footer = new QHBoxLayout;
    auto *remove = new QPushButton(QStringLiteral("Remove"));
    remove->setObjectName(QStringLiteral("ghostButton"));
    auto *open = new QPushButton(QStringLiteral("Open"));
    open->setObjectName(QStringLiteral("ghostButton"));
    auto *close = new QPushButton(QStringLiteral("Close"));
    close->setAutoDefault(false);
    footer->addWidget(open);
    footer->addWidget(remove);
    footer->addStretch();
    footer->addWidget(close);
    layout->addLayout(footer);
    connect(close, &QPushButton::clicked, dialog, &QDialog::reject);
    for (QWidget *widget : {static_cast<QWidget *>(input),
                            static_cast<QWidget *>(kind),
                            static_cast<QWidget *>(add),
                            static_cast<QWidget *>(remove)})
        widget->setEnabled(canEdit);
    if (!canEdit)
        notice->setText(QStringLiteral("Only the owner and editors can change "
                                       "the links."));

    QPointer<QDialog> guard(dialog);
    auto fill = [this, list, open, remove, canEdit] {
        list->clear();
        const QJsonArray links =
            m_selectedNote.value(QStringLiteral("links")).toArray();
        for (const QJsonValue &value : links) {
            const QJsonObject link = value.toObject();
            auto *item = new QListWidgetItem(
                forkmesh::notes::conversationLabel(link));
            item->setData(Qt::UserRole, link);
            list->addItem(item);
        }
        if (!list->count()) {
            auto *empty = new QListWidgetItem(QStringLiteral("Nothing linked yet."));
            empty->setFlags(Qt::NoItemFlags);
            list->addItem(empty);
        }
        const bool picked = list->currentItem() &&
                            list->currentItem()->flags() != Qt::NoItemFlags;
        open->setEnabled(picked);
        remove->setEnabled(picked && canEdit);
    };
    fill();
    connect(list, &QListWidget::currentItemChanged, dialog,
            [list, open, remove, canEdit] {
        const bool picked = list->currentItem() &&
                            list->currentItem()->flags() != Qt::NoItemFlags;
        open->setEnabled(picked);
        remove->setEnabled(picked && canEdit);
    });
    auto openPicked = [this, list] {
        QListWidgetItem *item = list->currentItem();
        if (!item || item->flags() == Qt::NoItemFlags)
            return;
        openReferenceLink(forkmesh::notes::conversationHref(
            item->data(Qt::UserRole).toJsonObject(), catalogApiUrl()));
    };
    connect(open, &QPushButton::clicked, dialog, openPicked);
    connect(list, &QListWidget::itemActivated, dialog, openPicked);

    auto applyLocal = [this, fill](const QJsonArray &links) {
        m_selectedNote.insert(QStringLiteral("links"), links);
        renderNoteLinkedItems();
        fill();
        m_noteDirty = true;
        saveNote();
    };
    auto send = [this, guard, cloudId, fill, notice](
                    const QByteArray &method, const QJsonObject &link,
                    const QString &success) {
        requestNotes(method, QStringLiteral("/api/notes/%1/links").arg(cloudId),
                     link,
                     [this, guard, cloudId, fill, notice, success](
                         bool ok, const QJsonObject &payload,
                         const QString &error) {
            if (!ok) {
                if (guard)
                    notice->setText(
                        payload.value(QStringLiteral("error")).toString() ==
                                QLatin1String("repository_not_found")
                            ? QStringLiteral("That repository was not found, "
                                             "or you cannot see it.")
                            : QStringLiteral("Could not change the links: %1")
                                  .arg(error));
                return;
            }
            applyNoteLinks(cloudId,
                           payload.value(QStringLiteral("links")).toArray());
            if (!guard)
                return;
            fill();
            notice->setText(success);
        });
    };
    connect(add, &QPushButton::clicked, dialog,
            [input, kind, notice, cloudId, send, applyLocal, this] {
        const forkmesh::notes::ConversationRef ref =
            forkmesh::notes::parseConversationRef(input->text(),
                                                  kind->currentData().toString());
        if (!ref.isValid()) {
            notice->setText(QStringLiteral(
                "Paste an issue, pull request or discussion URL, or type "
                "owner/repo#123."));
            input->setFocus();
            return;
        }
        const QJsonObject link{{QStringLiteral("owner"), ref.owner},
                               {QStringLiteral("repo"), ref.repo},
                               {QStringLiteral("kind"), ref.kind},
                               {QStringLiteral("number"), ref.number}};
        const QString label = QStringLiteral("Linked %1/%2#%3")
                                  .arg(ref.owner, ref.repo)
                                  .arg(ref.number);
        input->clear();
        if (!cloudId.isEmpty()) {
            send(QByteArrayLiteral("POST"), link, label);
            return;
        }
        QJsonArray links = m_selectedNote.value(QStringLiteral("links")).toArray();
        for (const QJsonValue &value : std::as_const(links)) {
            if (forkmesh::notes::sameConversation(value.toObject(), link)) {
                notice->setText(QStringLiteral("Already linked."));
                return;
            }
        }
        links.append(link);
        applyLocal(links);
        notice->setText(label);
    });
    connect(remove, &QPushButton::clicked, dialog,
            [list, notice, cloudId, send, applyLocal, this] {
        QListWidgetItem *item = list->currentItem();
        if (!item || item->flags() == Qt::NoItemFlags)
            return;
        const QJsonObject link = item->data(Qt::UserRole).toJsonObject();
        const QString label = QStringLiteral("Removed %1")
                                  .arg(forkmesh::notes::conversationLabel(link));
        if (!cloudId.isEmpty()) {
            send(QByteArrayLiteral("DELETE"),
                 QJsonObject{{QStringLiteral("owner"), link.value(QStringLiteral("owner"))},
                             {QStringLiteral("repo"), link.value(QStringLiteral("repo"))},
                             {QStringLiteral("kind"), link.value(QStringLiteral("kind"))},
                             {QStringLiteral("number"), link.value(QStringLiteral("number"))}},
                 label);
            return;
        }
        QJsonArray links = m_selectedNote.value(QStringLiteral("links")).toArray();
        for (int i = links.size() - 1; i >= 0; --i) {
            if (forkmesh::notes::sameConversation(links.at(i).toObject(), link))
                links.removeAt(i);
        }
        applyLocal(links);
        notice->setText(label);
    });
    if (!cloudId.isEmpty() && canEdit) {
        requestNotes(QByteArrayLiteral("GET"),
                     QStringLiteral("/api/notes/%1/links").arg(cloudId), {},
                     [this, guard, cloudId, fill](bool ok,
                                                  const QJsonObject &payload,
                                                  const QString &) {
            if (!ok)
                return;
            applyNoteLinks(cloudId,
                           payload.value(QStringLiteral("links")).toArray());
            if (guard)
                fill();
        });
    }
    dialog->open();
    input->setFocus();
}

void NotesPage::searchCloudNotes()
{
    const QString query = forkmesh::notes::noteSearchQuery(
        m_noteSearch ? m_noteSearch->text() : QString());
    const int sequence = ++m_noteSearchSequence;
    if (!forkmesh::notes::noteSearchWorthAsking(query) || m_cloudNotes.isEmpty()) {
        m_noteSearchCloudQuery.clear();
        m_noteSearchCloudIds.clear();
        return;
    }
    requestNotes(
        QByteArrayLiteral("GET"),
        QStringLiteral("/api/notes?query=%1")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(query))),
        {},
        [this, sequence, query](bool ok, const QJsonObject &payload,
                                const QString &) {
        if (!ok || sequence != m_noteSearchSequence || !m_noteSearch ||
            forkmesh::notes::noteSearchQuery(m_noteSearch->text()) != query)
            return;
        m_noteSearchCloudQuery = query;
        m_noteSearchCloudIds.clear();
        for (const QJsonValue &value :
             payload.value(QStringLiteral("notes")).toArray())
            m_noteSearchCloudIds.insert(
                value.toObject().value(QStringLiteral("id")).toString());
        renderNotesList(m_selectedNote.value(QStringLiteral("id")).toString(),
                        false);
    });
}

bool NotesPage::openNoteLink(const QString &href)
{
    const forkmesh::notes::NoteLink link =
        forkmesh::notes::trustedNoteLink(href, noteRelayHosts());
    if (link.id.isEmpty())
        return false;
    m_pendingCommentNote = link.comment.isEmpty() ? QString() : link.id;
    m_pendingCommentId = link.comment;
    openNoteByCloudId(link.id);
    openPendingComment();
    return true;
}

void NotesPage::openNoteFromSystemLink(const QString &target)
{
    if (openNoteLink(target))
        return;
    const forkmesh::notes::NoteLink link = forkmesh::notes::parseNoteLink(target);
    flashMessage(link.host.isEmpty()
                     ? QStringLiteral("That note link is not valid.")
                     : QStringLiteral("That note lives on %1, not on the relay "
                                      "this app uses (%2).")
                           .arg(link.host, catalogApiUrl().host()),
                 true, QString(), forkmesh::kLocalOperationNoticeKind);
}

void NotesPage::openNoteByCloudId(const QString &cloudId)
{
    if (m_host.showPage)
        m_host.showPage();
    if (!m_notesList || cloudId.isEmpty())
        return;
    m_localNotes = readLocalNotes();
    for (const QJsonValue &value : std::as_const(m_localNotes)) {
        const QJsonObject note = value.toObject();
        if (note.value(QStringLiteral("cloudId")).toString() == cloudId) {
            openNoteFromSearch(note.value(QStringLiteral("id")).toString(),
                               QStringLiteral("local"));
            return;
        }
    }
    openNoteFromSearch(cloudId, QStringLiteral("cloud"));
}

bool NotesPage::noteConversionSource(QString *title, QString *markdown)
{
    if (!m_noteTitle || !m_noteEditor)
        return false;
    if (m_selectedNote.isEmpty()) {
        flashMessage(QStringLiteral("Select a note to convert."), true,
                     QString(), forkmesh::kLocalOperationNoticeKind);
        return false;
    }
    *title = m_noteTitle->text();
    *markdown = m_noteEditor->markdown();
    return true;
}

void NotesPage::convertNoteToTask()
{
    QString title, markdown;
    if (!noteConversionSource(&title, &markdown) || !m_host.windowCommand)
        return;
    m_host.windowCommand(
        QStringLiteral("task.composeFromText"),
        QJsonObject{
            {QStringLiteral("heading"), QStringLiteral("Convert note to task")},
            {QStringLiteral("title"), title},
            {QStringLiteral("markdown"), markdown},
            {QStringLiteral("link"), selectedNotePublicUrl()},
            {QStringLiteral("done"),
             QStringLiteral("Task created from this note. The note itself is unchanged.")},
        });
}

void NotesPage::convertNoteToIssue()
{
    QString title, markdown;
    if (!noteConversionSource(&title, &markdown) || !m_host.windowCommand)
        return;
    m_host.windowCommand(
        QStringLiteral("issue.composeFromText"),
        QJsonObject{
            {QStringLiteral("heading"), QStringLiteral("Convert note to issue")},
            {QStringLiteral("title"), title},
            {QStringLiteral("markdown"), markdown},
            {QStringLiteral("link"), selectedNotePublicUrl()},
        });
}

void NotesPage::showNoteVersions()
{
    if (m_selectedNote.isEmpty())
        return;
    const QString cloudId = selectedNoteCloudId();
    if (cloudId.isEmpty()) {
        const QJsonArray versions = m_selectedNote.value(QStringLiteral("versions")).toArray();
        QStringList labels;
        for (const QJsonValue &value : versions) {
            const QJsonObject version = value.toObject();
            labels << QStringLiteral("Version %1 — %2")
                .arg(version.value(QStringLiteral("version")).toInt())
                .arg(QDateTime::fromMSecsSinceEpoch(
                    static_cast<qint64>(version.value(QStringLiteral("createdAt")).toDouble()))
                    .toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        }
        bool ok = false;
        const QString chosen = QInputDialog::getItem(
            this, QStringLiteral("Local note versions"),
            QStringLiteral("Restore snapshot"), labels, 0, false, &ok);
        if (!ok || chosen.isEmpty())
            return;
        const int index = labels.indexOf(chosen);
        const QJsonObject version = versions.at(index).toObject();
        m_noteTitle->setText(version.value(QStringLiteral("title")).toString());
        m_noteEditor->setMarkdown(version.value(QStringLiteral("markdown")).toString());
        m_noteDirty = true;
        return;
    }
    requestNotes(QByteArrayLiteral("GET"),
                 QStringLiteral("/api/notes/%1/versions").arg(cloudId), {},
                 [this, cloudId](bool ok, const QJsonObject &payload,
                                 const QString &error) {
        if (!ok) {
            m_notesStatus->setText(QStringLiteral("Versions unavailable: %1").arg(error));
            return;
        }
        QStringList labels;
        const QJsonArray versions = payload.value(QStringLiteral("versions")).toArray();
        const int current = m_selectedNote.value(QStringLiteral("cloudVersion"))
            .toInt(m_selectedNote.value(QStringLiteral("version")).toInt());
        for (const QJsonValue &value : versions) {
            const QJsonObject entry = value.toObject();
            const int number = entry.value(QStringLiteral("version")).toInt();
            QString label = QStringLiteral("Version %1 — %2").arg(number).arg(
                QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(
                    entry.value(QStringLiteral("createdAt")).toDouble()))
                    .toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));
            const QString author = entry.value(QStringLiteral("author")).toString();
            if (!author.isEmpty())
                label += QStringLiteral(" — @%1").arg(author);
            if (number == current)
                label += QStringLiteral(" (current)");
            labels << label;
        }
        if (labels.isEmpty()) {
            m_notesStatus->setText(QStringLiteral("No saved versions yet."));
            return;
        }
        bool accepted = false;
        const QString chosen = QInputDialog::getItem(
            this, QStringLiteral("Note history"),
            QStringLiteral("Who edited this note and when. Pick one to restore it as a new version."),
            labels, 0, false, &accepted);
        const int index = labels.indexOf(chosen);
        if (!accepted || index < 0)
            return;
        const int version = versions.at(index).toObject()
            .value(QStringLiteral("version")).toInt();
        if (version == current)
            return;
        requestNotes(QByteArrayLiteral("POST"),
                     QStringLiteral("/api/notes/%1/versions").arg(cloudId),
                     QJsonObject{{QStringLiteral("version"), version},
                                 {QStringLiteral("baseVersion"), current}},
                     [this](bool restored, const QJsonObject &result,
                            const QString &restoreError) {
            if (!restored) {
                m_notesStatus->setText(QStringLiteral("Restore failed: %1").arg(restoreError));
                return;
            }
            m_selectedNote = result.value(QStringLiteral("note")).toObject();
            m_noteTitle->setText(m_selectedNote.value(QStringLiteral("title")).toString());
            m_noteEditor->setMarkdown(m_selectedNote.value(QStringLiteral("markdown")).toString());
            m_noteDirty = false;
            m_notesStatus->setText(QStringLiteral("Snapshot restored as a new version."));
        });
    });
}

QString NotesPage::collabNoteId() const
{
    const QString cloudId = selectedNoteCloudId();
    if (!forkmesh::notes::isNoteId(cloudId))
        return QString();
    if (m_selectedNoteStorage == QLatin1String("cloud"))
        return cloudId;
    return m_selectedNote.value(QStringLiteral("storage"))
                   .toString(QStringLiteral("local")) == QLatin1String("local")
        ? QString()
        : cloudId;
}

void NotesPage::syncNoteCollab()
{
    const QString id = collabNoteId();
    if (id != m_presenceNoteId) {
        leaveNotePresence();
        m_presenceNoteId = id;
        m_commentsKey = QJsonValue();
        m_lastTypedMs = 0;
        hideNotePresence();
        clearCommentHighlight();
        if (m_commentsPanel)
            m_commentsPanel->setNote(id);
        if (!id.isEmpty()) {
            recordNoteView(id);
            schedulePresencePoll(0);
        }
        else if (m_presenceTimer)
            m_presenceTimer->stop();
    }
    updateCommentControls();
    openPendingComment();
}

void NotesPage::leaveNotePresence()
{
    ++m_presenceSequence;
    m_presenceInFlight = false;
    m_presenceQueued = false;
    if (m_presenceNoteId.isEmpty() || m_presenceSentState.isEmpty())
        return;
    m_presenceSentState.clear();
    requestNotes(QByteArrayLiteral("POST"),
                 QStringLiteral("/api/notes/%1/presence").arg(m_presenceNoteId),
                 QJsonObject{{QStringLiteral("clientId"), m_presenceClientId},
                             {QStringLiteral("state"), QStringLiteral("left")}},
                 [](bool, const QJsonObject &, const QString &) {},
                 m_host.network);
}

void NotesPage::schedulePresencePoll(int delay)
{
    if (m_presenceTimer && !m_presenceNoteId.isEmpty())
        m_presenceTimer->start(delay);
}

QString NotesPage::notePresenceStateNow() const
{
    return forkmesh::notes::notePresenceState(
        forkmesh::notes::noteCanEdit(selectedNoteRole()), m_lastTypedMs,
        QDateTime::currentMSecsSinceEpoch());
}

void NotesPage::noteTyped()
{
    m_lastTypedMs = QDateTime::currentMSecsSinceEpoch();
    if (!m_presenceNoteId.isEmpty() && !m_presencePaused &&
        m_presenceSentState != QLatin1String("editing") &&
        notePresenceStateNow() == QLatin1String("editing"))
        schedulePresencePoll(0);
}

void NotesPage::pollNotePresence()
{
    const QString id = collabNoteId();
    if (id != m_presenceNoteId) {
        syncNoteCollab();
        return;
    }
    if (id.isEmpty() || m_presencePaused)
        return;
    if (m_presenceInFlight) {
        m_presenceQueued = true;
        return;
    }
    const QString state = notePresenceStateNow();
    const int sequence = ++m_presenceSequence;
    m_presenceInFlight = true;
    m_presenceSentState = state;
    requestNotes(QByteArrayLiteral("POST"),
                 QStringLiteral("/api/notes/%1/presence").arg(id),
                 QJsonObject{{QStringLiteral("clientId"), m_presenceClientId},
                             {QStringLiteral("state"), state}},
                 [this, sequence](bool ok, const QJsonObject &payload,
                                  const QString &) {
        if (sequence != m_presenceSequence)
            return;
        m_presenceInFlight = false;
        if (!ok) {
            m_presenceQueued = false;
            hideNotePresence();
            schedulePresencePoll(forkmesh::notes::kPresenceRetryMs);
            return;
        }
        renderNotePresence(forkmesh::notes::notePeers(
            payload.value(QStringLiteral("peers")).toArray()));
        const QJsonValue key = payload.value(QStringLiteral("commentsKey"));
        if (!key.isUndefined() && !key.isNull()) {
            if (!m_commentsKey.isUndefined() && key != m_commentsKey &&
                m_commentsPanel)
                m_commentsPanel->reload();
            m_commentsKey = key;
        }
        schedulePresencePoll(std::exchange(m_presenceQueued, false)
                                 ? 0
                                 : forkmesh::notes::kPresencePollMs);
    });
}

void NotesPage::renderNotePresence(const QList<NotePeer> &peers)
{
    if (!m_presenceBox)
        return;
    if (peers.isEmpty()) {
        hideNotePresence();
        return;
    }
    m_presenceFaces->setPeers(peers);
    const QString line = forkmesh::notes::notePresenceLine(peers);
    m_presenceLine->setText(line);
    QStringList everyone;
    for (const NotePeer &peer : peers)
        everyone << forkmesh::notes::notePeerLabel(peer);
    m_presenceLine->setToolTip(everyone.join(QLatin1Char('\n')));
    m_presenceBox->show();
}

void NotesPage::hideNotePresence()
{
    if (!m_presenceBox)
        return;
    m_presenceFaces->setPeers({});
    m_presenceLine->clear();
    m_presenceBox->hide();
}

void NotesPage::updateCommentControls()
{
    if (!m_commentsButton || !m_commentButton || !m_commentsPanel)
        return;
    const bool collab = !collabNoteId().isEmpty();
    m_commentsButton->setVisible(collab);
    m_commentsButton->setText(forkmesh::notes::commentsButtonLabel(
        collab ? m_commentsPanel->openCount() : 0));
    m_commentsButton->setChecked(collab && m_commentsPanel->isVisible());
    m_commentButton->setVisible(collab);
    bool selected = m_noteEditor->sourceEdit()->textCursor().hasSelection();
    auto *stack = m_noteEditor->findChild<QStackedWidget *>(
        QStringLiteral("markdownStack"));
    auto *preview = m_noteEditor->findChild<QTextBrowser *>(
        QStringLiteral("markdownPreview"));
    if (stack && preview && stack->currentWidget() == preview)
        selected = preview->textCursor().hasSelection();
    m_commentButton->setEnabled(collab && selected);
    if (!collab && m_commentsPanel->isVisible())
        setCommentsPanelOpen(false);
}

void NotesPage::setCommentsPanelOpen(bool open)
{
    if (!m_commentsPanel || !m_noteSplitter)
        return;
    if (open && collabNoteId().isEmpty())
        open = false;
    if (open && !m_commentsPanel->isVisible()) {
        m_commentsPanel->show();
        QList<int> sizes = m_noteSplitter->sizes();
        if (sizes.size() == 3 && sizes.at(2) < m_commentsPanel->minimumWidth()) {
            const int wanted = qMax(m_commentsPanel->minimumWidth(), 320);
            sizes[1] = qMax(240, sizes.at(1) - wanted);
            sizes[2] = wanted;
            m_noteSplitter->setSizes(sizes);
        }
    } else if (!open) {
        m_commentsPanel->hide();
        m_commentsPanel->clearActive();
        clearCommentHighlight();
    }
    if (m_commentsButton)
        m_commentsButton->setChecked(open);
}

CommentQuote NotesPage::noteSelectionQuote(bool *previewMiss) const
{
    if (previewMiss)
        *previewMiss = false;
    const QString markdown = m_noteEditor->markdown();
    auto *stack = m_noteEditor->findChild<QStackedWidget *>(
        QStringLiteral("markdownStack"));
    auto *preview = m_noteEditor->findChild<QTextBrowser *>(
        QStringLiteral("markdownPreview"));
    if (stack && preview && stack->currentWidget() == preview) {
        QString text = preview->textCursor().selectedText();
        text.replace(QChar(0x2029), QLatin1Char('\n'));
        text.replace(QChar(0x2028), QLatin1Char('\n'));
        if (text.trimmed().isEmpty())
            return {};
        const CommentQuote quote =
            forkmesh::notes::commentQuoteForText(markdown, text);
        if (quote.isEmpty() && previewMiss)
            *previewMiss = true;
        return quote;
    }
    const QTextCursor cursor = m_noteEditor->sourceEdit()->textCursor();
    if (!cursor.hasSelection())
        return {};
    return forkmesh::notes::commentQuoteAt(markdown, cursor.selectionStart(),
                                           cursor.selectionEnd());
}

void NotesPage::startNoteComment()
{
    if (collabNoteId().isEmpty()) {
        flashMessage(QStringLiteral("Save this note to the cloud to comment on "
                                    "it."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    bool previewMiss = false;
    const CommentQuote quote = noteSelectionQuote(&previewMiss);
    if (previewMiss) {
        flashMessage(QStringLiteral("That passage is formatted in the Markdown. "
                                    "Select it in Write to comment on it."),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    if (quote.quote.size() > forkmesh::notes::kMaxCommentQuote) {
        flashMessage(QStringLiteral("Select at most %1 characters to comment on.")
                         .arg(forkmesh::notes::kMaxCommentQuote),
                     true, QString(), forkmesh::kLocalOperationNoticeKind);
        return;
    }
    setCommentsPanelOpen(true);
    m_commentsPanel->compose(quote);
}

void NotesPage::clearCommentHighlight()
{
    if (!m_noteEditor)
        return;
    m_noteEditor->sourceEdit()->setExtraSelections({});
    if (auto *preview = m_noteEditor->findChild<QTextBrowser *>(
            QStringLiteral("markdownPreview")))
        preview->setExtraSelections({});
}

void NotesPage::showCommentPassage(const CommentThread *thread)
{
    clearCommentHighlight();
    if (!thread || thread->quote().isEmpty())
        return;
    const forkmesh::notes::CommentAnchor anchor =
        forkmesh::notes::commentThreadAnchor(thread->root,
                                             m_noteEditor->markdown());
    if (!anchor.found())
        return;
    QTextCharFormat mark;
    mark.setBackground(Theme::color(Theme::Role::AccentSubtle));
    QPlainTextEdit *source = m_noteEditor->sourceEdit();
    QTextCursor cursor(source->document());
    cursor.setPosition(anchor.start);
    cursor.setPosition(anchor.end, QTextCursor::KeepAnchor);
    QTextEdit::ExtraSelection sourceMark;
    sourceMark.cursor = cursor;
    sourceMark.format = mark;
    source->setExtraSelections({sourceMark});
    source->setTextCursor(cursor);
    source->ensureCursorVisible();
    auto *stack = m_noteEditor->findChild<QStackedWidget *>(
        QStringLiteral("markdownStack"));
    auto *preview = m_noteEditor->findChild<QTextBrowser *>(
        QStringLiteral("markdownPreview"));
    if (!stack || !preview || stack->currentWidget() != preview)
        return;
    QTextCursor found = preview->document()->find(thread->quote());
    if (found.isNull()) {
        const QString text = forkmesh::notes::previewSearchText(thread->quote());
        if (!text.isEmpty())
            found = preview->document()->find(text);
    }
    if (found.isNull()) {
        m_noteEditor->showWriteArea();
        return;
    }
    QTextEdit::ExtraSelection previewMark;
    previewMark.cursor = found;
    previewMark.format = mark;
    preview->setExtraSelections({previewMark});
    QTextCursor caret = found;
    caret.clearSelection();
    preview->setTextCursor(caret);
    preview->ensureCursorVisible();
}

void NotesPage::showNoteContextMenu(QWidget *view, const QPoint &pos)
{
    QMenu *menu = nullptr;
    QWidget *viewport = nullptr;
    bool selected = false;
    if (auto *plain = qobject_cast<QPlainTextEdit *>(view)) {
        menu = plain->createStandardContextMenu(pos);
        viewport = plain->viewport();
        selected = plain->textCursor().hasSelection();
    } else if (auto *rich = qobject_cast<QTextEdit *>(view)) {
        menu = rich->createStandardContextMenu(pos);
        viewport = rich->viewport();
        selected = rich->textCursor().hasSelection();
    }
    if (!menu)
        return;
    if (!collabNoteId().isEmpty()) {
        auto *comment = new QAction(
            themedOcticon(QStringLiteral("comment"),
                          Theme::color(Theme::Role::FgMuted), 14),
            QStringLiteral("Comment"), menu);
        comment->setObjectName(QStringLiteral("noteCommentAction"));
        comment->setEnabled(selected);
        connect(comment, &QAction::triggered, this, &NotesPage::startNoteComment);
        QAction *first = menu->actions().value(0);
        menu->insertAction(first, comment);
        menu->insertSeparator(first);
    }
    menu->exec(viewport->mapToGlobal(pos));
    delete menu;
}

void NotesPage::showNoteRowMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_notesList ? m_notesList->itemAt(pos) : nullptr;
    if (!item || item->data(kNoteIdRole).toString().isEmpty())
        return;
    const QString id = item->data(kNoteIdRole).toString();
    const QString storage = item->data(kNoteStorageRole).toString();
    const QString cloudId = item->data(kNoteRowCloudIdRole).toString();
    const bool owner = item->data(kNoteRowRoleRole).toString() ==
                       QLatin1String("owner");
    QMenu menu(m_notesList);
    menu.setObjectName(QStringLiteral("noteRowMenu"));
    for (const QString &line : item->data(kNoteRowHeaderRole).toStringList())
        menu.addAction(line)->setEnabled(false);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Open"), this,
                   [this, id, storage] { openNoteFromSearch(id, storage); });
    if (forkmesh::notes::isNoteId(cloudId)) {
        menu.addAction(QStringLiteral("Copy link"), this, [this, cloudId] {
            QGuiApplication::clipboard()->setText(
                forkmesh::notes::noteAccessLink(catalogApiUrl(), cloudId));
            flashMessage(QStringLiteral("Link copied. It opens this note for "
                                        "anyone who has access."),
                         false, QString(), forkmesh::kLocalOperationNoticeKind);
        });
        if (owner)
            menu.addAction(QStringLiteral("Share…"), this, [this, id, storage] {
                runRowAction(QStringLiteral("share"), id, storage);
            });
    }
    if (owner) {
        menu.addSeparator();
        menu.addAction(QStringLiteral("Delete"), this, [this, id, storage] {
            runRowAction(QStringLiteral("delete"), id, storage);
        });
    }
    menu.exec(m_notesList->viewport()->mapToGlobal(pos));
}

void NotesPage::runRowAction(const QString &action, const QString &id,
                             const QString &storage)
{
    if (m_selectedNote.value(QStringLiteral("id")).toString() == id &&
        m_selectedNoteStorage == storage) {
        m_pendingRowAction.clear();
        m_pendingRowNote.clear();
        if (action == QLatin1String("share"))
            shareNote();
        else if (action == QLatin1String("delete"))
            deleteNote();
        return;
    }
    m_pendingRowAction = action;
    m_pendingRowNote = id;
    openNoteFromSearch(id, storage);
}

void NotesPage::runPendingRowAction()
{
    const QString action = std::exchange(m_pendingRowAction, QString());
    const QString note = std::exchange(m_pendingRowNote, QString());
    if (action.isEmpty() ||
        m_selectedNote.value(QStringLiteral("id")).toString() != note)
        return;
    QPointer<NotesPage> guard(this);
    QTimer::singleShot(0, this, [guard, action] {
        if (!guard)
            return;
        if (action == QLatin1String("share"))
            guard->shareNote();
        else if (action == QLatin1String("delete"))
            guard->deleteNote();
    });
}

void NotesPage::openPendingComment()
{
    if (m_pendingCommentId.isEmpty() || !m_commentsPanel ||
        m_pendingCommentNote != collabNoteId())
        return;
    const QString commentId = std::exchange(m_pendingCommentId, QString());
    m_pendingCommentNote.clear();
    setCommentsPanelOpen(true);
    m_commentsPanel->showThread(commentId);
}

namespace {

const forkmesh::market::NativePackageRegistration notesRegistration({
    QStringLiteral("builtin:notes"),
    {QStringLiteral("extension.native")},
    QStringLiteral("Notes"),
    QStringLiteral("note"),
    QStringLiteral("/api/notes"),
    forkmesh::notes::kNotesRequestProof,
    [](const forkmesh::market::NativePackageHost &host, QWidget *parent) {
        return new NotesPage(host, parent);
    },
    QString(),
    nullptr,
    {},
    {},
});

}

}
