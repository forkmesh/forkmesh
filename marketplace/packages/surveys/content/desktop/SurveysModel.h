#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QTimeZone>

namespace forkmesh::surveys {

inline constexpr qint64 kDayMs = 24LL * 60 * 60 * 1000;
inline constexpr int kMaxQuestions = 50;
inline constexpr int kMinOptions = 2;
inline constexpr int kMaxOptions = 30;
inline constexpr int kMinScale = 3;
inline constexpr int kMaxScale = 10;
inline constexpr int kDefaultScale = 5;
inline constexpr int kTitleLimit = 300;
inline constexpr int kHelpLimit = 1000;
inline constexpr int kOptionLimit = 200;
inline constexpr int kMaxResponsesLimit = 10000;
inline constexpr int kShownTextAnswers = 20;

struct SurveySettings {
    bool oneResponsePerPerson = false;
    qint64 opensAt = 0;
    qint64 closesAt = 0;
    int maxResponses = 0;
    bool notifyOnResponse = true;
    bool shuffleOptions = false;
};

struct Question {
    QString id;
    QString type = QStringLiteral("single");
    QString title;
    QString help;
    bool required = false;
    QStringList options;
    int scale = 0;
};

struct QuestionCheck {
    Question question;
    QString error;
    bool blank = false;
};

struct OptionCount {
    QString label;
    qint64 count = 0;
};

struct QuestionResult {
    QString id;
    QString type;
    QString title;
    qint64 answered = 0;
    qint64 skipped = 0;
    QList<OptionCount> options;
    double average = 0;
    QList<qint64> distribution;
    QStringList answers;
};

struct ResultLine {
    QString label;
    qint64 count = 0;
    int percent = 0;
};

struct Survey {
    QString id;
    QString organization;
    QString title;
    QString description;
    QString thankYou;
    QString status;
    QString displayStatus;
    SurveySettings settings;
    qint64 questionCount = 0;
    qint64 responseCount = 0;
    qint64 lastResponseAt = 0;
    qint64 publishedAt = 0;
    qint64 closedAt = 0;
    QString createdBy;
    QString updatedBy;
    qint64 createdAt = 0;
    qint64 updatedAt = 0;
    bool canDelete = false;
    QString publicUrl;
    bool detailed = false;
    QList<Question> questions;
    QList<QuestionResult> results;
};

struct SurveyResponse {
    QString id;
    qint64 submittedAt = 0;
    QJsonObject answers;
};

struct Summary {
    qint64 surveys = 0;
    qint64 open = 0;
    qint64 drafts = 0;
    qint64 closed = 0;
    qint64 responses = 0;
    qint64 responsesLast7Days = 0;
};

QString text(const QJsonObject &object, const char *key);
qint64 integer(const QJsonObject &object, const char *key);

SurveySettings parseSettings(const QJsonObject &object);
QJsonObject settingsJson(const SurveySettings &settings);
Question parseQuestion(const QJsonObject &object);
QList<Question> parseQuestions(const QJsonArray &items);
QJsonObject questionJson(const Question &question);
QJsonArray questionsJson(const QList<Question> &questions);
QuestionResult parseResult(const QJsonObject &object);
Survey parseSurvey(const QJsonObject &object);
QList<Survey> parseSurveys(const QJsonArray &items);
SurveyResponse parseResponse(const QJsonObject &object);
QList<SurveyResponse> parseResponses(const QJsonArray &items);
Summary parseSummary(const QJsonObject &object);

QString statusKey(const Survey &survey);
QString statusLabel(const QString &key);
QStringList filterKeys();
QString filterLabel(const QString &filter);
bool matchesFilter(const Survey &survey, const QString &filter);
bool matchesQuery(const Survey &survey, const QString &query);
int openCount(const QList<Survey> &surveys);
QString surveyTitle(const Survey &survey);

bool canOpen(const Survey &survey);
bool canClose(const Survey &survey);
bool canReopen(const Survey &survey);
bool canDuplicate(const Survey &survey);
bool canDeleteSurvey(const Survey &survey);
bool canCopyLink(const Survey &survey);
bool canExport(const Survey &survey);

QStringList questionTypeKeys();
QString questionTypeLabel(const QString &type);
bool isChoiceType(const QString &type);
bool isRatingType(const QString &type);
bool validQuestionId(const QString &id);
QStringList optionsFromText(const QString &value);
QuestionCheck checkQuestion(const Question &question);
QString questionsError(const QList<Question> &questions, QList<Question> *normalised = nullptr);
bool moveQuestion(QList<Question> &questions, int index, int delta);

int percentOf(qint64 count, qint64 total);
QList<ResultLine> optionLines(const QuestionResult &result);
QList<ResultLine> distributionLines(const QuestionResult &result);
QString resultLineText(const ResultLine &line);
QString answeredText(const QuestionResult &result);
QString averageText(const QuestionResult &result);
QStringList resultTextLines(const QuestionResult &result);

QString answerText(const QJsonValue &answer);
QList<QPair<QString, QString>> responseLines(const SurveyResponse &response, const QList<Question> &questions);

QString dateTimeText(qint64 stamp, const QTimeZone &zone = QTimeZone::systemTimeZone());
QString relativeText(qint64 nowMs, qint64 atMs, const QTimeZone &zone = QTimeZone::systemTimeZone());
QString countPhrase(qint64 count, const QString &one, const QString &many = QString());
QString responsesText(const Survey &survey, qint64 nowMs, const QTimeZone &zone = QTimeZone::systemTimeZone());
QString scheduleText(const Survey &survey, qint64 nowMs, const QTimeZone &zone = QTimeZone::systemTimeZone());
QStringList settingsLines(const SurveySettings &settings, const QTimeZone &zone = QTimeZone::systemTimeZone());

QString notInstalledError(const QJsonObject &payload);
QString readableError(const QString &error);

}
