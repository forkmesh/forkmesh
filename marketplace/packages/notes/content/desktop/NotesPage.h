#pragma once

#include "NativePackageExtension.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QUrl>

class MarkdownEditor;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTimer;

namespace forkmesh::notes {

class NotesPage : public forkmesh::market::NativePackageWidget
{
public:
    using NoteReplyHandler =
        std::function<void(bool, const QJsonObject &, const QString &)>;

    NotesPage(const forkmesh::market::NativePackageHost &host, QWidget *parent);

    void refresh() override;
    QList<forkmesh::market::NativePackageSearchHit> searchHits(const QString &query,
                                                               int limit) const override;
    void activateSearchHit(const QString &id) override;
    bool acceptsHostCommand(const QString &command) const override;
    bool hostCommand(const QString &command, const QJsonObject &args) override;

    void openNoteFromSearch(const QString &id, const QString &storage);
    bool takeQuickAddNote(const QString &text, const QStringList &attachments);
    bool openNoteLink(const QString &href);
    void openNoteFromSystemLink(const QString &target);

private:
    void buildNotesSection();
    void refreshNotes();
    void renderNotesList(const QString &selectId = QString(), bool reselect = true);
    void selectNote(const QString &id, const QString &storage);
    void showOpenedNoteView();
    void createNote();
    void saveNote();
    void deleteNote();
    void updateNotePublishState();
    QJsonObject cloudNoteFor(const QJsonObject &note) const;
    void shareNote();
    void showNoteLinks();
    void renderNoteLinkedItems();
    void applyNoteShares(const QString &cloudId, const QJsonArray &shares);
    void applyNoteLinks(const QString &cloudId, const QJsonArray &links);
    void storeCloudNote(const QJsonObject &cloud);
    QString selectedNoteRole() const;
    QString selectedNoteAccessLink() const;
    QStringList noteRelayHosts() const;
    void copyNoteLink();
    void copyNotePublicLink();
    void exportNote();
    bool noteAutosaveApplies() const;
    void scheduleNoteAutosave();
    void autosaveNote();
    void markNoteClean(const QString &state = QString());
    void setNoteSaveState(const QString &text);
    void showNoteConflict(const QJsonObject &theirs);
    void clearNoteConflict();
    void keepMyNoteVersion();
    void takeTheirNoteVersion();
    void openNoteByCloudId(const QString &cloudId);
    void searchCloudNotes();
    void convertNoteToTask();
    void convertNoteToIssue();
    bool noteConversionSource(QString *title, QString *markdown);
    QString selectedNoteCloudId() const;
    QString selectedNotePublicUrl() const;
    void showNoteVersions();
    void requestNotes(const QByteArray &method, const QString &path,
                      const QJsonObject &body, NoteReplyHandler handler);
    QUrl catalogApiUrl() const;
    void flashMessage(const QString &text, bool error = false,
                      const QString &clickHref = QString(),
                      const QString &kind = QString());
    void openReferenceLink(const QString &href);
    void searchIndexChanged();

    forkmesh::market::NativePackageHost m_host;
    std::function<void()> m_searchIndexChanged;
    QListWidget *m_notesList = nullptr;
    QLineEdit *m_noteTitle = nullptr;
    MarkdownEditor *m_noteEditor = nullptr;
    QComboBox *m_noteStorageMode = nullptr;
    QCheckBox *m_notePublic = nullptr;
    QLabel *m_notePublicLink = nullptr;
    QLabel *m_notesStatus = nullptr;
    QJsonArray m_localNotes;
    QJsonArray m_cloudNotes;
    QJsonObject m_selectedNote;
    QString m_selectedNoteStorage;
    bool m_notesLoading = false;
    bool m_noteDirty = false;
    QLineEdit *m_noteSearch = nullptr;
    QList<QPushButton *> m_noteFilterButtons;
    QString m_noteFilter = QStringLiteral("all");
    QLabel *m_noteSaveState = nullptr;
    QPushButton *m_notePublicCopy = nullptr;
    QWidget *m_noteConflictBar = nullptr;
    QLabel *m_noteLinkedItems = nullptr;
    QTimer *m_noteAutosaveTimer = nullptr;
    QTimer *m_noteSearchTimer = nullptr;
    int m_noteSearchSequence = 0;
    QString m_noteSearchCloudQuery;
    QSet<QString> m_noteSearchCloudIds;
    QJsonObject m_noteConflict;
    bool m_noteSaveInFlight = false;
    bool m_noteSaveQueued = false;
};

}
