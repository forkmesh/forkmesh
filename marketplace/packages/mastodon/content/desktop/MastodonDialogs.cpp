#include "MastodonDialogs.h"
#include "MastodonClient.h"
#include "MastodonCore.h"
#include "MastodonWidgets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMimeDatabase>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace forkmesh::mastodon {

namespace {

constexpr qint64 kMaxUploadBytes = 100 * 1024 * 1024;

const QList<QPair<int, QString>> &pollDurations()
{
    static const QList<QPair<int, QString>> durations{
        {300, QStringLiteral("5 minutes")}, {1800, QStringLiteral("30 minutes")}, {3600, QStringLiteral("1 hour")},
        {21600, QStringLiteral("6 hours")}, {43200, QStringLiteral("12 hours")},  {86400, QStringLiteral("1 day")},
        {259200, QStringLiteral("3 days")}, {604800, QStringLiteral("7 days")},
    };
    return durations;
}

QToolButton *toggleButton(const QString &iconName, const QString &tip, QWidget *parent)
{
    QToolButton *button = iconButton(iconName, tip, parent);
    button->setCheckable(true);
    return button;
}

void paintToggle(QToolButton *button)
{
    const QString name = button->property("mastodonIcon").toString();
    button->setIcon(icon(name, button->isChecked() ? accentColor(button) : mutedColor(button)));
}

}

QStringList languageCodes()
{
    return {QStringLiteral("en"), QStringLiteral("de"), QStringLiteral("fr"), QStringLiteral("es"),
            QStringLiteral("it"), QStringLiteral("pt"), QStringLiteral("nl"), QStringLiteral("sv"),
            QStringLiteral("da"), QStringLiteral("nb"), QStringLiteral("fi"), QStringLiteral("pl"),
            QStringLiteral("cs"), QStringLiteral("uk"), QStringLiteral("ru"), QStringLiteral("tr"),
            QStringLiteral("el"), QStringLiteral("ar"), QStringLiteral("he"), QStringLiteral("fa"),
            QStringLiteral("hi"), QStringLiteral("ja"), QStringLiteral("ko"), QStringLiteral("zh"),
            QStringLiteral("id"), QStringLiteral("vi"), QStringLiteral("th"), QStringLiteral("eo")};
}

QString languageName(const QString &code)
{
    const QLocale locale(code);
    if (locale.language() == QLocale::C)
        return code;
    const QString native = locale.nativeLanguageName();
    const QString english = QLocale::languageToString(locale.language());
    if (native.isEmpty() || native == english)
        return english;
    return QStringLiteral("%1 (%2)").arg(english, native);
}

QJsonObject composeBody(const ComposeDraft &draft, bool editing)
{
    QJsonObject body;
    body.insert(QStringLiteral("status"), draft.text);
    if (editing || !draft.spoiler.isEmpty())
        body.insert(QStringLiteral("spoiler_text"), draft.spoiler);
    if (!editing) {
        body.insert(QStringLiteral("visibility"), draft.visibility);
        if (!draft.inReplyToId.isEmpty())
            body.insert(QStringLiteral("in_reply_to_id"), draft.inReplyToId);
        if (draft.scheduledAt.isValid())
            body.insert(QStringLiteral("scheduled_at"), draft.scheduledAt.toUTC().toString(Qt::ISODate));
    }
    if (!draft.language.isEmpty())
        body.insert(QStringLiteral("language"), draft.language);
    if (editing || !draft.mediaIds.isEmpty()) {
        body.insert(QStringLiteral("media_ids"), QJsonArray::fromStringList(draft.mediaIds));
        body.insert(QStringLiteral("sensitive"), draft.sensitive && !draft.mediaIds.isEmpty());
    }
    if (editing && !draft.mediaAttributes.isEmpty())
        body.insert(QStringLiteral("media_attributes"), draft.mediaAttributes);
    if (draft.pollOptions.size() >= 2 && draft.mediaIds.isEmpty()) {
        body.insert(QStringLiteral("poll"), QJsonObject{
                                                {QStringLiteral("options"), QJsonArray::fromStringList(draft.pollOptions)},
                                                {QStringLiteral("expires_in"), draft.pollExpiresIn},
                                                {QStringLiteral("multiple"), draft.pollMultiple},
                                            });
    }
    return body;
}

