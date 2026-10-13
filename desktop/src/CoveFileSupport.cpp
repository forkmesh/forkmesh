#include "CoveFileSupport.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>
#include <QUrl>

namespace forkmesh::cove {
namespace {

QString indentedJson(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

FileTemplate makeTemplate(const QString &id, const QString &title,
                          const QString &description, const QString &defaultPath,
                          const QString &mime, const QString &body,
                          EditorKind editor = EditorKind::Text)
{
    return {id, title, description, defaultPath, mime, body, editor};
}

} // namespace

QJsonObject defaultPipelineObject()
{
    const QJsonArray stages{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("new")},
                    {QStringLiteral("name"), QStringLiteral("New")},
                    {QStringLiteral("color"), QStringLiteral("#58a6ff")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("qualified")},
                    {QStringLiteral("name"), QStringLiteral("Qualified")},
                    {QStringLiteral("color"), QStringLiteral("#a371f7")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("proposal")},
                    {QStringLiteral("name"), QStringLiteral("Proposal")},
                    {QStringLiteral("color"), QStringLiteral("#d29922")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("won")},
                    {QStringLiteral("name"), QStringLiteral("Won")},
                    {QStringLiteral("color"), QStringLiteral("#3fb950")}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("lost")},
                    {QStringLiteral("name"), QStringLiteral("Lost")},
                    {QStringLiteral("color"), QStringLiteral("#f85149")}},
    };
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("name"), QStringLiteral("Lead pipeline")},
            {QStringLiteral("stages"), stages},
            {QStringLiteral("leads"), QJsonArray()},
            {QStringLiteral("history"), QJsonArray()},
            {QStringLiteral("createdAt"),
             QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}};
}

QString defaultPipelineJson()
{
    return indentedJson(defaultPipelineObject());
}

const QList<FileTemplate> &fileTemplates()
{
    static const QList<FileTemplate> templates{
        makeTemplate(
            QStringLiteral("blank-note"), QStringLiteral("Blank note"),
            QStringLiteral("A clean Markdown document for ideas and drafts."),
            QStringLiteral("notes.md"), QStringLiteral("text/markdown"),
            QStringLiteral("# Notes\n\nStart writing here.\n")),
        makeTemplate(
            QStringLiteral("spreadsheet"), QStringLiteral("Spreadsheet"),
            QStringLiteral("An editable CSV grid for lists, tracking, and analysis."),
            QStringLiteral("spreadsheet.csv"), QStringLiteral("text/csv"),
            QStringLiteral("Name,Status,Owner,Due date\r\n"
                           "First item,Not started,,\r\n"),
            EditorKind::Spreadsheet),
        makeTemplate(
            QStringLiteral("lead-pipeline"), QStringLiteral("Lead pipeline"),
            QStringLiteral("Stages, leads, movement history, and value reporting."),
            QStringLiteral("leads.pipeline.json"),
            QStringLiteral("application/vnd.forkmesh.pipeline+json"),
            defaultPipelineJson(), EditorKind::Pipeline),
        makeTemplate(
            QStringLiteral("project-brief"), QStringLiteral("Project brief"),
            QStringLiteral("Goals, audience, scope, milestones, and success measures."),
            QStringLiteral("project-brief.md"), QStringLiteral("text/markdown"),
            QStringLiteral("# Project brief\n\n## Goal\n\n## Audience\n\n"
                           "## Scope\n\n### In scope\n\n### Out of scope\n\n"
                           "## Milestones\n\n## Success measures\n")),
        makeTemplate(
            QStringLiteral("meeting-notes"), QStringLiteral("Meeting notes"),
            QStringLiteral("Agenda, attendees, decisions, and follow-up actions."),
            QStringLiteral("meeting-notes.md"), QStringLiteral("text/markdown"),
            QStringLiteral("# Meeting notes\n\n**Date:** \n**Attendees:** \n\n"
                           "## Agenda\n\n## Notes\n\n## Decisions\n\n"
                           "## Action items\n\n- [ ] Owner — next step\n")),
        makeTemplate(
            QStringLiteral("contacts"), QStringLiteral("Contact list"),
            QStringLiteral("A spreadsheet ready for people and account details."),
            QStringLiteral("contacts.csv"), QStringLiteral("text/csv"),
            QStringLiteral("Name,Company,Role,Email,Phone,Notes\r\n"),
            EditorKind::Spreadsheet),
        makeTemplate(
            QStringLiteral("budget"), QStringLiteral("Budget tracker"),
            QStringLiteral("Plan and compare categories, estimates, and actual spend."),
            QStringLiteral("budget.csv"), QStringLiteral("text/csv"),
            QStringLiteral("Category,Item,Estimated,Actual,Owner,Notes\r\n"
                           "Operations,Example,0,0,,\r\n"),
            EditorKind::Spreadsheet),
        makeTemplate(
            QStringLiteral("checklist"), QStringLiteral("Checklist"),
            QStringLiteral("A lightweight Markdown checklist grouped by phase."),
            QStringLiteral("checklist.md"), QStringLiteral("text/markdown"),
            QStringLiteral("# Checklist\n\n## Prepare\n\n- [ ] First task\n\n"
                           "## Review\n\n- [ ] Final review\n")),
        makeTemplate(
            QStringLiteral("decision-log"), QStringLiteral("Decision log"),
            QStringLiteral("Record context, options, owners, and outcomes over time."),
            QStringLiteral("decision-log.md"), QStringLiteral("text/markdown"),
            QStringLiteral("# Decision log\n\n## Decision title\n\n"
                           "**Date:** \n**Owner:** \n**Status:** Proposed\n\n"
                           "### Context\n\n### Options considered\n\n"
                           "### Decision\n\n### Consequences\n")),
        makeTemplate(
            QStringLiteral("structured-data"), QStringLiteral("Structured JSON"),
            QStringLiteral("A valid JSON starting point for structured private data."),
            QStringLiteral("data.json"), QStringLiteral("application/json"),
            QStringLiteral("{\n  \"items\": []\n}\n")),
    };
    return templates;
}

