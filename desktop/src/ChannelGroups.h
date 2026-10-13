#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

// Sidebar channel groups ("categories") for the Chat page.
//
// Grouping is a presentation-layer view over MainWindow::m_channels: the chat
// backend, the wire format, and the unread/badge ordering (which reads the flat
// channel list) never see it. A group either comes from the user — created by
// hand, or filled by dragging channels into it — or is automatic: a provider
// such as the Discord package names a home group for each of its channels
// ("Discord · Forkmesh"), and those channels land there unless the user moved
// them somewhere else explicitly. Automatic groups therefore appear and vanish
// with their provider without leaving persisted state behind; the only thing
// remembered about one is a collapse toggle or an explicit member order, and
// purge() drops even that when a provider is removed.
//
// The whole state round-trips through one compact JSON document so it can be
// kept in a single QSettings key (every setValue() rewrites the whole ini).
namespace ChannelGroups {

constexpr int kMaxGroupNameLength = 48;
constexpr int kMaxGroups = 64;

struct Group {
    QString name;
    QStringList channels; // explicit, ordered membership
    bool collapsed = false;
    // Materialized from a provider's automatic group (collapse toggle or a
    // reorder) rather than created by the user: it hides when empty.
    bool automatic = false;
};

struct State {
    QList<Group> groups;   // sidebar order
    QStringList ungrouped; // channels explicitly moved out of their automatic group

