#include "MarkdownEditor.h"
#include "MarkdownDocument.h"
#include "MarkdownRender.h"
#include "PromptSpellCheckHighlighter.h"

#include <QClipboard>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QListWidget>
#include <QMimeData>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QTemporaryFile>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QToolButton>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>

#include <algorithm>

namespace {
const QStringList kImageSuffixes = {"png", "jpg", "jpeg", "gif", "webp", "bmp", "svg"};

bool isImagePath(const QString &path)
{
    return kImageSuffixes.contains(QFileInfo(path).suffix().toLower());
}

QString pendingImagePlaceholder(int index)
{
    return QStringLiteral("forkmesh-pending-image:%1").arg(index);
}
}  // namespace

MarkdownEditor::MarkdownEditor(QWidget *parent) : QWidget(parent)
{
    m_source = new QPlainTextEdit;
    m_source->setObjectName("markdownSource");
    m_source->setAcceptDrops(false);  // drops are handled by this widget instead
    (new PromptSpellCheckHighlighter(m_source->document()))->attachTo(m_source);
    m_preview = new QTextBrowser;
    m_preview->setObjectName("markdownPreview");
    m_preview->setOpenExternalLinks(true);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(0);
    m_writeTab = new QPushButton("Write");
    m_previewTab = new QPushButton("Preview");
    for (QPushButton *tab : {m_writeTab, m_previewTab}) {
        tab->setObjectName("markdownTab");
        tab->setCheckable(true);
        tab->setCursor(Qt::PointingHandCursor);
    }
    m_writeTab->setChecked(true);
    header->addWidget(m_writeTab);
    header->addWidget(m_previewTab);
    header->addSpacing(8);

    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    toolbar->setSpacing(2);
    struct Tool { const char *label; const char *left; const char *right; };
    const Tool tools[] = {
        {"H", "\n## ", ""},
        {"B", "**", "**"},
        {"I", "*", "*"},
        {"Code", "`", "`"},
        {"List", "\n- ", ""},
        {"<>", "\n```\n", "\n```\n"},
        {"Link", "[", "](https://)"},
    };
    for (const Tool &t : tools) {
        auto *btn = new QToolButton;
        btn->setObjectName("markdownTool");
        btn->setText(QString::fromUtf8(t.label));
        const QString left = QString::fromUtf8(t.left);
        const QString right = QString::fromUtf8(t.right);
        connect(btn, &QToolButton::clicked, this,
                [this, left, right]() { wrapSelection(left, right); });
        toolbar->addWidget(btn);
    }
    auto *imgBtn = new QToolButton;
    imgBtn->setObjectName("markdownTool");
    imgBtn->setText("Image");
    connect(imgBtn, &QToolButton::clicked, this, &MarkdownEditor::chooseImage);
    toolbar->addWidget(imgBtn);
    toolbar->addStretch();
    header->addLayout(toolbar, 1);

    m_stack = new QStackedWidget;
    m_stack->setObjectName("markdownStack");
    m_stack->addWidget(m_source);
    m_stack->addWidget(m_preview);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(header);
    layout->addWidget(m_stack, 1);

    connect(m_source, &QPlainTextEdit::textChanged, this,
            &MarkdownEditor::updatePreview);
    connect(m_source, &QPlainTextEdit::textChanged, this,
            &MarkdownEditor::updateMentionPopup);
    connect(m_source, &QPlainTextEdit::cursorPositionChanged, this,
            &MarkdownEditor::updateMentionPopup);
    connect(m_writeTab, &QPushButton::clicked, this, &MarkdownEditor::showWrite);
    connect(m_previewTab, &QPushButton::clicked, this, &MarkdownEditor::showPreview);

    setAcceptDrops(true);
    m_source->viewport()->installEventFilter(this);
    m_source->installEventFilter(this);
}

void MarkdownEditor::setMarkdown(const QString &text)
{
    m_source->setPlainText(text);
    updatePreview();
}

QString MarkdownEditor::markdown() const { return m_source->toPlainText(); }

void MarkdownEditor::setPreviewBasePath(const QString &dir)
{
    m_basePath = dir;
    if (!dir.isEmpty())
        m_preview->setSearchPaths({dir});
    updatePreview();
}

void MarkdownEditor::setPlaceholderText(const QString &text)
{
    m_source->setPlaceholderText(text);
}

void MarkdownEditor::focusEditor() { m_source->setFocus(); }

void MarkdownEditor::showWriteArea() { showWrite(); }