ComposeDialog::ComposeDialog(Client *client, const ComposeLimits &limits, QWidget *parent)
    : QDialog(parent), m_client(client), m_limits(limits)
{
    setObjectName(QStringLiteral("mastodonCompose"));
    setWindowTitle(QStringLiteral("New post"));
    setMinimumWidth(580);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 14);
    layout->setSpacing(10);

    m_replyPreview = new QLabel;
    m_replyPreview->setWordWrap(true);
    m_replyPreview->setTextFormat(Qt::RichText);
    m_replyPreview->setStyleSheet(QStringLiteral("color: %1; font-size: 12px; padding: 6px 8px; border-left: 3px solid %2;")
                                      .arg(mutedColor(this).name(), accentColor(this).name()));
    m_replyPreview->hide();
    layout->addWidget(m_replyPreview);

    m_spoiler = new QLineEdit;
    m_spoiler->setObjectName(QStringLiteral("mastodonComposeSpoiler"));
    m_spoiler->setPlaceholderText(QStringLiteral("Content warning (shown before the post)"));
    m_spoiler->hide();
    layout->addWidget(m_spoiler);

    m_text = new QPlainTextEdit;
    m_text->setObjectName(QStringLiteral("mastodonComposeText"));
    m_text->setPlaceholderText(QStringLiteral("What's on your mind?"));
    m_text->setMinimumHeight(150);
    m_text->setTabChangesFocus(true);
    m_text->setStyleSheet(QStringLiteral("font-size: 15px;"));
    layout->addWidget(m_text, 1);

    m_mediaBox = new QWidget;
    m_mediaRow = new QHBoxLayout(m_mediaBox);
    m_mediaRow->setContentsMargins(0, 0, 0, 0);
    m_mediaRow->setSpacing(8);
    m_mediaBox->hide();
    layout->addWidget(m_mediaBox);

    m_pollBox = new QWidget;
    auto *pollOuter = new QVBoxLayout(m_pollBox);
    pollOuter->setContentsMargins(0, 0, 0, 0);
    pollOuter->setSpacing(6);
    m_pollLayout = new QVBoxLayout;
    m_pollLayout->setSpacing(6);
    pollOuter->addLayout(m_pollLayout);
    auto *pollFooter = new QHBoxLayout;
    m_addOption = new QPushButton(QStringLiteral("Add option"));
    m_addOption->setObjectName(QStringLiteral("ghostButton"));
    connect(m_addOption, &QPushButton::clicked, this, [this] { addPollOption(); });
    pollFooter->addWidget(m_addOption);
    m_pollDuration = new QComboBox;
    for (const auto &duration : pollDurations())
        m_pollDuration->addItem(duration.second, duration.first);
    m_pollDuration->setCurrentIndex(5);
    pollFooter->addWidget(m_pollDuration);
    m_pollMultiple = new QCheckBox(QStringLiteral("Allow several choices"));
    pollFooter->addWidget(m_pollMultiple);
    pollFooter->addStretch(1);
    pollOuter->addLayout(pollFooter);
    m_pollBox->hide();
    layout->addWidget(m_pollBox);

    auto *options = new QHBoxLayout;
    options->setSpacing(10);
    m_sensitive = new QCheckBox(QStringLiteral("Mark media as sensitive"));
    m_sensitive->hide();
    options->addWidget(m_sensitive);
    m_schedule = new QCheckBox(QStringLiteral("Schedule for"));
    options->addWidget(m_schedule);
    m_scheduleAt = new QDateTimeEdit(QDateTime::currentDateTime().addSecs(3600));
    m_scheduleAt->setCalendarPopup(true);
    m_scheduleAt->setDisplayFormat(QStringLiteral("d MMM yyyy  HH:mm"));
    m_scheduleAt->setEnabled(false);
    options->addWidget(m_scheduleAt);
    options->addStretch(1);
    connect(m_schedule, &QCheckBox::toggled, m_scheduleAt, &QWidget::setEnabled);
    layout->addLayout(options);

    auto *bar = new QHBoxLayout;
    bar->setSpacing(4);
    m_attach = iconButton(QStringLiteral("paperclip"), QStringLiteral("Add images, video or audio"), this);
    connect(m_attach, &QToolButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(
            this, QStringLiteral("Add media"), QString(),
            QStringLiteral("Media (*.png *.jpg *.jpeg *.gif *.webp *.heic *.avif *.mp4 *.webm *.mov *.mp3 *.ogg *.wav *.flac *.m4a);;All files (*)"));
        for (const QString &file : files)
            addFile(file);
    });
    bar->addWidget(m_attach);
    m_pollToggle = toggleButton(QStringLiteral("graph"), QStringLiteral("Add a poll"), this);
    connect(m_pollToggle, &QToolButton::toggled, this, [this](bool on) {
        paintToggle(m_pollToggle);
        if (on && m_pollOptions.isEmpty()) {
            addPollOption();
            addPollOption();
        }
        m_pollBox->setVisible(on);
        updateCounter();
    });
    bar->addWidget(m_pollToggle);
    m_spoilerToggle = toggleButton(QStringLiteral("alert"), QStringLiteral("Add a content warning"), this);
    connect(m_spoilerToggle, &QToolButton::toggled, this, [this](bool on) {
        paintToggle(m_spoilerToggle);
        m_spoiler->setVisible(on);
        if (on)
            m_spoiler->setFocus();
        updateCounter();
    });
    bar->addWidget(m_spoilerToggle);
    m_visibility = new QComboBox;
    m_visibility->setObjectName(QStringLiteral("mastodonComposeVisibility"));
    for (const QString &value : visibilityValues())
        m_visibility->addItem(icon(visibilityIcon(value), mutedColor(this)), visibilityLabel(value), value);
    m_visibility->setToolTip(QStringLiteral("Who can see this post"));
    bar->addWidget(m_visibility);
    m_language = new QComboBox;
    m_language->setObjectName(QStringLiteral("mastodonComposeLanguage"));
    m_language->addItem(QStringLiteral("Language: automatic"), QString());
    for (const QString &code : languageCodes())
        m_language->addItem(languageName(code), code);
    m_language->setToolTip(QStringLiteral("Language of this post"));
    bar->addWidget(m_language);
    bar->addStretch(1);
    m_counter = new QLabel;
    m_counter->setStyleSheet(QStringLiteral("font-size: 12px;"));
    bar->addWidget(m_counter);
    m_submit = new QPushButton(QStringLiteral("Post"));
    m_submit->setObjectName(QStringLiteral("primaryButton"));
    m_submit->setDefault(false);
    m_submit->setAutoDefault(false);
    connect(m_submit, &QPushButton::clicked, this, [this] { submit(); });
    bar->addWidget(m_submit);
    layout->addLayout(bar);

    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("statusLine"));
    m_status->setWordWrap(true);
    m_status->hide();
    layout->addWidget(m_status);

    auto *send = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(send, &QShortcut::activated, this, [this] { submit(); });
    connect(m_text, &QPlainTextEdit::textChanged, this, [this] { updateCounter(); });
    connect(m_spoiler, &QLineEdit::textChanged, this, [this] { updateCounter(); });
    updateCounter();
    m_text->setFocus();
}

