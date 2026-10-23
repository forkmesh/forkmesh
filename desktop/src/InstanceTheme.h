#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>

namespace forkmesh::instancetheme {

inline constexpr char kMatchSetting[] = "ui/matchInstanceTheme";
inline constexpr char kColorsSetting[] = "ui/instanceThemeColors";

struct Colors {
    QMap<QString, QString> dark;
    QMap<QString, QString> light;
    QString name;

    bool isEmpty() const { return dark.isEmpty() && light.isEmpty(); }
    bool operator==(const Colors &other) const
    {
        return dark == other.dark && light == other.light;
    }
    bool operator!=(const Colors &other) const { return !(*this == other); }
};

QMap<QString, QString> roleColors(const QJsonObject &tokens, bool dark);
Colors fromInstanceDocument(const QJsonObject &document);
QJsonObject toJson(const Colors &colors);
Colors fromJson(const QJsonObject &object);
Colors stored();
void store(const Colors &colors);
bool matchEnabled();
void setMatchEnabled(bool enabled);
void apply(const Colors &colors);
void applyStored();

}
