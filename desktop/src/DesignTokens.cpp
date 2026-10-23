#include "DesignTokens.h"

#include "Theme.h"

#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QStringList>

#include <array>

namespace Theme {

const char *const kDesignSetting = "app/design";
const char *const kEdgeIndicatorSetting = "ui/edgeIndicators";

namespace {

struct RoleValues {
    const char *name;
    const char *classicDark;
    const char *classicLight;
    const char *nextDark;
    const char *nextLight;
};

constexpr int kRoleCount = static_cast<int>(Role::Count);

const std::array<RoleValues, kRoleCount> kRoles = {{
    {"bg", "#0d1117", "#ffffff", "#09090b", "#ffffff"},
    {"bgSubtle", "#010409", "#f6f8fa", "#0c0c0e", "#fafafa"},
    {"surface", "#161b22", "#ffffff", "#111113", "#ffffff"},
    {"surfaceRaised", "#21262d", "#f6f8fa", "#18181b", "#f4f4f5"},
    {"surfaceActive", "#30363d", "#eaeef2", "#232326", "#e8e8eb"},
    {"border", "#30363d", "#d0d7de", "#232326", "#e4e4e7"},
    {"borderStrong", "#484f58", "#afb8c1", "#303036", "#d4d4d8"},
    {"fg", "#e6edf3", "#1f2328", "#f4f4f5", "#18181b"},
    {"fgSecondary", "#c9d1d9", "#424a53", "#d4d4d8", "#3f3f46"},
    {"fgMuted", "#8b949e", "#656d76", "#a1a1aa", "#6b6b74"},
    {"fgSubtle", "#6e7681", "#8c959f", "#71717a", "#8a8a94"},
    {"accent", "#1f6feb", "#0969da", "#1f6feb", "#0969da"},
    {"accentHover", "#388bfd", "#0550ae", "#388bfd", "#0550ae"},
    {"accentPressed", "#1158c7", "#0a5cc7", "#1158c7", "#0a5cc7"},
    {"accentFg", "#58a6ff", "#0969da", "#58a6ff", "#0969da"},
    {"accentSubtle", "#132b45", "#ddf4ff", "#12233b", "#e6f0fd"},
    {"onAccent", "#ffffff", "#ffffff", "#ffffff", "#ffffff"},
    {"selection", "#1f6feb", "#0969da", "#17396b", "#cfe2fb"},
    {"success", "#3fb950", "#1a7f37", "#3fb950", "#1a7f37"},
    {"successSubtle", "#12261a", "#dafbe1", "#102417", "#e3f7e8"},
    {"warning", "#d29922", "#9a6700", "#d29922", "#9a6700"},
    {"warningSubtle", "#2b210b", "#fff8c5", "#26200e", "#fdf6d3"},
    {"danger", "#f85149", "#cf222e", "#f85149", "#cf222e"},
    {"dangerSubtle", "#2d1215", "#ffebe9", "#2a1214", "#fdecea"},
}};

struct PaletteEntry {
    const char *from;
    const char *to;
};

struct ChatToneValues {
    const char *name;
    const char *dark;
    const char *light;
};

const ChatToneValues kChatTones[] = {
    {"chatRail", "#2b2d31", "#f2f3f5"},
    {"chatRailFooter", "#232428", "#ebedef"},
    {"chatPane", "#313338", "#ffffff"},
    {"chatRowHover", "#35373c", "#e8e9eb"},
    {"chatRowSelected", "#404249", "#d7d9dc"},
    {"chatMessageHover", "#2e3035", "#f5f6f7"},
    {"chatText", "#dbdee1", "#313338"},
    {"chatTextStrong", "#f2f3f5", "#060607"},
    {"chatMuted", "#949ba4", "#5c5f66"},
    {"chatFaint", "#80848e", "#80848e"},
    {"chatDivider", "#3f4147", "#e1e2e4"},
    {"chatEdge", "#1f2023", "#e1e2e4"},
    {"chatComposer", "#383a40", "#ebedef"},
    {"chatComposerIcon", "#b5bac1", "#4e5058"},
    {"chatBrand", "#5865f2", "#5865f2"},
    {"chatNew", "#f23f43", "#f23f43"},
    {"chatOnline", "#23a55a", "#23a55a"},
    {"chatAway", "#f0b232", "#f0b232"},
    {"chatChip", "#2b2d31", "#f2f3f5"},
};

const PaletteEntry kNextDarkPalette[] = {
    {"#010409", "#060607"}, {"#0d1117", "#09090b"}, {"#10151c", "#0c0c0e"},
    {"#161b22", "#111113"}, {"#1b2129", "#151517"}, {"#1c2433", "#17171a"},
    {"#1f2630", "#18181b"}, {"#1f2937", "#18181b"}, {"#21262d", "#18181b"},
    {"#282e35", "#1f1f23"}, {"#30363d", "#232326"}, {"#374151", "#303036"},
    {"#444c56", "#35353b"}, {"#484f58", "#3a3a41"}, {"#656d76", "#71717a"},
    {"#6e7681", "#71717a"}, {"#8b949e", "#a1a1aa"}, {"#b1bac4", "#bdbdc4"},
    {"#c9d1d9", "#d4d4d8"}, {"#d1d5db", "#d4d4d8"}, {"#e6edf3", "#f4f4f5"},
    {"#f0f6fc", "#fafafa"}, {"#f9fafb", "#fafafa"},
};

const PaletteEntry kNextLightPalette[] = {
    {"#f6f8fa", "#fafafa"}, {"#d0d7de", "#e4e4e7"}, {"#1f2328", "#18181b"},
    {"#656d76", "#71717a"}, {"#eaeef2", "#f0f0f2"}, {"#8c959f", "#a1a1aa"},
    {"#d8dee4", "#e4e4e7"}, {"#424a53", "#3f3f46"}, {"#57606a", "#5b5b65"},
    {"#24292f", "#202024"}, {"#afb8c1", "#b8b8c0"}, {"#6e7781", "#71717a"},
    {"#6e7681", "#71717a"}, {"#f3f4f6", "#f4f4f5"}, {"#eef1f5", "#f0f0f2"},
    {"#ebecf0", "#ececef"}, {"#e6ebf1", "#e8e8ec"}, {"#e5e7eb", "#e4e4e7"},
    {"#e1e6eb", "#e2e2e6"},
};

const char kNextSheetDir[] = ":/theme/next";
const char kCompactSheet[] = "95-compact.qss";
const char kEdgeSheet[] = "96-edge-indicators.qss";

Design g_design = Design::Classic;
bool g_dark = true;
QMap<QString, QString> g_overrideDark;
QMap<QString, QString> g_overrideLight;

const QMap<QString, QString> &overrides(bool dark)
{
    return dark ? g_overrideDark : g_overrideLight;
}

const RoleValues &values(Role role)
{
    const int index = static_cast<int>(role);
    return kRoles[index >= 0 && index < kRoleCount ? index : 0];
}

QString remapHexColours(const QString &text, const QMap<QString, QString> &palette)
{
    if (palette.isEmpty())
        return text;
    static const QRegularExpression pattern(
        QStringLiteral("#[0-9a-fA-F]{6}(?![0-9a-fA-F])"));
    QString result;
    result.reserve(text.size());
    int last = 0;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += QStringView(text).mid(last, match.capturedStart() - last);
        const QString found = match.captured();
        const QString replacement = palette.value(found.toLower());
        result += replacement.isEmpty() ? found : replacement;
        last = match.capturedEnd();
    }
    result += QStringView(text).mid(last);
    return result;
}

QString capFontWeights(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("font-weight:\\s*(?:6[5-9]\\d|[7-9]\\d\\d|bold)"));
    QString result = text;
    result.replace(pattern, QStringLiteral("font-weight: 600"));
    return result;
}

}

