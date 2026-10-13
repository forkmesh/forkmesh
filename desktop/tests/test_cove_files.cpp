#include "../src/CoveFileEditor.h"
#include "../src/CoveFileSupport.h"

#include <QApplication>
#include <QComboBox>
#include <QJsonArray>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        qInfo("PASS: %s", message);
    else {
        qCritical("FAIL: %s", message);
        ++failures;
    }
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    using namespace forkmesh::cove;

    const QList<FileTemplate> &templates = fileTemplates();
    check(templates.size() == 10,
          "Cove quick create exposes exactly ten supported templates");
    QSet<QString> ids;
    QSet<QString> paths;
    bool sawSpreadsheet = false;
    bool sawPipeline = false;
    for (const FileTemplate &item : templates) {
        ids.insert(item.id);
        paths.insert(item.defaultPath);
        check(!item.title.isEmpty() && !item.description.isEmpty() &&
                  !item.defaultPath.isEmpty() && !item.mime.isEmpty(),
              "each Cove template has complete picker metadata");
        if (item.editor == EditorKind::Spreadsheet) {
            sawSpreadsheet = true;
            check(!parseCsv(item.body).isEmpty(),
                  "spreadsheet starter content parses as CSV");
        } else if (item.editor == EditorKind::Pipeline) {
            sawPipeline = true;
            QJsonObject pipeline;
            check(parsePipeline(item.body, &pipeline) &&
                      pipeline.value(QStringLiteral("stages")).toArray().size() >= 3,
                  "pipeline starter content has editable stages");
        }
    }
    check(ids.size() == templates.size() && paths.size() == templates.size(),
          "Cove template ids and default paths are unique");
    check(sawSpreadsheet && sawPipeline,
          "Cove templates include spreadsheet and pipeline editors");

    const QString csv = QStringLiteral(
        "Name,Notes,Amount\r\n\"Acme, Inc.\",\"Line one\nLine two\",\"12\"\"34\"\r\n");
    const QList<QStringList> parsed = parseCsv(csv);
    check(parsed.size() == 2 && parsed.at(1).at(0) == QLatin1String("Acme, Inc.") &&
              parsed.at(1).at(1).contains(QLatin1Char('\n')) &&
              parsed.at(1).at(2) == QLatin1String("12\"34"),
          "CSV parsing preserves commas, multiline cells, and escaped quotes");
    check(parseCsv(serializeCsv(parsed)) == parsed,
          "CSV serialization round-trips spreadsheet cells");

    bool spreadsheetChanged = false;
    SpreadsheetEditor spreadsheet(QStringLiteral("A,B\r\n1,2\r\n"), false);
    spreadsheet.setChangeHandler(
        [&spreadsheetChanged](bool changed) { spreadsheetChanged = changed; });
    QTableWidget *grid =
        spreadsheet.findChild<QTableWidget *>(QStringLiteral("coveSpreadsheetGrid"));
    check(grid && grid->rowCount() == 2 && grid->columnCount() == 2,
          "spreadsheet editor presents CSV as an editable grid");
    if (grid && grid->item(1, 1))
        grid->item(1, 1)->setText(QStringLiteral("updated"));
    check(spreadsheetChanged && spreadsheet.isModified() &&
              spreadsheet.body().contains(QStringLiteral("updated")),
          "spreadsheet cell edits serialize back into the Cove document");
    SpreadsheetEditor emptyColumns(QStringLiteral("A,,\r\n,,\r\n"), false);
    const QList<QStringList> emptyColumnRows = parseCsv(emptyColumns.body());
    check(emptyColumnRows.size() == 2 && emptyColumnRows.at(0).size() == 3 &&
              emptyColumnRows.at(1).size() == 3,
          "spreadsheet saves preserve intentionally empty rows and columns");

    bool pipelineChanged = false;
    PipelineEditor pipeline(defaultPipelineJson(), QStringLiteral("alice"), false);
    pipeline.resize(760, 520);
    pipeline.show();
    app.processEvents();
    pipeline.setChangeHandler(
        [&pipelineChanged](bool changed) { pipelineChanged = changed; });
    QPushButton *addLead =
        pipeline.findChild<QPushButton *>(QStringLiteral("covePipelineAddLead"));
    QTableWidget *leads =
        pipeline.findChild<QTableWidget *>(QStringLiteral("covePipelineLeads"));
    check(addLead && leads, "pipeline editor exposes lead controls and a lead table");
    if (addLead)
        addLead->click();
    check(leads && leads->rowCount() == 1,
          "pipeline editor can add a lead without editing raw JSON");
    if (leads) {
        auto *stage = qobject_cast<QComboBox *>(leads->cellWidget(0, 4));
        check(stage && stage->count() == 5,
              "new leads can move through every configured pipeline stage");
        app.processEvents();
        check(stage && leads->columnWidth(4) >= stage->sizeHint().width() &&
                  leads->rowHeight(0) >= stage->sizeHint().height(),
              "pipeline stage dropdown is fully contained by its table cell");
        if (stage && stage->count() > 1)
            stage->setCurrentIndex(1);
    }
    QJsonObject savedPipeline;
    check(parsePipeline(pipeline.body(), &savedPipeline) && pipelineChanged &&
              savedPipeline.value(QStringLiteral("leads")).toArray().size() == 1 &&
              savedPipeline.value(QStringLiteral("history")).toArray().size() >= 2,
          "pipeline edits persist leads and append movement history");
    QTableWidget *report =
        pipeline.findChild<QTableWidget *>(QStringLiteral("covePipelineReport"));
    check(report && report->rowCount() == 5,
          "pipeline editor builds a stage report from current leads");

    const QString link = shareUrl(QStringLiteral("Alice Team"),
                                  QStringLiteral("Private Leads"),
                                  QStringLiteral("cove-123"));
    QString owner;
    QString repository;
    QString coveId;
    check(parseShareUrl(link, &owner, &repository, &coveId) &&
              owner == QLatin1String("Alice Team") &&
              repository == QLatin1String("Private Leads") &&
              coveId == QLatin1String("cove-123"),
          "Cove share links round-trip encoded repository names without secrets");
    check(!parseShareUrl(QStringLiteral("https://example.com/cove/123"), nullptr,
                         nullptr, nullptr),
          "Cove share-link parsing rejects external schemes");

    return failures == 0 ? 0 : 1;
}
