#include "SurveysModel.h"

#include <QDateTime>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace forkmesh::surveys {

namespace {

const QString kDot = QString::fromUtf8(" \xC2\xB7 ");
const QString kDash = QString::fromUtf8("\xE2\x80\x94");

QLocale english()
{
    return QLocale(QLocale::English, QLocale::UnitedStates);
}

bool flag(const QJsonObject &object, const char *key, bool fallback)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isBool() ? value.toBool() : fallback;
}

QString dateText(qint64 stamp, const QTimeZone &zone)
{
    if (stamp <= 0)
        return kDash;
    return english().toString(QDateTime::fromMSecsSinceEpoch(stamp, zone).date(), QStringLiteral("d MMM yyyy"));
}

}

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

qint64 integer(const QJsonObject &object, const char *key)
{
    return qint64(std::llround(object.value(QLatin1String(key)).toDouble()));
}

SurveySettings parseSettings(const QJsonObject &object)
{
    SurveySettings settings;
    settings.oneResponsePerPerson = flag(object, "oneResponsePerPerson", false);
    settings.opensAt = std::max<qint64>(0, integer(object, "opensAt"));
    settings.closesAt = std::max<qint64>(0, integer(object, "closesAt"));
    settings.maxResponses = int(std::clamp<qint64>(integer(object, "maxResponses"), 0, kMaxResponsesLimit));
    settings.notifyOnResponse = flag(object, "notifyOnResponse", true);
    settings.notifyOnVisit = flag(object, "notifyOnVisit", true);
    settings.shuffleOptions = flag(object, "shuffleOptions", false);
    return settings;
}

QJsonObject settingsJson(const SurveySettings &settings)
{
    return QJsonObject{
        {QStringLiteral("oneResponsePerPerson"), settings.oneResponsePerPerson},
        {QStringLiteral("opensAt"), double(std::max<qint64>(0, settings.opensAt))},
        {QStringLiteral("closesAt"), double(std::max<qint64>(0, settings.closesAt))},
        {QStringLiteral("maxResponses"), std::clamp(settings.maxResponses, 0, kMaxResponsesLimit)},
        {QStringLiteral("notifyOnResponse"), settings.notifyOnResponse},
        {QStringLiteral("notifyOnVisit"), settings.notifyOnVisit},
        {QStringLiteral("shuffleOptions"), settings.shuffleOptions},
    };
}

Question parseQuestion(const QJsonObject &object)
{
    Question question;
    question.id = text(object, "id");
    const QString type = text(object, "type");
    if (!type.isEmpty())
        question.type = type;
    question.title = text(object, "title");
    question.help = text(object, "help");
    question.required = object.value(QStringLiteral("required")).toBool();
    for (const QJsonValue &value : object.value(QStringLiteral("options")).toArray())
        question.options << value.toString();
    question.scale = int(integer(object, "scale"));
    return question;
}

QList<Question> parseQuestions(const QJsonArray &items)
{
    QList<Question> questions;
    for (const QJsonValue &value : items)
        questions.append(parseQuestion(value.toObject()));
    return questions;
}

QJsonObject questionJson(const Question &question)
{
    QJsonObject object{
        {QStringLiteral("type"), question.type},
        {QStringLiteral("title"), question.title},
        {QStringLiteral("help"), question.help},
        {QStringLiteral("required"), question.required},
        {QStringLiteral("options"), isChoiceType(question.type) ? QJsonArray::fromStringList(question.options)
                                                                : QJsonArray()},
        {QStringLiteral("scale"), isRatingType(question.type) ? question.scale : 0},
    };
    if (!question.id.isEmpty())
        object.insert(QStringLiteral("id"), question.id);
    return object;
}

QJsonArray questionsJson(const QList<Question> &questions)
{
    QJsonArray items;
    for (const Question &question : questions)
        items.append(questionJson(question));
    return items;
}

QuestionResult parseResult(const QJsonObject &object)
{
    QuestionResult result;
    result.id = text(object, "id");
    result.type = text(object, "type");
    result.title = text(object, "title");
    result.answered = std::max<qint64>(0, integer(object, "answered"));
    result.skipped = std::max<qint64>(0, integer(object, "skipped"));
    for (const QJsonValue &value : object.value(QStringLiteral("options")).toArray()) {
        const QJsonObject option = value.toObject();
        result.options.append({text(option, "label"), std::max<qint64>(0, integer(option, "count"))});
    }
    result.average = object.value(QStringLiteral("average")).toDouble();
    for (const QJsonValue &value : object.value(QStringLiteral("distribution")).toArray())
        result.distribution.append(std::max<qint64>(0, qint64(std::llround(value.toDouble()))));
    for (const QJsonValue &value : object.value(QStringLiteral("answers")).toArray()) {
        const QString answer = value.toString();
        if (!answer.trimmed().isEmpty())
            result.answers << answer;
    }
    return result;
}

