#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::cove {

enum class EditorKind {
    Text,
    Spreadsheet,
    Pipeline,
};

struct FileTemplate {
    QString id;
    QString title;
    QString description;
    QString defaultPath;
    QString mime;
    QString body;
    EditorKind editor = EditorKind::Text;
};

// The ten quick-create choices presented in Cove Explorer. Keeping these as
// values (rather than UI-only strings) makes the encrypted starter content and
// editor routing testable without constructing MainWindow.
const QList<FileTemplate> &fileTemplates();
const FileTemplate *fileTemplate(const QString &id);

EditorKind editorKindForFile(const QString &path, const QString &mime);

// RFC 4180-style CSV helpers used by the spreadsheet editor. Rows may be
// ragged on input; serialization quotes commas, quotes and embedded newlines.
QList<QStringList> parseCsv(const QString &csv);
QString serializeCsv(const QList<QStringList> &rows);

QJsonObject defaultPipelineObject();
QString defaultPipelineJson();
bool parsePipeline(const QString &body, QJsonObject *pipeline,
                   QString *error = nullptr);

// Stable internal links delivered with Cove invitations and exposed by the
// sharing panel. No encryption material is embedded in the URL.
QString shareUrl(const QString &owner, const QString &repository,
                 const QString &coveId);
bool parseShareUrl(const QString &url, QString *owner, QString *repository,
                   QString *coveId);

} // namespace forkmesh::cove