void ComposeDialog::setDefaults(const QString &visibility, const QString &language, bool sensitive)
{
    setVisibility(visibility);
    const int languageIndex = m_language->findData(language);
    if (languageIndex >= 0)
        m_language->setCurrentIndex(languageIndex);
    else if (!language.isEmpty()) {
        m_language->addItem(languageName(language), language);
        m_language->setCurrentIndex(m_language->count() - 1);
    }
    m_sensitive->setChecked(sensitive);
}

void ComposeDialog::setReplyTo(const QJsonObject &status, const QString &selfAcct)
{
    m_replyId = status.value(QStringLiteral("id")).toString();
    setWindowTitle(QStringLiteral("Reply"));
    m_submit->setText(QStringLiteral("Reply"));
    const QJsonObject account = status.value(QStringLiteral("account")).toObject();
    m_replyPreview->setText(QStringLiteral("Replying to <b>%1</b>: %2")
                                .arg(escapeText(displayName(account)),
                                     escapeText(elide(plainText(status.value(QStringLiteral("content")).toString()), 160))));
    m_replyPreview->show();
    QStringList mentions;
    const QString author = account.value(QStringLiteral("acct")).toString();
    if (!author.isEmpty() && author != selfAcct)
        mentions << QLatin1Char('@') + author;
    for (const QJsonValue &value : status.value(QStringLiteral("mentions")).toArray()) {
        const QString acct = value.toObject().value(QStringLiteral("acct")).toString();
        const QString mention = QLatin1Char('@') + acct;
        if (!acct.isEmpty() && acct != selfAcct && !mentions.contains(mention))
            mentions << mention;
    }
    if (!mentions.isEmpty())
        setText(mentions.join(QLatin1Char(' ')) + QLatin1Char(' '));
    const QString spoiler = status.value(QStringLiteral("spoiler_text")).toString();
    if (!spoiler.isEmpty())
        setSpoiler(spoiler);
    const QStringList order = visibilityValues();
    const QString original = status.value(QStringLiteral("visibility")).toString();
    const QString current = m_visibility->currentData().toString();
    if (order.indexOf(original) > order.indexOf(current))
        setVisibility(original);
    const QString language = status.value(QStringLiteral("language")).toString();
    const int languageIndex = m_language->findData(language);
    if (!language.isEmpty() && languageIndex >= 0)
        m_language->setCurrentIndex(languageIndex);
}

void ComposeDialog::setEditing(const QJsonObject &status, const QString &text, const QString &spoiler)
{
    m_editingId = status.value(QStringLiteral("id")).toString();
    setWindowTitle(QStringLiteral("Edit post"));
    m_submit->setText(QStringLiteral("Save"));
    setText(text);
    setSpoiler(spoiler);
    setVisibility(status.value(QStringLiteral("visibility")).toString());
    m_visibility->setEnabled(false);
    m_schedule->hide();
    m_scheduleAt->hide();
    const QString language = status.value(QStringLiteral("language")).toString();
    const int languageIndex = m_language->findData(language);
    if (languageIndex >= 0)
        m_language->setCurrentIndex(languageIndex);
    m_sensitive->setChecked(status.value(QStringLiteral("sensitive")).toBool());
    for (const QJsonValue &value : status.value(QStringLiteral("media_attachments")).toArray())
        addExisting(value.toObject());
    const QJsonObject poll = status.value(QStringLiteral("poll")).toObject();
    if (!poll.isEmpty()) {
        QStringList options;
        for (const QJsonValue &value : poll.value(QStringLiteral("options")).toArray())
            options << value.toObject().value(QStringLiteral("title")).toString();
        const QDateTime end = QDateTime::fromString(poll.value(QStringLiteral("expires_at")).toString(), Qt::ISODateWithMs);
        const int left = end.isValid() ? static_cast<int>(QDateTime::currentDateTimeUtc().secsTo(end)) : 86400;
        setPoll(options, qMax(300, left), poll.value(QStringLiteral("multiple")).toBool());
    }
}