void MarkdownEditor::addImageFile(const QString &path)
{
    if (path.isEmpty() || !isImagePath(path))
        return;
    int index = m_attachments.indexOf(path);
    if (index < 0) {
        index = m_attachments.size();
        m_attachments.append(path);
        m_attachmentPlaceholders.append(pendingImagePlaceholder(index));
    }
    const QString placeholder = m_attachmentPlaceholders.value(index);
    const QString name = QFileInfo(path).fileName();
    QTextCursor cursor = m_source->textCursor();
    cursor.insertText(QStringLiteral("\n![%1](%2)\n").arg(name, placeholder));
    m_source->setFocus();
}

void MarkdownEditor::clearPendingAttachments()
{
    m_attachments.clear();
    m_attachmentPlaceholders.clear();
}

void MarkdownEditor::showWrite()
{
    m_stack->setCurrentWidget(m_source);
    m_writeTab->setChecked(true);
    m_previewTab->setChecked(false);
    m_source->setFocus();
}

void MarkdownEditor::showPreview()
{
    hideMentionPopup();
    updatePreview();
    m_stack->setCurrentWidget(m_preview);
    forkmesh::ui::styleReadmeDocument(m_preview);
    m_writeTab->setChecked(false);
    m_previewTab->setChecked(true);
}

void MarkdownEditor::setMentionCandidates(const QStringList &names)
{
    m_mentionCandidates = names;
    if (names.isEmpty())
        hideMentionPopup();
}

void MarkdownEditor::updateMentionPopup()
{
    if (m_mentionCandidates.isEmpty() || m_stack->currentWidget() != m_source) {
        hideMentionPopup();
        return;
    }
    QTextCursor cursor = m_source->textCursor();
    if (cursor.hasSelection()) {
        hideMentionPopup();
        return;
    }

    const QString block = cursor.block().text();
    const int col = cursor.positionInBlock();
    int at = -1;
    for (int i = col - 1; i >= 0; --i) {
        const QChar c = block.at(i);
        if (c == QChar('@')) {
            at = i;
            break;
        }
        if (!(c.isLetterOrNumber() || c == QChar('-')))
            break;
    }
    if (at < 0) {
        hideMentionPopup();
        return;
    }
    if (at > 0) {
        const QChar prev = block.at(at - 1);
        if (prev.isLetterOrNumber() || prev == QChar('-') || prev == QChar('_')) {
            hideMentionPopup();
            return;
        }
    }

    const QString prefix = block.mid(at + 1, col - at - 1);
    QStringList matches;
    for (const QString &name : std::as_const(m_mentionCandidates)) {
        if (prefix.isEmpty() || name.startsWith(prefix, Qt::CaseInsensitive)) {
            matches.append(name);
            if (matches.size() >= 50)
                break;
        }
    }
    if (matches.isEmpty()) {
        hideMentionPopup();
        return;
    }

    m_mentionAnchor = cursor.block().position() + at;

    if (!m_mentionPopup) {
        m_mentionPopup = new QListWidget(this);
        m_mentionPopup->setObjectName("mentionPopup");
        m_mentionPopup->setWindowFlags(Qt::ToolTip);
        m_mentionPopup->setFocusPolicy(Qt::NoFocus);
        m_mentionPopup->setUniformItemSizes(true);
        connect(m_mentionPopup, &QListWidget::itemClicked, this,
                [this](QListWidgetItem *item) { acceptMention(item->text()); });
    }
    m_mentionPopup->clear();
    m_mentionPopup->addItems(matches);
    m_mentionPopup->setCurrentRow(0);

    const int rowH = m_mentionPopup->sizeHintForRow(0);
    const int rows = qMin(matches.size(), 6);
    m_mentionPopup->setFixedHeight(rows * rowH + 4);
    const QFontMetrics fm(m_mentionPopup->font());
    int textW = 0;
    for (const QString &name : std::as_const(matches))
        textW = qMax(textW, fm.horizontalAdvance(name));
    m_mentionPopup->setFixedWidth(qBound(160, textW + 28, 360));

    const QRect cr = m_source->cursorRect();
    m_mentionPopup->move(m_source->viewport()->mapToGlobal(cr.bottomLeft()));
    m_mentionPopup->show();
}

void MarkdownEditor::hideMentionPopup()
{
    if (m_mentionPopup)
        m_mentionPopup->hide();
    m_mentionAnchor = -1;
}

