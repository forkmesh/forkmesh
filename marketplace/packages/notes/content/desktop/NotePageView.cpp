#include "NotePageView.h"

#include "DesignTokens.h"
#include "MarkdownDocument.h"
#include "MarkdownRender.h"
#include "NotePages.h"
#include "NoteTools.h"
#include "OcticonIcons.h"
#include "Theme.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>
#include <QVBoxLayout>

namespace forkmesh::notes {

namespace {
constexpr int kEntryIndexRole = Qt::UserRole + 40;
constexpr int kNoteAnchorProperty = QTextFormat::UserProperty + 7;

QIcon dotIcon(const QColor &color)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(5, 5, 6, 6));
    return QIcon(pixmap);
}

QPushButton *ghost(const QString &text, const char *icon)
{
    auto *button = new QPushButton(text);
    button->setObjectName(QStringLiteral("ghostButton"));
    button->setCursor(Qt::PointingHandCursor);
    forkmesh::ui::setOcticon(button, QString::fromLatin1(icon), 14);
    return button;
}

QString noteAnchor(const QString &noteId)
{
    return QStringLiteral("note-") + noteId;
}
}

NotePageView::NotePageView(Hooks hooks, QWidget *parent)
    : QWidget(parent), m_hooks(std::move(hooks))
{
    setObjectName(QStringLiteral("notePageView"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 0, 0, 0);
    layout->setSpacing(6);

    auto *header = new QHBoxLayout;
    header->setSpacing(6);
    m_back = ghost(QStringLiteral("Notes"), "arrow-left");
    m_back->setToolTip(QStringLiteral("Back to the note editor"));
    connect(m_back, &QPushButton::clicked, this, [this] {
        if (m_hooks.back)
            m_hooks.back();
    });
    header->addWidget(m_back);
    m_title = new QLabel;
    m_title->setObjectName(QStringLiteral("notePageTitle"));
    m_title->setTextFormat(Qt::PlainText);
    QSizePolicy titlePolicy = m_title->sizePolicy();
    titlePolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    m_title->setSizePolicy(titlePolicy);
    header->addWidget(m_title, 1);
    m_openEditor = ghost(QStringLiteral("Open in editor"), "pencil");
    m_openEditor->setToolTip(QStringLiteral("Open the picked note in the note editor"));
    connect(m_openEditor, &QPushButton::clicked, this, [this] {
        if (!m_current.isEmpty() && m_hooks.openNote)
            m_hooks.openNote(m_current);
    });
    header->addWidget(m_openEditor);
    m_copyLink = ghost(QStringLiteral("Copy link"), "link");
    connect(m_copyLink, &QPushButton::clicked, this, [this] {
        if (m_hooks.copyLink)
            m_hooks.copyLink(m_pageId, m_current);
    });
    header->addWidget(m_copyLink);
    m_share = ghost(QStringLiteral("Share"), "people");
    connect(m_share, &QPushButton::clicked, this, [this] {
        if (m_hooks.share)
            m_hooks.share();
    });
    header->addWidget(m_share);
    m_customize = ghost(QStringLiteral("Customize"), "gear");
    connect(m_customize, &QPushButton::clicked, this, [this] {
        if (m_hooks.customize)
            m_hooks.customize();
    });
    header->addWidget(m_customize);
    layout->addLayout(header);

    m_accentBar = new QFrame;
    m_accentBar->setObjectName(QStringLiteral("notePageAccent"));
    m_accentBar->setFixedHeight(3);
    layout->addWidget(m_accentBar);

    m_description = new QLabel;
    m_description->setObjectName(QStringLiteral("notePageDescription"));
    m_description->setTextFormat(Qt::MarkdownText);
    m_description->setWordWrap(true);
    m_description->setOpenExternalLinks(true);
    m_description->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_description->hide();
    layout->addWidget(m_description);

    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("mutedLabel"));
    m_status->setWordWrap(true);
    m_status->hide();
    layout->addWidget(m_status);

    auto *split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    m_outline = new QListWidget;
    m_outline->setObjectName(QStringLiteral("notePageOutline"));
    m_outline->setMinimumWidth(180);
    m_outline->setSelectionMode(QAbstractItemView::SingleSelection);
    m_outline->setUniformItemSizes(true);
    connect(m_outline, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        pickEntry(item->data(kEntryIndexRole).toInt(), false);
    });
    connect(m_outline, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) {
        pickEntry(item->data(kEntryIndexRole).toInt(), true);
    });
    split->addWidget(m_outline);
    m_content = new QTextBrowser;
    m_content->setObjectName(QStringLiteral("notePageContent"));
    m_content->setOpenLinks(false);
    connect(m_content, &QTextBrowser::anchorClicked, this, [](const QUrl &url) {
        if (url.scheme() == QLatin1String("https") ||
            url.scheme() == QLatin1String("http"))
            QDesktopServices::openUrl(url);
    });
    split->addWidget(m_content);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    layout->addWidget(split, 1);
    renderHeader();
}