void ComposeDialog::setText(const QString &text)
{
    m_text->setPlainText(text);
    QTextCursor cursor = m_text->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_text->setTextCursor(cursor);
}

void ComposeDialog::setSpoiler(const QString &spoiler)
{
    m_spoiler->setText(spoiler);
    m_spoilerToggle->setChecked(!spoiler.isEmpty());
}

void ComposeDialog::setVisibility(const QString &visibility)
{
    const int index = m_visibility->findData(visibility);
    if (index >= 0)
        m_visibility->setCurrentIndex(index);
}

void ComposeDialog::setPoll(const QStringList &options, int expiresIn, bool multiple)
{
    for (QLineEdit *edit : std::as_const(m_pollOptions))
        edit->parentWidget()->deleteLater();
    m_pollOptions.clear();
    for (const QString &option : options)
        addPollOption(option);
    while (m_pollOptions.size() < 2)
        addPollOption();
    int best = 0;
    for (int index = 0; index < pollDurations().size(); ++index) {
        if (qAbs(pollDurations().at(index).first - expiresIn) < qAbs(pollDurations().at(best).first - expiresIn))
            best = index;
    }
    m_pollDuration->setCurrentIndex(best);
    m_pollMultiple->setChecked(multiple);
    m_pollToggle->setChecked(true);
}

void ComposeDialog::setScheduledAt(const QDateTime &when)
{
    m_schedule->setChecked(when.isValid());
    if (when.isValid())
        m_scheduleAt->setDateTime(when.toLocalTime());
}

void ComposeDialog::addPollOption(const QString &text)
{
    if (m_pollOptions.size() >= m_limits.maxOptions)
        return;
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *edit = new QLineEdit(text);
    edit->setMaxLength(m_limits.maxOptionCharacters);
    edit->setPlaceholderText(QStringLiteral("Option %1").arg(m_pollOptions.size() + 1));
    layout->addWidget(edit, 1);
    auto *remove = iconButton(QStringLiteral("x"), QStringLiteral("Remove option"), row);
    connect(remove, &QToolButton::clicked, this, [this, row, edit] {
        if (m_pollOptions.size() <= 2)
            return;
        m_pollOptions.removeAll(edit);
        row->deleteLater();
        m_addOption->setEnabled(m_pollOptions.size() < m_limits.maxOptions);
    });
    layout->addWidget(remove);
    m_pollLayout->addWidget(row);
    m_pollOptions.append(edit);
    m_addOption->setEnabled(m_pollOptions.size() < m_limits.maxOptions);
    connect(edit, &QLineEdit::textChanged, this, [this] { updateCounter(); });
}

bool ComposeDialog::addFile(const QString &path, const QString &description)
{
    if (m_media.size() >= m_limits.maxMedia) {
        fail(QStringLiteral("A post can carry at most %1 attachments.").arg(m_limits.maxMedia));
        return false;
    }
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > kMaxUploadBytes) {
        fail(QStringLiteral("%1 cannot be attached.").arg(info.fileName()));
        return false;
    }
    PendingMedia media;
    media.path = path;
    media.description = description;
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (reader.canRead()) {
        const QSize size = reader.size();
        if (size.isValid())
            reader.setScaledSize(size.scaled(240, 240, Qt::KeepAspectRatio));
        media.preview = reader.read();
    }
    m_media.append(media);
    m_pollToggle->setChecked(false);
    rebuildMedia();
    return true;
}

void ComposeDialog::addExisting(const QJsonObject &attachment)
{
    PendingMedia media;
    media.existingId = attachment.value(QStringLiteral("id")).toString();
    media.description = attachment.value(QStringLiteral("description")).toString();
    media.previewUrl = attachment.value(QStringLiteral("preview_url")).toString();
    m_media.append(media);
    rebuildMedia();
}

void ComposeDialog::rebuildMedia()
{
    while (QLayoutItem *item = m_mediaRow->takeAt(0)) {
        if (QWidget *gone = item->widget()) {
            gone->hide();
            gone->deleteLater();
        }
        delete item;
    }
    for (int index = 0; index < m_media.size(); ++index) {
        const PendingMedia &media = m_media.at(index);
        auto *tile = new QWidget;
        auto *column = new QVBoxLayout(tile);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(4);
        auto *thumb = new MediaTile(QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                                {QStringLiteral("description"), media.description}},
                                    false, tile);
        thumb->setFixedSize(110, 80);
        if (!media.preview.isNull())
            thumb->setImage(media.preview);
        else if (!media.previewUrl.isEmpty() && m_client) {
            QPointer<MediaTile> guard(thumb);
            m_client->image(media.previewUrl, [guard](const QImage &image) {
                if (guard)
                    guard->setImage(image);
            });
        } else {
            thumb->setToolTip(QFileInfo(media.path).fileName());
        }
        column->addWidget(thumb);
        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(2);
        auto *alt = new QPushButton(media.description.isEmpty() ? QStringLiteral("Add alt text") : QStringLiteral("Edit alt"));
        alt->setObjectName(QStringLiteral("ghostButton"));
        alt->setToolTip(QStringLiteral("Describe this for people who use screen readers"));
        connect(alt, &QPushButton::clicked, this, [this, index] {
            if (index >= m_media.size())
                return;
            bool ok = false;
            const QString text = QInputDialog::getMultiLineText(this, QStringLiteral("Alt text"),
                                                                QStringLiteral("Describe this attachment:"),
                                                                m_media.at(index).description, &ok);
            if (!ok)
                return;
            m_media[index].description = text.trimmed();
            rebuildMedia();
        });
        buttons->addWidget(alt);
        auto *remove = iconButton(QStringLiteral("x"), QStringLiteral("Remove attachment"), tile);
        connect(remove, &QToolButton::clicked, this, [this, index] {
            if (index < m_media.size())
                m_media.removeAt(index);
            rebuildMedia();
        });
        buttons->addWidget(remove);
        column->addLayout(buttons);
        m_mediaRow->addWidget(tile);
    }
    m_mediaRow->addStretch(1);
    m_mediaBox->setVisible(!m_media.isEmpty());
    m_sensitive->setVisible(!m_media.isEmpty());
    m_attach->setEnabled(m_media.size() < m_limits.maxMedia);
    m_pollToggle->setEnabled(m_media.isEmpty());
    updateCounter();
}

