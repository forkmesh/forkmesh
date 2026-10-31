#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QWidget>

#include <functional>

class QFrame;
class QLabel;
class QListWidget;
class QPushButton;
class QTextBrowser;

namespace forkmesh::notes {

class NotePageView : public QWidget
{
public:
    using Reply = std::function<void(bool, const QJsonObject &, const QString &)>;
    struct Hooks {
        std::function<void(const QByteArray &, const QString &,
                           const QJsonObject &, Reply)>
            request;
        std::function<QString()> me;
        std::function<void(const QString &)> openNote;
        std::function<void()> back;
        std::function<void()> customize;
        std::function<void()> share;
        std::function<void(const QString &, const QString &)> copyLink;
        std::function<void(const QJsonObject &)> loaded;
        std::function<void(const QString &)> noteViewed;
    };

    explicit NotePageView(Hooks hooks, QWidget *parent = nullptr);

    void showPage(const QString &pageId, const QString &noteId = QString());
    void reload();
    QString pageId() const { return m_pageId; }
    QJsonObject page() const { return m_page; }
    QJsonObject notes() const { return m_notes; }
    QString currentNote() const { return m_current; }

private:
    void apply(const QJsonObject &payload);
    void renderHeader();
    void renderOutline();
    void renderContent();
    void pickEntry(int index, bool open);
    void pickNote(const QString &noteId);
    void fetchMissing(const QStringList &ids);
    QString markdownFor(const QString &noteId) const;
    void setStatus(const QString &text);

    Hooks m_hooks;
    QString m_pageId;
    QString m_current;
    QString m_wantedNote;
    QJsonObject m_page;
    QJsonObject m_notes;
    QHash<QString, QString> m_markdown;
    QSet<QString> m_fetching;
    QSet<QString> m_collapsed;
    int m_sequence = 0;

    QPushButton *m_back = nullptr;
    QLabel *m_title = nullptr;
    QFrame *m_accentBar = nullptr;
    QLabel *m_description = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_openEditor = nullptr;
    QPushButton *m_copyLink = nullptr;
    QPushButton *m_share = nullptr;
    QPushButton *m_customize = nullptr;
    QListWidget *m_outline = nullptr;
    QTextBrowser *m_content = nullptr;
};

}
