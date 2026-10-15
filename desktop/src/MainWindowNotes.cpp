
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "DesignTokens.h"
#include "MarkdownEditor.h"
#include "NoteListEntry.h"
#include "NoteRequestProof.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
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
#include <QPushButton>
#include <QSaveFile>
#include <QSizePolicy>
#include <QSplitter>
#include <QStandardPaths>
#include <QSpinBox>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>

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
        forkmesh::paths::dataRoot() +
        QStringLiteral("/notes");
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
    auto *attach = new QPushButton(QStringLiteral("Attach"));
    attach->setObjectName(QStringLiteral("ghostButton"));
    setOcticon(attach, "link", 14);
    connect(attach, &QPushButton::clicked, this,
            &MainWindow::attachNoteConversation);
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
        if (item)
            selectNote(item->data(kNoteIdRole).toString(),
                       item->data(kNoteStorageRole).toString());
    });
    splitter->addWidget(m_notesList);

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
    auto *save = new QPushButton(QStringLiteral("Save"));
    save->setObjectName(QStringLiteral("primaryButton"));
    setOcticon(save, "check-circle", 14);
    connect(save, &QPushButton::clicked, this, &MainWindow::saveNote);
    toolbar->addWidget(save);
    editorLayout->addLayout(toolbar);
    m_noteEditor = new MarkdownEditor;
    m_noteEditor->setPlaceholderText(QStringLiteral(
        "Write Markdown. Preview it with the Preview tab."));
    editorLayout->addWidget(m_noteEditor, 1);
    splitter->addWidget(editorHost);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 4);
    outer->addWidget(splitter, 1);

    connect(m_noteTitle, &QLineEdit::textChanged, this,
            [this] { m_noteDirty = true; });
    connect(m_noteEditor->sourceEdit(), &QPlainTextEdit::textChanged, this,
            [this] { m_noteDirty = true; });
    connect(m_noteStorageMode, &QComboBox::currentIndexChanged, this, [this] {
        m_noteDirty = true;
        updateNotePublishState();
    });
    connect(m_notePublic, &QCheckBox::toggled, this, [this] {
        m_noteDirty = true;
        updateNotePublishState();
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
    QUrl url = catalogApiUrl();
    url.setPath(QStringLiteral("/notes/%1").arg(cloudId));
    url.setQuery(QString());
    return url.toString();
}

void MainWindow::updateNotePublishState()
{
    if (!m_notePublic || !m_noteStorageMode)
        return;
    const QString mode = m_noteStorageMode->currentData().toString();
    const QString role = m_selectedNote.value(QStringLiteral("role"))
                             .toString(QStringLiteral("owner"));
    const bool publishable = mode != QLatin1String("local") &&
                             role == QLatin1String("owner");
    m_notePublic->setEnabled(publishable);
    if (!publishable)
        m_notePublic->setChecked(false);
    if (!m_notePublicLink)
        return;
    const QString address = selectedNotePublicUrl();
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
    QUrl url = catalogApiUrl();
    url.setPath(path);
    url.setQuery(QString());
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
                forkmesh::notes::noteRequestResource(method, path, encoded),
                &node, &ts, &sig)) {
            handler(false, {}, QStringLiteral(
                "Sign in to your ForkMesh account on this device to load "
                "cloud notes; local notes remain available."));
            return;
        }
        QUrlQuery query;
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

void MainWindow::renderNotesList(const QString &selectId)
{
    if (!m_notesList)
        return;
    m_notesList->blockSignals(true);
    m_notesList->clear();
    QSet<QString> cloudIds;
    for (const QJsonValue &value : m_localNotes) {
        const QJsonObject note = value.toObject();
        const QString cloudId = note.value(QStringLiteral("cloudId")).toString();
        if (!cloudId.isEmpty())
            cloudIds.insert(cloudId);
        QListWidgetItem *item = noteListItem(
            note, cloudNoteFor(note),
            note.value(QStringLiteral("storage")).toString(
                QStringLiteral("local")),
            !cloudId.isEmpty());
        item->setData(kNoteIdRole,
                      note.value(QStringLiteral("id")).toString());
        item->setData(kNoteStorageRole, QStringLiteral("local"));
        m_notesList->addItem(item);
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
        m_notesList->addItem(item);
        if (id == selectId)
            m_notesList->setCurrentItem(item);
    }
    m_notesList->blockSignals(false);
    if (!m_notesList->currentItem() && m_notesList->count())
        m_notesList->setCurrentRow(0);
    if (m_notesList->currentItem())
        selectNote(m_notesList->currentItem()->data(kNoteIdRole).toString(),
                   m_notesList->currentItem()->data(kNoteStorageRole).toString());
}