ComposeDraft ComposeDialog::draft() const
{
    ComposeDraft draft;
    draft.text = m_text->toPlainText();
    draft.spoiler = m_spoilerToggle->isChecked() ? m_spoiler->text().trimmed() : QString();
    draft.visibility = m_visibility->currentData().toString();
    draft.language = m_language->currentData().toString();
    draft.sensitive = m_sensitive->isChecked();
    draft.inReplyToId = m_replyId;
    if (m_pollToggle->isChecked()) {
        for (const QLineEdit *edit : m_pollOptions) {
            if (!edit->text().trimmed().isEmpty())
                draft.pollOptions << edit->text().trimmed();
        }
        draft.pollExpiresIn = m_pollDuration->currentData().toInt();
        draft.pollMultiple = m_pollMultiple->isChecked();
    }
    if (m_schedule->isChecked() && m_editingId.isEmpty())
        draft.scheduledAt = m_scheduleAt->dateTime();
    for (const PendingMedia &media : m_media) {
        if (!media.existingId.isEmpty()) {
            draft.mediaAttributes.append(QJsonObject{{QStringLiteral("id"), media.existingId},
                                                     {QStringLiteral("description"), media.description}});
        }
    }
    return draft;
}

int ComposeDialog::remaining() const
{
    const QString spoiler = m_spoilerToggle->isChecked() ? m_spoiler->text() : QString();
    return m_limits.maxCharacters - statusLength(m_text->toPlainText()) - statusLength(spoiler);
}

bool ComposeDialog::canSubmit() const
{
    if (m_busy || remaining() < 0)
        return false;
    const ComposeDraft current = draft();
    if (m_pollToggle->isChecked() && current.pollOptions.size() < 2)
        return false;
    return !current.text.trimmed().isEmpty() || !m_media.isEmpty();
}

QString ComposeDialog::statusText() const
{
    return m_status->text();
}

void ComposeDialog::updateCounter()
{
    const int left = remaining();
    m_counter->setText(QString::number(left));
    m_counter->setStyleSheet(QStringLiteral("font-size: 12px; color: %1;")
                                 .arg(left < 0 ? QStringLiteral("#d1242f") : mutedColor(this).name()));
    m_submit->setEnabled(canSubmit());
}

void ComposeDialog::setBusy(bool busy, const QString &message)
{
    m_busy = busy;
    m_text->setReadOnly(busy);
    m_attach->setEnabled(!busy && m_media.size() < m_limits.maxMedia);
    m_status->setText(message);
    m_status->setStyleSheet(QString());
    m_status->setVisible(!message.isEmpty());
    updateCounter();
}

void ComposeDialog::fail(const QString &message)
{
    m_busy = false;
    m_text->setReadOnly(false);
    m_status->setText(message);
    m_status->setStyleSheet(QStringLiteral("color: #d1242f;"));
    m_status->setVisible(!message.isEmpty());
    updateCounter();
}

void ComposeDialog::submit()
{
    if (!canSubmit() || !m_client)
        return;
    setBusy(true, m_media.isEmpty() ? QStringLiteral("Posting…") : QStringLiteral("Uploading media…"));
    uploadNext(0, {});
}