Design designFromSetting(const QString &value)
{
    return value.trimmed().compare(QLatin1String("next"), Qt::CaseInsensitive) == 0
               ? Design::Next
               : Design::Classic;
}

QString designSettingValue(Design design)
{
    return design == Design::Next ? QStringLiteral("next")
                                  : QStringLiteral("classic");
}

Design designFromSettings()
{
    const QString forced = qEnvironmentVariable("FORKMESH_DESIGN").trimmed();
    if (!forced.isEmpty())
        return designFromSetting(forced);
    return designFromSetting(
        QSettings()
            .value(QLatin1String(kDesignSetting), QStringLiteral("classic"))
            .toString());
}

Design activeDesign()
{
    return g_design;
}

bool activeDark()
{
    return g_dark;
}

bool isNext()
{
    return g_design == Design::Next;
}

bool edgeIndicators()
{
    return QSettings().value(QLatin1String(kEdgeIndicatorSetting), false).toBool();
}

void setActiveDesign(Design design, bool dark)
{
    g_design = design;
    g_dark = dark;
}

QString roleName(Role role)
{
    return QString::fromLatin1(values(role).name);
}

void setRoleOverrides(const QMap<QString, QString> &dark,
                      const QMap<QString, QString> &light)
{
    const auto clean = [](const QMap<QString, QString> &source) {
        QMap<QString, QString> result;
        for (int index = 0; index < kRoleCount; ++index) {
            const QString name = QString::fromLatin1(kRoles[index].name);
            const QColor value(source.value(name));
            if (value.isValid())
                result.insert(name, value.name(QColor::HexRgb));
        }
        return result;
    };
    g_overrideDark = clean(dark);
    g_overrideLight = clean(light);
}

