#pragma once

#include <QMetaType>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>

namespace forkmesh::settings {

inline bool sameStoredValue(const QVariant &stored, const QVariant &value)
{
    if (stored == value)
        return true;
    if (value.typeId() == QMetaType::QStringList) {
        const QStringList list = value.toStringList();
        if (!stored.isValid())
            return list.isEmpty();
        switch (stored.typeId()) {
        case QMetaType::QString:
            return list.size() == 1 && list.first() == stored.toString();
        case QMetaType::QStringList:
        case QMetaType::QVariantList:
            return stored.toStringList() == list;
        default:
            return false;
        }
    }
    if (stored.typeId() != QMetaType::QString)
        return false;
    switch (value.typeId()) {
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
    case QMetaType::Double:
        return stored.toString() == value.toString();
    default:
        return false;
    }
}

inline bool put(QSettings &settings, const QString &key, const QVariant &value)
{
    if (settings.contains(key) && sameStoredValue(settings.value(key), value))
        return false;
    settings.setValue(key, value);
    return true;
}

inline bool put(QSettings &&settings, const QString &key, const QVariant &value)
{
    return put(settings, key, value);
}

inline bool drop(QSettings &settings, const QString &key)
{
    if (!settings.contains(key))
        return false;
    settings.remove(key);
    return true;
}

inline bool drop(QSettings &&settings, const QString &key)
{
    return drop(settings, key);
}

} // namespace forkmesh::settings
