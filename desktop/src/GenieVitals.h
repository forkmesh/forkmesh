#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtGlobal>

struct GenieWatch {
    static constexpr const char *ownOrganization = "forkmesh";

    QSet<QString> topics;

    GenieWatch();
    static GenieWatch only(const QStringList &topics);
    static GenieWatch decode(const QString &saved);
    static GenieWatch forOrganizations(const QStringList &organizations);
    static QStringList all();
    static QString label(const QString &topic);
    static QString detail(const QString &topic);

    QString encoded() const;
    bool has(const QString &topic) const;
    bool code() const;
    bool outreach() const;
    bool nothing() const;
    QString described() const;
    bool operator==(const GenieWatch &other) const;
};

struct GenieVitalSigns {
    int commits = 0;
    int agentRuns = 0;
    int merged = 0;
    int emailsSent = 0;
    int contactsTouched = 0;
    int marketingVisits = 0;
    int replies = 0;
    int bounced = 0;
    int published = 0;
    qint64 measuredAtMs = 0;

    QStringList encoded() const;
    static bool decode(const QStringList &values, GenieVitalSigns *out);
    bool operator==(const GenieVitalSigns &other) const;
};

enum class GenieVitalLevel { Thriving, Happy, Peckish, Hungry, Sick };

struct GenieVitals {
    static constexpr qint64 windowMs = 7LL * 24 * 3600 * 1000;
    static constexpr qint64 staleAfterMs = 2LL * 24 * 3600 * 1000;
    static constexpr double codeGoal = 14;
    static constexpr double marketingGoal = 16;
    static constexpr double silentCap = 0.45;
    static constexpr double fewRepliesCap = 0.7;
    static constexpr int fewReplies = 3;
    static constexpr int maxHearts = 5;
    static constexpr double restingHealth = 0.6;

    GenieWatch watch;
    GenieVitalSigns signs;
    double code = 0;
    double marketing = 0;
    double health = 0;

    static GenieVitals measure(const GenieVitalSigns &signs, qint64 nowMs, const GenieWatch &watch = GenieWatch());
    static GenieVitalSigns watched(GenieVitalSigns signs, const GenieWatch &watch);

    GenieVitalLevel level() const;
    int hearts() const;
    QString weakest() const;
    QString summary(const QString &organization = QString()) const;

    static GenieVitalLevel levelOf(double health);
    static QString title(GenieVitalLevel level);
    static QString word(double value);
    static double codePoints(const GenieVitalSigns &signs);
    static double marketingPoints(const GenieVitalSigns &signs);
    static double marketingCap(const GenieVitalSigns &signs);
    static bool unanswered(const GenieVitalSigns &signs);
    static double fill(double points, double goal);
    static double combine(double code, double marketing);
    static double freshness(qint64 measuredMs, qint64 nowMs);
    static bool isMarketingPage(const QString &page);
};

struct GenieCrmCounts {
    int contacts = 0;
    int contactsTouched = 0;
    int emailsSent = 0;
    int emailsSentEver = 0;
    int replies = 0;
    int bounced = 0;

    static GenieCrmCounts read(const QJsonObject &summary, qint64 sinceMs);
};

enum class GenieSource { Unknown, Missing, Failed, Ready };

struct GenieLesson {
    QString contact;
    QString reason;
    qint64 atMs = 0;
};

struct GenieLessons {
    static constexpr int most = 200;
    static constexpr const char *teammateId = "genie";

    QList<GenieLesson> skip;

    static GenieLessons decode(const QString &saved);
    QString encoded() const;
    QStringList learn(const QString &agentOutput, qint64 nowMs);
    bool skips(const QString &id, const QStringList &names, const QStringList &addresses) const;
    QString described() const;
    static bool closedStage(const QString &stage);
    static bool closedStage(const QString &stage, const QStringList &closing);
    static QStringList closedStages();
    static QStringList closedStages(const QJsonObject &settings);
    static bool closedTag(const QJsonObject &contact);
    static QString contract();
};

struct GenieCrmDigest {
    static constexpr qint64 awaitingMs = 14LL * 24 * 3600 * 1000;
    static constexpr qint64 staleStepMs = 7LL * 24 * 3600 * 1000;

    GenieSource source = GenieSource::Unknown;
    QString organization;
    QString error;
    int contacts = 0;
    int addressed = 0;
    QStringList awaiting;
    QStringList bounced;
    QStringList nextSteps;
    int nextStepContacts = 0;
    int neverEmailed = 0;
    int closed = 0;
    QStringList taughtOut;
    QStringList closingStages;

    static GenieCrmDigest read(const QJsonObject &summary, qint64 nowMs,
                               const GenieLessons &lessons = GenieLessons());
};

struct GenieMarketingDigest {
    static constexpr qint64 aheadMs = 7LL * 24 * 3600 * 1000;

