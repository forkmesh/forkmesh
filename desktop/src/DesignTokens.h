#pragma once

#include <QColor>
#include <QFont>
#include <QMap>
#include <QPalette>
#include <QString>

namespace Theme {

enum class Design { Classic, Next };

enum class Role {
    Bg,
    BgSubtle,
    Surface,
    SurfaceRaised,
    SurfaceActive,
    Border,
    BorderStrong,
    Fg,
    FgSecondary,
    FgMuted,
    FgSubtle,
    Accent,
    AccentHover,
    AccentPressed,
    AccentFg,
    AccentSubtle,
    OnAccent,
    Selection,
    Success,
    SuccessSubtle,
    Warning,
    WarningSubtle,
    Danger,
    DangerSubtle,
    Count
};

extern const char *const kDesignSetting;
extern const char *const kEdgeIndicatorSetting;

Design designFromSetting(const QString &value);
QString designSettingValue(Design design);
Design designFromSettings();

Design activeDesign();
bool activeDark();
bool isNext();
bool edgeIndicators();
void setActiveDesign(Design design, bool dark);

QString roleName(Role role);
QString hex(Role role);
QString hex(Role role, Design design, bool dark);
QColor color(Role role);
QColor color(Role role, Design design, bool dark);
QColor color(Role role, int alpha);
QColor nextOr(Role role, const QColor &classic);
QString nextOrHex(Role role, const QString &classicHex);
QColor iconColorOverride(const QString &objectName, const QColor &classic);
QString chatHex(const QString &name, bool dark);
QColor chatColor(const QString &name);

void ensureBundledFonts();
QString uiFontFamily();
QString monoFontFamily();
QFont interfaceFont(Design design, const QFont &classicFont);
QFont monospaceFont(const QFont &classicFont);

QMap<QString, QString> nextPalette(bool dark);
QString expandTokens(const QString &text, Design design, bool dark);
QString nextOverlay(bool dark);
QString nextStyleSheet(bool dark);
QString styleSheetForDesign(Design design, bool dark);
QPalette paletteForDesign(Design design, bool dark);

}