bool hasRoleOverrides()
{
    return !g_overrideDark.isEmpty() || !g_overrideLight.isEmpty();
}

QMap<QString, QString> overridePalette(Design design, bool dark)
{
    QMap<QString, QString> palette;
    const QMap<QString, QString> &chosen = overrides(dark);
    if (chosen.isEmpty())
        return palette;
    for (int index = 0; index < kRoleCount; ++index) {
        const RoleValues &entry = kRoles[index];
        const QString wanted = chosen.value(QString::fromLatin1(entry.name));
        if (wanted.isEmpty())
            continue;
        const QString base = QString::fromLatin1(
            design == Design::Next ? (dark ? entry.nextDark : entry.nextLight)
                                   : (dark ? entry.classicDark : entry.classicLight));
        if (!palette.contains(base))
            palette.insert(base, wanted);
        const QString classic =
            QString::fromLatin1(dark ? entry.classicDark : entry.classicLight);
        if (!palette.contains(classic))
            palette.insert(classic, wanted);
    }
    return palette;
}

QString hex(Role role, Design design, bool dark)
{
    const RoleValues &entry = values(role);
    const QString overridden =
        overrides(dark).value(QString::fromLatin1(entry.name));
    if (!overridden.isEmpty())
        return overridden;
    if (design == Design::Next)
        return QString::fromLatin1(dark ? entry.nextDark : entry.nextLight);
    return QString::fromLatin1(dark ? entry.classicDark : entry.classicLight);
}

QString hex(Role role)
{
    const QString value = hex(role, g_design, g_dark);
    if (g_design == Design::Next)
        return value;
    return remapColor(QColor(value)).name(QColor::HexRgb);
}

QColor color(Role role, Design design, bool dark)
{
    return QColor(hex(role, design, dark));
}

QColor color(Role role)
{
    return QColor(hex(role));
}

QColor color(Role role, int alpha)
{
    QColor value = color(role);
    value.setAlpha(alpha);
    return value;
}

QColor nextOr(Role role, const QColor &classic)
{
    return g_design == Design::Next ? color(role) : classic;
}

QString nextOrHex(Role role, const QString &classicHex)
{
    return g_design == Design::Next ? hex(role) : classicHex;
}

QString chatHex(const QString &name, bool dark)
{
    for (const ChatToneValues &tone : kChatTones) {
        if (name == QLatin1String(tone.name))
            return QString::fromLatin1(dark ? tone.dark : tone.light);
    }
    return QString();
}

QColor chatColor(const QString &name)
{
    return QColor(chatHex(name, g_dark));
}

QColor iconColorOverride(const QString &objectName, const QColor &classic)
{
    if (g_design != Design::Next)
        return classic;
    if (objectName == QLatin1String("quickAddSendIcon"))
        return color(Role::AccentFg);
    if (objectName == QLatin1String("quickAddGenieButton") ||
        objectName == QLatin1String("chatIntegrationButton") ||
        objectName == QLatin1String("chatAddChannelButton"))
        return color(Role::FgSecondary);
    if (objectName == QLatin1String("chatNavButton") ||
        objectName == QLatin1String("chatSectionToggle") ||
        objectName == QLatin1String("chatSectionAddButton") ||
        objectName == QLatin1String("chatComposeButton") ||
        objectName == QLatin1String("chatWorkspaceMenu"))
        return chatColor(QStringLiteral("chatMuted"));
    if (objectName == QLatin1String("composerToolButton") ||
        objectName == QLatin1String("composerSendButton") ||
        objectName == QLatin1String("chatHeaderIconButton") ||
        objectName == QLatin1String("chatHeaderMeta"))
        return chatColor(QStringLiteral("chatComposerIcon"));
    if (objectName == QLatin1String("composerAttachButton"))
        return chatColor(QStringLiteral("chatComposer"));
    return classic;
}