void NotePageView::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

void NotePageView::showPage(const QString &pageId, const QString &noteId)
{
    if (pageId != m_pageId) {
        m_pageId = pageId;
        m_page = QJsonObject();
        m_notes = QJsonObject();
        m_markdown.clear();
        m_fetching.clear();
        m_collapsed.clear();
        m_current.clear();
        renderHeader();
        renderOutline();
        m_content->clear();
        setStatus(QStringLiteral("Loading the page…"));
    }
    m_wantedNote = noteId;
    reload();
}

void NotePageView::reload()
{
    if (m_pageId.isEmpty() || !m_hooks.request)
        return;
    const int sequence = ++m_sequence;
    const QString pageId = m_pageId;
    QPointer<NotePageView> guard(this);
    m_hooks.request(QByteArrayLiteral("GET"),
                    QStringLiteral("/api/notes/pages/%1?content=1").arg(pageId), {},
                    [guard, sequence, pageId](bool ok, const QJsonObject &payload,
                                              const QString &error) {
        if (!guard || guard->m_sequence != sequence || guard->m_pageId != pageId)
            return;
        if (!ok) {
            guard->setStatus(pageError(
                payload.value(QStringLiteral("error")).toString(),
                QStringLiteral("This page could not be opened: %1").arg(error)));
            return;
        }
        guard->apply(payload);
    });
}

void NotePageView::apply(const QJsonObject &payload)
{
    const bool first = m_page.isEmpty();
    m_page = payload.value(QStringLiteral("page")).toObject();
    m_notes = payload.value(QStringLiteral("notes")).toObject();
    for (auto it = m_notes.constBegin(); it != m_notes.constEnd(); ++it) {
        const QJsonObject note = it.value().toObject();
        if (note.contains(QStringLiteral("markdown")))
            m_markdown.insert(it.key(), note.value(QStringLiteral("markdown")).toString());
    }
    if (first)
        m_collapsed = collapsedOutlineSections(
            m_page.value(QStringLiteral("outline")).toArray());
    setStatus(QString());
    const QJsonArray outline = m_page.value(QStringLiteral("outline")).toArray();
    const QStringList ids = outlineNoteIds(outline, m_notes);
    if (!m_wantedNote.isEmpty() && ids.contains(m_wantedNote))
        m_current = m_wantedNote;
    else if (!m_wantedNote.isEmpty())
        setStatus(QStringLiteral("You don't have access to that note, or it is "
                                 "not on this page."));
    m_wantedNote.clear();
    if (!ids.contains(m_current))
        m_current = ids.value(0);
    renderHeader();
    renderOutline();
    renderContent();
    if (m_hooks.loaded)
        m_hooks.loaded(m_page);
}

void NotePageView::renderHeader()
{
    const QString accent = pageAccentHex(m_page.value(QStringLiteral("accent")).toString());
    m_title->setText(m_page.isEmpty()
                         ? QString()
                         : noteDisplayTitle(m_page.value(QStringLiteral("emoji")).toString(),
                                            pageTitle(m_page), QStringLiteral("Untitled page")));
    if (m_title->text().isEmpty() && !m_page.isEmpty())
        m_title->setText(pageTitle(m_page));
    m_title->setToolTip(m_title->text());
    m_title->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 600; color: %1;")
                               .arg(accent));
    m_accentBar->setStyleSheet(
        QStringLiteral("QFrame#notePageAccent { background: %1; border: none; "
                       "border-radius: 1px; }")
            .arg(accent));
    const QString description =
        m_page.value(QStringLiteral("description")).toString().trimmed();
    m_description->setText(description);
    m_description->setVisible(!description.isEmpty());
    const QString role = m_page.value(QStringLiteral("role")).toString();
    m_customize->setVisible(noteCanEdit(role));
    m_share->setVisible(role == QLatin1String("owner"));
    m_copyLink->setEnabled(!m_pageId.isEmpty());
    m_openEditor->setEnabled(!m_current.isEmpty());
}