    GenieSource source = GenieSource::Unknown;
    QString organization;
    QString error;
    int published = 0;
    qint64 lastPublishedAt = 0;
    QString lastPublishedTitle;
    QString lastPublishedChannel;
    int scheduledAhead = 0;
    int queuedStuck = 0;
    int redditOpen = 0;
    int experimentsDue = 0;
    int experimentsRunning = 0;
    int experimentsLimit = 0;
    int adsAttention = 0;
    bool funnelLogged = false;
    QString bottleneck;
    bool bottleneckNamed = false;
    QStringList notes;
    QStringList insights;

    static GenieMarketingDigest read(const QJsonObject &overview, const QJsonObject &performance, qint64 nowMs,
                                     qint64 sinceMs);
};

struct GenieShippedChange {
    qint64 atMs = 0;
    QString subject;
    QString repository;
};

QList<GenieShippedChange> genieShippedChanges(const QByteArray &gitLog, const QString &repository);

struct GenieActivityLog {
    static constexpr int most = 400;
    static constexpr qint64 repeatGapMs = 20000;

    static QStringList record(QStringList entries, const QString &page, qint64 nowMs);
    static int visits(const QStringList &entries, qint64 sinceMs, const QString &page = QString());
};

bool genieReadmeHasPicture(const QString &readme);

struct GenieMarketingFacts {
    GenieVitals vitals;
    int contacts = -1;
    int emailsSentEver = -1;
    int analyticsVisits = 0;
    bool hasReadme = false;
    bool readmeHasPicture = false;
    QString readmeRepository;
    QSet<QString> ticked;
    GenieCrmDigest crm;
    GenieMarketingDigest marketing;
    bool shippedKnown = false;
    QList<GenieShippedChange> shipped;
    qint64 nowMs = 0;
};

struct GenieSuggestion {
    QString text;
    QString page;
    QString action;
    QString task;
    QString prompt;
};

struct GenieMarketingStep {
    QString id;
    QString title;
    QString detail;
    QString page;
    QString action;
    QString task;
    QString prompt;
    bool automatic = false;
    bool done = false;
    QString progress;
};

struct GenieTip {
    QString text;
    QString task;
    QString prompt;
};

struct GenieSaidComment {
    QString text;
    qint64 atMs = 0;
};

struct GenieSaid {
    QString id;
    QString kind;
    QString text;
    QString task;
    QString basis;
    qint64 firstAtMs = 0;
    qint64 lastAtMs = 0;
    int times = 0;
    QList<GenieSaidComment> comments;
    QString verdict;
    QString verdictNote;
    qint64 checkedAtMs = 0;

    bool awaitingCheck() const;
};

struct GenieSaidLog {
    static constexpr int most = 300;
    static constexpr int briefed = 20;
    static constexpr int textLimit = 2000;
    static constexpr int commentLimit = 2000;
    static constexpr const char *kindClick = "click";
    static constexpr const char *kindUnasked = "unasked";
    static constexpr const char *kindFeeling = "feeling";
    static constexpr const char *kindLearned = "learned";
    static constexpr const char *verdictAccurate = "accurate";
    static constexpr const char *verdictWrong = "wrong";

    QList<GenieSaid> said;

    static GenieSaidLog decode(const QByteArray &saved);
    QByteArray encoded() const;
    QString record(const QString &kind, const QString &text, const QString &task, const QString &basis,
                   qint64 nowMs);
    const GenieSaid *find(const QString &id) const;
    bool comment(const QString &id, const QString &text, qint64 nowMs);
    bool uncomment(const QString &id, int index);
    int awaitingCheck() const;
    QStringList learn(const QString &agentOutput, qint64 nowMs);
    QString described() const;
    static QString kindLabel(const QString &kind);
    static QString contract();
};

QString genieBasis(const GenieMarketingFacts &facts);

QString genieAgentBriefing(const QString &health, const QList<GenieTip> &tips, const GenieLessons &lessons,
                           const GenieSaidLog &said = GenieSaidLog(),
                           const QStringList &closingStages = QStringList());

struct GenieCoaching {
    QString stage;
    QString headline;
    QString advice;
    QString page;
    QString action;
    QString task;
    QString prompt;
    QList<GenieSuggestion> suggestions;
    QList<GenieMarketingStep> steps;
    int next = -1;
    int doneCount = 0;

    QList<GenieTip> tips() const;
    QStringList lines() const;
};

namespace GenieMarketingCoach {

constexpr qint64 shippedWindowMs = 14LL * 24 * 3600 * 1000;
constexpr int mostSuggestions = 12;

QList<GenieMarketingStep> playbook();
QList<GenieSuggestion> suggestions(const GenieMarketingFacts &facts);
GenieCoaching coach(const GenieMarketingFacts &facts);

}

namespace GeniePings {

QString badgeLine(const QJsonObject &alert);

}