Survey parseSurvey(const QJsonObject &object)
{
    Survey survey;
    survey.id = text(object, "id");
    survey.organization = text(object, "organization");
    survey.title = text(object, "title");
    survey.description = text(object, "description");
    survey.thankYou = text(object, "thankYou");
    survey.status = text(object, "status");
    if (survey.status.isEmpty())
        survey.status = QStringLiteral("draft");
    survey.displayStatus = text(object, "displayStatus");
    survey.settings = parseSettings(object.value(QStringLiteral("settings")).toObject());
    survey.responseCount = std::max<qint64>(0, integer(object, "responseCount"));
    survey.lastResponseAt = integer(object, "lastResponseAt");
    survey.publishedAt = integer(object, "publishedAt");
    survey.closedAt = integer(object, "closedAt");
    survey.createdBy = text(object, "createdBy");
    survey.updatedBy = text(object, "updatedBy");
    survey.createdAt = integer(object, "createdAt");
    survey.updatedAt = integer(object, "updatedAt");
    survey.canDelete = object.value(QStringLiteral("canDelete")).toBool();
    survey.publicUrl = text(object, "publicUrl");
    survey.detailed = object.value(QStringLiteral("questions")).isArray();
    survey.questions = parseQuestions(object.value(QStringLiteral("questions")).toArray());
    for (const QJsonValue &value : object.value(QStringLiteral("results")).toArray())
        survey.results.append(parseResult(value.toObject()));
    survey.activity = parseActivity(object.value(QStringLiteral("activity")).toObject());
    survey.questionCount = object.contains(QStringLiteral("questionCount"))
        ? std::max<qint64>(0, integer(object, "questionCount"))
        : survey.questions.size();
    return survey;
}

QList<Survey> parseSurveys(const QJsonArray &items)
{
    QList<Survey> surveys;
    for (const QJsonValue &value : items)
        surveys.append(parseSurvey(value.toObject()));
    return surveys;
}

SurveyResponse parseResponse(const QJsonObject &object)
{
    return {text(object, "id"), integer(object, "submittedAt"), object.value(QStringLiteral("answers")).toObject()};
}

QList<SurveyResponse> parseResponses(const QJsonArray &items)
{
    QList<SurveyResponse> responses;
    for (const QJsonValue &value : items)
        responses.append(parseResponse(value.toObject()));
    return responses;
}

SurveyActivity parseActivity(const QJsonObject &object)
{
    SurveyActivity activity;
    if (object.contains(QStringLiteral("days")))
        activity.days = std::max<qint64>(1, integer(object, "days"));
    activity.views = std::max<qint64>(0, integer(object, "views"));
    activity.visitors = std::max<qint64>(0, integer(object, "visitors"));
    activity.interactions = std::max<qint64>(0, integer(object, "interactions"));
    activity.starts = std::max<qint64>(0, integer(object, "starts"));
    activity.submits = std::max<qint64>(0, integer(object, "submits"));
    activity.leaves = std::max<qint64>(0, integer(object, "leaves"));
    activity.lastActivityAt = std::max<qint64>(0, integer(object, "lastActivityAt"));
    for (const QJsonValue &value : object.value(QStringLiteral("recent")).toArray()) {
        const QJsonObject item = value.toObject();
        activity.recent.append({text(item, "kind"), text(item, "detail"), text(item, "label"), text(item, "visitor"),
                                integer(item, "at")});
    }
    return activity;
}

Summary parseSummary(const QJsonObject &object)
{
    return {integer(object, "surveys"), integer(object, "open"), integer(object, "drafts"),
            integer(object, "closed"), integer(object, "responses"), integer(object, "responsesLast7Days")};
}

QString statusKey(const Survey &survey)
{
    if (!survey.displayStatus.isEmpty())
        return survey.displayStatus;
    return survey.status.isEmpty() ? QStringLiteral("draft") : survey.status;
}