void MainWindow::selectNote(const QString &id, const QString &storage)
{
    if (m_noteDirty && !m_selectedNote.isEmpty()) {
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Unsaved note"),
            QStringLiteral("Discard the unsaved changes to this note?"));
        if (answer != QMessageBox::Yes)
            return;
    }
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
        m_noteDirty = false;
        return;
    }
    requestNotes(QByteArrayLiteral("GET"),
                 QStringLiteral("/api/notes/%1").arg(id), {},
                 [this](bool ok, const QJsonObject &payload,
                        const QString &error) {
        if (!ok) {
            m_notesStatus->setText(QStringLiteral("Note unavailable: %1").arg(error));
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
        m_noteDirty = false;
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
    m_noteDirty = true;
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
        m_noteDirty = false;
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
        {QStringLiteral("visibility"),
         m_notePublic->isChecked() ? QStringLiteral("public")
                                   : QStringLiteral("private")},
    };
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
    requestNotes(method, path, requestBody,
                 [this, keepLocal, saveLocal, announce, pending](
                     bool ok, const QJsonObject &payload,
                     const QString &error) mutable {
        if (!ok) {
            announce(saveLocal(pending), false, {}, error);
            return;
        }
        QJsonObject cloud = payload.value(QStringLiteral("note")).toObject();
        const QString id = cloud.value(QStringLiteral("id")).toString();
        int cloudIndex = -1;
        for (int i = 0; i < m_cloudNotes.size(); ++i) {
            if (m_cloudNotes.at(i).toObject().value(QStringLiteral("id"))
                    .toString() == id)
                cloudIndex = i;
        }
        if (cloudIndex >= 0)
            m_cloudNotes.replace(cloudIndex, cloud);
        else
            m_cloudNotes.prepend(cloud);
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
        } else {
            const int localIndex = localNoteIndex(
                m_localNotes, pending.value(QStringLiteral("id")).toString());
            if (localIndex >= 0) {
                m_localNotes.removeAt(localIndex);
                writeLocalNotes(m_localNotes);
            }
            m_selectedNote = cloud;
            m_selectedNoteStorage = QStringLiteral("cloud");
            m_noteDirty = false;
            updateNotePublishState();
            renderNotesList(cloud.value(QStringLiteral("id")).toString());
            announce(false, true, cloud, QString());
        }
    });
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

void MainWindow::shareNote()
{
    const QString cloudId = selectedNoteCloudId();
    if (cloudId.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("Share note"),
            QStringLiteral("Choose Cloud only or Local + cloud and save before sharing."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Share note"));
    auto *form = new QFormLayout(&dialog);
    QComboBox type; type.addItems({QStringLiteral("user"), QStringLiteral("organization")});
    QLineEdit name;
    QComboBox role; role.addItems({QStringLiteral("editor"), QStringLiteral("viewer")});
    form->addRow(QStringLiteral("Share with"), &type);
    form->addRow(QStringLiteral("Name"), &name);
    form->addRow(QStringLiteral("Permission"), &role);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    requestNotes(QByteArrayLiteral("POST"),
                 QStringLiteral("/api/notes/%1/shares").arg(cloudId),
                 QJsonObject{{QStringLiteral("type"), type.currentText()},
                             {QStringLiteral("name"), name.text().trimmed()},
                             {QStringLiteral("role"), role.currentText()}},
                 [this, cloudId](bool ok, const QJsonObject &payload,
                                 const QString &error) {
        if (!ok) {
            m_notesStatus->setText(
                QStringLiteral("Share failed: %1").arg(error));
            return;
        }
        const QJsonArray shares = payload.value(QStringLiteral("shares")).toArray();
        m_selectedNote.insert(QStringLiteral("shares"), shares);
        for (int i = 0; i < m_cloudNotes.size(); ++i) {
            QJsonObject cloud = m_cloudNotes.at(i).toObject();
            if (cloud.value(QStringLiteral("id")).toString() != cloudId)
                continue;
            cloud.insert(QStringLiteral("shares"), shares);
            m_cloudNotes.replace(i, cloud);
            break;
        }
        renderNotesList(m_selectedNote.value(QStringLiteral("id")).toString());
        m_notesStatus->setText(QStringLiteral("Note shared."));
    });
}

void MainWindow::attachNoteConversation()
{
    if (m_selectedNote.isEmpty())
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Attach PR, issue, or discussion"));
    auto *form = new QFormLayout(&dialog);
    QLineEdit owner, repo;
    QComboBox kind; kind.addItems({QStringLiteral("issue"), QStringLiteral("pull"), QStringLiteral("discussion")});
    QSpinBox number; number.setRange(1, 999999999);
    form->addRow(QStringLiteral("Owner"), &owner);
    form->addRow(QStringLiteral("Repository"), &repo);
    form->addRow(QStringLiteral("Kind"), &kind);
    form->addRow(QStringLiteral("Number"), &number);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const QJsonObject link{{QStringLiteral("owner"), owner.text().trimmed()},
                           {QStringLiteral("repo"), repo.text().trimmed()},
                           {QStringLiteral("kind"), kind.currentText()},
                           {QStringLiteral("number"), number.value()}};
    const QString cloudId = selectedNoteCloudId();
    if (!cloudId.isEmpty()) {
        requestNotes(QByteArrayLiteral("POST"),
                     QStringLiteral("/api/notes/%1/links").arg(cloudId), link,
                     [this](bool ok, const QJsonObject &, const QString &error) {
            m_notesStatus->setText(ok ? QStringLiteral("Conversation attached.")
                                      : QStringLiteral("Attach failed: %1").arg(error));
        });
    }
    QJsonArray links = m_selectedNote.value(QStringLiteral("links")).toArray();
    links.append(link);
    m_selectedNote.insert(QStringLiteral("links"), links);
    m_noteDirty = true;
    if (cloudId.isEmpty())
        saveNote();
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
