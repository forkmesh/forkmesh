
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "DesignTokens.h"
#include "MarkdownEditor.h"
#include "NoteListEntry.h"
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

#include <memory>
#include <utility>

using namespace forkmesh::ui;

namespace {
constexpr int kNoteIdRole = Qt::UserRole;
constexpr int kNoteStorageRole = Qt::UserRole + 1;
constexpr int kNoteRowTitleRole = Qt::UserRole + 2;
constexpr int kNoteRowStorageRole = Qt::UserRole + 3;
constexpr int kNoteRowVisibilityRole = Qt::UserRole + 4;
constexpr int kNoteRowPublicRole = Qt::UserRole + 5;
constexpr int kNoteRowDetailRole = Qt::UserRole + 6;
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

QString noteRowStamp(const QJsonObject &note)
{
    const qint64 stamp = static_cast<qint64>(
        note.value(QStringLiteral("updatedAt")).toDouble());
    if (stamp <= 0)
        return QString();
    const QDateTime when = QDateTime::fromMSecsSinceEpoch(stamp).toLocalTime();
    const QDate today = QDate::currentDate();
    if (when.date() == today)
        return when.toString(QStringLiteral("HH:mm"));
    return when.toString(when.date().year() == today.year()
                             ? QStringLiteral("MMM d")
                             : QStringLiteral("MMM d, yyyy"));
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
        const QFontMetrics titleMetrics(fonts.title);
        const QRect titleRect(content.left(), content.top(), content.width(),
                              titleMetrics.height());
        painter->setFont(fonts.title);
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
                              bool synced)
{
    const QStringList lines = NoteListEntry::lines(
        note, cloud, mode, synced,
        noteTimestampLabel(cloud.isEmpty() ? note : cloud));
    const QString text = lines.join(QLatin1Char('\n'));
    auto *item = new QListWidgetItem(text);
    item->setToolTip(text);
    item->setTextAlignment(Qt::AlignTop | Qt::AlignLeft);

    const bool local = mode == QLatin1String("local");
    const QString visibility =
        NoteListEntry::visibilityLabel(note, cloud, mode, synced);
    QStringList detail;
    if (!local && !synced)
        detail << visibility;
    const QString stamp = noteRowStamp(cloud.isEmpty() ? note : cloud);
    if (!stamp.isEmpty())
        detail << stamp;
    if (lines.size() > 2)
        detail << lines.at(2);
    const QString views = NoteListEntry::viewsLabel(cloud);
    if (!views.isEmpty())
        detail << views;
    item->setData(kNoteRowTitleRole, lines.value(0));
    item->setData(kNoteRowStorageRole,
                  local ? QStringLiteral("Local")
                        : NoteListEntry::storageLabel(mode));
    item->setData(kNoteRowVisibilityRole,
                  !local && synced ? visibility : QString());
    item->setData(kNoteRowPublicRole,
                  !local && synced && visibility == QLatin1String("Public"));
    item->setData(kNoteRowDetailRole, detail);
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
} // namespace

QWidget *MainWindow::buildNotesSection()
{
    auto *page = new QWidget;
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
    // Notes are read when this page opens and when Refresh is pressed. The
    // former collaborator-version timer was a full authenticated note read
    // every few seconds for as long as a synced note sat on screen.
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
            &MainWindow::showNoteVersions);
    heading->addWidget(versions);
    auto *share = new QPushButton(QStringLiteral("Share"));
    share->setObjectName(QStringLiteral("ghostButton"));
    setOcticon(share, "people", 14);
    connect(share, &QPushButton::clicked, this, &MainWindow::shareNote);
    heading->addWidget(share);
    auto *attach = new QPushButton(QStringLiteral("Links"));
    attach->setObjectName(QStringLiteral("ghostButton"));
    setOcticon(attach, "link", 14);
    attach->setToolTip(QStringLiteral(
        "Issues, pull requests and discussions linked to this note"));
    connect(attach, &QPushButton::clicked, this, &MainWindow::showNoteLinks);
    heading->addWidget(attach);
    auto *convert = new QPushButton(QStringLiteral("Convert"));
    convert->setObjectName(QStringLiteral("ghostButton"));
    convert->setCursor(Qt::PointingHandCursor);
    setOcticon(convert, "issue-opened", 14);
    convert->setToolTip(QStringLiteral(
        "Open a task or an issue prefilled with this note. The note itself "
        "stays here, unchanged."));
    auto *convertMenu = new QMenu(convert);
    convertMenu->addAction(QStringLiteral("To an organization task…"), this,
                           &MainWindow::convertNoteToTask);
    convertMenu->addAction(QStringLiteral("To a repository issue…"), this,
                           &MainWindow::convertNoteToIssue);
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
                                            &MainWindow::copyNoteLink);
    QAction *copyPublic = moreMenu->addAction(
        QStringLiteral("Copy public link"), this,
        &MainWindow::copyNotePublicLink);
    QAction *openPublic =
        moreMenu->addAction(QStringLiteral("Open published page"), this, [this] {
            const QString address = selectedNotePublicUrl();
            if (!address.isEmpty())
                QDesktopServices::openUrl(QUrl(address));
        });
    moreMenu->addSeparator();
    moreMenu->addAction(QStringLiteral("Export Markdown…"), this,
                        &MainWindow::exportNote);
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
    connect(remove, &QPushButton::clicked, this, &MainWindow::deleteNote);
    heading->addWidget(remove);
    auto *add = new QPushButton(QStringLiteral("New note"));
    add->setObjectName(QStringLiteral("primaryButton"));
    setOcticon(add, "plus", 14);
    connect(add, &QPushButton::clicked, this, &MainWindow::createNote);
    heading->addWidget(add);
    auto *header = new QVBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(2);
    header->addLayout(heading);
    header->addWidget(m_notesStatus);
    outer->addLayout(header);

    auto *splitter = new QSplitter(Qt::Horizontal);
    splitter->setChildrenCollapsible(false);
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
            &MainWindow::searchCloudNotes);
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
            &MainWindow::copyNotePublicLink);
    toolbar->addWidget(m_notePublicCopy);
    m_noteSaveState = new QLabel;
    m_noteSaveState->setObjectName(QStringLiteral("mutedLabel"));
    m_noteSaveState->setProperty("noteSaveState", true);
    toolbar->addWidget(m_noteSaveState);
    auto *save = new QPushButton(QStringLiteral("Save"));
    save->setObjectName(QStringLiteral("primaryButton"));
    setOcticon(save, "check-circle", 14);
    connect(save, &QPushButton::clicked, this, &MainWindow::saveNote);
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
            &MainWindow::keepMyNoteVersion);
    conflictLayout->addWidget(keepMine);
    auto *takeTheirs = new QPushButton(QStringLiteral("Take theirs"));
    takeTheirs->setObjectName(QStringLiteral("ghostButton"));
    connect(takeTheirs, &QPushButton::clicked, this,
            &MainWindow::takeTheirNoteVersion);
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
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 4);
    outer->addWidget(splitter, 1);

    m_noteAutosaveTimer = new QTimer(page);
    m_noteAutosaveTimer->setSingleShot(true);
    m_noteAutosaveTimer->setInterval(kNoteAutosaveMs);
    connect(m_noteAutosaveTimer, &QTimer::timeout, this,
            &MainWindow::autosaveNote);

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

    return page;
}

