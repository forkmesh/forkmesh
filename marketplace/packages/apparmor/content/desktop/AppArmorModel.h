#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <optional>

namespace forkmesh::apparmor {

enum class Mode {
    Enforce,
    Complain,
    Kill,
    Unconfined,
    Prompt,
    Mixed,
    Unknown,
};

QString modeName(Mode mode);
Mode modeFromText(const QString &text);

struct LoadedProfile {
    QString name;
    Mode mode = Mode::Unknown;
};

QList<LoadedProfile> parseLoadedProfiles(const QString &text);

struct ProfileDeclaration {
    QString name;
    QString attachment;
    QStringList flags;
    QString localInclude;
    qsizetype headerStart = -1;
    qsizetype flagsStart = -1;
    qsizetype flagsEnd = -1;
    qsizetype bodyOpen = -1;
    qsizetype bodyClose = -1;
};

QList<ProfileDeclaration> parseProfileDeclarations(const QString &text);
Mode declaredMode(const QStringList &flags);
Mode fileMode(const QList<ProfileDeclaration> &profiles);
QString withMode(const QString &text, Mode mode);
QString withRule(const QString &text, const QString &profileName, const QString &rule);
QString withLocalRule(const QString &localText, const QString &rule);
bool containsRule(const QString &text, const QString &rule);

QString newProfileTemplate(const QString &name, const QString &program, const QString &abi);
bool isSafeProfileName(const QString &name);
bool isSafeFileName(const QString &name);
QString fileNameForProfile(const QString &name);
bool isPolicyFileName(const QString &name);

QString globPattern(const QString &attachment);
bool attachmentMatches(const QString &attachment, const QString &path);

struct Event {
    QDateTime time;
    QString key;
    QString result;
    QString operation;
    QString profile;
    QString name;
    QString target;
    QString comm;
    qint64 pid = 0;
    QString requested;
    QString denied;
    QString className;
    QString capability;
    QString family;
    QString sockType;
    QString info;
    QHash<QString, QString> fields;
    QString raw;
};

std::optional<Event> parseEvent(const QString &line);
QList<Event> parseEvents(const QString &text);
QString eventTarget(const Event &event);
QString topLevelProfile(const QString &profile);
QString suggestedRule(const Event &event);

struct ProcessLabel {
    QString profile;
    Mode mode = Mode::Unknown;
};

ProcessLabel parseLabel(const QString &current);

QHash<QString, QString> splitSections(const QString &output);

struct HourBucket {
    QDateTime start;
    int denied = 0;
    int allowed = 0;
};

QList<HourBucket> hourlyBuckets(const QList<Event> &events, const QDateTime &end, int hours);

enum class AdviceTone {
    Good,
    Info,
    Warning,
    Problem,
};

enum class AdviceAction {
    None,
    Enforce,
    Complain,
    Enable,
    Reload,
    Events,
    LiveView,
};

struct Advice {
    AdviceTone tone = AdviceTone::Info;
    QString text;
    AdviceAction action = AdviceAction::None;
};

struct ProfileFacts {
    Mode mode = Mode::Unknown;
    bool hasFile = false;
    bool editable = true;
    bool disabled = false;
    bool liveKnown = false;
    bool loaded = false;
    bool unconfinedFlag = false;
    bool logKnown = false;
    int processes = 0;
    int missed = 0;
    int denied = 0;
    int allowed = 0;
};

QString programDescription(const QString &profileName);
QString modeExplanation(Mode mode);
QList<Advice> profileAdvice(const ProfileFacts &facts);

}
