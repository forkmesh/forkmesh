#include "InstanceTheme.h"

#include "DesignTokens.h"

#include <QColor>
#include <QJsonDocument>
#include <QSettings>

namespace forkmesh::instancetheme {

namespace {

QColor token(const QJsonObject &tokens, const char *name)
{
    const QColor value(tokens.value(QLatin1String(name)).toString().trimmed());
    return value.isValid() ? value : QColor();
}

QColor blend(const QColor &from, const QColor &to, qreal amount)
{
    const qreal keep = 1.0 - amount;
    return QColor::fromRgbF(from.redF() * keep + to.redF() * amount,
                            from.greenF() * keep + to.greenF() * amount,
                            from.blueF() * keep + to.blueF() * amount);
}

QMap<QString, QString> cleanMap(const QJsonObject &object)
{
    QMap<QString, QString> result;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QColor value(it.value().toString());
        if (value.isValid())
            result.insert(it.key(), value.name(QColor::HexRgb));
    }
    return result;
}

QJsonObject mapObject(const QMap<QString, QString> &map)
{
    QJsonObject object;
    for (auto it = map.constBegin(); it != map.constEnd(); ++it)
        object.insert(it.key(), it.value());
    return object;
}

}

QMap<QString, QString> roleColors(const QJsonObject &tokens, bool dark)
{
    QMap<QString, QString> roles;
    const auto put = [&roles](Theme::Role role, const QColor &value) {
        if (value.isValid())
            roles.insert(Theme::roleName(role), value.name(QColor::HexRgb));
    };
    const QColor background = token(tokens, "background");
    const QColor foreground = token(tokens, "foreground");
    const QColor card = token(tokens, "card");
    const QColor sidebar = token(tokens, "sidebar");
    const QColor muted = token(tokens, "muted").isValid() ? token(tokens, "muted")
                                                          : token(tokens, "secondary");
    const QColor border = token(tokens, "border");
    const QColor mutedFg = token(tokens, "muted-foreground");
    const QColor secondaryFg = token(tokens, "secondary-foreground");
    const QColor primary = token(tokens, "primary");
    const QColor onPrimary = token(tokens, "primary-foreground");
    const QColor link = token(tokens, "link").isValid() ? token(tokens, "link")
                                                        : token(tokens, "ring");
    const QColor success = token(tokens, "success");
    const QColor attention = token(tokens, "attention");
    const QColor destructive = token(tokens, "destructive");

    put(Theme::Role::Bg, background);
    put(Theme::Role::BgSubtle,
        sidebar.isValid() ? sidebar
        : background.isValid() && foreground.isValid()
            ? blend(background, foreground, 0.02)
            : QColor());
    put(Theme::Role::Surface, card.isValid() ? card : background);
    put(Theme::Role::SurfaceRaised, muted);
    if (muted.isValid() && foreground.isValid())
        put(Theme::Role::SurfaceActive, blend(muted, foreground, 0.06));
    put(Theme::Role::Border, border);
    if (border.isValid() && mutedFg.isValid())
        put(Theme::Role::BorderStrong, blend(border, mutedFg, 0.3));
    put(Theme::Role::Fg, foreground);
    put(Theme::Role::FgSecondary,
        secondaryFg.isValid() ? secondaryFg
        : foreground.isValid() && background.isValid()
            ? blend(foreground, background, 0.15)
            : QColor());
    put(Theme::Role::FgMuted, mutedFg);
    if (mutedFg.isValid() && background.isValid())
        put(Theme::Role::FgSubtle, blend(mutedFg, background, 0.3));
    if (primary.isValid()) {
        put(Theme::Role::Accent, primary);
        put(Theme::Role::AccentHover,
            dark ? primary.lighter(115) : primary.darker(115));
        put(Theme::Role::AccentPressed,
            dark ? primary.darker(110) : primary.darker(130));
        put(Theme::Role::AccentFg, link.isValid() ? link : primary);
        if (background.isValid()) {
            put(Theme::Role::AccentSubtle, blend(primary, background, 0.82));
            put(Theme::Role::Selection,
                blend(primary, background, dark ? 0.6 : 0.78));
        }
    }
    put(Theme::Role::OnAccent, onPrimary);
    const auto status = [&](Theme::Role role, Theme::Role subtle, const QColor &value) {
        put(role, value);
        if (value.isValid() && background.isValid())
            put(subtle, blend(value, background, 0.85));
    };
    status(Theme::Role::Success, Theme::Role::SuccessSubtle, success);
    status(Theme::Role::Warning, Theme::Role::WarningSubtle, attention);
    status(Theme::Role::Danger, Theme::Role::DangerSubtle, destructive);
    return roles;
}

Colors fromInstanceDocument(const QJsonObject &document)
{
    QJsonObject source = document.value(QStringLiteral("themeColors")).toObject();
    if (source.isEmpty())
        source = document.value(QStringLiteral("theme")).toObject();
    Colors colors;
    colors.dark = roleColors(source.value(QStringLiteral("dark")).toObject(), true);
    colors.light = roleColors(source.value(QStringLiteral("light")).toObject(), false);
    colors.name = document.value(QStringLiteral("theme"))
                      .toObject()
                      .value(QStringLiteral("preset"))
                      .toString();
    return colors;
}

QJsonObject toJson(const Colors &colors)
{
    QJsonObject object;
    object.insert(QStringLiteral("dark"), mapObject(colors.dark));
    object.insert(QStringLiteral("light"), mapObject(colors.light));
    if (!colors.name.isEmpty())
        object.insert(QStringLiteral("name"), colors.name);
    return object;
}

Colors fromJson(const QJsonObject &object)
{
    Colors colors;
    colors.dark = cleanMap(object.value(QStringLiteral("dark")).toObject());
    colors.light = cleanMap(object.value(QStringLiteral("light")).toObject());
    colors.name = object.value(QStringLiteral("name")).toString();
    return colors;
}

Colors stored()
{
    const QByteArray raw =
        QSettings().value(QLatin1String(kColorsSetting)).toByteArray();
    return fromJson(QJsonDocument::fromJson(raw).object());
}

void store(const Colors &colors)
{
    if (colors.isEmpty()) {
        QSettings().remove(QLatin1String(kColorsSetting));
        return;
    }
    QSettings().setValue(
        QLatin1String(kColorsSetting),
        QJsonDocument(toJson(colors)).toJson(QJsonDocument::Compact));
}

bool matchEnabled()
{
    return QSettings().value(QLatin1String(kMatchSetting), false).toBool();
}

void setMatchEnabled(bool enabled)
{
    QSettings().setValue(QLatin1String(kMatchSetting), enabled);
}

void apply(const Colors &colors)
{
    Theme::setRoleOverrides(colors.dark, colors.light);
}

void applyStored()
{
    apply(matchEnabled() ? stored() : Colors());
}

}