QString MainWindow::selectedNoteCloudId() const
{
    return m_selectedNote.value(QStringLiteral("cloudId"))
        .toString(m_selectedNoteStorage == QLatin1String("cloud")
                      ? m_selectedNote.value(QStringLiteral("id")).toString()
                      : QString());
}

QString MainWindow::selectedNotePublicUrl() const
{
    const QString cloudId = selectedNoteCloudId();
    if (cloudId.isEmpty() ||
        m_selectedNote.value(QStringLiteral("visibility")).toString() !=
            QLatin1String("public"))
        return QString();
    return forkmesh::notes::notePublicLink(catalogApiUrl(), cloudId);
}

QString MainWindow::selectedNoteAccessLink() const
{
    return forkmesh::notes::noteAccessLink(catalogApiUrl(),
                                           selectedNoteCloudId());
}

QString MainWindow::selectedNoteRole() const
{
    return m_selectedNote.value(QStringLiteral("role"))
        .toString(QStringLiteral("owner"));
}

QStringList MainWindow::noteRelayHosts() const
{
    const QString host = catalogApiUrl().host().toLower();
    return host.isEmpty() ? QStringList() : QStringList{host};
}

void MainWindow::updateNotePublishState()
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

void MainWindow::requestNotes(const QByteArray &method, const QString &path,
                              const QJsonObject &body,
                              NoteReplyHandler handler)
{
    const qsizetype mark = path.indexOf(QLatin1Char('?'));
    const QString cleanPath = mark < 0 ? path : path.left(mark);
    QUrlQuery query(mark < 0 ? QString() : path.mid(mark + 1));
    QUrl url = catalogApiUrl();
    url.setPath(cleanPath);
    url.setQuery(query);
    const QString token = accountSessionTokenForUrl(url);
    const QByteArray encoded = body.isEmpty()
        ? QByteArray()
        : QJsonDocument(body).toJson(QJsonDocument::Compact);
    if (!m_networkAccess) {
        handler(false, {}, QStringLiteral(
            "Cloud notes are unavailable; local notes remain available."));
        return;
    }
    if (token.isEmpty()) {
        QString node, ts, sig;
        if (!signOrgTaskProof(
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
        ? m_networkAccess->get(request)
        : method == QByteArrayLiteral("POST")
        ? m_networkAccess->post(request, encoded)
        : m_networkAccess->sendCustomRequest(request, method, encoded);
    connect(reply, &QNetworkReply::finished, this,
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

void MainWindow::refreshNotes()
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
        nativePackageSearchIndexChanged();
    });
}

QJsonObject MainWindow::cloudNoteFor(const QJsonObject &note) const
{
    const QString cloudId = note.value(QStringLiteral("cloudId")).toString();
    if (cloudId.isEmpty())
        return {};
    for (const QJsonValue &value : m_cloudNotes) {
        const QJsonObject cloud = value.toObject();
        if (cloud.value(QStringLiteral("id")).toString() == cloudId)
            return cloud;
    }
    // Signed out, or the listing call failed: fall back to the metadata the
    // last successful cloud save mirrored onto the local record.
    return note.contains(QStringLiteral("shares")) ? note : QJsonObject();
}

void MainWindow::renderNotesList(const QString &selectId, bool reselect)
{
    if (!m_notesList)
        return;
    const QString query = m_noteSearch ? m_noteSearch->text() : QString();
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
            !cloudId.isEmpty());
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
            noteListItem(note, note, QStringLiteral("cloud"), true);
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

void MainWindow::selectNote(const QString &id, const QString &storage)
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
    if (id != m_selectedNote.value(QStringLiteral("id")).toString())
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
        return;
    }
    requestNotes(QByteArrayLiteral("GET"),
                 QStringLiteral("/api/notes/%1").arg(id), {},
                 [this](bool ok, const QJsonObject &payload,
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
        if (m_notesList && !m_notesList->currentItem()) {
            const QSignalBlocker blocker(m_notesList);
            for (int row = 0; row < m_notesList->count(); ++row) {
                if (m_notesList->item(row)->data(kNoteIdRole).toString() ==
                    m_selectedNote.value(QStringLiteral("id")).toString())
                    m_notesList->setCurrentRow(row);
            }
        }
    });
}