namespace {

QString g_uiFamily;
QString g_monoFamily;
bool g_fontsLoaded = false;

QString registerFamily(const QStringList &files)
{
    QString family;
    for (const QString &file : files) {
        const int id = QFontDatabase::addApplicationFont(file);
        if (id < 0)
            continue;
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (family.isEmpty() && !families.isEmpty())
            family = families.first();
    }
    return family;
}

}

void ensureBundledFonts()
{
    if (g_fontsLoaded || !qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return;
    g_fontsLoaded = true;
    g_uiFamily = registerFamily({QStringLiteral(":/theme/fonts/Geist-Regular.ttf"),
                                 QStringLiteral(":/theme/fonts/Geist-Medium.ttf"),
                                 QStringLiteral(":/theme/fonts/Geist-SemiBold.ttf")});
    g_monoFamily =
        registerFamily({QStringLiteral(":/theme/fonts/GeistMono-Regular.ttf"),
                        QStringLiteral(":/theme/fonts/GeistMono-Medium.ttf"),
                        QStringLiteral(":/theme/fonts/GeistMono-SemiBold.ttf")});
}

QString uiFontFamily()
{
    return g_uiFamily;
}

QString monoFontFamily()
{
    return g_monoFamily;
}

QFont interfaceFont(Design design, const QFont &classicFont)
{
    if (design != Design::Next)
        return classicFont;
    ensureBundledFonts();
    if (g_uiFamily.isEmpty())
        return classicFont;
    QFont font = classicFont;
    QStringList families{g_uiFamily};
    for (const QString &family : classicFont.families()) {
        if (!families.contains(family))
            families.append(family);
    }
    font.setFamilies(families);
    return font;
}

QFont monospaceFont(const QFont &classicFont)
{
    if (g_design != Design::Next)
        return classicFont;
    ensureBundledFonts();
    if (g_monoFamily.isEmpty())
        return classicFont;
    QFont font = classicFont;
    QStringList families{g_monoFamily};
    for (const QString &family : classicFont.families()) {
        if (!families.contains(family))
            families.append(family);
    }
    font.setFamilies(families);
    font.setStyleHint(QFont::Monospace);
    font.setFixedPitch(true);
    return font;
}

QMap<QString, QString> nextPalette(bool dark)
{
    QMap<QString, QString> palette;
    if (dark) {
        for (const PaletteEntry &entry : kNextDarkPalette)
            palette.insert(QString::fromLatin1(entry.from),
                           QString::fromLatin1(entry.to));
    } else {
        for (const PaletteEntry &entry : kNextLightPalette)
            palette.insert(QString::fromLatin1(entry.from),
                           QString::fromLatin1(entry.to));
    }
    const QMap<QString, QString> instance = overridePalette(Design::Next, dark);
    if (instance.isEmpty())
        return palette;
    for (auto it = palette.begin(); it != palette.end(); ++it) {
        const QString wanted = instance.value(it.value());
        if (!wanted.isEmpty())
            it.value() = wanted;
    }
    for (auto it = instance.constBegin(); it != instance.constEnd(); ++it)
        if (!palette.contains(it.key()))
            palette.insert(it.key(), it.value());
    return palette;
}

QString expandTokens(const QString &text, Design design, bool dark)
{
    static const QRegularExpression pattern(QStringLiteral("@([A-Za-z]+)"));
    QString result;
    result.reserve(text.size());
    int last = 0;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += QStringView(text).mid(last, match.capturedStart() - last);
        const QString name = match.captured(1);
        QString replacement;
        for (int index = 0; index < kRoleCount; ++index) {
            if (name == QLatin1String(kRoles[index].name)) {
                replacement = hex(static_cast<Role>(index), design, dark);
                break;
            }
        }
        if (replacement.isEmpty() && name.startsWith(QLatin1String("chat")))
            replacement = chatHex(name, dark);
        if (replacement.isEmpty()) {
            if (name == QLatin1String("checkIcon")) {
                replacement = QStringLiteral(":/icons/octicons/check-white.svg");
            } else if (name == QLatin1String("chevronIcon")) {
                replacement = dark ? QStringLiteral(":/theme/next/chevron-dark.svg")
                                   : QStringLiteral(":/theme/next/chevron-light.svg");
            } else if (name == QLatin1String("chevronRightIcon")) {
                replacement = dark ? QStringLiteral(":/theme/next/chevron-right-dark.svg")
                                   : QStringLiteral(":/theme/next/chevron-right-light.svg");
            } else if (name == QLatin1String("menuCheckIcon")) {
                replacement = dark ? QStringLiteral(":/theme/next/check-dark.svg")
                                   : QStringLiteral(":/theme/next/check-light.svg");
            } else if (name == QLatin1String("closeIcon")) {
                replacement = dark ? QStringLiteral(":/theme/next/close-dark.svg")
                                   : QStringLiteral(":/theme/next/close-light.svg");
            } else if (name == QLatin1String("closeHoverIcon")) {
                replacement = dark ? QStringLiteral(":/theme/next/close-hover-dark.svg")
                                   : QStringLiteral(":/theme/next/close-hover-light.svg");
            }
        }
        result += replacement.isEmpty() ? match.captured() : replacement;
        last = match.capturedEnd();
    }
    result += QStringView(text).mid(last);
    return result;
}