void NotePageView::renderOutline()
{
    const QJsonArray outline = m_page.value(QStringLiteral("outline")).toArray();
    const bool showEmoji = m_page.value(QStringLiteral("showEmoji")).toBool(true);
    const QColor muted = Theme::color(Theme::Role::FgMuted);
    const QColor accent(pageAccentHex(m_page.value(QStringLiteral("accent")).toString()));
    const int scroll = m_outline->verticalScrollBar()->value();
    m_outline->clear();
    for (const int index : visibleOutlineRows(outline, m_collapsed)) {
        const QJsonObject entry = outline.at(index).toObject();
        const QString kind = outlineKind(entry);
        const QString indent(outlineDepth(entry) * 3, QChar(0x2002));
        auto *item = new QListWidgetItem;
        item->setData(kEntryIndexRole, index);
        QString label = outlineEntryLabel(entry, m_notes, showEmoji);
        QFont font = m_outline->font();
        if (kind == QLatin1String("section")) {
            const bool closed = m_collapsed.contains(entry.value(QStringLiteral("id")).toString());
            label = QString(closed ? QChar(0x25B8) : QChar(0x25BE)) + QLatin1Char(' ') + label;
            font.setWeight(QFont::DemiBold);
            item->setToolTip(closed ? QStringLiteral("Show the entries under this section")
                                    : QStringLiteral("Hide the entries under this section"));
        } else if (kind == QLatin1String("link")) {
            item->setIcon(forkmesh::ui::themedOcticon(QStringLiteral("link-external"), muted, 14));
            item->setToolTip(entry.value(QStringLiteral("url")).toString());
        } else if (outlineEntryLocked(entry, m_notes)) {
            item->setIcon(forkmesh::ui::themedOcticon(QStringLiteral("lock"), muted, 14));
            item->setForeground(muted);
            item->setToolTip(QStringLiteral("You don't have access to this note"));
        } else {
            const QString noteId = entry.value(QStringLiteral("noteId")).toString();
            const QJsonObject note = m_notes.value(noteId).toObject();
            const bool unread = note.value(QStringLiteral("unread")).toBool();
            item->setIcon(unread ? dotIcon(accent) : dotIcon(Qt::transparent));
            if (unread)
                font.setWeight(QFont::DemiBold);
            const QString edited = noteLastEditLabel(
                qint64(note.value(QStringLiteral("updatedAt")).toDouble()),
                note.value(QStringLiteral("lastEditedBy")).toString(),
                m_hooks.me ? m_hooks.me() : QString(), QDateTime::currentDateTime());
            item->setToolTip(unread ? edited + QStringLiteral("\nChanged since you last opened it")
                                    : edited);
            if (noteId == m_current)
                m_outline->setCurrentItem(item);
        }
        item->setFont(font);
        item->setText(indent + label);
        m_outline->addItem(item);
    }
    if (!m_page.isEmpty() && outline.isEmpty()) {
        auto *empty = new QListWidgetItem(
            noteCanEdit(m_page.value(QStringLiteral("role")).toString())
                ? QStringLiteral("No notes yet. Use Customize to add some.")
                : QStringLiteral("This page has no notes yet."));
        empty->setFlags(Qt::NoItemFlags);
        empty->setData(kEntryIndexRole, -1);
        m_outline->addItem(empty);
    }
    m_outline->verticalScrollBar()->setValue(scroll);
}

QString NotePageView::markdownFor(const QString &noteId) const
{
    return m_markdown.value(noteId);
}

