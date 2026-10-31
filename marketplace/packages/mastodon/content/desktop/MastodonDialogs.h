#pragma once

#include <QDateTime>
#include <QDialog>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace forkmesh::mastodon {

class Client;

struct ComposeLimits {
    int maxCharacters = 500;
    int maxMedia = 4;
    int maxOptions = 4;
    int maxOptionCharacters = 50;
};

struct ComposeDraft {
    QString text;
    QString spoiler;
    QString visibility = QStringLiteral("public");
    QString language;
    bool sensitive = false;
    QStringList mediaIds;
    QJsonArray mediaAttributes;
    QStringList pollOptions;
    int pollExpiresIn = 86400;
    bool pollMultiple = false;
    QDateTime scheduledAt;
    QString inReplyToId;
};

QJsonObject composeBody(const ComposeDraft &draft, bool editing);
QStringList languageCodes();
QString languageName(const QString &code);

struct PendingMedia {
    QString path;
    QString existingId;
    QString description;
    QString previewUrl;
    QImage preview;
};

class ComposeDialog : public QDialog
{
public:
    ComposeDialog(Client *client, const ComposeLimits &limits, QWidget *parent = nullptr);

    void setDefaults(const QString &visibility, const QString &language, bool sensitive);
    void setReplyTo(const QJsonObject &status, const QString &selfAcct);
    void setEditing(const QJsonObject &status, const QString &text, const QString &spoiler);
    void setText(const QString &text);
    void setSpoiler(const QString &spoiler);
    void setVisibility(const QString &visibility);
    void setPoll(const QStringList &options, int expiresIn, bool multiple);
    void setScheduledAt(const QDateTime &when);
    bool addFile(const QString &path, const QString &description = QString());
    void addExisting(const QJsonObject &attachment);
    ComposeDraft draft() const;
    int remaining() const;
    bool canSubmit() const;
    void submit();
    QString statusText() const;

    std::function<void(const QJsonObject &, bool)> published;

private:
    void updateCounter();
    void rebuildMedia();
    void uploadNext(int index, QStringList ids);
    void waitForMedia(const QString &id, int attempts, std::function<void(bool)> done);
    void finish(const QStringList &ids);
    void fail(const QString &message);
    void setBusy(bool busy, const QString &message = QString());
    void addPollOption(const QString &text = QString());

    Client *m_client = nullptr;
    ComposeLimits m_limits;
    QString m_editingId;
    QString m_replyId;
    QList<PendingMedia> m_media;
    QList<QLineEdit *> m_pollOptions;
    bool m_busy = false;

    QLabel *m_replyPreview = nullptr;
    QLineEdit *m_spoiler = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QHBoxLayout *m_mediaRow = nullptr;
    QWidget *m_mediaBox = nullptr;
    QWidget *m_pollBox = nullptr;
    QVBoxLayout *m_pollLayout = nullptr;
    QComboBox *m_pollDuration = nullptr;
    QCheckBox *m_pollMultiple = nullptr;
    QPushButton *m_addOption = nullptr;
    QCheckBox *m_sensitive = nullptr;
    QCheckBox *m_schedule = nullptr;
    QDateTimeEdit *m_scheduleAt = nullptr;
    QToolButton *m_attach = nullptr;
    QToolButton *m_pollToggle = nullptr;
    QToolButton *m_spoilerToggle = nullptr;
    QComboBox *m_visibility = nullptr;
    QComboBox *m_language = nullptr;
    QLabel *m_counter = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_submit = nullptr;
};

class MediaViewer : public QDialog
{
public:
    MediaViewer(Client *client, const QJsonArray &media, int index, QWidget *parent = nullptr);

private:
    void showItem(int index);

    Client *m_client = nullptr;
    QJsonArray m_media;
    int m_index = 0;
    QLabel *m_image = nullptr;
    QLabel *m_caption = nullptr;
    QPushButton *m_previous = nullptr;
    QPushButton *m_next = nullptr;
};

class FilterDialog : public QDialog
{
public:
    FilterDialog(const QJsonObject &filter, QWidget *parent = nullptr);
    QJsonObject body() const;

private:
    QJsonObject m_filter;
    QLineEdit *m_title = nullptr;
    QList<QCheckBox *> m_contexts;
    QComboBox *m_action = nullptr;
    QComboBox *m_expires = nullptr;
    QPlainTextEdit *m_keywords = nullptr;
    QCheckBox *m_wholeWord = nullptr;
};

QJsonObject filterBody(const QJsonObject &existing, const QString &title, const QStringList &contexts,
                       const QString &action, int expiresIn, const QStringList &keywords, bool wholeWord);

class ReportDialog : public QDialog
{
public:
    ReportDialog(const QJsonObject &account, const QJsonObject &status, bool remote, QWidget *parent = nullptr);
    QJsonObject body() const;

private:
    QJsonObject m_account;
    QJsonObject m_status;
    QComboBox *m_category = nullptr;
    QPlainTextEdit *m_comment = nullptr;
    QCheckBox *m_forward = nullptr;
    QCheckBox *m_includePost = nullptr;
};

}