QString statusLabel(const QString &key)
{
    static const QHash<QString, QString> labels{
        {QStringLiteral("draft"), QStringLiteral("Draft")},
        {QStringLiteral("open"), QStringLiteral("Open")},
        {QStringLiteral("scheduled"), QStringLiteral("Scheduled")},
        {QStringLiteral("closed"), QStringLiteral("Closed")},
    };
    return labels.value(key, key);
}

QStringList filterKeys()
{
    return {QString(), QStringLiteral("open"), QStringLiteral("draft"), QStringLiteral("closed")};
}

QString filterLabel(const QString &filter)
{
    if (filter.isEmpty())
        return QStringLiteral("All surveys");
    if (filter == QLatin1String("draft"))
        return QStringLiteral("Drafts");
    return statusLabel(filter);
}

bool matchesFilter(const Survey &survey, const QString &filter)
{
    if (filter.isEmpty())
        return true;
    const QString key = statusKey(survey);
    if (filter == QLatin1String("open"))
        return key == QLatin1String("open") || key == QLatin1String("scheduled");
    return key == filter;
}

bool matchesQuery(const Survey &survey, const QString &query)
{
    const QString needle = query.trimmed();
    return needle.isEmpty() || survey.title.contains(needle, Qt::CaseInsensitive);
}

int openCount(const QList<Survey> &surveys)
{
    return int(std::count_if(surveys.cbegin(), surveys.cend(),
                             [](const Survey &survey) { return statusKey(survey) == QLatin1String("open"); }));
}

QString surveyTitle(const Survey &survey)
{
    return survey.title.trimmed().isEmpty() ? QStringLiteral("Untitled survey") : survey.title.trimmed();
}

bool canOpen(const Survey &survey)
{
    return survey.status == QLatin1String("draft");
}

bool canClose(const Survey &survey)
{
    return survey.status == QLatin1String("open");
}

bool canReopen(const Survey &survey)
{
    return survey.status == QLatin1String("closed");
}

bool canDuplicate(const Survey &survey)
{
    return !survey.id.isEmpty();
}

bool canDeleteSurvey(const Survey &survey)
{
    return !survey.id.isEmpty() && survey.canDelete;
}

bool canCopyLink(const Survey &survey)
{
    return survey.status != QLatin1String("draft") && !survey.publicUrl.isEmpty();
}

bool canExport(const Survey &survey)
{
    return !survey.id.isEmpty() && survey.responseCount > 0;
}

QStringList questionTypeKeys()
{
    return {QStringLiteral("single"), QStringLiteral("multiple"), QStringLiteral("text"),
            QStringLiteral("paragraph"), QStringLiteral("rating")};
}

QString questionTypeLabel(const QString &type)
{
    static const QHash<QString, QString> labels{
        {QStringLiteral("single"), QStringLiteral("One choice")},
        {QStringLiteral("multiple"), QStringLiteral("Multiple choice")},
        {QStringLiteral("text"), QStringLiteral("Short text")},
        {QStringLiteral("paragraph"), QStringLiteral("Paragraph")},
        {QStringLiteral("rating"), QStringLiteral("Rating")},
    };
    return labels.value(type, type);
}

bool isChoiceType(const QString &type)
{
    return type == QLatin1String("single") || type == QLatin1String("multiple");
}

bool isRatingType(const QString &type)
{
    return type == QLatin1String("rating");
}

bool validQuestionId(const QString &id)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{8}$"));
    return pattern.match(id).hasMatch();
}

QStringList optionsFromText(const QString &value)
{
    return value.split(QLatin1Char('\n'));
}