    bool isEmpty() const { return groups.isEmpty() && ungrouped.isEmpty(); }
};

// One rendered sidebar row: a group header (channel empty) or a channel under
// `group` (empty for the top-level, ungrouped region).
struct Row {
    QString group;
    QString channel;
    bool header = false;
    bool collapsed = false;
    bool automatic = false;
};

using AutoGroupResolver = std::function<QString(const QString &channel)>;

inline QString normalizeName(const QString &name)
{
    QString normalized = name.simplified();
    if (normalized.size() > kMaxGroupNameLength)
        normalized.truncate(kMaxGroupNameLength);
    return normalized;
}

inline int indexOfGroup(const State &state, const QString &name)
{
    for (int i = 0; i < state.groups.size(); ++i)
        if (state.groups.at(i).name == name)
            return i;
    return -1;
}

inline const Group *findGroup(const State &state, const QString &name)
{
    const int index = indexOfGroup(state, name);
    return index < 0 ? nullptr : &state.groups.at(index);
}

// The group a channel was put in by hand, or an empty string when it follows
// its provider's automatic placement (or has none).
inline QString explicitGroupOf(const State &state, const QString &channel)
{
    for (const Group &group : state.groups)
        if (group.channels.contains(channel))
            return group.name;
    return QString();
}

inline bool isExplicitlyUngrouped(const State &state, const QString &channel)
{
    return state.ungrouped.contains(channel);
}

inline void detach(State &state, const QString &channel)
{
    for (Group &group : state.groups)
        group.channels.removeAll(channel);
    state.ungrouped.removeAll(channel);
}

inline bool createGroup(State &state, const QString &rawName)
{
    const QString name = normalizeName(rawName);
    if (name.isEmpty() || indexOfGroup(state, name) >= 0 ||
        state.groups.size() >= kMaxGroups)
        return false;
    Group group;
    group.name = name;
    state.groups.append(group);
    return true;
}

// Replace a group's explicit membership with the order the user currently
// sees. Automatic members become explicit so a rename, delete, or reorder
// keeps them where they are instead of snapping back to the provider's group.
inline void materialize(State &state, const QString &name,
                        const QStringList &renderedMembers, bool automatic)
{
    int index = indexOfGroup(state, name);
    if (index < 0) {
        if (name.isEmpty() || state.groups.size() >= kMaxGroups)
            return;
        Group group;
        group.name = name;
        group.automatic = automatic;
        state.groups.append(group);
        index = state.groups.size() - 1;
    }
    for (const QString &channel : renderedMembers) {
        for (int i = 0; i < state.groups.size(); ++i)
            if (i != index)
                state.groups[i].channels.removeAll(channel);
        state.ungrouped.removeAll(channel);
    }
    state.groups[index].channels = renderedMembers;
}

inline bool renameGroup(State &state, const QString &from, const QString &rawTo,
                        const QStringList &renderedMembers)
{
    const QString to = normalizeName(rawTo);
    if (to.isEmpty() || to == from || indexOfGroup(state, to) >= 0)
        return false;
    materialize(state, from, renderedMembers, false);
    const int index = indexOfGroup(state, from);
    if (index < 0)
        return false;
    state.groups[index].name = to;
    state.groups[index].automatic = false;
    return true;
}

// Remove a group. Its rendered members become explicitly ungrouped so an
// automatic provider group does not immediately re-collect them.
inline void removeGroup(State &state, const QString &name,
                        const QStringList &renderedMembers)
{
    const int index = indexOfGroup(state, name);
    if (index >= 0)
        state.groups.removeAt(index);
    for (const QString &channel : renderedMembers) {
        detach(state, channel);
        state.ungrouped.append(channel);
    }
}

inline void setCollapsed(State &state, const QString &name, bool collapsed,
                         bool automatic)
{
    int index = indexOfGroup(state, name);
    if (index < 0) {
        if (name.isEmpty() || state.groups.size() >= kMaxGroups)
            return;
        Group group;
        group.name = name;
        group.automatic = automatic;
        state.groups.append(group);
        index = state.groups.size() - 1;
    }
    state.groups[index].collapsed = collapsed;
}

// Move a channel into `group` at `index` among the members the user currently
// sees (-1 appends). An empty group name moves it to the top-level region and
// pins it there even if a provider would otherwise auto-group it.
inline bool assign(State &state, const QString &channel, const QString &group,
                   int index, const QStringList &renderedMembers,
                   bool automaticGroup)
{
    if (channel.isEmpty())
        return false;
    if (group.isEmpty()) {
        detach(state, channel);
        state.ungrouped.append(channel);
        return true;
    }
    // The drop index counts the rows the user saw, which still include the
    // channel's own row when it moves within the same group; once that row
    // is vacated everything below it shifts up by one.
    const int oldIndex = renderedMembers.indexOf(channel);
    if (oldIndex >= 0 && index > oldIndex)
        --index;
    QStringList members = renderedMembers;
    members.removeAll(channel);
    materialize(state, group, members, automaticGroup);
    const int groupIndex = indexOfGroup(state, group);
    if (groupIndex < 0)
        return false;
    detach(state, channel);
    QStringList &channels = state.groups[groupIndex].channels;
    if (index < 0 || index > channels.size())
        index = channels.size();
    channels.insert(index, channel);
    return true;
}

// Forget everything about channels matching `channelMatches`, and drop
// automatic groups whose names match `groupMatches` once they hold nothing
// explicit. Used when a provider package is removed so no trace of its
// channels stays in the user's settings.
inline bool purge(State &state,
                  const std::function<bool(const QString &)> &channelMatches,
                  const std::function<bool(const QString &)> &groupMatches)
{
    bool changed = false;
    for (int i = state.groups.size() - 1; i >= 0; --i) {
        Group &group = state.groups[i];
        for (int j = group.channels.size() - 1; j >= 0; --j) {
            if (channelMatches(group.channels.at(j))) {
                group.channels.removeAt(j);
                changed = true;
            }
        }
        if (group.automatic && group.channels.isEmpty() &&
            groupMatches(group.name)) {
            state.groups.removeAt(i);
            changed = true;
        }
    }
    for (int i = state.ungrouped.size() - 1; i >= 0; --i) {
        if (channelMatches(state.ungrouped.at(i))) {
            state.ungrouped.removeAt(i);
            changed = true;
        }
    }
    return changed;
}

inline QString toJson(const State &state)
{
    if (state.isEmpty())
        return QString();
    QJsonArray groups;
    for (const Group &group : state.groups) {
        QJsonObject entry;
        entry.insert(QStringLiteral("name"), group.name);
        if (!group.channels.isEmpty())
            entry.insert(QStringLiteral("channels"),
                         QJsonArray::fromStringList(group.channels));
        if (group.collapsed)
            entry.insert(QStringLiteral("collapsed"), true);
        if (group.automatic)
            entry.insert(QStringLiteral("automatic"), true);
        groups.append(entry);
    }
    QJsonObject root;
    root.insert(QStringLiteral("groups"), groups);
    if (!state.ungrouped.isEmpty())
        root.insert(QStringLiteral("ungrouped"),
                    QJsonArray::fromStringList(state.ungrouped));
    return QString::fromUtf8(
        QJsonDocument(root).toJson(QJsonDocument::Compact));
}

// Tolerant: garbage, duplicates, and over-long names are dropped rather than
// failing the whole sidebar.
inline State fromJson(const QString &json)
{
    State state;
    if (json.trimmed().isEmpty())
        return state;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isObject())
        return state;
    const QJsonObject root = document.object();
    QSet<QString> seenChannels;
    const QJsonArray groups = root.value(QStringLiteral("groups")).toArray();
    for (const QJsonValue &value : groups) {
        if (!value.isObject() || state.groups.size() >= kMaxGroups)
            continue;
        const QJsonObject entry = value.toObject();
        Group group;
        group.name = normalizeName(entry.value(QStringLiteral("name")).toString());
        if (group.name.isEmpty() || indexOfGroup(state, group.name) >= 0)
            continue;
        group.collapsed = entry.value(QStringLiteral("collapsed")).toBool();
        group.automatic = entry.value(QStringLiteral("automatic")).toBool();
        const QJsonArray channels =
            entry.value(QStringLiteral("channels")).toArray();
        for (const QJsonValue &channelValue : channels) {
            const QString channel = channelValue.toString();
            if (channel.isEmpty() || seenChannels.contains(channel))
                continue;
            seenChannels.insert(channel);
            group.channels.append(channel);
        }
        state.groups.append(group);
    }
    const QJsonArray ungrouped = root.value(QStringLiteral("ungrouped")).toArray();
    for (const QJsonValue &value : ungrouped) {
        const QString channel = value.toString();
        if (channel.isEmpty() || seenChannels.contains(channel))
            continue;
        seenChannels.insert(channel);
        state.ungrouped.append(channel);
    }
    return state;
}