void MarkdownEditor::acceptMention(const QString &name)
{
    if (m_mentionAnchor < 0) {
        hideMentionPopup();
        return;
    }
    QTextCursor cursor = m_source->textCursor();
    cursor.setPosition(m_mentionAnchor);
    cursor.setPosition(m_source->textCursor().position(), QTextCursor::KeepAnchor);
    cursor.insertText(QStringLiteral("@%1 ").arg(name));
    m_source->setTextCursor(cursor);
    hideMentionPopup();
    m_source->setFocus();
}

bool MarkdownEditor::handleMentionKey(QKeyEvent *event)
{
    if (!m_mentionPopup || !m_mentionPopup->isVisible())
        return false;
    switch (event->key()) {
    case Qt::Key_Down:
        m_mentionPopup->setCurrentRow(
            qMin(m_mentionPopup->currentRow() + 1, m_mentionPopup->count() - 1));
        return true;
    case Qt::Key_Up:
        m_mentionPopup->setCurrentRow(qMax(m_mentionPopup->currentRow() - 1, 0));
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Tab:
        if (QListWidgetItem *item = m_mentionPopup->currentItem())
            acceptMention(item->text());
        return true;
    case Qt::Key_Escape:
        hideMentionPopup();
        return true;
    default:
        return false;
    }
}

void MarkdownEditor::updatePreview()
{
    QString previewMarkdown = m_source->toPlainText();
    const int count = std::min(m_attachments.size(), m_attachmentPlaceholders.size());
    QList<int> order;
    order.reserve(count);
    for (int i = 0; i < count; ++i)
        order.append(i);
    std::sort(order.begin(), order.end(), [this](int a, int b) {
        return m_attachmentPlaceholders.at(a).size() >
               m_attachmentPlaceholders.at(b).size();
    });
    for (int i : order) {
        previewMarkdown.replace(m_attachmentPlaceholders.at(i),
                                QUrl::fromLocalFile(m_attachments.at(i)).toString());
    }
    m_preview->setMarkdown(MarkdownRender::forPreview(previewMarkdown));
    if (m_stack && m_stack->currentWidget() == m_preview)
        forkmesh::ui::styleReadmeDocument(m_preview);
}

void MarkdownEditor::wrapSelection(const QString &left, const QString &right)
{
    QTextCursor cursor = m_source->textCursor();
    const QString selected = cursor.selectedText();
    cursor.insertText(left + selected + right);
    m_source->setFocus();
}

void MarkdownEditor::chooseImage()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Add image", QString(),
        "Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg)");
    for (const QString &f : files)
        addImageFile(f);
}

bool MarkdownEditor::pasteImageFromClipboard()
{
    const QClipboard *clipboard = QGuiApplication::clipboard();
    if (!clipboard)
        return false;
    const QMimeData *mime = clipboard->mimeData();
    if (!mime || !mime->hasImage())
        return false;

    const QVariant data = mime->imageData();
    QImage image = qvariant_cast<QImage>(data);
    if (image.isNull()) {
        const QPixmap pixmap = qvariant_cast<QPixmap>(data);
        if (!pixmap.isNull())
            image = pixmap.toImage();
    }
    if (image.isNull())
        return false;

    QTemporaryFile file(QDir::tempPath() + "/forkmesh-paste-XXXXXX.png");
    file.setAutoRemove(false);
    if (!file.open())
        return false;
    const QString path = file.fileName();
    if (!image.save(&file, "PNG")) {
        file.close();
        QFile::remove(path);
        return false;
    }
    file.close();
    addImageFile(path);
    showPreview();
    return true;
}

bool MarkdownEditor::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_source) {
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->matches(QKeySequence::Paste) && pasteImageFromClipboard()) {
                key->accept();
                return true;
            }
            if (handleMentionKey(key))
                return true;
        }
        if (event->type() == QEvent::FocusOut)
            hideMentionPopup();
    }
    if (obj == m_source->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto *de = static_cast<QDragEnterEvent *>(event);
            if (de->mimeData()->hasUrls()) {
                de->acceptProposedAction();
                return true;
            }
        } else if (event->type() == QEvent::Drop) {
            auto *de = static_cast<QDropEvent *>(event);
            if (de->mimeData()->hasUrls()) {
                bool any = false;
                for (const QUrl &url : de->mimeData()->urls()) {
                    if (url.isLocalFile() && isImagePath(url.toLocalFile())) {
                        addImageFile(url.toLocalFile());
                        any = true;
                    }
                }
                if (any) {
                    de->acceptProposedAction();
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}