QuestionCheck checkQuestion(const Question &question)
{
    QuestionCheck check;
    Question &clean = check.question;
    clean.id = validQuestionId(question.id.trimmed()) ? question.id.trimmed() : QString();
    clean.type = question.type.trimmed().toLower();
    clean.title = question.title.trimmed().left(kTitleLimit);
    clean.help = question.help.trimmed().left(kHelpLimit);
    clean.required = question.required;
    clean.options.clear();
    clean.scale = 0;
    QSet<QString> seen;
    QStringList options;
    for (const QString &option : question.options) {
        const QString label = option.trimmed().left(kOptionLimit).trimmed();
        if (label.isEmpty() || seen.contains(label))
            continue;
        seen.insert(label);
        options << label;
    }
    check.blank = clean.title.isEmpty() && clean.help.isEmpty() && options.isEmpty();
    if (!questionTypeKeys().contains(clean.type)) {
        check.error = QStringLiteral("Pick a question type.");
        return check;
    }
    if (isChoiceType(clean.type))
        clean.options = options;
    if (isRatingType(clean.type))
        clean.scale = question.scale == 0 ? kDefaultScale : question.scale;
    if (clean.title.isEmpty())
        check.error = QStringLiteral("Give the question a title.");
    else if (isChoiceType(clean.type) && clean.options.size() < kMinOptions)
        check.error = QStringLiteral("Add at least %1 different options.").arg(kMinOptions);
    else if (isChoiceType(clean.type) && clean.options.size() > kMaxOptions)
        check.error = QStringLiteral("A question can have at most %1 options.").arg(kMaxOptions);
    else if (isRatingType(clean.type) && (clean.scale < kMinScale || clean.scale > kMaxScale))
        check.error = QStringLiteral("Pick a rating scale from %1 to %2.").arg(kMinScale).arg(kMaxScale);
    return check;
}

QString questionsError(const QList<Question> &questions, QList<Question> *normalised)
{
    QList<Question> kept;
    QString error;
    for (int index = 0; index < questions.size(); ++index) {
        const QuestionCheck check = checkQuestion(questions.at(index));
        if (check.blank)
            continue;
        if (!check.error.isEmpty() && error.isEmpty())
            error = QStringLiteral("Question %1: %2").arg(index + 1).arg(check.error);
        kept.append(check.question);
    }
    if (error.isEmpty() && kept.size() > kMaxQuestions)
        error = QStringLiteral("A survey can have at most %1 questions.").arg(kMaxQuestions);
    if (normalised)
        *normalised = kept;
    return error;
}

bool moveQuestion(QList<Question> &questions, int index, int delta)
{
    const int target = index + delta;
    if (index < 0 || index >= questions.size() || target < 0 || target >= questions.size() || delta == 0)
        return false;
    questions.move(index, target);
    return true;
}

int percentOf(qint64 count, qint64 total)
{
    if (total <= 0 || count <= 0)
        return 0;
    return int(std::lround(100.0 * double(count) / double(total)));
}

QList<ResultLine> optionLines(const QuestionResult &result)
{
    QList<ResultLine> lines;
    for (const OptionCount &option : result.options)
        lines.append({option.label, option.count, percentOf(option.count, result.answered)});
    return lines;
}

QList<ResultLine> distributionLines(const QuestionResult &result)
{
    QList<ResultLine> lines;
    for (int index = 0; index < result.distribution.size(); ++index) {
        const qint64 count = result.distribution.at(index);
        lines.append({QString::number(index + 1), count, percentOf(count, result.answered)});
    }
    return lines;
}

QString resultLineText(const ResultLine &line)
{
    return QStringLiteral("%1: %2 (%3%)").arg(line.label).arg(line.count).arg(line.percent);
}

QString answeredText(const QuestionResult &result)
{
    QString value = QStringLiteral("%1 answered").arg(result.answered);
    if (result.skipped > 0)
        value += kDot + QStringLiteral("%1 skipped").arg(result.skipped);
    return value;
}

QString averageText(const QuestionResult &result)
{
    if (result.answered <= 0)
        return QStringLiteral("No ratings yet");
    const QString average = english().toString(result.average, 'f', 1);
    if (result.distribution.isEmpty())
        return QStringLiteral("Average %1").arg(average);
    return QStringLiteral("Average %1 out of %2").arg(average).arg(result.distribution.size());
}

QStringList resultTextLines(const QuestionResult &result)
{
    QStringList lines;
    if (isChoiceType(result.type)) {
        for (const ResultLine &line : optionLines(result))
            lines << resultLineText(line);
    } else if (isRatingType(result.type)) {
        lines << averageText(result);
        for (const ResultLine &line : distributionLines(result))
            lines << resultLineText(line);
    } else {
        if (result.answers.isEmpty())
            lines << QStringLiteral("No answers yet");
        lines << result.answers;
        if (result.answered > result.answers.size() && !result.answers.isEmpty())
            lines << QStringLiteral("Showing the %1 newest of %2 answers").arg(result.answers.size()).arg(result.answered);
    }
    return lines;
}