void ComposeDialog::uploadNext(int index, QStringList ids)
{
    if (index >= m_media.size()) {
        finish(ids);
        return;
    }
    const PendingMedia media = m_media.at(index);
    if (!media.existingId.isEmpty()) {
        ids << media.existingId;
        uploadNext(index + 1, ids);
        return;
    }
    QFile file(media.path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("Could not read %1.").arg(QFileInfo(media.path).fileName()));
        return;
    }
    const QByteArray data = file.readAll();
    QList<UploadPart> parts;
    parts.append({QStringLiteral("file"), data, QFileInfo(media.path).fileName(),
                  QMimeDatabase().mimeTypeForFileNameAndData(media.path, data).name()});
    if (!media.description.isEmpty())
        parts.append({QStringLiteral("description"), media.description.toUtf8(), QString(), QString()});
    setBusy(true, QStringLiteral("Uploading %1 of %2…").arg(index + 1).arg(m_media.size()));
    QPointer<ComposeDialog> guard(this);
    m_client->upload("POST", QStringLiteral("/api/v2/media"), parts, [guard, index, ids](const Result &result) mutable {
        if (!guard)
            return;
        if (!result.ok) {
            guard->fail(result.error);
            return;
        }
        const QString id = result.object().value(QStringLiteral("id")).toString();
        const bool ready = !result.object().value(QStringLiteral("url")).toString().isEmpty() && result.status != 202;
        ids << id;
        if (ready) {
            guard->uploadNext(index + 1, ids);
            return;
        }
        guard->setBusy(true, QStringLiteral("Mastodon is processing the upload…"));
        guard->waitForMedia(id, 60, [guard, index, ids](bool ok) {
            if (!guard)
                return;
            if (!ok) {
                guard->fail(QStringLiteral("Mastodon did not finish processing the upload."));
                return;
            }
            guard->uploadNext(index + 1, ids);
        });
    });
}

void ComposeDialog::waitForMedia(const QString &id, int attempts, std::function<void(bool)> done)
{
    QPointer<ComposeDialog> guard(this);
    m_client->get(QStringLiteral("/api/v1/media/") + id, {}, [guard, id, attempts, done](const Result &result) {
        if (!guard)
            return;
        if (result.ok && result.status == 200 && !result.object().value(QStringLiteral("url")).toString().isEmpty()) {
            done(true);
            return;
        }
        if (attempts <= 0 || (!result.ok && result.status != 206)) {
            done(false);
            return;
        }
        QTimer::singleShot(1000, guard, [guard, id, attempts, done] {
            if (guard)
                guard->waitForMedia(id, attempts - 1, done);
        });
    });
}

void ComposeDialog::finish(const QStringList &ids)
{
    ComposeDraft current = draft();
    current.mediaIds = ids;
    const bool editing = !m_editingId.isEmpty();
    const QJsonObject body = composeBody(current, editing);
    setBusy(true, editing ? QStringLiteral("Saving…") : QStringLiteral("Posting…"));
    QPointer<ComposeDialog> guard(this);
    const bool scheduled = current.scheduledAt.isValid();
    m_client->send(editing ? QByteArray("PUT") : QByteArray("POST"),
                   editing ? QStringLiteral("/api/v1/statuses/") + m_editingId : QStringLiteral("/api/v1/statuses"),
                   body, [guard, scheduled](const Result &result) {
                       if (!guard)
                           return;
                       if (!result.ok) {
                           guard->fail(result.error);
                           return;
                       }
                       guard->setBusy(false);
                       if (guard->published)
                           guard->published(result.object(), scheduled);
                       guard->accept();
                   });
}

MediaViewer::MediaViewer(Client *client, const QJsonArray &media, int index, QWidget *parent)
    : QDialog(parent), m_client(client), m_media(media)
{
    setWindowTitle(QStringLiteral("Media"));
    resize(960, 720);
    auto *layout = new QVBoxLayout(this);
    m_image = new QLabel;
    m_image->setAlignment(Qt::AlignCenter);
    m_image->setMinimumSize(320, 240);
    layout->addWidget(m_image, 1);
    m_caption = new QLabel;
    m_caption->setWordWrap(true);
    m_caption->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_caption);
    auto *buttons = new QHBoxLayout;
    m_previous = new QPushButton(QStringLiteral("Previous"));
    m_previous->setObjectName(QStringLiteral("ghostButton"));
    connect(m_previous, &QPushButton::clicked, this, [this] { showItem(m_index - 1); });
    buttons->addWidget(m_previous);
    m_next = new QPushButton(QStringLiteral("Next"));
    m_next->setObjectName(QStringLiteral("ghostButton"));
    connect(m_next, &QPushButton::clicked, this, [this] { showItem(m_index + 1); });
    buttons->addWidget(m_next);
    buttons->addStretch(1);
    auto *original = new QPushButton(QStringLiteral("Open original"));
    original->setObjectName(QStringLiteral("ghostButton"));
    connect(original, &QPushButton::clicked, this, [this] {
        const QJsonObject item = m_media.at(m_index).toObject();
        QUrl url(item.value(QStringLiteral("url")).toString());
        if (!safeWebUrl(url))
            url = QUrl(item.value(QStringLiteral("remote_url")).toString());
        if (safeWebUrl(url))
            QDesktopServices::openUrl(url);
    });
    buttons->addWidget(original);
    auto *close = new QPushButton(QStringLiteral("Close"));
    close->setObjectName(QStringLiteral("ghostButton"));
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    auto *left = new QShortcut(QKeySequence(Qt::Key_Left), this);
    connect(left, &QShortcut::activated, this, [this] { showItem(m_index - 1); });
    auto *right = new QShortcut(QKeySequence(Qt::Key_Right), this);
    connect(right, &QShortcut::activated, this, [this] { showItem(m_index + 1); });
    showItem(index);
}