// Lay the sidebar out: ungrouped channels first (in `channels` order), then
// the persisted groups in their order, then provider groups that have no
// persisted entry, in order of first appearance. Explicit members come first
// inside a group, followed by its automatic members. Rows under a collapsed
// group are still returned; the view hides them. An automatic group with no
// channel present is not rendered at all, so a removed provider leaves no
// empty header behind; user-created groups stay visible even when empty so
// they can receive a drop.
inline QList<Row> layout(const State &state, const QStringList &channels,
                         const AutoGroupResolver &autoGroupFor)
{
    QList<Row> rows;
    QSet<QString> present(channels.constBegin(), channels.constEnd());

    struct Bucket {
        QString name;
        QStringList members;
        bool collapsed = false;
        bool automatic = false;
    };
    QList<Bucket> buckets;
    for (const Group &group : state.groups) {
        Bucket bucket;
        bucket.name = group.name;
        bucket.collapsed = group.collapsed;
        bucket.automatic = group.automatic;
        for (const QString &channel : group.channels)
            if (present.contains(channel))
                bucket.members.append(channel);
        buckets.append(bucket);
    }
    auto bucketIndex = [&buckets](const QString &name) {
        for (int i = 0; i < buckets.size(); ++i)
            if (buckets.at(i).name == name)
                return i;
        return -1;
    };

    QStringList ungrouped;
    for (const QString &channel : channels) {
        if (!explicitGroupOf(state, channel).isEmpty())
            continue;
        const QString autoGroup =
            (autoGroupFor && !isExplicitlyUngrouped(state, channel))
                ? normalizeName(autoGroupFor(channel))
                : QString();
        if (autoGroup.isEmpty()) {
            ungrouped.append(channel);
            continue;
        }
        int index = bucketIndex(autoGroup);
        if (index < 0) {
            Bucket bucket;
            bucket.name = autoGroup;
            bucket.automatic = true;
            buckets.append(bucket);
            index = buckets.size() - 1;
        }
        buckets[index].members.append(channel);
    }

    for (const QString &channel : ungrouped) {
        Row row;
        row.channel = channel;
        rows.append(row);
    }
    for (const Bucket &bucket : buckets) {
        if (bucket.automatic && bucket.members.isEmpty())
            continue;
        Row header;
        header.group = bucket.name;
        header.header = true;
        header.collapsed = bucket.collapsed;
        header.automatic = bucket.automatic;
        rows.append(header);
        for (const QString &channel : bucket.members) {
            Row row;
            row.group = bucket.name;
            row.channel = channel;
            row.automatic = bucket.automatic;
            rows.append(row);
        }
    }
    return rows;
}

// The group a channel is rendered under, given a layout().
inline QString renderedGroupOf(const QList<Row> &rows, const QString &channel)
{
    for (const Row &row : rows)
        if (!row.header && row.channel == channel)
            return row.group;
    return QString();
}

inline QStringList renderedMembersOf(const QList<Row> &rows, const QString &group)
{
    QStringList members;
    if (group.isEmpty())
        return members;
    for (const Row &row : rows)
        if (!row.header && row.group == group)
            members.append(row.channel);
    return members;
}

inline QStringList renderedGroupNames(const QList<Row> &rows)
{
    QStringList names;
    for (const Row &row : rows)
        if (row.header)
            names.append(row.group);
    return names;
}

inline bool renderedGroupIsAutomatic(const QList<Row> &rows, const QString &group)
{
    for (const Row &row : rows)
        if (row.header && row.group == group)
            return row.automatic;
    return false;
}

} // namespace ChannelGroups