QString answerText(const QJsonValue &answer)
{
    if (answer.isString())
        return answer.toString().trimmed();
    if (answer.isArray()) {
        QStringList parts;
        for (const QJsonValue &value : answer.toArray()) {
            const QString part = answerText(value);
            if (!part.isEmpty())
                parts << part;
        }
        return parts.join(QStringLiteral("; "));
    }
    if (answer.isDouble()) {
        const double number = answer.toDouble();
        if (std::isfinite(number) && number == std::floor(number))
            return QString::number(qint64(number));
        return english().toString(number, 'g', 6);
    }
    if (answer.isBool())
        return answer.toBool() ? QStringLiteral("Yes") : QStringLiteral("No");
    return QString();
}

QList<QPair<QString, QString>> responseLines(const SurveyResponse &response, const QList<Question> &questions)
{
    QList<QPair<QString, QString>> lines;
    for (const Question &question : questions) {
        const QString value = answerText(response.answers.value(question.id));
        lines.append({question.title.trimmed().isEmpty() ? QStringLiteral("Untitled question") : question.title,
                      value.isEmpty() ? QStringLiteral("No answer") : value});
    }
    return lines;
}

QString dateTimeText(qint64 stamp, const QTimeZone &zone)
{
    if (stamp <= 0)
        return kDash;
    return english().toString(QDateTime::fromMSecsSinceEpoch(stamp, zone), QStringLiteral("d MMM yyyy, HH:mm"));
}

QString relativeText(qint64 nowMs, qint64 atMs, const QTimeZone &zone)
{
    if (atMs <= 0)
        return QString();
    const qint64 seconds = std::max<qint64>(0, (nowMs - atMs) / 1000);
    if (seconds < 60)
        return QStringLiteral("just now");
    if (seconds < 3600)
        return QStringLiteral("%1 min ago").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1 h ago").arg(seconds / 3600);
    if (seconds < 86400 * 7)
        return countPhrase(seconds / 86400, QStringLiteral("day")) + QStringLiteral(" ago");
    return dateText(atMs, zone);
}

QString countPhrase(qint64 count, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(count).arg(count == 1 ? one : (many.isEmpty() ? one + QLatin1Char('s') : many));
}

QString responsesText(const Survey &survey, qint64 nowMs, const QTimeZone &zone)
{
    if (survey.responseCount <= 0)
        return QStringLiteral("No responses yet");
    QString value = countPhrase(survey.responseCount, QStringLiteral("response"));
    if (survey.lastResponseAt > 0)
        value += kDot + QStringLiteral("last ") + relativeText(nowMs, survey.lastResponseAt, zone);
    return value;
}

QString scheduleText(const Survey &survey, qint64 nowMs, const QTimeZone &zone)
{
    const QString key = statusKey(survey);
    const SurveySettings &settings = survey.settings;
    QStringList parts;
    if (survey.status == QLatin1String("draft")) {
        parts << QStringLiteral("Not published; open it to get its link");
    } else if (key == QLatin1String("scheduled")) {
        parts << QStringLiteral("Opens ") + dateTimeText(settings.opensAt, zone);
        if (settings.closesAt > 0)
            parts << QStringLiteral("closes ") + dateTimeText(settings.closesAt, zone);
    } else if (survey.status == QLatin1String("open") && key == QLatin1String("closed")) {
        if (settings.closesAt > 0 && settings.closesAt <= nowMs)
            parts << QStringLiteral("Stopped taking answers at its closing time, ") + dateTimeText(settings.closesAt, zone);
        else if (settings.maxResponses > 0 && survey.responseCount >= settings.maxResponses)
            parts << QStringLiteral("Stopped taking answers at its limit of %1")
                         .arg(countPhrase(settings.maxResponses, QStringLiteral("response")));
        else
            parts << QStringLiteral("Not taking answers");
    } else if (survey.status == QLatin1String("open")) {
        parts << QStringLiteral("Taking answers");
        if (settings.closesAt > 0)
            parts << QStringLiteral("closes ") + dateTimeText(settings.closesAt, zone);
    } else if (survey.status == QLatin1String("closed")) {
        parts << (survey.closedAt > 0 ? QStringLiteral("Closed ") + relativeText(nowMs, survey.closedAt, zone)
                                      : QStringLiteral("Closed"));
    }
    if (settings.maxResponses > 0 && survey.status != QLatin1String("draft"))
        parts << QStringLiteral("%1 of %2 responses").arg(survey.responseCount).arg(settings.maxResponses);
    return parts.join(kDot);
}

