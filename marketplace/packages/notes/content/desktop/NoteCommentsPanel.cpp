#include "NoteCommentsPanel.h"

#include "DesignTokens.h"
#include "OcticonIcons.h"
#include "Theme.h"

#include <QApplication>
#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <utility>

namespace forkmesh::notes {

namespace {
constexpr int kAnchorDebounceMs = 500;
constexpr int kQuotePreviewChars = 180;
constexpr int kBoxLines = 3;

class ThreadCard : public QFrame
{
public:
    using QFrame::QFrame;
    std::function<void()> clicked;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && clicked) {
            clicked();
            event->accept();
            return;
        }
        QFrame::mousePressEvent(event);
    }
};

class CommentBoxHelper : public QObject
{
public:
    CommentBoxHelper(QPlainTextEdit *box, std::function<QStringList()> people,
                     std::function<QString()> me, std::function<void()> submit)
        : QObject(box), m_box(box), m_people(std::move(people)),
          m_me(std::move(me)), m_submit(std::move(submit))
    {
        box->installEventFilter(this);
        connect(box, &QPlainTextEdit::textChanged, this, [this] { update(); });
        connect(box, &QPlainTextEdit::cursorPositionChanged, this,
                [this] { update(); });
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_box && event->type() == QEvent::FocusOut)
            hide();
        if (watched != m_box || event->type() != QEvent::KeyPress)
            return QObject::eventFilter(watched, event);
        auto *key = static_cast<QKeyEvent *>(event);
        const bool popup = m_popup && m_popup->isVisible();
        if (popup) {
            switch (key->key()) {
            case Qt::Key_Down:
                m_popup->setCurrentRow(
                    qMin(m_popup->currentRow() + 1, m_popup->count() - 1));
                return true;
            case Qt::Key_Up:
                m_popup->setCurrentRow(qMax(m_popup->currentRow() - 1, 0));
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Tab:
                if (QListWidgetItem *item = m_popup->currentItem())
                    accept(item->text());
                return true;
            case Qt::Key_Escape:
                hide();
                return true;
            default:
                break;
            }
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
            key->modifiers().testFlag(Qt::ControlModifier) && m_submit) {
            m_submit();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void update()
    {
        if (!m_box->hasFocus() || m_box->textCursor().hasSelection()) {
            hide();
            return;
        }
        const QString text = m_box->toPlainText();
        const MentionQuery query =
            mentionQueryAt(text, m_box->textCursor().position());
        const QStringList matches = query.active()
            ? mentionMatches(m_people ? m_people() : QStringList(), query.prefix,
                             m_me ? m_me() : QString())
            : QStringList();
        if (matches.isEmpty()) {
            hide();
            return;
        }
        m_anchor = query.at;
        if (!m_popup) {
            m_popup = new QListWidget(m_box);
            m_popup->setObjectName(QStringLiteral("mentionPopup"));
            m_popup->setWindowFlags(Qt::ToolTip);
            m_popup->setFocusPolicy(Qt::NoFocus);
            m_popup->setUniformItemSizes(true);
            connect(m_popup, &QListWidget::itemClicked, this,
                    [this](QListWidgetItem *item) { accept(item->text()); });
        }
        m_popup->clear();
        m_popup->addItems(matches);
        m_popup->setCurrentRow(0);
        const int rows = qMin(int(matches.size()), 6);
        m_popup->setFixedHeight(rows * m_popup->sizeHintForRow(0) + 4);
        const QFontMetrics metrics(m_popup->font());
        int widest = 0;
        for (const QString &name : matches)
            widest = qMax(widest, metrics.horizontalAdvance(name));
        m_popup->setFixedWidth(qBound(140, widest + 28, 320));
        m_popup->move(m_box->viewport()->mapToGlobal(
            m_box->cursorRect().bottomLeft()));
        m_popup->show();
    }

    void hide()
    {
        if (m_popup)
            m_popup->hide();
        m_anchor = -1;
    }

    void accept(const QString &name)
    {
        if (m_anchor < 0) {
            hide();
            return;
        }
        QTextCursor cursor = m_box->textCursor();
        const int end = cursor.position();
        cursor.setPosition(m_anchor);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        cursor.insertText(QStringLiteral("@%1 ").arg(name));
        m_box->setTextCursor(cursor);
        hide();
    }

    QPlainTextEdit *m_box;
    std::function<QStringList()> m_people;
    std::function<QString()> m_me;
    std::function<void()> m_submit;
    QListWidget *m_popup = nullptr;
    int m_anchor = -1;
};

QString elided(const QString &text, int chars)
{
    const QString flat = text.simplified();
    return flat.size() > chars ? flat.left(chars - 1) + QString(QChar(0x2026))
                               : flat;
}

QToolButton *iconButton(const QString &icon, const QString &tip)
{
    auto *button = new QToolButton;
    button->setAutoRaise(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    button->setIcon(forkmesh::ui::themedOcticon(
        icon, Theme::color(Theme::Role::FgMuted), 14));
    button->setIconSize(QSize(14, 14));
    return button;
}

QPushButton *textButton(const QString &text, const char *objectName)
{
    auto *button = new QPushButton(text);
    button->setObjectName(QString::fromLatin1(objectName));
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

QLabel *mutedLabel(const QString &text = QString())
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QString quoteSheet()
{
    return QStringLiteral("QLabel { border-left: 3px solid %1; padding-left: 6px;"
                          " color: %2; background: transparent; }")
        .arg(Theme::hex(Theme::Role::Accent),
             Theme::hex(Theme::Role::FgSecondary));
}
}

NoteCommentsPanel::NoteCommentsPanel(Hooks hooks, QWidget *parent)
    : QWidget(parent), m_hooks(std::move(hooks))
{
    setObjectName(QStringLiteral("noteCommentsPanel"));
    setMinimumWidth(260);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 0, 0, 0);
    layout->setSpacing(8);

    auto *header = new QHBoxLayout;
    header->setSpacing(4);
    auto *title = new QLabel(QStringLiteral("Comments"));
    title->setStyleSheet(Theme::remapSheet(QStringLiteral("font-weight:600;")));
    header->addWidget(title);
    header->addStretch();
    auto *add = textButton(QStringLiteral("New"), "ghostButton");
    forkmesh::ui::setOcticon(add, QStringLiteral("comment"), 14);
    add->setToolTip(QStringLiteral(
        "Comment on the selected text, or on the whole note when nothing is "
        "selected"));
    connect(add, &QPushButton::clicked, this, [this] {
        if (m_hooks.newComment)
            m_hooks.newComment();
    });
    header->addWidget(add);
    auto *close = iconButton(QStringLiteral("x"), QStringLiteral("Close comments"));
    connect(close, &QToolButton::clicked, this, [this] {
        hide();
        if (m_hooks.closed)
            m_hooks.closed();
    });
    header->addWidget(close);
    layout->addLayout(header);

    m_status = mutedLabel();
    m_status->hide();
    layout->addWidget(m_status);

    m_composer = new QWidget;
    m_composer->setObjectName(QStringLiteral("noteCommentComposer"));
    auto *composerLayout = new QVBoxLayout(m_composer);
    composerLayout->setContentsMargins(0, 0, 0, 0);
    composerLayout->setSpacing(6);
    m_composeQuoteLabel = new QLabel;
    m_composeQuoteLabel->setWordWrap(true);
    m_composeQuoteLabel->setTextFormat(Qt::PlainText);
    composerLayout->addWidget(m_composeQuoteLabel);
    m_composeBox = new QPlainTextEdit;
    m_composeBox->setObjectName(QStringLiteral("noteCommentComposeBox"));
    m_composeBox->setPlaceholderText(
        QStringLiteral("Add a comment. Type @ to mention someone."));
    m_composeBox->setTabChangesFocus(true);
    m_composeBox->setFixedHeight(
        m_composeBox->fontMetrics().lineSpacing() * (kBoxLines + 1) + 12);
    new CommentBoxHelper(
        m_composeBox, [this] { return m_people; },
        [this] { return m_hooks.me ? m_hooks.me() : QString(); },
        [this] { submitThread(); });
    composerLayout->addWidget(m_composeBox);
    m_composeError = mutedLabel();
    m_composeError->hide();
    composerLayout->addWidget(m_composeError);
    auto *composeButtons = new QHBoxLayout;
    composeButtons->addStretch();
    auto *cancel = textButton(QStringLiteral("Cancel"), "ghostButton");
    connect(cancel, &QPushButton::clicked, this, [this] { closeComposer(); });
    composeButtons->addWidget(cancel);
    m_composeSend = textButton(QStringLiteral("Comment"), "primaryButton");
    m_composeSend->setToolTip(QStringLiteral("Post the comment (Ctrl+Enter)"));
    connect(m_composeSend, &QPushButton::clicked, this,
            [this] { submitThread(); });
    composeButtons->addWidget(m_composeSend);
    composerLayout->addLayout(composeButtons);
    m_composer->hide();
    layout->addWidget(m_composer);

    m_scroll = new QScrollArea;
    m_scroll->setObjectName(QStringLiteral("noteCommentsScroll"));
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list = new QWidget;
    m_list->setObjectName(QStringLiteral("noteCommentsList"));
    m_listLayout = new QVBoxLayout(m_list);
    m_listLayout->setContentsMargins(0, 0, 4, 0);
    m_listLayout->setSpacing(8);
    m_scroll->setWidget(m_list);
    layout->addWidget(m_scroll, 1);

    m_anchorTimer = new QTimer(this);
    m_anchorTimer->setSingleShot(true);
    m_anchorTimer->setInterval(kAnchorDebounceMs);
    connect(m_anchorTimer, &QTimer::timeout, this, [this] {
        if (!m_loaded)
            return;
        const QList<CommentThread> threads = commentThreads(
            m_comments, m_hooks.markdown ? m_hooks.markdown() : QString());
        if (commentThreadsSignature(threads) != m_signature)
            render();
    });
    render();
}

QString NoteCommentsPanel::commentsPath(const QString &commentId) const
{
    QString path = QStringLiteral("/api/notes/%1/comments").arg(m_noteId);
    if (!commentId.isEmpty())
        path += QLatin1Char('/') + commentId;
    return path;
}

void NoteCommentsPanel::setNote(const QString &noteId)
{
    if (noteId == m_noteId)
        return;
    m_noteId = noteId;
    ++m_sequence;
    m_comments = QJsonArray();
    m_people.clear();
    m_canResolve = false;
    m_loaded = false;
    m_busy = false;
    m_openCount = 0;
    m_signature.clear();
    m_activeId.clear();
    m_pendingId.clear();
    m_editingId.clear();
    m_showResolved = false;
    m_drafts.clear();
    m_errors.clear();
    closeComposer();
    setStatus(QString());
    render();
    if (m_hooks.countChanged)
        m_hooks.countChanged();
    reload();
}

void NoteCommentsPanel::reload()
{
    if (m_noteId.isEmpty() || !m_hooks.request)
        return;
    const int sequence = ++m_sequence;
    const QString noteId = m_noteId;
    QPointer<NoteCommentsPanel> guard(this);
    m_hooks.request(QByteArrayLiteral("GET"), commentsPath(), {},
                    [guard, sequence, noteId](bool ok, const QJsonObject &payload,
                                              const QString &error) {
        if (!guard || guard->m_sequence != sequence || guard->m_noteId != noteId)
            return;
        if (!ok) {
            if (!guard->m_loaded)
                guard->setStatus(
                    QStringLiteral("Comments are unavailable: %1").arg(error));
            return;
        }
        guard->apply(payload);
    });
}

void NoteCommentsPanel::apply(const QJsonObject &payload)
{
    m_comments = payload.value(QStringLiteral("comments")).toArray();
    m_people.clear();
    for (const QJsonValue &value : payload.value(QStringLiteral("people")).toArray()) {
        const QString name = value.toString().trimmed();
        if (!name.isEmpty())
            m_people << name;
    }
    m_canResolve = payload.value(QStringLiteral("canResolve")).toBool();
    m_loaded = true;
    if (!m_editingId.isEmpty() && threadIdFor(m_editingId).isEmpty())
        m_editingId.clear();
    if (!m_activeId.isEmpty() && threadIdFor(m_activeId).isEmpty())
        m_activeId.clear();
    setStatus(QString());
    render();
    if (m_hooks.countChanged)
        m_hooks.countChanged();
    if (!m_pendingId.isEmpty()) {
        const QString pending = std::exchange(m_pendingId, QString());
        showThread(pending);
    }
}

QString NoteCommentsPanel::threadIdFor(const QString &commentId) const
{
    for (const QJsonValue &value : m_comments) {
        const QJsonObject comment = value.toObject();
        if (comment.value(QStringLiteral("id")).toString() != commentId)
            continue;
        const QString parent = comment.value(QStringLiteral("parentId")).toString();
        return parent.isEmpty() ? commentId : parent;
    }
    return QString();
}

void NoteCommentsPanel::markdownChanged()
{
    if (m_loaded)
        m_anchorTimer->start();
}

void NoteCommentsPanel::setStatus(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

void NoteCommentsPanel::render()
{
    QString focusKey;
    int focusPosition = 0;
    if (auto *focus = qobject_cast<QPlainTextEdit *>(QApplication::focusWidget());
        focus && m_list->isAncestorOf(focus)) {
        focusKey = focus->property("draftKey").toString();
        focusPosition = focus->textCursor().position();
    }
    const int scroll = m_scroll->verticalScrollBar()->value();

    m_cards.clear();
    m_boxes.clear();
    while (QLayoutItem *item = m_listLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    const QList<CommentThread> threads = commentThreads(
        m_comments, m_hooks.markdown ? m_hooks.markdown() : QString());
    m_signature = commentThreadsSignature(threads);
    m_openCount = openThreadCount(threads);
    const int resolved = int(threads.size()) - m_openCount;

    if (!m_loaded) {
        if (!m_noteId.isEmpty())
            m_listLayout->addWidget(mutedLabel(QStringLiteral("Loading comments…")));
    } else if (m_openCount == 0) {
        m_listLayout->addWidget(mutedLabel(
            resolved ? QStringLiteral("No open comments.")
                     : QStringLiteral("No comments yet. Select text in the note "
                                      "and choose Comment, or press New to "
                                      "comment on the whole note.")));
    }
    for (const CommentThread &thread : threads) {
        if (!thread.resolved)
            m_listLayout->addWidget(threadCard(thread));
    }
    if (resolved > 0) {
        auto *fold = new QToolButton;
        fold->setObjectName(QStringLiteral("noteCommentsResolvedToggle"));
        fold->setAutoRaise(true);
        fold->setCursor(Qt::PointingHandCursor);
        fold->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        fold->setArrowType(m_showResolved ? Qt::DownArrow : Qt::RightArrow);
        fold->setText(resolvedFolderLabel(resolved));
        connect(fold, &QToolButton::clicked, this, [this] {
            m_showResolved = !m_showResolved;
            render();
        });
        m_listLayout->addWidget(fold, 0, Qt::AlignLeft);
        if (m_showResolved) {
            for (const CommentThread &thread : threads) {
                if (thread.resolved)
                    m_listLayout->addWidget(threadCard(thread));
            }
        }
    }
    m_listLayout->addStretch(1);
    restyleCards();

    if (QPlainTextEdit *box = m_boxes.value(focusKey)) {
        box->setFocus();
        QTextCursor cursor = box->textCursor();
        cursor.setPosition(qMin(focusPosition, int(box->toPlainText().size())));
        box->setTextCursor(cursor);
    }
    QPointer<QScrollArea> scrollArea(m_scroll);
    QTimer::singleShot(0, this, [scrollArea, scroll] {
        if (scrollArea)
            scrollArea->verticalScrollBar()->setValue(scroll);
    });
}

QPlainTextEdit *NoteCommentsPanel::commentBox(const QString &placeholder,
                                              const QString &key,
                                              std::function<void()> submit)
{
    auto *box = new QPlainTextEdit;
    box->setObjectName(QStringLiteral("noteCommentBox"));
    box->setProperty("draftKey", key);
    box->setPlaceholderText(placeholder);
    box->setTabChangesFocus(true);
    box->setPlainText(m_drafts.value(key));
    box->setFixedHeight(box->fontMetrics().lineSpacing() * kBoxLines + 12);
    connect(box, &QPlainTextEdit::textChanged, this, [this, box, key] {
        m_drafts.insert(key, box->toPlainText());
    });
    new CommentBoxHelper(
        box, [this] { return m_people; },
        [this] { return m_hooks.me ? m_hooks.me() : QString(); },
        std::move(submit));
    m_boxes.insert(key, box);
    return box;
}

QWidget *NoteCommentsPanel::commentBlock(const QJsonObject &comment, bool root)
{
    const QString id = comment.value(QStringLiteral("id")).toString();
    const QString author = comment.value(QStringLiteral("author")).toString();
    const QString me = m_hooks.me ? m_hooks.me() : QString();
    const bool mine = sameAccount(author, me);
    const bool deletable =
        canDeleteComment(author, me, m_hooks.noteOwner && m_hooks.noteOwner());

    auto *block = new QWidget;
    block->setObjectName(root ? QStringLiteral("noteComment")
                              : QStringLiteral("noteCommentReply"));
    auto *layout = new QVBoxLayout(block);
    layout->setContentsMargins(root ? 0 : 12, 0, 0, 0);
    layout->setSpacing(2);

    auto *header = new QHBoxLayout;
    header->setSpacing(6);
    auto *who = new QLabel(author.isEmpty() ? QStringLiteral("Someone")
                                            : QStringLiteral("@") + author);
    who->setStyleSheet(Theme::remapSheet(
        QStringLiteral("font-weight:600; background:transparent;")));
    header->addWidget(who);
    QString when = commentTimeLabel(commentStamp(comment, "createdAt"),
                                    QDateTime::currentMSecsSinceEpoch());
    if (commentStamp(comment, "editedAt") > 0)
        when += QStringLiteral(" · edited");
    auto *time = new QLabel(when);
    time->setObjectName(QStringLiteral("mutedLabel"));
    time->setToolTip(QDateTime::fromMSecsSinceEpoch(
                         commentStamp(comment, "createdAt"))
                         .toLocalTime()
                         .toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    header->addWidget(time);
    header->addStretch();
    if (mine && m_editingId != id) {
        auto *edit = iconButton(QStringLiteral("pencil"), QStringLiteral("Edit"));
        connect(edit, &QToolButton::clicked, this, [this, id, comment] {
            m_editingId = id;
            m_drafts.insert(QStringLiteral("edit:") + id,
                            comment.value(QStringLiteral("body")).toString());
            render();
            if (QPlainTextEdit *box = m_boxes.value(QStringLiteral("edit:") + id)) {
                box->setFocus();
                box->moveCursor(QTextCursor::End);
            }
        });
        header->addWidget(edit);
    }
    if (deletable && m_editingId != id) {
        auto *remove = iconButton(
            QStringLiteral("trash"),
            mine ? QStringLiteral("Delete")
                 : QStringLiteral("Delete (you own this note)"));
        connect(remove, &QToolButton::clicked, this,
                [this, comment] { removeComment(comment); });
        header->addWidget(remove);
    }
    layout->addLayout(header);

    if (mine && m_editingId == id) {
        const QString key = QStringLiteral("edit:") + id;
        QPlainTextEdit *box = commentBox(QStringLiteral("Edit your comment"), key,
                                         [this, id] { submitEdit(id); });
        layout->addWidget(box);
        auto *buttons = new QHBoxLayout;
        buttons->addStretch();
        auto *cancel = textButton(QStringLiteral("Cancel"), "ghostButton");
        connect(cancel, &QPushButton::clicked, this, [this, key] {
            m_editingId.clear();
            m_drafts.remove(key);
            render();
        });
        buttons->addWidget(cancel);
        auto *save = textButton(QStringLiteral("Save"), "primaryButton");
        connect(save, &QPushButton::clicked, this, [this, id] { submitEdit(id); });
        buttons->addWidget(save);
        layout->addLayout(buttons);
    } else {
        QStringList mentions;
        for (const QJsonValue &value : comment.value(QStringLiteral("mentions")).toArray())
            mentions << value.toString();
        auto *body = new QLabel(commentBodyHtml(
            comment.value(QStringLiteral("body")).toString(), mentions,
            Theme::hex(Theme::Role::Accent)));
        body->setObjectName(QStringLiteral("noteCommentBody"));
        body->setTextFormat(Qt::RichText);
        body->setWordWrap(true);
        body->setStyleSheet(QStringLiteral("background:transparent;"));
        layout->addWidget(body);
    }
    return block;
}

QWidget *NoteCommentsPanel::threadCard(const CommentThread &thread)
{
    const QString id = thread.id();
    const QString me = m_hooks.me ? m_hooks.me() : QString();
    auto *card = new ThreadCard;
    card->setObjectName(QStringLiteral("noteCommentThread"));
    card->setProperty("threadId", id);
    card->setCursor(Qt::PointingHandCursor);
    card->clicked = [this, id] { activate(id, false); };
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(6);

    if (thread.place == CommentPlace::WholeNote) {
        auto *scope = mutedLabel(QStringLiteral("On the whole note"));
        layout->addWidget(scope);
    } else {
        auto *quote = new QLabel;
        quote->setObjectName(QStringLiteral("noteCommentQuote"));
        quote->setWordWrap(true);
        quote->setTextFormat(Qt::RichText);
        const QString preview = elided(thread.quote(), kQuotePreviewChars).toHtmlEscaped();
        quote->setText(thread.place == CommentPlace::Detached
                           ? QStringLiteral("<s>%1</s><br><i>Removed from the note</i>")
                                 .arg(preview)
                           : preview);
        quote->setToolTip(thread.quote());
        quote->setStyleSheet(quoteSheet());
        layout->addWidget(quote);
    }

    layout->addWidget(commentBlock(thread.root, true));
    for (const QJsonObject &reply : thread.replies)
        layout->addWidget(commentBlock(reply, false));

    const bool canToggle = canResolveThread(m_canResolve, thread.author(), me);
    if (thread.resolved) {
        auto *row = new QHBoxLayout;
        QString by = thread.root.value(QStringLiteral("resolvedBy")).toString();
        auto *note = mutedLabel(
            (by.isEmpty() ? QStringLiteral("Resolved")
                          : QStringLiteral("Resolved by @%1").arg(by)) +
            QStringLiteral(" · ") +
            commentTimeLabel(commentStamp(thread.root, "resolvedAt"),
                             QDateTime::currentMSecsSinceEpoch()));
        row->addWidget(note, 1);
        if (canToggle) {
            auto *reopen = textButton(QStringLiteral("Reopen"), "ghostButton");
            forkmesh::ui::setOcticon(reopen, QStringLiteral("issue-reopened"), 14);
            connect(reopen, &QPushButton::clicked, this,
                    [this, id] { setResolved(id, false); });
            row->addWidget(reopen);
        }
        layout->addLayout(row);
    } else {
        layout->addWidget(commentBox(QStringLiteral("Reply…"),
                                     QStringLiteral("reply:") + id,
                                     [this, id] { submitReply(id); }));
        auto *row = new QHBoxLayout;
        if (canToggle) {
            auto *resolve = textButton(QStringLiteral("Resolve"), "ghostButton");
            forkmesh::ui::setOcticon(resolve, QStringLiteral("check"), 14);
            resolve->setToolTip(QStringLiteral("Mark this thread as done; it "
                                               "moves under Resolved"));
            connect(resolve, &QPushButton::clicked, this,
                    [this, id] { setResolved(id, true); });
            row->addWidget(resolve);
        }
        row->addStretch();
        auto *reply = textButton(QStringLiteral("Reply"), "ghostButton");
        reply->setToolTip(QStringLiteral("Post the reply (Ctrl+Enter)"));
        connect(reply, &QPushButton::clicked, this,
                [this, id] { submitReply(id); });
        row->addWidget(reply);
        layout->addLayout(row);
    }
    const QString error = m_errors.value(id);
    if (!error.isEmpty()) {
        auto *label = mutedLabel(error);
        label->setStyleSheet(QStringLiteral("color:%1; background:transparent;")
                                 .arg(Theme::hex(Theme::Role::Danger)));
        layout->addWidget(label);
    }
    m_cards.insert(id, card);
    return card;
}

void NoteCommentsPanel::restyleCards()
{
    for (auto it = m_cards.constBegin(); it != m_cards.constEnd(); ++it) {
        const bool active = it.key() == m_activeId;
        it.value()->setStyleSheet(
            QStringLiteral("QFrame#noteCommentThread { background: %1;"
                           " border: 1px solid %2; border-radius: 8px; }")
                .arg(Theme::hex(Theme::Role::SurfaceRaised),
                     Theme::hex(active ? Theme::Role::Accent
                                       : Theme::Role::Border)));
    }
}

void NoteCommentsPanel::activate(const QString &threadId, bool reveal)
{
    m_activeId = threadId;
    restyleCards();
    const QList<CommentThread> threads = commentThreads(
        m_comments, m_hooks.markdown ? m_hooks.markdown() : QString());
    const CommentThread *found = nullptr;
    for (const CommentThread &thread : threads) {
        if (thread.id() == threadId)
            found = &thread;
    }
    if (m_hooks.showPassage)
        m_hooks.showPassage(found);
    if (!reveal)
        return;
    QPointer<NoteCommentsPanel> guard(this);
    QTimer::singleShot(0, this, [guard, threadId] {
        if (!guard)
            return;
        if (QWidget *card = guard->m_cards.value(threadId))
            guard->m_scroll->ensureWidgetVisible(card, 0, 12);
    });
}

void NoteCommentsPanel::clearActive()
{
    if (m_activeId.isEmpty())
        return;
    m_activeId.clear();
    restyleCards();
}

void NoteCommentsPanel::showThread(const QString &commentId)
{
    if (commentId.isEmpty())
        return;
    if (!m_loaded) {
        m_pendingId = commentId;
        return;
    }
    const QString threadId = threadIdFor(commentId);
    if (threadId.isEmpty()) {
        setStatus(QStringLiteral("That comment is no longer in this note."));
        return;
    }
    for (const QJsonValue &value : std::as_const(m_comments)) {
        const QJsonObject comment = value.toObject();
        if (comment.value(QStringLiteral("id")).toString() == threadId &&
            commentStamp(comment, "resolvedAt") > 0 && !m_showResolved) {
            m_showResolved = true;
            render();
        }
    }
    activate(threadId, true);
}

void NoteCommentsPanel::compose(const CommentQuote &quote)
{
    m_composeQuote = quote;
    if (quote.isEmpty()) {
        m_composeQuoteLabel->setStyleSheet(QString());
        m_composeQuoteLabel->setObjectName(QStringLiteral("mutedLabel"));
        m_composeQuoteLabel->setText(QStringLiteral("Comment on the whole note"));
    } else {
        m_composeQuoteLabel->setObjectName(QStringLiteral("noteCommentQuote"));
        m_composeQuoteLabel->setStyleSheet(quoteSheet());
        m_composeQuoteLabel->setText(elided(quote.quote, kQuotePreviewChars));
        m_composeQuoteLabel->setToolTip(quote.quote);
    }
    m_composeError->hide();
    m_composer->show();
    m_composeBox->setFocus();
}

void NoteCommentsPanel::closeComposer()
{
    m_composeQuote = CommentQuote();
    m_composeBox->clear();
    m_composeError->hide();
    m_composeSend->setEnabled(true);
    m_composer->hide();
}

void NoteCommentsPanel::send(const QByteArray &method, const QString &path,
                             const QJsonObject &body, const QString &errorKey,
                             std::function<void(const QJsonObject &)> done)
{
    if (!m_hooks.request || m_noteId.isEmpty() || m_busy)
        return;
    m_busy = true;
    m_errors.remove(errorKey);
    const QString noteId = m_noteId;
    QPointer<NoteCommentsPanel> guard(this);
    m_hooks.request(method, path, body,
                    [guard, noteId, errorKey, done = std::move(done)](
                        bool ok, const QJsonObject &payload, const QString &error) {
        if (!guard || guard->m_noteId != noteId)
            return;
        guard->m_busy = false;
        if (!ok) {
            const QString text = commentError(
                payload.value(QStringLiteral("error")).toString(),
                QStringLiteral("That did not work: %1").arg(error));
            if (errorKey.isEmpty()) {
                guard->m_composeError->setText(text);
                guard->m_composeError->show();
                guard->m_composeSend->setEnabled(true);
            } else {
                guard->m_errors.insert(errorKey, text);
                guard->render();
            }
            if (payload.value(QStringLiteral("error")).toString() ==
                QLatin1String("comment_not_found"))
                guard->reload();
            return;
        }
        guard->apply(payload);
        if (done)
            done(payload);
    });
}

void NoteCommentsPanel::submitThread()
{
    const QString body = m_composeBox->toPlainText().trimmed();
    if (body.isEmpty())
        return;
    if (body.size() > kMaxCommentBody) {
        m_composeError->setText(commentError(QStringLiteral("invalid_comment"), {}));
        m_composeError->show();
        return;
    }
    if (m_busy)
        return;
    QJsonObject request = commentQuoteFields(m_composeQuote);
    request.insert(QStringLiteral("body"), body);
    m_composeSend->setEnabled(false);
    send(QByteArrayLiteral("POST"), commentsPath(), request, QString(),
         [this](const QJsonObject &payload) {
        closeComposer();
        showThread(payload.value(QStringLiteral("comment")).toObject()
                       .value(QStringLiteral("id")).toString());
    });
}

void NoteCommentsPanel::submitReply(const QString &threadId)
{
    const QString key = QStringLiteral("reply:") + threadId;
    const QString body = m_drafts.value(key).trimmed();
    if (body.isEmpty())
        return;
    if (body.size() > kMaxCommentBody) {
        m_errors.insert(threadId, commentError(QStringLiteral("invalid_comment"), {}));
        render();
        return;
    }
    send(QByteArrayLiteral("POST"), commentsPath(),
         QJsonObject{{QStringLiteral("body"), body},
                     {QStringLiteral("parentId"), threadId}},
         threadId, [this, key, threadId](const QJsonObject &) {
        m_drafts.remove(key);
        if (QPlainTextEdit *box = m_boxes.value(key)) {
            const QSignalBlocker blocker(box);
            box->clear();
        }
        activate(threadId, true);
    });
}

void NoteCommentsPanel::submitEdit(const QString &commentId)
{
    const QString key = QStringLiteral("edit:") + commentId;
    const QString body = m_drafts.value(key).trimmed();
    const QString threadId = threadIdFor(commentId);
    if (body.isEmpty())
        return;
    if (body.size() > kMaxCommentBody) {
        m_errors.insert(threadId, commentError(QStringLiteral("invalid_comment"), {}));
        render();
        return;
    }
    send(QByteArrayLiteral("PATCH"), commentsPath(commentId),
         QJsonObject{{QStringLiteral("body"), body}}, threadId,
         [this, key](const QJsonObject &) {
        m_editingId.clear();
        m_drafts.remove(key);
        render();
    });
}

void NoteCommentsPanel::removeComment(const QJsonObject &comment)
{
    const QString id = comment.value(QStringLiteral("id")).toString();
    const bool thread = comment.value(QStringLiteral("parentId")).toString().isEmpty();
    const QString author = comment.value(QStringLiteral("author")).toString();
    if (QMessageBox::question(
            this, QStringLiteral("Delete comment"),
            deleteCommentQuestion(
                thread, thread ? replyCount(m_comments, id) : 0,
                sameAccount(author, m_hooks.me ? m_hooks.me() : QString()),
                author)) != QMessageBox::Yes)
        return;
    send(QByteArrayLiteral("DELETE"), commentsPath(id), {}, threadIdFor(id));
}

void NoteCommentsPanel::setResolved(const QString &threadId, bool resolved)
{
    send(QByteArrayLiteral("PATCH"), commentsPath(threadId),
         QJsonObject{{QStringLiteral("resolved"), resolved}}, threadId,
         [this, threadId, resolved](const QJsonObject &) {
        if (resolved && m_activeId == threadId) {
            m_activeId.clear();
            restyleCards();
            if (m_hooks.showPassage)
                m_hooks.showPassage(nullptr);
        }
    });
}

}