QString nextOverlay(bool dark)
{
    const QDir dir(QString::fromLatin1(kNextSheetDir));
    const QStringList names =
        dir.entryList({QStringLiteral("*.qss")}, QDir::Files, QDir::Name);
    QString overlay;
    for (const QString &name : names) {
        if (name == QLatin1String(kCompactSheet) ||
            name == QLatin1String(kEdgeSheet))
            continue;
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        overlay += QString::fromUtf8(file.readAll());
        overlay += QLatin1Char('\n');
    }
    return expandTokens(overlay, Design::Next, dark);
}

namespace {

const char kDensitySetting[] = "ui/density";
constexpr int kCompactScreenWidth = 1500;

bool compactDensity()
{
    QString value = qEnvironmentVariable("FORKMESH_DENSITY").trimmed().toLower();
    if (value.isEmpty()) {
        value = QSettings()
                    .value(QLatin1String(kDensitySetting), QStringLiteral("auto"))
                    .toString()
                    .trimmed()
                    .toLower();
    }
    if (value == QLatin1String("compact"))
        return true;
    if (value == QLatin1String("comfortable"))
        return false;
    if (!qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        return false;
    const QScreen *screen = QGuiApplication::primaryScreen();
    return screen && screen->availableGeometry().width() < kCompactScreenWidth;
}

QString shrinkFontSizes(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("font-size:\\s*(\\d+)px"));
    QString result;
    result.reserve(text.size());
    int last = 0;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        result += QStringView(text).mid(last, match.capturedStart() - last);
        const int size = match.captured(1).toInt();
        result += QStringLiteral("font-size: %1px").arg(size >= 13 ? size - 1 : size);
        last = match.capturedEnd();
    }
    result += QStringView(text).mid(last);
    return result;
}

}

QString nextStyleSheet(bool dark)
{
    QString sheet = remapHexColours(QString::fromLatin1(styleSheetForDark(dark)),
                                    nextPalette(dark));
    sheet = capFontWeights(sheet);
    sheet += QLatin1Char('\n');
    sheet += nextOverlay(dark);
    if (edgeIndicators()) {
        QFile edges(QDir(QString::fromLatin1(kNextSheetDir))
                        .filePath(QString::fromLatin1(kEdgeSheet)));
        if (edges.open(QIODevice::ReadOnly)) {
            sheet += QLatin1Char('\n');
            sheet += expandTokens(QString::fromUtf8(edges.readAll()),
                                  Design::Next, dark);
        }
    }
    if (compactDensity()) {
        QFile compact(QDir(QString::fromLatin1(kNextSheetDir))
                          .filePath(QString::fromLatin1(kCompactSheet)));
        if (compact.open(QIODevice::ReadOnly)) {
            sheet += QLatin1Char('\n');
            sheet += expandTokens(QString::fromUtf8(compact.readAll()),
                                  Design::Next, dark);
        }
        sheet = shrinkFontSizes(sheet);
    }
    return sheet;
}

QString styleSheetForDesign(Design design, bool dark)
{
    return design == Design::Next ? nextStyleSheet(dark)
                                  : QString::fromLatin1(styleSheetForDark(dark));
}

QPalette paletteForDesign(Design design, bool dark)
{
    QPalette palette = applicationPalette(dark);
    if (design != Design::Next)
        return palette;
    palette.setColor(QPalette::Highlight, color(Role::Selection, design, dark));
    palette.setColor(QPalette::HighlightedText, color(Role::Fg, design, dark));
    palette.setColor(QPalette::Link, color(Role::AccentFg, design, dark));
    QColor placeholder = color(Role::FgSubtle, design, dark);
    palette.setColor(QPalette::PlaceholderText, placeholder);
    return palette;
}

}