QStringList settingsLines(const SurveySettings &settings, const QTimeZone &zone)
{
    QStringList lines;
    lines << (settings.oneResponsePerPerson ? QStringLiteral("One response per person")
                                            : QStringLiteral("People can answer more than once"));
    if (settings.opensAt > 0)
        lines << QStringLiteral("Opens ") + dateTimeText(settings.opensAt, zone);
    if (settings.closesAt > 0)
        lines << QStringLiteral("Closes ") + dateTimeText(settings.closesAt, zone);
    if (settings.maxResponses > 0)
        lines << QStringLiteral("At most %1").arg(countPhrase(settings.maxResponses, QStringLiteral("response")));
    lines << (settings.notifyOnResponse ? QStringLiteral("Pings the creator when responses arrive")
                                        : QStringLiteral("No Pings for new responses"));
    lines << (settings.notifyOnVisit ? QStringLiteral("Pings the creator when someone opens or starts it")
                                     : QStringLiteral("No Pings for visits"));
    if (settings.shuffleOptions)
        lines << QStringLiteral("Choice options shown in random order");
    return lines;
}

QString activityText(const SurveyActivity &activity, qint64 nowMs, const QTimeZone &zone)
{
    const QString period = QStringLiteral("last %1").arg(countPhrase(activity.days, QStringLiteral("day")));
    if (activity.views <= 0 && activity.interactions <= 0)
        return QStringLiteral("No visits in the ") + period;
    QStringList parts{countPhrase(activity.views, QStringLiteral("view")),
                      countPhrase(activity.visitors, QStringLiteral("visitor")),
                      countPhrase(activity.interactions, QStringLiteral("interaction")),
                      QStringLiteral("%1 started").arg(activity.starts),
                      QStringLiteral("%1 sent").arg(activity.submits)};
    if (activity.lastActivityAt > 0)
        parts << QStringLiteral("last activity ") + relativeText(nowMs, activity.lastActivityAt, zone);
    QString lead = period;
    lead[0] = lead.at(0).toUpper();
    return lead + QStringLiteral(": ") + parts.join(kDot);
}

QString visitEventText(const VisitEvent &event, qint64 nowMs, const QTimeZone &zone)
{
    const QString label = event.label.isEmpty() ? event.kind : event.label;
    QStringList parts{label};
    parts << QStringLiteral("visitor ") + (event.visitor.isEmpty() ? QStringLiteral("unknown") : event.visitor);
    if (event.at > 0)
        parts << relativeText(nowMs, event.at, zone);
    return parts.join(kDot);
}

QString notInstalledError(const QJsonObject &payload)
{
    return text(payload, "error") == QLatin1String("plugin_not_installed") ? text(payload, "organization")
                                                                           : QString();
}

QString readableError(const QString &error)
{
    QString key = error.trimmed();
    key.replace(QLatin1Char(' '), QLatin1Char('_'));
    static const QHash<QString, QString> sentences{
        {QStringLiteral("survey_has_no_questions"), QStringLiteral("add at least one question before opening the survey")},
        {QStringLiteral("survey_delete_forbidden"),
         QStringLiteral("only the survey's creator or an organization owner or admin can delete it")},
        {QStringLiteral("invalid_question_options"),
         QStringLiteral("every choice question needs at least two different options")},
        {QStringLiteral("survey_not_found"), QStringLiteral("the survey no longer exists")},
        {QStringLiteral("response_not_found"), QStringLiteral("that response was already deleted")},
        {QStringLiteral("plugin_not_installed"), QStringLiteral("Surveys is not installed for this organization")},
        {QStringLiteral("too_many_questions"), QStringLiteral("a survey can have at most 50 questions")},
        {QStringLiteral("survey_close_date_passed"), QStringLiteral("its closing time has passed; change it first")},
        {QStringLiteral("survey_response_limit_reached"), QStringLiteral("it reached its response limit; raise it first")},
        {QStringLiteral("survey_not_open"), QStringLiteral("only an open survey can be closed")},
        {QStringLiteral("survey_catalog_full"), QStringLiteral("the organization already has 500 surveys")},
        {QStringLiteral("invalid_question_type"), QStringLiteral("a question has an unknown type")},
        {QStringLiteral("invalid_schedule"), QStringLiteral("the closing time must be after the opening time")},
        {QStringLiteral("organization_not_found"), QStringLiteral("you are not a member of this organization")},
    };
    if (sentences.contains(key))
        return sentences.value(key);
    QString readable = error.trimmed();
    readable.replace(QLatin1Char('_'), QLatin1Char(' '));
    return readable;
}

}