void MediaViewer::showItem(int index)
{
    if (m_media.isEmpty())
        return;
    m_index = qBound(0, index, static_cast<int>(m_media.size()) - 1);
    m_previous->setVisible(m_media.size() > 1);
    m_next->setVisible(m_media.size() > 1);
    m_previous->setEnabled(m_index > 0);
    m_next->setEnabled(m_index < m_media.size() - 1);
    const QJsonObject item = m_media.at(m_index).toObject();
    const QString type = item.value(QStringLiteral("type")).toString();
    const QString description = item.value(QStringLiteral("description")).toString();
    QString caption = description.isEmpty() ? QStringLiteral("No description.") : description;
    if (type != QLatin1String("image"))
        caption.prepend(QStringLiteral("This is a %1; use Open original to play it. ").arg(type == QLatin1String("audio") ? QStringLiteral("sound clip") : QStringLiteral("video")));
    m_caption->setText(caption);
    m_image->setText(QStringLiteral("Loading…"));
    const QString url = type == QLatin1String("image") ? item.value(QStringLiteral("url")).toString()
                                                        : item.value(QStringLiteral("preview_url")).toString();
    if (!m_client || url.isEmpty())
        return;
    QPointer<MediaViewer> guard(this);
    const int shown = m_index;
    m_client->image(url, [guard, shown](const QImage &image) {
        if (!guard || guard->m_index != shown)
            return;
        const QSize box = guard->m_image->size();
        QPixmap pixmap = QPixmap::fromImage(image);
        if (image.width() > box.width() || image.height() > box.height())
            pixmap = pixmap.scaled(box, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        guard->m_image->setPixmap(pixmap);
    });
}

QJsonObject filterBody(const QJsonObject &existing, const QString &title, const QStringList &contexts,
                       const QString &action, int expiresIn, const QStringList &keywords, bool wholeWord)
{
    QJsonObject body;
    body.insert(QStringLiteral("title"), title);
    body.insert(QStringLiteral("context"), QJsonArray::fromStringList(contexts));
    body.insert(QStringLiteral("filter_action"), action);
    if (expiresIn > 0)
        body.insert(QStringLiteral("expires_in"), expiresIn);
    else if (!existing.isEmpty())
        body.insert(QStringLiteral("expires_in"), QString());
    QJsonArray attributes;
    QStringList kept;
    for (const QJsonValue &value : existing.value(QStringLiteral("keywords")).toArray()) {
        const QJsonObject keyword = value.toObject();
        const QString text = keyword.value(QStringLiteral("keyword")).toString();
        QJsonObject row{{QStringLiteral("id"), keyword.value(QStringLiteral("id")).toString()}};
        if (keywords.contains(text)) {
            row.insert(QStringLiteral("keyword"), text);
            row.insert(QStringLiteral("whole_word"), wholeWord);
            kept << text;
        } else {
            row.insert(QStringLiteral("_destroy"), true);
        }
        attributes.append(row);
    }
    for (const QString &keyword : keywords) {
        if (kept.contains(keyword))
            continue;
        attributes.append(QJsonObject{{QStringLiteral("keyword"), keyword}, {QStringLiteral("whole_word"), wholeWord}});
        kept << keyword;
    }
    body.insert(QStringLiteral("keywords_attributes"), attributes);
    return body;
}

FilterDialog::FilterDialog(const QJsonObject &filter, QWidget *parent)
    : QDialog(parent), m_filter(filter)
{
    setWindowTitle(filter.isEmpty() ? QStringLiteral("New filter") : QStringLiteral("Edit filter"));
    setMinimumWidth(460);
    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    m_title = new QLineEdit(filter.value(QStringLiteral("title")).toString());
    m_title->setPlaceholderText(QStringLiteral("For example: Spoilers"));
    form->addRow(QStringLiteral("Name"), m_title);
    auto *contexts = new QWidget;
    auto *contextRow = new QHBoxLayout(contexts);
    contextRow->setContentsMargins(0, 0, 0, 0);
    const QJsonArray current = filter.value(QStringLiteral("context")).toArray();
    const QList<QPair<QString, QString>> names{
        {QStringLiteral("home"), QStringLiteral("Home & lists")}, {QStringLiteral("notifications"), QStringLiteral("Notifications")},
        {QStringLiteral("public"), QStringLiteral("Public timelines")}, {QStringLiteral("thread"), QStringLiteral("Conversations")},
        {QStringLiteral("account"), QStringLiteral("Profiles")},
    };
    for (const auto &name : names) {
        auto *box = new QCheckBox(name.second);
        box->setProperty("context", name.first);
        box->setChecked(filter.isEmpty() ? name.first != QLatin1String("account") : current.contains(name.first));
        m_contexts.append(box);
        contextRow->addWidget(box);
    }
    form->addRow(QStringLiteral("Filter in"), contexts);
    m_action = new QComboBox;
    m_action->addItem(QStringLiteral("Hide behind a warning"), QStringLiteral("warn"));
    m_action->addItem(QStringLiteral("Hide completely"), QStringLiteral("hide"));
    m_action->addItem(QStringLiteral("Blur media only"), QStringLiteral("blur"));
    const int actionIndex = m_action->findData(filter.value(QStringLiteral("filter_action")).toString());
    if (actionIndex >= 0)
        m_action->setCurrentIndex(actionIndex);
    form->addRow(QStringLiteral("Action"), m_action);
    m_expires = new QComboBox;
    m_expires->addItem(filter.value(QStringLiteral("expires_at")).toString().isEmpty() ? QStringLiteral("Never")
                                                                                     : QStringLiteral("Keep current expiry"),
                       filter.value(QStringLiteral("expires_at")).toString().isEmpty() ? 0 : -1);
    if (!filter.value(QStringLiteral("expires_at")).toString().isEmpty())
        m_expires->addItem(QStringLiteral("Never"), 0);
    for (const auto &entry : QList<QPair<int, QString>>{{1800, QStringLiteral("30 minutes")}, {3600, QStringLiteral("1 hour")},
                                                         {21600, QStringLiteral("6 hours")}, {43200, QStringLiteral("12 hours")},
                                                         {86400, QStringLiteral("1 day")}, {604800, QStringLiteral("1 week")}})
        m_expires->addItem(entry.second, entry.first);
    form->addRow(QStringLiteral("Expires after"), m_expires);
    QStringList keywords;
    bool wholeWord = true;
    for (const QJsonValue &value : filter.value(QStringLiteral("keywords")).toArray()) {
        keywords << value.toObject().value(QStringLiteral("keyword")).toString();
        wholeWord = value.toObject().value(QStringLiteral("whole_word")).toBool(true);
    }
    m_keywords = new QPlainTextEdit(keywords.join(QLatin1Char('\n')));
    m_keywords->setPlaceholderText(QStringLiteral("One word or phrase per line"));
    m_keywords->setMinimumHeight(110);
    form->addRow(QStringLiteral("Keywords"), m_keywords);
    m_wholeWord = new QCheckBox(QStringLiteral("Match whole words only"));
    m_wholeWord->setChecked(wholeWord);
    form->addRow(QString(), m_wholeWord);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        if (!m_title->text().trimmed().isEmpty())
            accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QJsonObject FilterDialog::body() const
{
    QStringList contexts;
    for (const QCheckBox *box : m_contexts) {
        if (box->isChecked())
            contexts << box->property("context").toString();
    }
    QStringList keywords;
    for (const QString &line : m_keywords->toPlainText().split(QLatin1Char('\n'))) {
        if (!line.trimmed().isEmpty() && !keywords.contains(line.trimmed()))
            keywords << line.trimmed();
    }
    QJsonObject body = filterBody(m_filter, m_title->text().trimmed(), contexts, m_action->currentData().toString(),
                                  qMax(0, m_expires->currentData().toInt()), keywords, m_wholeWord->isChecked());
    if (m_expires->currentData().toInt() < 0)
        body.remove(QStringLiteral("expires_in"));
    return body;
}

ReportDialog::ReportDialog(const QJsonObject &account, const QJsonObject &status, bool remote, QWidget *parent)
    : QDialog(parent), m_account(account), m_status(status)
{
    setWindowTitle(QStringLiteral("Report %1").arg(displayName(account)));
    setMinimumWidth(440);
    auto *layout = new QVBoxLayout(this);
    auto *intro = new QLabel(QStringLiteral("Your report goes to the moderators of your Mastodon server."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    m_category = new QComboBox;
    m_category->addItem(QStringLiteral("It is spam"), QStringLiteral("spam"));
    m_category->addItem(QStringLiteral("It is illegal"), QStringLiteral("legal"));
    m_category->addItem(QStringLiteral("Something else"), QStringLiteral("other"));
    form->addRow(QStringLiteral("Reason"), m_category);
    m_comment = new QPlainTextEdit;
    m_comment->setPlaceholderText(QStringLiteral("Anything the moderators should know"));
    m_comment->setMaximumHeight(110);
    form->addRow(QStringLiteral("Comment"), m_comment);
    layout->addLayout(form);
    m_includePost = new QCheckBox(QStringLiteral("Include this post"));
    m_includePost->setChecked(!status.isEmpty());
    m_includePost->setVisible(!status.isEmpty());
    layout->addWidget(m_includePost);
    m_forward = new QCheckBox(QStringLiteral("Also send an anonymous copy to the account's own server"));
    m_forward->setVisible(remote);
    layout->addWidget(m_forward);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    auto *send = buttons->addButton(QStringLiteral("Send report"), QDialogButtonBox::AcceptRole);
    send->setObjectName(QStringLiteral("dangerButton"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QJsonObject ReportDialog::body() const
{
    QJsonObject body{
        {QStringLiteral("account_id"), m_account.value(QStringLiteral("id")).toString()},
        {QStringLiteral("comment"), m_comment->toPlainText().trimmed()},
        {QStringLiteral("category"), m_category->currentData().toString()},
        {QStringLiteral("forward"), !m_forward->isHidden() && m_forward->isChecked()},
    };
    if (!m_status.isEmpty() && m_includePost->isChecked())
        body.insert(QStringLiteral("status_ids"), QJsonArray{m_status.value(QStringLiteral("id")).toString()});
    return body;
}

}
