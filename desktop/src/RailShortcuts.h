#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

namespace RailShortcuts {

constexpr int kMaxLabelLength = 24;
constexpr int kMaxShortcuts = 48;
constexpr int kMaxKnownPageButtons = 400;
inline const QString kKeyPrefix = QStringLiteral("shortcut:");
inline const QString kDefaultIcon = QStringLiteral("link");

struct Shortcut {
    QString id;
    QString label;
    QString icon;
    QString path;
    int section = -1;
    int repoTab = -1;
};

using State = QList<Shortcut>;

inline QString normalizeLabel(const QString &label)
{
    QString normalized = label;
    normalized.remove(QLatin1Char('&'));
    normalized = normalized.section(QLatin1Char('\n'), 0, 0).simplified();
    static const QRegularExpression count(QStringLiteral("\\s*\\(\\d+\\+?\\)$"));
    normalized.remove(count);
    if (normalized.size() > kMaxLabelLength)
        normalized.truncate(kMaxLabelLength);
    return normalized.trimmed();
}

inline QString railKey(const QString &id)
{
    return kKeyPrefix + id;
}

inline bool isShortcutKey(const QString &key)
{
    return key.startsWith(kKeyPrefix);
}

inline QString idOf(const QString &key)
{
    return isShortcutKey(key) ? key.mid(kKeyPrefix.size()) : QString();
}

inline int indexOf(const State &state, const QString &id)
{
    for (int i = 0; i < state.size(); ++i)
        if (state.at(i).id == id)
            return i;
    return -1;
}

inline int indexOfPath(const State &state, const QString &path)
{
    if (path.isEmpty())
        return -1;
    for (int i = 0; i < state.size(); ++i)
        if (state.at(i).path == path)
            return i;
    return -1;
}

inline State parse(const QString &json)
{
    State state;
    QSet<QString> ids;
    QSet<QString> paths;
    for (const QJsonValue &value : QJsonDocument::fromJson(json.toUtf8()).array()) {
        if (state.size() >= kMaxShortcuts)
            break;
        const QJsonObject object = value.toObject();
        Shortcut shortcut;
        shortcut.id = object.value(QStringLiteral("id")).toString().trimmed();
        shortcut.path = object.value(QStringLiteral("path")).toString();
        shortcut.label = normalizeLabel(object.value(QStringLiteral("label")).toString());
        shortcut.icon = object.value(QStringLiteral("icon")).toString().trimmed();
        shortcut.section = object.value(QStringLiteral("section")).toInt(-1);
        shortcut.repoTab = object.value(QStringLiteral("repoTab")).toInt(-1);
        if (shortcut.id.isEmpty() || shortcut.path.isEmpty() ||
            ids.contains(shortcut.id) || paths.contains(shortcut.path))
            continue;
        if (shortcut.label.isEmpty())
            shortcut.label = QStringLiteral("Page");
        if (shortcut.icon.isEmpty() || shortcut.icon.contains(QLatin1Char('/')))
            shortcut.icon = kDefaultIcon;
        ids.insert(shortcut.id);
        paths.insert(shortcut.path);
        state.append(shortcut);
    }
    return state;
}

inline QString serialize(const State &state)
{
    QJsonArray shortcuts;
    for (const Shortcut &shortcut : state) {
        shortcuts.append(QJsonObject{
            {QStringLiteral("id"), shortcut.id},
            {QStringLiteral("label"), shortcut.label},
            {QStringLiteral("icon"), shortcut.icon},
            {QStringLiteral("path"), shortcut.path},
            {QStringLiteral("section"), shortcut.section},
            {QStringLiteral("repoTab"), shortcut.repoTab},
        });
    }
    return QString::fromUtf8(
        QJsonDocument(shortcuts).toJson(QJsonDocument::Compact));
}

inline QString add(State &state, Shortcut shortcut)
{
    if (shortcut.path.isEmpty())
        return QString();
    const int existing = indexOfPath(state, shortcut.path);
    if (existing >= 0)
        return state.at(existing).id;
    if (state.size() >= kMaxShortcuts)
        return QString();
    shortcut.label = normalizeLabel(shortcut.label);
    if (shortcut.label.isEmpty())
        shortcut.label = QStringLiteral("Page");
    if (shortcut.icon.isEmpty() || shortcut.icon.contains(QLatin1Char('/')))
        shortcut.icon = kDefaultIcon;
    int next = 1;
    while (indexOf(state, QString::number(next)) >= 0)
        ++next;
    shortcut.id = QString::number(next);
    state.append(shortcut);
    return shortcut.id;
}

inline bool remove(State &state, const QString &id)
{
    const int index = indexOf(state, id);
    if (index < 0)
        return false;
    state.removeAt(index);
    return true;
}

inline bool rememberPageButton(QStringList &known, const QString &path)
{
    if (path.isEmpty() || known.contains(path))
        return false;
    known.append(path);
    while (known.size() > kMaxKnownPageButtons)
        known.removeFirst();
    return true;
}

}