void NotePageView::renderContent()
{
    const QJsonArray outline = m_page.value(QStringLiteral("outline")).toArray();
    const bool continuous =
        pageLayout(m_page.value(QStringLiteral("layout")).toString()) ==
        QLatin1String("continuous");
    const QStringList ids = continuous ? outlineNoteIds(outline, m_notes)
                          : m_current.isEmpty() ? QStringList()
                                                : QStringList{m_current};
    QStringList missing;
    for (const QString &id : ids) {
        if (!m_markdown.contains(id))
            missing << id;
    }
    const int scroll = m_content->verticalScrollBar()->value();
    QTextDocument *document = m_content->document();
    document->clear();
    QTextCursor cursor(document);
    const QString me = m_hooks.me ? m_hooks.me() : QString();
    for (int i = 0; i < ids.size(); ++i) {
        const QString id = ids.at(i);
        const QJsonObject note = m_notes.value(id).toObject();
        QTextBlockFormat headingBlock;
        headingBlock.setHeadingLevel(1);
        headingBlock.setProperty(kNoteAnchorProperty, id);
        if (i > 0)
            headingBlock.setTopMargin(28);
        QTextCharFormat headingChar;
        headingChar.setFontWeight(QFont::Bold);
        headingChar.setAnchor(true);
        headingChar.setAnchorNames({noteAnchor(id)});
        if (i == 0) {
            cursor.setBlockFormat(headingBlock);
            cursor.setCharFormat(headingChar);
        } else {
            cursor.insertBlock(headingBlock, headingChar);
        }
        cursor.insertText(noteDisplayTitle(note.value(QStringLiteral("emoji")).toString(),
                                           note.value(QStringLiteral("title")).toString()
                                               .trimmed().isEmpty()
                                               ? QStringLiteral("Untitled note")
                                               : note.value(QStringLiteral("title")).toString()),
                          headingChar);
        QTextCharFormat metaChar;
        metaChar.setForeground(Theme::color(Theme::Role::FgMuted));
        cursor.insertBlock(QTextBlockFormat(), metaChar);
        cursor.insertText(noteLastEditLabel(
                              qint64(note.value(QStringLiteral("updatedAt")).toDouble()),
                              note.value(QStringLiteral("lastEditedBy")).toString(), me,
                              QDateTime::currentDateTime()),
                          metaChar);
        cursor.insertBlock(QTextBlockFormat(), QTextCharFormat());
        if (m_markdown.contains(id))
            cursor.insertMarkdown(MarkdownRender::forPreview(m_markdown.value(id)));
        else
            cursor.insertText(QStringLiteral("Loading…"), metaChar);
    }
    if (ids.isEmpty() && !m_page.isEmpty()) {
        QTextCharFormat metaChar;
        metaChar.setForeground(Theme::color(Theme::Role::FgMuted));
        cursor.insertText(continuous ? QStringLiteral("No notes you can open are on this page.")
                                     : QStringLiteral("Pick a note in the outline."),
                          metaChar);
    }
    forkmesh::ui::styleReadmeDocument(m_content);
    QTextCursor anchors(document);
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const QString id = block.blockFormat().property(kNoteAnchorProperty).toString();
        if (id.isEmpty())
            continue;
        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorNames({noteAnchor(id)});
        anchors.setPosition(block.position());
        anchors.setPosition(block.position() + block.length() - 1, QTextCursor::KeepAnchor);
        anchors.mergeCharFormat(format);
    }
    if (continuous)
        m_content->verticalScrollBar()->setValue(scroll);
    else
        m_content->verticalScrollBar()->setValue(0);
    m_openEditor->setEnabled(!m_current.isEmpty());
    fetchMissing(missing);
}

void NotePageView::fetchMissing(const QStringList &ids)
{
    if (!m_hooks.request)
        return;
    for (const QString &id : ids) {
        if (m_fetching.contains(id))
            continue;
        m_fetching.insert(id);
        const QString pageId = m_pageId;
        QPointer<NotePageView> guard(this);
        m_hooks.request(QByteArrayLiteral("GET"), QStringLiteral("/api/notes/%1").arg(id),
                        {}, [guard, pageId, id](bool ok, const QJsonObject &payload,
                                                const QString &) {
            if (!guard || guard->m_pageId != pageId)
                return;
            guard->m_fetching.remove(id);
            guard->m_markdown.insert(
                id, ok ? payload.value(QStringLiteral("note")).toObject()
                             .value(QStringLiteral("markdown")).toString()
                       : QStringLiteral("*This note could not be loaded.*"));
            if (guard->m_fetching.isEmpty())
                guard->renderContent();
        });
    }
}

void NotePageView::pickNote(const QString &noteId)
{
    if (noteId.isEmpty())
        return;
    m_current = noteId;
    const bool continuous =
        pageLayout(m_page.value(QStringLiteral("layout")).toString()) ==
        QLatin1String("continuous");
    if (continuous) {
        m_content->scrollToAnchor(noteAnchor(noteId));
        m_openEditor->setEnabled(true);
        return;
    }
    renderContent();
    QJsonObject note = m_notes.value(noteId).toObject();
    if (note.value(QStringLiteral("unread")).toBool()) {
        note.insert(QStringLiteral("unread"), false);
        m_notes.insert(noteId, note);
        renderOutline();
        if (m_hooks.noteViewed)
            m_hooks.noteViewed(noteId);
    }
}

void NotePageView::pickEntry(int index, bool open)
{
    const QJsonArray outline = m_page.value(QStringLiteral("outline")).toArray();
    if (index < 0 || index >= outline.size())
        return;
    const QJsonObject entry = outline.at(index).toObject();
    const QString kind = outlineKind(entry);
    if (kind == QLatin1String("section")) {
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (m_collapsed.contains(id))
            m_collapsed.remove(id);
        else
            m_collapsed.insert(id);
        renderOutline();
        return;
    }
    if (kind == QLatin1String("link")) {
        const QString url = entry.value(QStringLiteral("url")).toString();
        if (isOutlineUrl(url))
            QDesktopServices::openUrl(QUrl(url));
        return;
    }
    if (outlineEntryLocked(entry, m_notes)) {
        setStatus(QStringLiteral("You don't have access to this note. Ask the "
                                 "page's owner to share it with you."));
        return;
    }
    setStatus(QString());
    const QString noteId = entry.value(QStringLiteral("noteId")).toString();
    pickNote(noteId);
    if (open && m_hooks.openNote)
        m_hooks.openNote(noteId);
}

}