const FileTemplate *fileTemplate(const QString &id)
{
    const QList<FileTemplate> &templates = fileTemplates();
    for (const FileTemplate &item : templates) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

EditorKind editorKindForFile(const QString &path, const QString &mime)
{
    const QString lowerPath = path.trimmed().toLower();
    const QString lowerMime = mime.trimmed().toLower();
    if (lowerMime == QLatin1String("application/vnd.forkmesh.pipeline+json") ||
        lowerPath.endsWith(QLatin1String(".pipeline.json")))
        return EditorKind::Pipeline;
    if (lowerMime == QLatin1String("text/csv") ||
        lowerPath.endsWith(QLatin1String(".csv")))
        return EditorKind::Spreadsheet;
    return EditorKind::Text;
}

QList<QStringList> parseCsv(const QString &csv)
{
    QList<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false;
    bool sawAnyCharacter = false;

    for (int i = 0; i < csv.size(); ++i) {
        const QChar ch = csv.at(i);
        sawAnyCharacter = true;
        if (quoted) {
            if (ch == QLatin1Char('"')) {
                if (i + 1 < csv.size() && csv.at(i + 1) == QLatin1Char('"')) {
                    field += QLatin1Char('"');
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                field += ch;
            }
            continue;
        }

        if (ch == QLatin1Char('"') && field.isEmpty()) {
            quoted = true;
        } else if (ch == QLatin1Char(',')) {
            row << field;
            field.clear();
        } else if (ch == QLatin1Char('\r') || ch == QLatin1Char('\n')) {
            row << field;
            field.clear();
            rows << row;
            row.clear();
            if (ch == QLatin1Char('\r') && i + 1 < csv.size() &&
                csv.at(i + 1) == QLatin1Char('\n'))
                ++i;
        } else {
            field += ch;
        }
    }

    // A terminal newline terminates the previous row; it does not create a new
    // empty record. An otherwise empty document still opens as one editable cell.
    const bool endedWithNewline =
        !csv.isEmpty() && (csv.endsWith(QLatin1Char('\n')) ||
                           csv.endsWith(QLatin1Char('\r')));
    if (!endedWithNewline || !row.isEmpty() || !field.isEmpty()) {
        row << field;
        rows << row;
    }
    if (!sawAnyCharacter || rows.isEmpty())
        rows = {QStringList{QString()}};
    return rows;
}

QString serializeCsv(const QList<QStringList> &rows)
{
    QStringList encodedRows;
    encodedRows.reserve(rows.size());
    for (const QStringList &row : rows) {
        QStringList encodedFields;
        encodedFields.reserve(row.size());
        for (QString field : row) {
            const bool quote = field.contains(QLatin1Char(',')) ||
                               field.contains(QLatin1Char('"')) ||
                               field.contains(QLatin1Char('\r')) ||
                               field.contains(QLatin1Char('\n'));
            if (quote) {
                field.replace(QStringLiteral("\""), QStringLiteral("\"\""));
                field = QLatin1Char('"') + field + QLatin1Char('"');
            }
            encodedFields << field;
        }
        encodedRows << encodedFields.join(QLatin1Char(','));
    }
    return encodedRows.join(QStringLiteral("\r\n")) + QStringLiteral("\r\n");
}

bool parsePipeline(const QString &body, QJsonObject *pipeline, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(body.toUtf8(), &parseError);
    if (!document.isObject()) {
        if (error) {
            *error = parseError.error == QJsonParseError::NoError
                         ? QStringLiteral("The pipeline root must be a JSON object.")
                         : QStringLiteral("Invalid pipeline JSON: %1")
                               .arg(parseError.errorString());
        }
        return false;
    }

    QJsonObject object = document.object();
    const QJsonArray stages = object.value(QStringLiteral("stages")).toArray();
    if (stages.isEmpty()) {
        if (error)
            *error = QStringLiteral("A pipeline needs at least one stage.");
        return false;
    }
    QSet<QString> stageIds;
    for (const QJsonValue &value : stages) {
        const QJsonObject stage = value.toObject();
        const QString id = stage.value(QStringLiteral("id")).toString().trimmed();
        const QString name = stage.value(QStringLiteral("name")).toString().trimmed();
        if (id.isEmpty() || name.isEmpty() || stageIds.contains(id)) {
            if (error)
                *error = QStringLiteral("Every pipeline stage needs a unique id and name.");
            return false;
        }
        stageIds.insert(id);
    }

    const QJsonValue leads = object.value(QStringLiteral("leads"));
    const QJsonValue history = object.value(QStringLiteral("history"));
    if ((!leads.isUndefined() && !leads.isArray()) ||
        (!history.isUndefined() && !history.isArray())) {
        if (error)
            *error = QStringLiteral("Pipeline leads and history must be JSON arrays.");
        return false;
    }
    if (!object.contains(QStringLiteral("leads")))
        object.insert(QStringLiteral("leads"), QJsonArray());
    if (!object.contains(QStringLiteral("history")))
        object.insert(QStringLiteral("history"), QJsonArray());
    if (!object.contains(QStringLiteral("version")))
        object.insert(QStringLiteral("version"), 1);
    if (pipeline)
        *pipeline = object;
    if (error)
        error->clear();
    return true;
}

QString shareUrl(const QString &owner, const QString &repository,
                 const QString &coveId)
{
    if (owner.trimmed().isEmpty() || repository.trimmed().isEmpty() ||
        coveId.trimmed().isEmpty())
        return {};
    QUrl url;
    url.setScheme(QStringLiteral("forkmesh"));
    url.setHost(QStringLiteral("cove"));
    url.setPath(QLatin1Char('/') + owner.trimmed() + QLatin1Char('/') +
                repository.trimmed() + QLatin1Char('/') + coveId.trimmed());
    return url.toString(QUrl::FullyEncoded);
}

bool parseShareUrl(const QString &raw, QString *owner, QString *repository,
                   QString *coveId)
{
    const QUrl url(raw.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("forkmesh") ||
        url.host() != QLatin1String("cove"))
        return false;
    const QStringList parts =
        url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.size() != 3 || parts.at(0).trimmed().isEmpty() ||
        parts.at(1).trimmed().isEmpty() || parts.at(2).trimmed().isEmpty())
        return false;
    if (owner)
        *owner = parts.at(0);
    if (repository)
        *repository = parts.at(1);
    if (coveId)
        *coveId = parts.at(2);
    return true;
}

} // namespace forkmesh::cove
