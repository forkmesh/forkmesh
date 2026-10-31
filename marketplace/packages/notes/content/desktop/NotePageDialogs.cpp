#include "NotePageDialogs.h"

#include "DesignTokens.h"
#include "NotePages.h"
#include "NoteTools.h"
#include "OcticonIcons.h"
#include "Theme.h"

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

namespace forkmesh::notes {

namespace {
constexpr int kOutlineIdRole = Qt::UserRole + 50;
constexpr int kPickNoteRole = Qt::UserRole + 51;

QIcon accentIcon(const QString &accent)
{
    QPixmap pixmap(14, 14);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(pageAccentHex(accent)));
    painter.drawEllipse(QRectF(1, 1, 12, 12));
    return QIcon(pixmap);
}

QPushButton *button(const QString &text, const char *objectName,
                    const char *icon = nullptr)
{
    auto *result = new QPushButton(text);
    result->setObjectName(QString::fromLatin1(objectName));
    result->setCursor(Qt::PointingHandCursor);
    if (icon)
        forkmesh::ui::setOcticon(result, QString::fromLatin1(icon), 14);
    return result;
}

QLabel *muted(const QString &text = QString())
{
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QLabel *bold(const QString &text)
{
    auto *label = new QLabel(text);
    label->setStyleSheet(Theme::remapSheet(QStringLiteral("font-weight:600;")));
    return label;
}
}

bool pickNoteEmoji(QWidget *parent, const QString &current, QString *chosen)
{
    QDialog dialog(parent);
    dialog.setObjectName(QStringLiteral("noteEmojiPicker"));
    dialog.setWindowTitle(QStringLiteral("Choose an emoji"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *grid = new QGridLayout;
    grid->setSpacing(2);
    const QStringList &emoji = commonNoteEmoji();
    for (int i = 0; i < emoji.size(); ++i) {
        auto *cell = new QToolButton;
        cell->setText(emoji.at(i));
        cell->setAutoRaise(true);
        cell->setCursor(Qt::PointingHandCursor);
        cell->setFixedSize(36, 36);
        QFont font = cell->font();
        font.setPixelSize(20);
        cell->setFont(font);
        cell->setCheckable(true);
        cell->setChecked(emoji.at(i) == current);
        const QString value = emoji.at(i);
        QObject::connect(cell, &QToolButton::clicked, &dialog, [&dialog, chosen, value] {
            *chosen = value;
            dialog.accept();
        });
        grid->addWidget(cell, i / 8, i % 8);
    }
    layout->addLayout(grid);
    auto *row = new QHBoxLayout;
    auto *field = new QLineEdit;
    field->setObjectName(QStringLiteral("noteEmojiField"));
    field->setPlaceholderText(QStringLiteral("Type or paste any emoji"));
    field->setMaxLength(kMaxEmojiBytes);
    if (!commonNoteEmoji().contains(current))
        field->setText(current);
    auto *use = button(QStringLiteral("Use"), "ghostButton");
    row->addWidget(field, 1);
    row->addWidget(use);
    layout->addLayout(row);
    auto *notice = muted();
    notice->hide();
    layout->addWidget(notice);
    const auto useField = [&dialog, field, notice, chosen] {
        const QString value = normalizedNoteEmoji(field->text());
        if (value.isEmpty() || !isValidNoteEmoji(value)) {
            notice->setText(QStringLiteral("That is not an emoji. Paste one emoji, a "
                                           "flag or a keycap such as 1️⃣."));
            notice->show();
            return;
        }
        *chosen = value;
        dialog.accept();
    };
    QObject::connect(use, &QPushButton::clicked, &dialog, useField);
    QObject::connect(field, &QLineEdit::returnPressed, &dialog, useField);
    auto *buttons = new QHBoxLayout;
    if (!current.isEmpty()) {
        auto *remove = button(QStringLiteral("Remove"), "ghostButton", "trash");
        QObject::connect(remove, &QPushButton::clicked, &dialog, [&dialog, chosen] {
            chosen->clear();
            dialog.accept();
        });
        buttons->addWidget(remove);
    }
    buttons->addStretch();
    auto *cancel = button(QStringLiteral("Cancel"), "ghostButton");
    QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    buttons->addWidget(cancel);
    layout->addLayout(buttons);
    return dialog.exec() == QDialog::Accepted;
}

void customizePage(QWidget *parent, const QJsonObject &page, const QJsonObject &notes,
                   const QJsonArray &myNotes, PageCustomizeHooks hooks)
{
    struct State {
        QJsonObject fields;
        QJsonArray outline;
        QJsonObject known;
    };
    auto state = std::make_shared<State>();
    state->fields = pageEditFields(page);
    state->outline = state->fields.value(QStringLiteral("outline")).toArray();
    state->known = notes;
    for (const QJsonValue &value : myNotes) {
        const QJsonObject note = value.toObject();
        const QString id = note.value(QStringLiteral("id")).toString();
        if (!id.isEmpty() && !state->known.contains(id))
            state->known.insert(id, QJsonObject{
                {QStringLiteral("id"), id},
                {QStringLiteral("title"), note.value(QStringLiteral("title"))},
                {QStringLiteral("emoji"), note.value(QStringLiteral("emoji"))}});
    }
    const bool owner = page.value(QStringLiteral("role")).toString() == QLatin1String("owner");

    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("notePageCustomize"));
    dialog->setWindowTitle(QStringLiteral("Customize page"));
    dialog->setMinimumWidth(640);
    auto *layout = new QVBoxLayout(dialog);
    auto *form = new QFormLayout;
    auto *title = new QLineEdit(state->fields.value(QStringLiteral("title")).toString());
    title->setObjectName(QStringLiteral("notePageTitleEdit"));
    title->setMaxLength(kMaxPageTitle);
    title->setPlaceholderText(QStringLiteral("Untitled page"));
    form->addRow(QStringLiteral("Title"), title);
    auto *emoji = button(QString(), "ghostButton");
    emoji->setObjectName(QStringLiteral("ghostButton"));
    const auto showEmojiButton = [emoji, state] {
        const QString value = state->fields.value(QStringLiteral("emoji")).toString();
        emoji->setText(value.isEmpty() ? QStringLiteral("Choose…") : value);
    };
    showEmojiButton();
    QObject::connect(emoji, &QPushButton::clicked, dialog, [dialog, state, showEmojiButton] {
        QString chosen;
        if (!pickNoteEmoji(dialog, state->fields.value(QStringLiteral("emoji")).toString(),
                           &chosen))
            return;
        state->fields.insert(QStringLiteral("emoji"), chosen);
        showEmojiButton();
    });
    form->addRow(QStringLiteral("Emoji"), emoji);
    auto *description = new QPlainTextEdit(
        state->fields.value(QStringLiteral("description")).toString());
    description->setPlaceholderText(QStringLiteral("Markdown, shown above the notes"));
    description->setFixedHeight(description->fontMetrics().lineSpacing() * 4 + 12);
    form->addRow(QStringLiteral("Description"), description);
    auto *accent = new QComboBox;
    for (const QString &value : pageAccents())
        accent->addItem(accentIcon(value), pageAccentLabel(value), value);
    accent->setCurrentIndex(qMax(0, accent->findData(
        state->fields.value(QStringLiteral("accent")).toString())));
    form->addRow(QStringLiteral("Colour"), accent);
    auto *pageLayoutBox = new QComboBox;
    for (const QString &value : {QStringLiteral("outline"), QStringLiteral("continuous")})
        pageLayoutBox->addItem(pageLayoutLabel(value), value);
    pageLayoutBox->setCurrentIndex(qMax(0, pageLayoutBox->findData(
        state->fields.value(QStringLiteral("layout")).toString())));
    form->addRow(QStringLiteral("Layout"), pageLayoutBox);
    auto *showEmoji = new QCheckBox(QStringLiteral("Show the notes' emoji in the outline"));
    showEmoji->setChecked(state->fields.value(QStringLiteral("showEmoji")).toBool(true));
    form->addRow(QString(), showEmoji);
    layout->addLayout(form);

    layout->addWidget(bold(QStringLiteral("Outline")));
    auto *outlineRow = new QHBoxLayout;
    auto *list = new QListWidget;
    list->setObjectName(QStringLiteral("notePageOutlineEditor"));
    list->setDragDropMode(QAbstractItemView::InternalMove);
    list->setDefaultDropAction(Qt::MoveAction);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setMinimumHeight(220);
    outlineRow->addWidget(list, 1);
    auto *tools = new QVBoxLayout;
    auto *indent = button(QStringLiteral("Indent"), "ghostButton", "chevron-right");
    auto *outdent = button(QStringLiteral("Outdent"), "ghostButton", "chevron-left");
    auto *up = button(QStringLiteral("Move up"), "ghostButton");
    auto *down = button(QStringLiteral("Move down"), "ghostButton");
    auto *rename = button(QStringLiteral("Rename…"), "ghostButton", "pencil");
    auto *remove = button(QStringLiteral("Remove"), "ghostButton", "trash");
    for (QPushButton *tool : {indent, outdent, up, down, rename, remove})
        tools->addWidget(tool);
    tools->addStretch();
    outlineRow->addLayout(tools);
    layout->addLayout(outlineRow);

    auto *addRow = new QHBoxLayout;
    auto *search = new QLineEdit;
    search->setObjectName(QStringLiteral("notePageAddNote"));
    search->setPlaceholderText(QStringLiteral("Add a note: type to search your notes"));
    search->setClearButtonEnabled(true);
    auto *model = new QStandardItemModel(dialog);
    for (const QJsonValue &value : myNotes) {
        const QJsonObject note = value.toObject();
        const QString id = note.value(QStringLiteral("id")).toString();
        if (!isNoteId(id))
            continue;
        QString shown = note.value(QStringLiteral("title")).toString().trimmed();
        if (shown.isEmpty())
            shown = QStringLiteral("Untitled note");
        auto *item = new QStandardItem(
            noteDisplayTitle(note.value(QStringLiteral("emoji")).toString(), shown));
        item->setData(id, kPickNoteRole);
        item->setEditable(false);
        model->appendRow(item);
    }
    auto *completer = new QCompleter(model, dialog);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    search->setCompleter(completer);
    addRow->addWidget(search, 1);
    auto *addSection = button(QStringLiteral("Add section"), "ghostButton", "plus");
    auto *addLink = button(QStringLiteral("Add link…"), "ghostButton", "link");
    addRow->addWidget(addSection);
    addRow->addWidget(addLink);
    layout->addLayout(addRow);
    auto *notice = muted();
    notice->hide();
    layout->addWidget(notice);
    const auto say = [notice](const QString &text) {
        notice->setText(text);
        notice->setVisible(!text.isEmpty());
    };

    auto render = std::make_shared<std::function<void(const QString &)>>();
    const auto currentIndex = [list] { return list->currentRow(); };
    const auto updateTools = [state, list, indent, outdent, up, down, rename, remove] {
        const int row = list->currentRow();
        const bool picked = row >= 0 && row < state->outline.size();
        indent->setEnabled(canIndentOutline(state->outline, row));
        outdent->setEnabled(canOutdentOutline(state->outline, row));
        up->setEnabled(picked && row > 0);
        down->setEnabled(picked && row + 1 < state->outline.size());
        rename->setEnabled(picked);
        remove->setEnabled(picked);
    };
    *render = [state, list, updateTools](const QString &select) {
        const QSignalBlocker blocker(list);
        list->clear();
        const QColor mutedColor = Theme::color(Theme::Role::FgMuted);
        for (const QJsonValue &value : std::as_const(state->outline)) {
            const QJsonObject entry = value.toObject();
            const QString kind = outlineKind(entry);
            auto *item = new QListWidgetItem(
                QString(outlineDepth(entry) * 3, QChar(0x2002)) +
                outlineEntryLabel(entry, state->known, true));
            item->setData(kOutlineIdRole, entry.value(QStringLiteral("id")).toString());
            if (kind == QLatin1String("section")) {
                QFont font = item->font();
                font.setWeight(QFont::DemiBold);
                item->setFont(font);
            } else if (kind == QLatin1String("link")) {
                item->setIcon(forkmesh::ui::themedOcticon(QStringLiteral("link-external"),
                                                          mutedColor, 14));
                item->setToolTip(entry.value(QStringLiteral("url")).toString());
            } else if (outlineEntryLocked(entry, state->known)) {
                item->setIcon(forkmesh::ui::themedOcticon(QStringLiteral("lock"), mutedColor, 14));
                item->setForeground(mutedColor);
                item->setToolTip(QStringLiteral("You don't have access to this note. It "
                                                "stays on the page unless you remove it."));
            } else {
                item->setIcon(forkmesh::ui::themedOcticon(QStringLiteral("note"), mutedColor, 14));
            }
            list->addItem(item);
            if (item->data(kOutlineIdRole).toString() == select)
                list->setCurrentItem(item);
        }
        updateTools();
    };
    (*render)(QString());
    QObject::connect(dialog, &QObject::destroyed, dialog, [render] {});
    const std::weak_ptr<std::function<void(const QString &)>> weakRender = render;
    const auto change = [state, weakRender](const QJsonArray &next, const QString &select) {
        state->outline = next;
        if (const auto again = weakRender.lock())
            (*again)(select);
    };
    const auto selectedId = [state, currentIndex] {
        const int row = currentIndex();
        return row >= 0 && row < state->outline.size()
            ? state->outline.at(row).toObject().value(QStringLiteral("id")).toString()
            : QString();
    };
    QObject::connect(list, &QListWidget::currentRowChanged, dialog, updateTools);
    QObject::connect(indent, &QPushButton::clicked, dialog, [state, change, selectedId, currentIndex] {
        change(indentOutline(state->outline, currentIndex(), 1), selectedId());
    });
    QObject::connect(outdent, &QPushButton::clicked, dialog, [state, change, selectedId, currentIndex] {
        change(indentOutline(state->outline, currentIndex(), -1), selectedId());
    });
    QObject::connect(up, &QPushButton::clicked, dialog, [state, change, selectedId, currentIndex] {
        change(moveOutline(state->outline, currentIndex(), -1), selectedId());
    });
    QObject::connect(down, &QPushButton::clicked, dialog, [state, change, selectedId, currentIndex] {
        change(moveOutline(state->outline, currentIndex(), 1), selectedId());
    });
    QObject::connect(remove, &QPushButton::clicked, dialog, [state, change, currentIndex] {
        change(removeOutline(state->outline, currentIndex()), QString());
    });
    QObject::connect(rename, &QPushButton::clicked, dialog,
                     [dialog, state, change, selectedId, currentIndex] {
        const int row = currentIndex();
        if (row < 0 || row >= state->outline.size())
            return;
        QJsonObject entry = state->outline.at(row).toObject();
        const bool note = outlineKind(entry) == QLatin1String("note");
        bool ok = false;
        const QString label = QInputDialog::getText(
            dialog, QStringLiteral("Rename"),
            note ? QStringLiteral("Name shown in the outline (empty uses the note's title)")
                 : QStringLiteral("Name"),
            QLineEdit::Normal, entry.value(QStringLiteral("label")).toString(), &ok);
        if (!ok)
            return;
        entry.insert(QStringLiteral("label"), label.trimmed().left(kMaxPageTitle));
        QJsonArray next = state->outline;
        next.replace(row, entry);
        change(next, selectedId());
    });
    QObject::connect(list->model(), &QAbstractItemModel::rowsMoved, dialog,
                     [dialog, list, state, change] {
        QTimer::singleShot(0, dialog, [list, state, change] {
            QHash<QString, QJsonObject> byId;
            for (const QJsonValue &value : std::as_const(state->outline)) {
                const QJsonObject entry = value.toObject();
                byId.insert(entry.value(QStringLiteral("id")).toString(), entry);
            }
            QJsonArray next;
            for (int row = 0; row < list->count(); ++row) {
                const QString id = list->item(row)->data(kOutlineIdRole).toString();
                if (byId.contains(id))
                    next.append(byId.take(id));
            }
            const QString select = list->currentItem()
                ? list->currentItem()->data(kOutlineIdRole).toString()
                : QString();
            change(normalizedOutline(next), select);
        });
    });
    QObject::connect(completer, qOverload<const QModelIndex &>(&QCompleter::activated),
                     dialog, [dialog, state, change, currentIndex, search, say](
                                 const QModelIndex &index) {
        const QString id = index.data(kPickNoteRole).toString();
        QTimer::singleShot(0, dialog, [search] { search->clear(); });
        if (id.isEmpty())
            return;
        for (const QJsonValue &value : std::as_const(state->outline)) {
            if (value.toObject().value(QStringLiteral("noteId")).toString() == id) {
                say(QStringLiteral("That note is already on this page."));
                return;
            }
        }
        if (state->outline.size() >= kMaxOutlineEntries) {
            say(pageError(QStringLiteral("too_many_entries"), QString()));
            return;
        }
        say(QString());
        const QJsonObject entry = outlineEntry(QStringLiteral("note"), 0, QString(), id);
        change(insertOutline(state->outline, currentIndex(), entry),
               entry.value(QStringLiteral("id")).toString());
    });
    QObject::connect(addSection, &QPushButton::clicked, dialog,
                     [dialog, state, change, currentIndex, say] {
        bool ok = false;
        const QString label = QInputDialog::getText(
            dialog, QStringLiteral("Add section"), QStringLiteral("Section heading"),
            QLineEdit::Normal, QString(), &ok);
        if (!ok || label.trimmed().isEmpty())
            return;
        if (state->outline.size() >= kMaxOutlineEntries) {
            say(pageError(QStringLiteral("too_many_entries"), QString()));
            return;
        }
        const QJsonObject entry =
            outlineEntry(QStringLiteral("section"), 0, label.trimmed().left(kMaxPageTitle));
        change(insertOutline(state->outline, currentIndex(), entry),
               entry.value(QStringLiteral("id")).toString());
    });
    QObject::connect(addLink, &QPushButton::clicked, dialog,
                     [dialog, state, change, currentIndex, say] {
        bool ok = false;
        const QString url = QInputDialog::getText(
            dialog, QStringLiteral("Add link"), QStringLiteral("Address (https://…)"),
            QLineEdit::Normal, QStringLiteral("https://"), &ok);
        if (!ok)
            return;
        if (!isOutlineUrl(url)) {
            say(QStringLiteral("A link needs a full https:// address."));
            return;
        }
        const QString label = QInputDialog::getText(
            dialog, QStringLiteral("Add link"), QStringLiteral("Name shown in the outline"),
            QLineEdit::Normal, QUrl(url.trimmed()).host(), &ok);
        if (!ok)
            return;
        if (state->outline.size() >= kMaxOutlineEntries) {
            say(pageError(QStringLiteral("too_many_entries"), QString()));
            return;
        }
        say(QString());
        const QJsonObject entry = outlineEntry(QStringLiteral("link"), 0,
                                               label.trimmed().left(kMaxPageTitle), QString(),
                                               url);
        change(insertOutline(state->outline, currentIndex(), entry),
               entry.value(QStringLiteral("id")).toString());
    });

    auto *footer = new QHBoxLayout;
    if (owner && hooks.remove) {
        auto *deletePage = button(QStringLiteral("Delete page"), "dangerButton", "trash");
        QObject::connect(deletePage, &QPushButton::clicked, dialog, [dialog, hooks] {
            if (QMessageBox::question(dialog, QStringLiteral("Delete page"),
                                      QStringLiteral("Delete this page? The notes on it "
                                                     "are kept.")) != QMessageBox::Yes)
                return;
            hooks.remove();
            dialog->accept();
        });
        footer->addWidget(deletePage);
    }
    footer->addStretch();
    auto *cancel = button(QStringLiteral("Cancel"), "ghostButton");
    QObject::connect(cancel, &QPushButton::clicked, dialog, &QDialog::reject);
    footer->addWidget(cancel);
    auto *save = button(QStringLiteral("Save"), "primaryButton", "check-circle");
    save->setDefault(true);
    footer->addWidget(save);
    layout->addLayout(footer);

    QPointer<QDialog> guard(dialog);
    QObject::connect(save, &QPushButton::clicked, dialog,
                     [guard, dialog, state, hooks, title, description, accent,
                      pageLayoutBox, showEmoji, save, say] {
        QJsonObject fields = state->fields;
        fields.insert(QStringLiteral("title"), title->text().trimmed());
        fields.insert(QStringLiteral("description"),
                      description->toPlainText().left(kMaxPageDescription));
        fields.insert(QStringLiteral("accent"), accent->currentData().toString());
        fields.insert(QStringLiteral("layout"), pageLayoutBox->currentData().toString());
        fields.insert(QStringLiteral("showEmoji"), showEmoji->isChecked());
        fields.insert(QStringLiteral("outline"), normalizedOutline(state->outline));
        if (!hooks.save)
            return;
        save->setEnabled(false);
        say(QStringLiteral("Saving…"));
        hooks.save(fields, [guard, dialog, save, say](bool ok, const QString &error) {
            if (!guard)
                return;
            if (ok) {
                dialog->accept();
                return;
            }
            save->setEnabled(true);
            say(error);
        });
    });
    dialog->open();
    title->setFocus();
}

void sharePage(QWidget *parent, const QJsonObject &page, PageRequest request,
               std::function<void(const QJsonArray &)> changed)
{
    const QString pageId = page.value(QStringLiteral("id")).toString();
    auto shares = std::make_shared<QJsonArray>(page.value(QStringLiteral("shares")).toArray());
    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("notePageShareDialog"));
    dialog->setWindowTitle(QStringLiteral("Share page"));
    dialog->setMinimumWidth(520);
    auto *layout = new QVBoxLayout(dialog);
    layout->addWidget(bold(QStringLiteral("Share “%1”").arg(pageTitle(page))));
    auto *addRow = new QHBoxLayout;
    auto *type = new QComboBox;
    type->addItem(QStringLiteral("User"), QStringLiteral("user"));
    type->addItem(QStringLiteral("Organization"), QStringLiteral("organization"));
    auto *name = new QLineEdit;
    name->setObjectName(QStringLiteral("notePageShareName"));
    name->setPlaceholderText(QStringLiteral("Name of a person or organization"));
    auto *role = new QComboBox;
    role->addItem(QStringLiteral("Can edit"), QStringLiteral("editor"));
    role->addItem(QStringLiteral("Can view"), QStringLiteral("viewer"));
    auto *add = button(QStringLiteral("Share"), "primaryButton", "people");
    add->setDefault(true);
    addRow->addWidget(type);
    addRow->addWidget(name, 1);
    addRow->addWidget(role);
    addRow->addWidget(add);
    layout->addLayout(addRow);
    auto *alsoNotes = new QCheckBox(QStringLiteral("Also share the notes on this page"));
    alsoNotes->setObjectName(QStringLiteral("notePageShareNotes"));
    alsoNotes->setChecked(true);
    alsoNotes->setToolTip(QStringLiteral("Shares every note on the page that you own "
                                         "with the same person. Nobody loses access "
                                         "they already have."));
    layout->addWidget(alsoNotes);
    auto *notice = muted();
    notice->hide();
    layout->addWidget(notice);
    layout->addWidget(bold(QStringLiteral("People with access")));
    auto *people = new QWidget;
    auto *peopleLayout = new QVBoxLayout(people);
    peopleLayout->setContentsMargins(0, 0, 0, 0);
    peopleLayout->setSpacing(4);
    layout->addWidget(people);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setAutoDefault(false);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);

