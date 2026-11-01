#include "SurveysModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cstdio>

using namespace forkmesh::surveys;

namespace {

int failures = 0;

void check(bool condition, const char *message, const QString &detail = QString())
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s%s%s\n", message, detail.isEmpty() ? "" : " -- ", detail.toUtf8().constData());
        ++failures;
    }
}

QJsonObject object(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

Question question(const QString &type, const QString &title, const QStringList &options = {}, int scale = 0)
{
    Question value;
    value.type = type;
    value.title = title;
    value.options = options;
    value.scale = scale;
    return value;
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString dot = QString::fromUtf8(" \xC2\xB7 ");
    const QString dash = QString::fromUtf8("\xE2\x80\x94");
    const QTimeZone utc = QTimeZone::utc();

    const Survey open = parseSurvey(object(R"({"id": "a", "organization": "acme", "title": "Team offsite feedback",
        "description": "How did it go?", "thankYou": "Thanks for answering.", "status": "open", "displayStatus": "open",
        "settings": {"oneResponsePerPerson": true, "opensAt": 0, "closesAt": 0, "maxResponses": 0,
                     "notifyOnResponse": false, "shuffleOptions": true},
        "questionCount": 4, "responseCount": 12, "lastResponseAt": 1000, "publishedAt": 900, "closedAt": 0,
        "createdBy": "alice", "updatedBy": "bob", "canDelete": true,
        "publicUrl": "https://app.forkmesh.com/survey?t=a.0123456789abcdef01234567"})"));
    const Survey draft = parseSurvey(object(R"({"id": "b", "title": "Lunch poll", "status": "draft",
        "displayStatus": "draft", "settings": {}, "questionCount": 0, "responseCount": 0, "canDelete": false,
        "publicUrl": ""})"));
    const Survey scheduled = parseSurvey(object(R"({"id": "c", "title": "Quarterly pulse", "status": "open",
        "displayStatus": "scheduled", "settings": {"opensAt": 1793145600000}, "responseCount": 0,
        "publicUrl": "https://app.forkmesh.com/survey?t=c.x"})"));
    const Survey expired = parseSurvey(object(R"({"id": "d", "title": "Old form", "status": "open",
        "displayStatus": "closed", "settings": {"closesAt": 1000}, "responseCount": 3,
        "publicUrl": "https://app.forkmesh.com/survey?t=d.x"})"));
    const Survey closed = parseSurvey(object(R"({"id": "e", "title": "Retro", "status": "closed",
        "displayStatus": "closed", "responseCount": 5, "closedAt": 1000, "canDelete": true,
        "publicUrl": "https://app.forkmesh.com/survey?t=e.x"})"));
    const Survey bare = parseSurvey(object(R"({"id": "f"})"));

    check(open.title == QStringLiteral("Team offsite feedback") && open.organization == QStringLiteral("acme") &&
              open.questionCount == 4 && open.responseCount == 12 && open.lastResponseAt == 1000 &&
              open.createdBy == QStringLiteral("alice") && open.canDelete && !open.detailed,
          "a list survey parses its fields");
    check(open.settings.oneResponsePerPerson && !open.settings.notifyOnResponse && open.settings.shuffleOptions,
          "settings parse their flags");
    check(!draft.settings.oneResponsePerPerson && draft.settings.notifyOnResponse && !draft.settings.shuffleOptions &&
              draft.settings.maxResponses == 0,
          "missing settings take the documented defaults");
    check(bare.status == QStringLiteral("draft") && statusKey(bare) == QStringLiteral("draft"),
          "a survey without a status is a draft");
    const SurveySettings clamped = parseSettings(object(R"({"maxResponses": 50000, "opensAt": -5})"));
    check(clamped.maxResponses == kMaxResponsesLimit && clamped.opensAt == 0, "settings are clamped to their limits");
    const QJsonObject settings = settingsJson(open.settings);
    check(settings.value(QStringLiteral("oneResponsePerPerson")).toBool() &&
              !settings.value(QStringLiteral("notifyOnResponse")).toBool() &&
              settings.value(QStringLiteral("shuffleOptions")).toBool() &&
              settings.value(QStringLiteral("maxResponses")).toInt() == 0 &&
              settings.value(QStringLiteral("opensAt")).toDouble() == 0 && settings.size() == 6,
          "settings serialise all six fields");

    check(statusKey(scheduled) == QStringLiteral("scheduled") && statusKey(expired) == QStringLiteral("closed"),
          "the shown status prefers displayStatus");
    check(statusLabel(QStringLiteral("scheduled")) == QStringLiteral("Scheduled") &&
              statusLabel(QStringLiteral("draft")) == QStringLiteral("Draft") &&
              statusLabel(QStringLiteral("open")) == QStringLiteral("Open") &&
              statusLabel(QStringLiteral("closed")) == QStringLiteral("Closed"),
          "statuses have labels");
    check(filterKeys().size() == 4 && filterLabel(QString()) == QStringLiteral("All surveys") &&
              filterLabel(QStringLiteral("draft")) == QStringLiteral("Drafts") &&
              filterLabel(QStringLiteral("closed")) == QStringLiteral("Closed"),
          "the filter offers all, open, drafts and closed");
    check(matchesFilter(open, QStringLiteral("open")) && matchesFilter(scheduled, QStringLiteral("open")) &&
              !matchesFilter(expired, QStringLiteral("open")) && !matchesFilter(draft, QStringLiteral("open")),
          "open lists open and scheduled surveys, not ones past their close");
    check(matchesFilter(expired, QStringLiteral("closed")) && matchesFilter(closed, QStringLiteral("closed")) &&
              matchesFilter(draft, QStringLiteral("draft")) && !matchesFilter(open, QStringLiteral("draft")) &&
              matchesFilter(draft, QString()),
          "closed and drafts filter by the shown status");
    check(matchesQuery(open, QStringLiteral("OFFSITE")) && matchesQuery(open, QStringLiteral("  ")) &&
              !matchesQuery(open, QStringLiteral("how did")) && !matchesQuery(draft, QStringLiteral("offsite")),
          "search matches the title only");
    check(openCount({open, draft, scheduled, expired, closed}) == 1, "the badge counts surveys taking answers");
    check(surveyTitle(bare) == QStringLiteral("Untitled survey") && surveyTitle(open) == open.title,
          "an empty title reads as untitled");

    check(canOpen(draft) && !canOpen(open) && !canOpen(closed), "only a draft is published");
    check(canClose(open) && canClose(scheduled) && canClose(expired) && !canClose(draft) && !canClose(closed),
          "an open survey can be closed");
    check(canReopen(closed) && !canReopen(open) && !canReopen(draft), "a closed survey can be reopened");
    check(canDuplicate(open) && canDuplicate(draft) && !canDuplicate(Survey{}), "any saved survey can be duplicated");
    check(canDeleteSurvey(open) && canDeleteSurvey(closed) && !canDeleteSurvey(draft),
          "delete follows the server's canDelete");
    check(canCopyLink(open) && canCopyLink(closed) && !canCopyLink(draft), "a published survey has a link to copy");
    check(canExport(open) && !canExport(draft) && !canExport(scheduled), "export needs responses");

    check(questionTypeKeys() == QStringList({QStringLiteral("single"), QStringLiteral("multiple"),
                                             QStringLiteral("text"), QStringLiteral("paragraph"),
                                             QStringLiteral("rating")}),
          "the five question types");
    check(questionTypeLabel(QStringLiteral("single")) == QStringLiteral("One choice") &&
              questionTypeLabel(QStringLiteral("multiple")) == QStringLiteral("Multiple choice") &&
              questionTypeLabel(QStringLiteral("text")) == QStringLiteral("Short text") &&
              questionTypeLabel(QStringLiteral("paragraph")) == QStringLiteral("Paragraph") &&
              questionTypeLabel(QStringLiteral("rating")) == QStringLiteral("Rating"),
          "question types have labels");
    check(isChoiceType(QStringLiteral("multiple")) && !isChoiceType(QStringLiteral("rating")) &&
              isRatingType(QStringLiteral("rating")) && !isRatingType(QStringLiteral("text")),
          "choice and rating types are recognised");
    check(validQuestionId(QStringLiteral("a1b2c3d4")) && !validQuestionId(QStringLiteral("A1B2C3D4")) &&
              !validQuestionId(QStringLiteral("a1b2c3")) && !validQuestionId(QString()),
          "question ids are 8 lowercase hex characters");

    Question choice = question(QStringLiteral("single"), QStringLiteral("  How was it?  "),
                               {QStringLiteral(" Great "), QString(), QStringLiteral("Fine"), QStringLiteral("Great"),
                                QStringLiteral("   ")});
    choice.id = QStringLiteral("a1b2c3d4");
    choice.help = QStringLiteral(" Pick one ");
    choice.required = true;
    choice.scale = 7;
    QuestionCheck checked = checkQuestion(choice);
    check(checked.error.isEmpty() && checked.question.title == QStringLiteral("How was it?") &&
              checked.question.help == QStringLiteral("Pick one") &&
              checked.question.options == QStringList({QStringLiteral("Great"), QStringLiteral("Fine")}) &&
              checked.question.scale == 0 && checked.question.required &&
              checked.question.id == QStringLiteral("a1b2c3d4"),
          "a choice question is trimmed, blank and repeated options dropped, scale cleared",
          checked.question.options.join(QLatin1Char('|')));
    check(!checkQuestion(question(QStringLiteral("multiple"), QStringLiteral("Pick"),
                                  {QStringLiteral("Only"), QStringLiteral("Only ")}))
               .error.isEmpty(),
          "fewer than two distinct options is an error");
    QStringList many;
    for (int index = 0; index < 31; ++index)
        many << QStringLiteral("Option %1").arg(index);
    check(checkQuestion(question(QStringLiteral("single"), QStringLiteral("Pick"), many)).error.contains(
              QStringLiteral("30")),
          "more than thirty options is an error");
    many.removeLast();
    check(checkQuestion(question(QStringLiteral("single"), QStringLiteral("Pick"), many)).error.isEmpty(),
          "thirty options are allowed");
    check(checkQuestion(question(QStringLiteral("single"), QStringLiteral("Pick"),
                                 {QString(250, QLatin1Char('x')), QStringLiteral("b")}))
                  .question.options.first()
                  .size() == kOptionLimit,
          "long options are cut to 200 characters");
    const QuestionCheck rating = checkQuestion(question(QStringLiteral("rating"), QStringLiteral("Rate it"),
                                                        {QStringLiteral("x"), QStringLiteral("y")}));
    check(rating.error.isEmpty() && rating.question.scale == kDefaultScale && rating.question.options.isEmpty(),
          "a rating defaults to a scale of five and keeps no options");
    check(!checkQuestion(question(QStringLiteral("rating"), QStringLiteral("Rate"), {}, 2)).error.isEmpty() &&
              !checkQuestion(question(QStringLiteral("rating"), QStringLiteral("Rate"), {}, 11)).error.isEmpty() &&
              checkQuestion(question(QStringLiteral("rating"), QStringLiteral("Rate"), {}, 3)).error.isEmpty() &&
              checkQuestion(question(QStringLiteral("rating"), QStringLiteral("Rate"), {}, 10)).error.isEmpty(),
          "a rating scale runs from 3 to 10");
    const QuestionCheck textual = checkQuestion(question(QStringLiteral("text"), QStringLiteral("Name"),
                                                         {QStringLiteral("x"), QStringLiteral("y")}, 5));
    check(textual.error.isEmpty() && textual.question.options.isEmpty() && textual.question.scale == 0,
          "text questions keep neither options nor scale");
    const QuestionCheck untitled = checkQuestion(question(QStringLiteral("paragraph"), QStringLiteral("   ")));
    check(untitled.blank && !untitled.error.isEmpty(), "a question needs a title");
    check(!checkQuestion(question(QStringLiteral("single"), QString(), {QStringLiteral("a"), QStringLiteral("b")}))
               .blank,
          "a question with options but no title is not blank");
    check(!checkQuestion(question(QStringLiteral("slider"), QStringLiteral("Slide"))).error.isEmpty(),
          "an unknown type is an error");
    Question badId = question(QStringLiteral("text"), QStringLiteral("Name"));
    badId.id = QStringLiteral("not-an-id");
    check(checkQuestion(badId).question.id.isEmpty(), "a malformed id is cleared for the server to replace");
    check(checkQuestion(question(QStringLiteral("text"), QString(400, QLatin1Char('t')))).question.title.size() ==
              kTitleLimit,
          "titles are cut to 300 characters");

    QList<Question> normalised;
    const QString listError = questionsError({question(QStringLiteral("text"), QStringLiteral("Name")),
                                              question(QStringLiteral("paragraph"), QString()),
                                              question(QStringLiteral("single"), QStringLiteral("Pick"),
                                                       {QStringLiteral("a")})},
                                             &normalised);
    check(listError.startsWith(QStringLiteral("Question 3:")) && normalised.size() == 2,
          "list errors name the question and blank questions are dropped", listError);
    QList<Question> fifty;
    for (int index = 0; index < 51; ++index)
        fifty << question(QStringLiteral("text"), QStringLiteral("Q%1").arg(index));
    check(questionsError(fifty).contains(QStringLiteral("50")), "a survey holds at most fifty questions");
    fifty.removeLast();
    check(questionsError(fifty).isEmpty(), "fifty questions are allowed");

    QList<Question> order{question(QStringLiteral("text"), QStringLiteral("A")),
                          question(QStringLiteral("text"), QStringLiteral("B")),
                          question(QStringLiteral("text"), QStringLiteral("C"))};
    check(moveQuestion(order, 0, 1) && order.at(0).title == QStringLiteral("B") &&
              order.at(1).title == QStringLiteral("A"),
          "a question moves down");
    check(moveQuestion(order, 2, -1) && order.at(1).title == QStringLiteral("C") &&
              !moveQuestion(order, 0, -1) && !moveQuestion(order, 2, 1),
          "a question moves up and stops at the ends");

    Question saved = question(QStringLiteral("rating"), QStringLiteral("Rate"), {QStringLiteral("x")}, 7);
    saved.id = QStringLiteral("0badcafe");
    const QJsonObject savedJson = questionJson(saved);
    check(savedJson.value(QStringLiteral("id")).toString() == QStringLiteral("0badcafe") &&
              savedJson.value(QStringLiteral("scale")).toInt() == 7 &&
              savedJson.value(QStringLiteral("options")).toArray().isEmpty(),
          "a rating question serialises its scale without options");
    const QJsonObject newJson = questionJson(question(QStringLiteral("multiple"), QStringLiteral("Pick"),
                                                      {QStringLiteral("a"), QStringLiteral("b")}, 4));
    check(!newJson.contains(QStringLiteral("id")) && newJson.value(QStringLiteral("scale")).toInt() == 0 &&
              newJson.value(QStringLiteral("options")).toArray().size() == 2,
          "a new question has no id and a choice question no scale");
    const QList<Question> roundTrip = parseQuestions(questionsJson({saved}));
    check(roundTrip.size() == 1 && roundTrip.first().id == saved.id && roundTrip.first().scale == 7 &&
              roundTrip.first().type == QStringLiteral("rating"),
          "questions read back from JSON");
    check(optionsFromText(QStringLiteral("a\nb\n\nc")).size() == 4, "options are one per line");

    const Survey detail = parseSurvey(object(R"({"id": "a", "title": "T", "status": "open", "questionCount": 3,
        "questions": [
            {"id": "a1b2c3d4", "type": "single", "title": "How was it?", "required": true,
             "options": ["Great", "Fine", "Poor"], "scale": 0},
            {"id": "b1b2c3d4", "type": "multiple", "title": "What did you like?", "options": ["Food", "Talks"]},
            {"id": "c1b2c3d4", "type": "rating", "title": "Rate the venue", "scale": 5},
            {"id": "d1b2c3d4", "type": "paragraph", "title": "Anything else?"}],
        "results": [
            {"id": "a1b2c3d4", "type": "single", "title": "How was it?", "answered": 10, "skipped": 2,
             "options": [{"label": "Great", "count": 7}, {"label": "Fine", "count": 3}, {"label": "Poor", "count": 0}],
             "average": 0, "distribution": [], "answers": []},
            {"id": "b1b2c3d4", "type": "multiple", "title": "What did you like?", "answered": 4, "skipped": 0,
             "options": [{"label": "Food", "count": 4}, {"label": "Talks", "count": 3}]},
            {"id": "c1b2c3d4", "type": "rating", "title": "Rate the venue", "answered": 8, "skipped": 4,
             "average": 4.25, "distribution": [0, 1, 1, 1, 5]},
            {"id": "d1b2c3d4", "type": "paragraph", "title": "Anything else?", "answered": 25, "skipped": 0,
             "answers": ["More coffee", "", "Great talks"]}]})"));
    check(detail.detailed && detail.questions.size() == 4 && detail.results.size() == 4 &&
              detail.questions.first().required && detail.questions.at(2).scale == 5,
          "a detail parses questions and results");
    const QuestionResult &single = detail.results.at(0);
    check(single.answered == 10 && single.skipped == 2 && single.options.size() == 3 &&
              single.options.first().label == QStringLiteral("Great") && single.options.first().count == 7,
          "choice results parse option counts");
    const QList<ResultLine> singleLines = optionLines(single);
    check(singleLines.size() == 3 && singleLines.at(0).percent == 70 && singleLines.at(1).percent == 30 &&
              singleLines.at(2).percent == 0,
          "option percentages are of the people who answered");
    check(resultLineText(singleLines.first()) == QStringLiteral("Great: 7 (70%)"), "a result line reads label, count and percent",
          resultLineText(singleLines.first()));
    check(answeredText(single) == QStringLiteral("10 answered") + dot + QStringLiteral("2 skipped") &&
              answeredText(detail.results.at(1)) == QStringLiteral("4 answered"),
          "answered and skipped counts", answeredText(single));
    const QList<ResultLine> multipleLines = optionLines(detail.results.at(1));
    check(multipleLines.at(0).percent == 100 && multipleLines.at(1).percent == 75,
          "multiple choice percentages can add up past 100");
    const QuestionResult &stars = detail.results.at(2);
    check(averageText(stars) == QStringLiteral("Average 4.3 out of 5") || averageText(stars) == QStringLiteral("Average 4.2 out of 5"),
          "a rating shows its average to one decimal and its scale", averageText(stars));
    const QList<ResultLine> spread = distributionLines(stars);
    check(spread.size() == 5 && spread.first().label == QStringLiteral("1") && spread.first().count == 0 &&
              spread.last().label == QStringLiteral("5") && spread.last().count == 5 && spread.last().percent == 63,
          "the distribution counts each rating from 1 up", QString::number(spread.last().percent));
    check(averageText(QuestionResult{}) == QStringLiteral("No ratings yet"), "a rating with no answers says so");
    check(resultTextLines(stars).size() == 6 && resultTextLines(stars).first().startsWith(QStringLiteral("Average")),
          "rating result lines are the average then the distribution");
    const QuestionResult &paragraph = detail.results.at(3);
    const QStringList paragraphLines = resultTextLines(paragraph);
    check(paragraph.answers == QStringList({QStringLiteral("More coffee"), QStringLiteral("Great talks")}) &&
              paragraphLines.size() == 3 && paragraphLines.last().contains(QStringLiteral("2 newest of 25")),
          "text answers drop empties and say how many are shown", paragraphLines.join(QLatin1Char('|')));
    check(resultTextLines(parseResult(object(R"({"type": "text", "answered": 0})"))) ==
              QStringList({QStringLiteral("No answers yet")}),
          "a text question with no answers says so");
    check(percentOf(1, 3) == 33 && percentOf(2, 3) == 67 && percentOf(5, 0) == 0, "percentages round");

    const QList<SurveyResponse> responses = parseResponses(object(R"({"responses": [
        {"id": "r2", "submittedAt": 2000, "answers": {"a1b2c3d4": "Great", "b1b2c3d4": ["Food", "Talks"],
                                                      "c1b2c3d4": 4, "zzzzzzzz": "stray"}},
        {"id": "r1", "submittedAt": 1000, "answers": {}}]})")
                                                        .value(QStringLiteral("responses"))
                                                        .toArray());
    check(responses.size() == 2 && responses.first().id == QStringLiteral("r2") &&
              responses.first().submittedAt == 2000,
          "responses parse in the order given");
    const QList<QPair<QString, QString>> lines = responseLines(responses.first(), detail.questions);
    check(lines.size() == 4 && lines.at(0) == qMakePair(QStringLiteral("How was it?"), QStringLiteral("Great")) &&
              lines.at(1).second == QStringLiteral("Food; Talks") && lines.at(2).second == QStringLiteral("4") &&
              lines.at(3).second == QStringLiteral("No answer"),
          "a response lists each question's answer in order", lines.at(1).second);
    check(answerText(QJsonValue(3.5)) == QStringLiteral("3.5") && answerText(QJsonValue()) == QString() &&
              answerText(QJsonValue(QStringLiteral("  hi "))) == QStringLiteral("hi"),
          "answers of every shape read as text");

    const Summary summary = parseSummary(object(R"({"surveys": 5, "open": 2, "drafts": 1, "closed": 2,
        "responses": 40, "responsesLast7Days": 9})"));
    check(summary.surveys == 5 && summary.open == 2 && summary.drafts == 1 && summary.closed == 2 &&
              summary.responses == 40 && summary.responsesLast7Days == 9,
          "the overview summary parses");

    const qint64 day = QDateTime(QDate(2026, 10, 28), QTime(14, 5), utc).toMSecsSinceEpoch();
    check(dateTimeText(day, utc) == QStringLiteral("28 Oct 2026, 14:05") && dateTimeText(0, utc) == dash,
          "date and time read as day month year and 24-hour time", dateTimeText(day, utc));
    check(relativeText(day + 30000, day, utc) == QStringLiteral("just now") &&
              relativeText(day + 5 * 60000, day, utc) == QStringLiteral("5 min ago") &&
              relativeText(day + 3 * 3600000LL, day, utc) == QStringLiteral("3 h ago") &&
              relativeText(day + 2 * kDayMs, day, utc) == QStringLiteral("2 days ago") &&
              relativeText(day + 30 * kDayMs, day, utc) == QStringLiteral("28 Oct 2026"),
          "relative times step from minutes to dates");
    check(countPhrase(1, QStringLiteral("response")) == QStringLiteral("1 response") &&
              countPhrase(3, QStringLiteral("response")) == QStringLiteral("3 responses"),
          "counts pluralize");
    check(responsesText(draft, day, utc) == QStringLiteral("No responses yet") &&
              responsesText(open, 1000 + 120000, utc) == QStringLiteral("12 responses") + dot + QStringLiteral("last 2 min ago"),
          "the response line counts and dates the last one", responsesText(open, 1000 + 120000, utc));

    check(scheduleText(draft, day, utc).startsWith(QStringLiteral("Not published")), "a draft is not published");
    check(scheduleText(scheduled, day - kDayMs, utc) == QStringLiteral("Opens 28 Oct 2026, 00:00"),
          "a scheduled survey says when it opens", scheduleText(scheduled, day - kDayMs, utc));
    check(scheduleText(expired, day, utc).contains(QStringLiteral("closing time")),
          "an open survey past its close says why it stopped", scheduleText(expired, day, utc));
    const Survey full = parseSurvey(object(R"({"id": "g", "status": "open", "displayStatus": "closed",
        "responseCount": 100, "settings": {"maxResponses": 100}})"));
    check(scheduleText(full, day, utc).contains(QStringLiteral("limit of 100 responses")) &&
              scheduleText(full, day, utc).contains(QStringLiteral("100 of 100 responses")),
          "a full survey says it reached its limit", scheduleText(full, day, utc));
    check(scheduleText(open, day, utc) == QStringLiteral("Taking answers"), "an open survey takes answers");
    check(scheduleText(closed, 1000 + 3 * 3600000LL, utc) == QStringLiteral("Closed 3 h ago"),
          "a closed survey says when", scheduleText(closed, 1000 + 3 * 3600000LL, utc));

    const QStringList settingLines = settingsLines(open.settings, utc);
    check(settingLines.contains(QStringLiteral("One response per person")) &&
              settingLines.contains(QStringLiteral("No Pings for new responses")) &&
              settingLines.contains(QStringLiteral("Choice options shown in random order")),
          "settings read as sentences", settingLines.join(QLatin1Char('|')));
    SurveySettings limited;
    limited.maxResponses = 250;
    limited.closesAt = day;
    check(settingsLines(limited, utc).contains(QStringLiteral("At most 250 responses")) &&
              settingsLines(limited, utc).contains(QStringLiteral("Closes 28 Oct 2026, 14:05")),
          "limits and dates are listed");

    check(notInstalledError(object(R"({"error": "plugin_not_installed", "organization": "acme", "canInstall": true})")) ==
              QStringLiteral("acme") &&
              notInstalledError(object(R"({"error": "not_found"})")).isEmpty(),
          "a 409 plugin_not_installed is recognised from its payload");
    check(readableError(QStringLiteral("survey has no questions")).contains(QStringLiteral("at least one question")) &&
              readableError(QStringLiteral("survey_delete_forbidden")).contains(QStringLiteral("creator")) &&
              readableError(QStringLiteral("invalid question options")).contains(QStringLiteral("two different")) &&
              readableError(QStringLiteral("something_else")) == QStringLiteral("something else"),
          "worker errors read as sentences");

    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