// A note chosen in the top search box. The list may still be loading (the
// search warms it without opening the page), so the row is highlighted when it
// is there and the editor is filled either way.
void MainWindow::openNoteFromSearch(const QString &id, const QString &storage)
{
    showSection(kNotesSectionIndex);
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

void MainWindow::createNote()
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
    m_noteStorageMode->setCurrentIndex(0);
    m_notePublic->setChecked(false);
    updateNotePublishState();
    clearNoteConflict();
    m_noteAutosaveTimer->stop();
    m_noteDirty = true;
    setNoteSaveState(QStringLiteral("Unsaved changes"));
    renderNoteLinkedItems();
    m_noteTitle->selectAll();
    m_noteTitle->setFocus();
}

bool MainWindow::takeQuickAddNote(const QString &text)
{
    QString markdown = text.trimmed();
    for (const QString &attachment : std::as_const(m_quickAddImages)) {
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
    nativePackageSearchIndexChanged();
    if (m_issueQuickAdd)
        m_issueQuickAdd->clear();
    clearQuickAddImages();
    flashMessage(QStringLiteral("Note saved on this computer: %1").arg(title),
                 false, QString(), forkmesh::kLocalOperationNoticeKind);
    return true;
}

void MainWindow::saveNote()
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
    // Reports whether the note actually reached the disk, so the alert names
    // the destinations that took the write instead of claiming both of them.
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
    // Every save ends here. The heading label and the page's inline notice say
    // the same sentence, so "did that land, and where?" is answered where the operator
    // is already looking instead of in a muted label beside the page title. A
    // failure that still left the note on this computer is an expected
    // degraded state — signed out, or the relay unreachable — so it is filed
    // as a local operation and never becomes a relay error report; only a save
    // that landed nowhere is worth reporting.
    auto announce = [this, title, mode, keepLocal, keepCloud](
                        bool savedLocal, bool savedCloud,
                        const QJsonObject &cloud, const QString &cloudError) {
        const QString text = NoteListEntry::savedAlert(
            title, mode, savedLocal, savedCloud, cloud, cloudError);
        m_notesStatus->setText(text);
        // The heading label is clipped by the buttons beside it, so a sentence
        // that names a failing destination only fits under the cursor.
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
        storeCloudNote(cloud);
        clearNoteConflict();
        if (keepLocal) {
            QJsonObject local = pending;
            local.insert(QStringLiteral("cloudId"),
                         cloud.value(QStringLiteral("id")));
            local.insert(QStringLiteral("cloudVersion"),
                         cloud.value(QStringLiteral("version")));
            local.insert(QStringLiteral("visibility"),
                         cloud.value(QStringLiteral("visibility")));
            // Mirror the sidebar metadata so the row keeps saying who the
            // note is shared with and how often it has been read while the
            // cloud listing is out of reach (offline, or signed out).
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

void MainWindow::storeCloudNote(const QJsonObject &cloud)
{
    const QString id = cloud.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        return;
    for (int i = 0; i < m_cloudNotes.size(); ++i) {
        if (m_cloudNotes.at(i).toObject().value(QStringLiteral("id"))
                .toString() == id) {
            m_cloudNotes.replace(i, cloud);
            return;
        }
    }
    m_cloudNotes.prepend(cloud);
}

bool MainWindow::noteAutosaveApplies() const
{
    return m_noteStorageMode &&
           m_selectedNoteStorage == QLatin1String("cloud") &&
           m_noteStorageMode->currentData().toString() ==
               QLatin1String("cloud") &&
           !selectedNoteCloudId().isEmpty() &&
           forkmesh::notes::noteCanEdit(selectedNoteRole());
}

void MainWindow::setNoteSaveState(const QString &text)
{
    if (m_noteSaveState)
        m_noteSaveState->setText(text);
}

void MainWindow::markNoteClean(const QString &state)
{
    m_noteDirty = false;
    if (m_noteAutosaveTimer)
        m_noteAutosaveTimer->stop();
    setNoteSaveState(m_noteConflict.isEmpty()
                         ? state
                         : QStringLiteral("Not saved — resolve the conflict"));
}

void MainWindow::scheduleNoteAutosave()
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

void MainWindow::autosaveNote()
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
        storeCloudNote(cloud);
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

void MainWindow::showNoteConflict(const QJsonObject &theirs)
{
    m_noteConflict = theirs;
    if (m_noteAutosaveTimer)
        m_noteAutosaveTimer->stop();
    if (m_noteConflictBar)
        m_noteConflictBar->show();
    setNoteSaveState(QStringLiteral("Not saved — resolve the conflict"));
}

void MainWindow::clearNoteConflict()
{
    m_noteConflict = QJsonObject();
    if (m_noteConflictBar)
        m_noteConflictBar->hide();
}

void MainWindow::keepMyNoteVersion()
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

void MainWindow::takeTheirNoteVersion()
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

void MainWindow::copyNoteLink()
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

void MainWindow::copyNotePublicLink()
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

void MainWindow::exportNote()
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

void MainWindow::deleteNote()
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

    // Keep the on-disk mirror until the cloud delete succeeds. Removing it
    // first made a Local + cloud note look deleted even when its cloud copy
    // rejected the request; the next refresh then brought the note back.
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
        // A list request may already be in flight, in which case refreshNotes
        // deliberately does nothing. Render the confirmed deletion now so the
        // old cloud row cannot remain on screen until another manual refresh.
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

void MainWindow::applyNoteShares(const QString &cloudId,
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

void MainWindow::shareNote()
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
                    : userProfileLinkHtml(shareName, QStringLiteral("@") + shareName));
            who->setTextFormat(Qt::RichText);
            who->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
            who->setOpenExternalLinks(false);
            connect(who, &QLabel::linkActivated, this,
                    [this](const QString &href) { openBodyReference(href); });
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

void MainWindow::applyNoteLinks(const QString &cloudId, const QJsonArray &links)
{
    if (selectedNoteCloudId() == cloudId)
        m_selectedNote.insert(QStringLiteral("links"), links);
    renderNoteLinkedItems();
}

void MainWindow::renderNoteLinkedItems()
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

void MainWindow::showNoteLinks()
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

void MainWindow::searchCloudNotes()
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

bool MainWindow::openNoteLink(const QString &href)
{
    const QString id =
        forkmesh::notes::noteIdFromLink(href, noteRelayHosts());
    if (id.isEmpty())
        return false;
    openNoteByCloudId(id);
    return true;
}

void MainWindow::openNoteFromSystemLink(const QString &target)
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

void MainWindow::openNoteByCloudId(const QString &cloudId)
{
    showSection(kNotesSectionIndex);
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

// Both conversions copy the text that is on screen rather than the text that
// last reached storage: converting a draft you have not saved yet is the
// ordinary case, and re-reading the stored copy would silently drop the
// sentence the operator just typed.
bool MainWindow::noteConversionSource(QString *title, QString *markdown)
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

void MainWindow::convertNoteToTask()
{
    QString title, markdown;
    if (!noteConversionSource(&title, &markdown))
        return;
    // Building the tasks page is what applies this organization's departments
    // and members; without it the composer would offer the placeholder list it
    // falls back to and file the task into a department nobody reads. The page
    // also owns the status label every reply to this write reports through.
    ensureSectionBuilt(kOrganizationTasksSectionIndex);
    if (!m_organizationTasksStatus) {
        flashMessage(QStringLiteral(
            "The tasks page is not available on this node yet."), true);
        return;
    }
    composeOrganizationTask(
        QStringLiteral("Convert note to task"),
        NoteListEntry::conversionTitle(title, markdown,
                                       kOrganizationTaskTitleLimit),
        NoteListEntry::conversionDetails(title, markdown,
                                         selectedNotePublicUrl()),
        [this] {
            flashMessage(QStringLiteral(
                "Task created from this note. The note itself is unchanged."));
            // The task page carries any dispatch error that lands after this
            // point.
            showSection(kOrganizationTasksSectionIndex);
        });
}

void MainWindow::convertNoteToIssue()
{
    QString title, markdown;
    if (!noteConversionSource(&title, &markdown))
        return;
    if (m_repositories.isEmpty()) {
        flashMessage(QStringLiteral(
            "Open a repository before converting a note into an issue."), true);
        return;
    }
    int repoIndex = m_repoDetailIndex >= 0 &&
                            m_repoDetailIndex < m_repositories.size()
                        ? m_repoDetailIndex
                        : 0;
    if (m_repositories.size() > 1) {
        QStringList labels;
        labels.reserve(m_repositories.size());
        for (const RepositoryRecord &repo : std::as_const(m_repositories))
            labels << repo.owner + QLatin1Char('/') + repo.name;
        bool ok = false;
        const QString pick = QInputDialog::getItem(
            this, QStringLiteral("Convert note to issue"),
            QStringLiteral("File this note as an issue in:"), labels,
            repoIndex, false, &ok);
        if (!ok || pick.isEmpty())
            return;
        repoIndex = labels.indexOf(pick);
        if (repoIndex < 0)
            return;
    }

    // The composer lives in the repository's Issues tab, so the conversion has
    // to go there; opening the detail page is also what points that tab at the
    // repository the operator just picked.
    if (repoIndex != m_repoDetailIndex)
        openRepoDetail(repoIndex);
    showSection(0);
    if (m_repoDetailTabs && m_repoDetailTabs->button(2))
        m_repoDetailTabs->button(2)->click();
    // A tab built for the first time by that click restores whatever it had
    // selected before, which is not necessarily the pick above.
    if (m_issuesRepoCombo) {
        const int combo = m_issuesRepoCombo->findData(repoIndex);
        if (combo >= 0)
            m_issuesRepoCombo->setCurrentIndex(combo);
    }
    if (issuesRepoIndex() < 0) {
        flashMessage(QStringLiteral(
            "Could not open the issue composer for %1.")
                .arg(m_repositories.at(repoIndex).owner + QLatin1Char('/') +
                     m_repositories.at(repoIndex).name), true);
        return;
    }
    composeNewIssue(
        NoteListEntry::conversionTitle(title, markdown, kIssueTitleLimit),
        NoteListEntry::conversionDetails(title, markdown,
                                         selectedNotePublicUrl()));
}

void MainWindow::showNoteVersions()
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