    QPointer<QDialog> guard(dialog);
    const auto say = [notice](const QString &text) {
        notice->setText(text);
        notice->setVisible(!text.isEmpty());
    };
    auto render = std::make_shared<std::function<void()>>();
    const std::weak_ptr<std::function<void()>> weakRender = render;
    const auto send = [guard, pageId, request, shares, changed, say, weakRender](
                          const QByteArray &method, const QJsonObject &body,
                          const QString &success) {
        request(method, QStringLiteral("/api/notes/pages/%1/shares").arg(pageId), body,
                [guard, shares, changed, say, weakRender, body, success](
                    bool ok, const QJsonObject &payload, const QString &error) {
            if (!guard)
                return;
            if (!ok) {
                const QString code = payload.value(QStringLiteral("error")).toString();
                say(code == QLatin1String("principal_not_found") ||
                            code == QLatin1String("invalid_principal")
                        ? shareError(code, body.value(QStringLiteral("name")).toString(),
                                     QString())
                        : pageError(code, QStringLiteral("Sharing failed: %1").arg(error)));
                return;
            }
            *shares = payload.value(QStringLiteral("shares")).toArray();
            if (changed)
                changed(*shares);
            if (const auto again = weakRender.lock())
                (*again)();
            say(success);
        });
    };
    *render = [people, peopleLayout, shares, send] {
        while (QLayoutItem *item = peopleLayout->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        if (shares->isEmpty())
            peopleLayout->addWidget(muted(QStringLiteral("Only you can open this page.")));
        for (const QJsonValue &value : std::as_const(*shares)) {
            const QJsonObject share = value.toObject();
            const QString shareType = share.value(QStringLiteral("type")).toString();
            const QString shareName = share.value(QStringLiteral("name")).toString();
            auto *row = new QWidget(people);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->addWidget(new QLabel(QStringLiteral("@") + shareName), 1);
            rowLayout->addWidget(muted(principalTypeLabel(shareType)));
            auto *access = new QComboBox;
            access->addItem(QStringLiteral("Can edit"), QStringLiteral("editor"));
            access->addItem(QStringLiteral("Can view"), QStringLiteral("viewer"));
            access->setCurrentIndex(qMax(0, access->findData(
                share.value(QStringLiteral("role")).toString(QStringLiteral("viewer")))));
            rowLayout->addWidget(access);
            auto *removeButton = button(QStringLiteral("Remove"), "ghostButton");
            rowLayout->addWidget(removeButton);
            peopleLayout->addWidget(row);
            const QJsonObject principal{{QStringLiteral("type"), shareType},
                                        {QStringLiteral("name"), shareName}};
            QObject::connect(access, &QComboBox::activated, row,
                             [send, principal, access, shareName] {
                QJsonObject body = principal;
                body.insert(QStringLiteral("role"), access->currentData().toString());
                send(QByteArrayLiteral("POST"), body,
                     QStringLiteral("@%1 can now %2 this page.")
                         .arg(shareName, access->currentData().toString() ==
                                                 QLatin1String("editor")
                                             ? QStringLiteral("edit")
                                             : QStringLiteral("view")));
            });
            QObject::connect(removeButton, &QPushButton::clicked, row,
                             [send, principal, shareName, removeButton] {
                removeButton->setEnabled(false);
                send(QByteArrayLiteral("DELETE"), principal,
                     QStringLiteral("Stopped sharing the page with @%1. Notes shared "
                                    "with them stay shared.")
                         .arg(shareName));
            });
        }
    };
    (*render)();
    QObject::connect(dialog, &QObject::destroyed, dialog, [render] {});
    QObject::connect(add, &QPushButton::clicked, dialog,
                     [name, type, role, alsoNotes, send, say] {
        const QString who = shareQuery(name->text());
        if (who.isEmpty()) {
            say(QStringLiteral("Type the name of an account or organization."));
            name->setFocus();
            return;
        }
        send(QByteArrayLiteral("POST"),
             QJsonObject{{QStringLiteral("type"), type->currentData().toString()},
                         {QStringLiteral("name"), who},
                         {QStringLiteral("role"), role->currentData().toString()},
                         {QStringLiteral("includeNotes"), alsoNotes->isChecked()}},
             alsoNotes->isChecked()
                 ? QStringLiteral("Shared the page and its notes with %1.").arg(who)
                 : QStringLiteral("Shared the page with %1.").arg(who));
        name->clear();
    });
    dialog->open();
    name->setFocus();
}

}
