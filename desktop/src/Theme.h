#pragma once

#include <QByteArray>
#include <QColor>
#include <QMap>
#include <QPalette>
#include <QString>

namespace Theme {

inline const char *kPrimary = "#2ea043";
inline const char *kTextTertiary = "#8b949e";

inline const char *kRunning = "#58a6ff";
inline const char *kAgentFireball = "#bc8cff";

inline const char *kGenie = "#bc8cff";

inline const char *kSenderPalette[] = {"#f85149", "#e3b341", "#3fb950",
                                       "#58a6ff", "#bc8cff", "#db61a2",
                                       "#39c5cf", "#db6d28"};
inline constexpr int kSenderPaletteSize = 8;

inline QMap<QString, QString> &activePalette()
{
    static QMap<QString, QString> palette;
    return palette;
}

inline QColor remapColor(const QColor &source)
{
    if (!source.isValid() || activePalette().isEmpty())
        return source;
    const QString replacement =
        activePalette().value(source.name(QColor::HexRgb).toLower());
    if (replacement.isEmpty())
        return source;
    QColor mapped(replacement);
    mapped.setAlpha(source.alpha());
    return mapped;
}

// Theme packages recolour the complete base stylesheet by exact palette token.
// Keep the handful of semantic colours used by custom-painted widgets on the
// same map so changing skins does not leave spinners, avatars, or agent status
// glyphs in the base theme's colours.
inline void setActivePalette(const QMap<QString, QString> &palette)
{
    QMap<QString, QString> normalized;
    for (auto it = palette.constBegin(); it != palette.constEnd(); ++it)
        normalized.insert(it.key().toLower(), it.value().toLower());
    activePalette() = normalized;

    static QByteArray primary;
    static QByteArray tertiary;
    static QByteArray running;
    static QByteArray fireball;
    static QByteArray genie;
    static QByteArray senders[kSenderPaletteSize];
    const auto mapped = [&normalized](const char *fallback) {
        return normalized.value(QString::fromLatin1(fallback),
                                QString::fromLatin1(fallback))
            .toLatin1();
    };
    primary = mapped("#2ea043");
    tertiary = mapped("#8b949e");
    running = mapped("#58a6ff");
    fireball = mapped("#bc8cff");
    genie = mapped("#bc8cff");
    const char *senderDefaults[kSenderPaletteSize] = {
        "#f85149", "#e3b341", "#3fb950", "#58a6ff",
        "#bc8cff", "#db61a2", "#39c5cf", "#db6d28",
    };
    for (int i = 0; i < kSenderPaletteSize; ++i)
        senders[i] = mapped(senderDefaults[i]);

    kPrimary = primary.constData();
    kTextTertiary = tertiary.constData();
    kRunning = running.constData();
    kAgentFireball = fireball.constData();
    kGenie = genie.constData();
    for (int i = 0; i < kSenderPaletteSize; ++i)
        kSenderPalette[i] = senders[i].constData();
}

inline QPalette applicationPalette(bool dark)
{
    const auto color = [](const char *value) {
        return remapColor(QColor(QString::fromLatin1(value)));
    };
    QPalette palette;
    palette.setColor(QPalette::Window, color(dark ? "#0d1117" : "#f6f8fa"));
    palette.setColor(QPalette::WindowText, color(dark ? "#e6edf3" : "#1f2328"));
    palette.setColor(QPalette::Base, color(dark ? "#0d1117" : "#ffffff"));
    palette.setColor(QPalette::AlternateBase,
                     color(dark ? "#161b22" : "#f6f8fa"));
    palette.setColor(QPalette::ToolTipBase,
                     color(dark ? "#161b22" : "#ffffff"));
    palette.setColor(QPalette::ToolTipText,
                     color(dark ? "#e6edf3" : "#1f2328"));
    palette.setColor(QPalette::Text, color(dark ? "#e6edf3" : "#1f2328"));
    palette.setColor(QPalette::Button, color(dark ? "#21262d" : "#f6f8fa"));
    palette.setColor(QPalette::ButtonText,
                     color(dark ? "#e6edf3" : "#1f2328"));
    palette.setColor(QPalette::BrightText, color(dark ? "#ffffff" : "#0d1117"));
    palette.setColor(QPalette::Highlight, color(dark ? "#1f6feb" : "#0969da"));
    palette.setColor(QPalette::HighlightedText,
                     color(dark ? "#010409" : "#ffffff"));
    palette.setColor(QPalette::Link, color(dark ? "#58a6ff" : "#0969da"));
    palette.setColor(QPalette::LinkVisited,
                     color(dark ? "#bc8cff" : "#8250df"));
    palette.setColor(QPalette::Light, color(dark ? "#424a53" : "#ffffff"));
    palette.setColor(QPalette::Midlight,
                     color(dark ? "#30363d" : "#eaeef2"));
    palette.setColor(QPalette::Mid, color(dark ? "#30363d" : "#d0d7de"));
    palette.setColor(QPalette::Dark, color(dark ? "#161b22" : "#afb8c1"));
    palette.setColor(QPalette::Shadow, color(dark ? "#010409" : "#656d76"));
    QColor placeholder = color(dark ? "#8b949e" : "#656d76");
    placeholder.setAlpha(190);
    palette.setColor(QPalette::PlaceholderText, placeholder);
    palette.setColor(QPalette::Disabled, QPalette::WindowText,
                     color(dark ? "#6e7681" : "#8c959f"));
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     color(dark ? "#6e7681" : "#8c959f"));
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     color(dark ? "#6e7681" : "#8c959f"));
    return palette;
}

inline QString remapSheet(const QString &sheet)
{
    const QMap<QString, QString> &palette = activePalette();
    if (palette.isEmpty() || !sheet.contains(QLatin1Char('#')))
        return sheet;
    QString result;
    result.reserve(sheet.size());
    const int size = sheet.size();
    int index = 0;
    while (index < size) {
        const QChar ch = sheet.at(index);
        if (ch != QLatin1Char('#') || index + 7 > size) {
            result += ch;
            ++index;
            continue;
        }
        bool hex = true;
        for (int offset = 1; offset <= 6 && hex; ++offset) {
            const char16_t code = sheet.at(index + offset).unicode();
            hex = (code >= u'0' && code <= u'9') ||
                  (code >= u'a' && code <= u'f') ||
                  (code >= u'A' && code <= u'F');
        }
        if (hex && index + 7 < size) {
            const char16_t next = sheet.at(index + 7).unicode();
            if ((next >= u'0' && next <= u'9') || (next >= u'a' && next <= u'f') ||
                (next >= u'A' && next <= u'F'))
                hex = false;
        }
        if (!hex) {
            result += ch;
            ++index;
            continue;
        }
        const QString found = sheet.mid(index, 7);
        const QString replacement = palette.value(found.toLower());
        result += replacement.isEmpty() ? found : replacement;
        index += 7;
    }
    return result;
}

inline const char *checkedIconColorForButton(const QString &objectName, bool dark)
{
    if (objectName == QStringLiteral("promptCloudButton") ||
        objectName == QStringLiteral("promptWorktreeCheck"))
        return dark ? "#58a6ff" : "#0969da";
    if (objectName == QStringLiteral("promptVibeCheck"))
        return dark ? "#d2a8ff" : "#8250df";
    return nullptr;
}

inline const char *iconColorForButton(const QString &objectName, bool dark)
{
    if (objectName == QStringLiteral("primaryButton") ||
        objectName == QStringLiteral("scmSyncButton") ||
        objectName == QStringLiteral("toolbarPrimaryButton") ||
        objectName == QStringLiteral("agentsMagicButton") ||
        objectName == QStringLiteral("teammateBriefingGenerate") ||
        objectName == QStringLiteral("teammateBriefingRunAgain") ||
        objectName == QStringLiteral("timerChipDone"))
        return "#ffffff";
    if (objectName == QStringLiteral("quickAddGenieButton"))
        return dark ? "#c084fc" : "#7c3aed";
    if (objectName == QStringLiteral("dangerButton"))
        return dark ? "#f85149" : "#cf222e";
    if (objectName == QStringLiteral("successButton"))
        return dark ? "#3fb950" : "#1a7f37";
    if (objectName == QStringLiteral("repoTab") ||
        objectName == QStringLiteral("inlineHelpButton") ||
        objectName == QStringLiteral("logFrequencyPromptButton") ||
        objectName == QStringLiteral("socialIconButton") ||
        objectName == QStringLiteral("timersClockButton") ||
        objectName == QStringLiteral("timerChipPaused"))
        return dark ? "#8b949e" : "#656d76";
    if (objectName == QStringLiteral("quickAddSendIcon"))
        return dark ? "#3fb950" : "#1a7f37";
    if (objectName == QStringLiteral("chatAddChannelButton"))
        return dark ? "#58a6ff" : "#0969da";
    if (objectName == QStringLiteral("chatIntegrationButton"))
        return dark ? "#a5b4fc" : "#5865f2";
    return dark ? "#e6edf3" : "#1f2328";
}

inline const char *kStyleSheet = R"(
* { outline: none; }
QWidget {
    background-color: #0d1117;
    color: #e6edf3;
    /* No font-family here: a family in this rule beats every widget-level
       setFont(), which silently turned each monospace log, code and transcript
       view into the UI face. main.cpp picks the UI family once and sets it as
       the application font, so widgets that never call setFont() still inherit
       it, while a QFont::Monospace setFont() now wins. */
    font-size: 14px;
}
#appWindowFrame { background-color: #30363d; }
QToolTip {
    background-color: #161b22; color: #e6edf3;
    border: 1px solid #30363d; padding: 4px;
}

#setupCard {
    background-color: #161b22;
    border: 1px solid #30363d;
    border-radius: 12px;
}
#appTitle { font-size: 26px; font-weight: 800; background: transparent; }
#appTitleAccent { color: #f85149; }
#appSubtitle { color: #8b949e; background: transparent; }
#setupCard QLabel { background: transparent; }
#onboardingCard {
    background-color: #161b22;
    border: 1px solid #484f58;
    border-radius: 18px;
}
#onboardingCard QLabel, #onboardingWelcomeFeatures { background: transparent; }
#onboardingBrand {
    color: #3fb950; font-size: 11px; font-weight: 850; letter-spacing: 2px;
}
#onboardingKicker {
    color: #58a6ff; font-size: 11px; font-weight: 800; letter-spacing: 1px;
}
#onboardingTitle {
    color: #f0f6fc; font-size: 27px; font-weight: 850;
}
#onboardingBody { color: #c9d1d9; font-size: 14px; }
#onboardingFeatureCard {
    background-color: #0d1117;
    border: 1px solid #30363d;
    border-radius: 10px;
}
#onboardingFeatureCard QLabel { background: transparent; }
#onboardingFeatureBadge {
    background-color: #1f6feb; color: #ffffff;
    border-radius: 14px; font-size: 10px; font-weight: 850;
}
#onboardingFeatureCopy { color: #c9d1d9; font-size: 12px; }
#onboardingDetails {
    color: #c9d1d9; background-color: #0d1117;
    border: 1px solid #30363d; border-radius: 9px; padding: 11px;
}
#onboardingHint { color: #8b949e; font-size: 12px; }
#onboardingProgressLabel { color: #8b949e; font-size: 11px; font-weight: 700; }
QProgressBar#onboardingProgress {
    background-color: #30363d; border: none; border-radius: 2px;
}
QProgressBar#onboardingProgress::chunk {
    background-color: #2ea043; border-radius: 2px;
}
QPushButton#onboardingCloseButton {
    background: transparent; border: none; color: #8b949e;
    border-radius: 15px; padding: 0; font-size: 22px; font-weight: 400;
}
QPushButton#onboardingCloseButton:hover { background: #30363d; color: #f0f6fc; }
#onboardingTrackRow { background: transparent; }
QPushButton#onboardingTrackButton {
    background-color: #0d1117; color: #c9d1d9; border: 1px solid #30363d;
    border-radius: 10px; padding: 11px 14px; text-align: left;
    font-size: 12px; font-weight: 600;
}
QPushButton#onboardingTrackButton:hover { border-color: #58a6ff; color: #f0f6fc; }
QPushButton#onboardingTrackButton:focus { border-color: #58a6ff; }
QPushButton#onboardingTrackButton:pressed { background-color: #161b22; }
#calendarAgentCard {
    background-color: #161b22;
    border: 1px solid #30363d;
    border-radius: 10px;
}
#calendarAgentCard QLabel { background: transparent; }
#calendarAgentHeading {
    font-size: 16px; font-weight: 700; color: #e6edf3;
}
#ledgerWalletCard {
    background-color: #161b22;
    border: 1px solid #30363d;
    border-radius: 10px;
}
#ledgerWalletCard QLabel { background: transparent; }
#ledgerWalletLabel {
    color: #3fb950; font-size: 10px; font-weight: 750; letter-spacing: 1px;
}
#ledgerWalletAge { color: #6e7681; font-size: 10px; }
#ledgerWalletAmount { font-size: 19px; font-weight: 750; color: #e6edf3; }
#ledgerWalletUsd { color: #8b949e; font-size: 12px; font-weight: 650; }
QPushButton#ledgerWalletRefresh {
    background: transparent; border: 1px solid #30363d; border-radius: 11px;
    padding: 0px;
}
QPushButton#ledgerWalletRefresh:hover { border-color: #58a6ff; }
#ledgerWalletAddress { color: #8b949e; font-size: 11px; }
#ledgerWalletNote { color: #6e7681; font-size: 11px; }
#ledgerWalletWarning { color: #d29922; font-size: 11px; font-weight: 650; }
#ledgerEntryForm {
    background-color: #161b22;
    border: 1px solid #30363d;
    border-radius: 10px;
}
#ledgerEntryForm QLabel, #ledgerEntryForm QCheckBox { background: transparent; }
#ledgerPayoutHoldPanel {
    background-color: #2b210b;
    border: 1px solid #9e6a03;
    border-radius: 10px;
}
#ledgerPayoutHoldPanel QLabel { background: transparent; }
#ledgerPayoutHoldTitle { color: #e3b341; font-weight: 750; }
#ledgerPayoutHoldDetails { color: #c9d1d9; font-size: 12px; }
QPushButton#ledgerPayoutUndoButton { color: #f85149; border-color: #6e3030; }
QPushButton#ledgerPayoutUndoButton:hover {
    color: #ffffff; background-color: #da3633; border-color: #f85149;
}
#ledgerTotals { color: #8b949e; font-size: 12px; font-weight: 650; }
QTextEdit#calendarAgentPrompt {
    background-color: #0d1117; color: #e6edf3;
    border: 1px solid #30363d; border-radius: 7px;
    padding: 8px; selection-background-color: #1f6feb;
}
QTextEdit#calendarAgentPrompt:focus { border-color: #58a6ff; }
#calendarDetailsScroll, #calendarDetailsScroll > QWidget,
#calendarDetailsScroll > QWidget > QWidget {
    background: transparent; border: none;
}
QRadioButton { background: transparent; spacing: 8px; padding: 4px 0; }
QRadioButton::indicator {
    width: 16px; height: 16px; border-radius: 9px;
    border: 2px solid #30363d; background: #0d1117;
}
QRadioButton::indicator:checked { border-color: #1f6feb; background: #1f6feb; }
QCheckBox { background: transparent; spacing: 8px; padding: 4px 0; }
QCheckBox::indicator {
    width: 16px; height: 16px; border-radius: 4px;
    border: 2px solid #30363d; background: #0d1117;
}
QCheckBox::indicator:checked { border-color: #1f6feb; background: #1f6feb; }
#modeHint { color: #8b949e; font-size: 12px; padding-left: 26px; background: transparent; }

#relayHostEdit { color: #8b949e; }
#relayHostEdit:focus { color: #e6edf3; }

#wizardStep { color: #3fb950; font-size: 11px; font-weight: 700; letter-spacing: 1px; background: transparent; }
#wizardTitle { font-size: 20px; font-weight: 800; background: transparent; }

QLineEdit, QSpinBox, QComboBox {
    background-color: #0d1117;
    border: 1px solid #30363d;
    border-radius: 6px;
    padding: 8px 10px;

    min-height: 19px;
    selection-background-color: #1f6feb;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #58a6ff; }
QLineEdit:disabled, QSpinBox:disabled { color: #6e7681; border-color: #21262d; }
QSpinBox::up-button, QSpinBox::down-button { width: 0; }
QComboBox QAbstractItemView {
    background-color: #161b22; border: 1px solid #30363d;
    selection-background-color: #1f6feb; selection-color: #0d1117; color: #e6edf3;
}
QComboBox QAbstractItemView::item:selected { color: #0d1117; }
QComboBox#quickAddAgentSelector, QComboBox#quickAddModelSelector, QComboBox#quickAddAgentModelSelector, QComboBox#quickAddModeSelector, QComboBox#quickAddSpeedSelector {
    border: none;
    background-color: transparent;
    padding: 0px 4px 0px 8px;
}
QComboBox#quickAddAgentSelector:focus, QComboBox#quickAddModelSelector:focus, QComboBox#quickAddAgentModelSelector:focus, QComboBox#quickAddModeSelector:focus, QComboBox#quickAddSpeedSelector:focus {
    border: none;
    background-color: rgba(88, 166, 255, 0.08);
}
QComboBox#quickAddAgentSelector::drop-down, QComboBox#quickAddModelSelector::drop-down, QComboBox#quickAddAgentModelSelector::drop-down {
    border: none;
    width: 20px;
}
QComboBox#quickAddAgentSelector::down-arrow, QComboBox#quickAddModelSelector::down-arrow, QComboBox#quickAddAgentModelSelector::down-arrow {
    image: url(:/icons/octicons/chevron-down.svg);
    width: 16px;
    height: 16px;
}

QComboBox#quickAddModeSelector, QComboBox#quickAddSpeedSelector {
    padding: 0px 4px;
}
QComboBox#quickAddModeSelector::drop-down, QComboBox#quickAddSpeedSelector::drop-down {
    border: none; width: 0px;
}
QComboBox#quickAddModeSelector::down-arrow, QComboBox#quickAddSpeedSelector::down-arrow {
    image: none; width: 0px; height: 0px;
}

QPushButton {
    background-color: #21262d;
    border: 1px solid #30363d;
    border-radius: 6px;
    padding: 8px 16px;
    font-weight: 600;
}
QPushButton:hover { background-color: #30363d; }
QPushButton:pressed { background-color: #282e35; }
QPushButton#primaryButton {
    background-color: #238636; border: 1px solid #2ea043; color: #ffffff;
}
QPushButton#primaryButton:hover { background-color: #2ea043; }
QPushButton#primaryButton:pressed { background-color: #1f7a31; }
QPushButton#scmSyncButton {
    background-color: #1f6feb; border: 1px solid #388bfd; color: #ffffff;
    padding: 6px 12px;
}
QPushButton#scmSyncButton:hover { background-color: #388bfd; }
QPushButton#scmSyncButton:pressed { background-color: #1158c7; }
QPushButton#scmSyncButton:disabled {
    background-color: #1f4b87; border-color: #315f91; color: #c9d1d9;
}
QPushButton#ghostButton {
    background: transparent; border: none; color: #8b949e;
    font-weight: 500; padding: 4px 8px; text-align: left;
}
QPushButton#ghostButton:hover { color: #e6edf3; }
QPushButton#ghostButton[needsPull="true"] {
    background-color: rgba(210, 153, 34, 0.15); border: 1px solid #d29922;
    border-radius: 6px; color: #e3b341;
}
QPushButton#ghostButton[needsPull="true"]:hover {
    background-color: rgba(210, 153, 34, 0.28); color: #f2cc60;
}
/* "Play" under a chat video card. It sits beside a ghost "Save", so it reads
   as the live action of the pair rather than a chunky default button. */
QPushButton#videoPlayButton {
    background: transparent; border: none; color: #58a6ff;
    font-weight: 600; padding: 4px 8px;
}
QPushButton#videoPlayButton:hover { color: #79c0ff; }
/* "carol  3 replies" under a message, behind the repliers' faces. A quiet
   link, not the chunky default push button. */
QPushButton#threadSummary {
    background: transparent; border: none; color: #58a6ff;
    font-size: 12px; font-weight: 600; padding: 1px 2px; text-align: left;
}
QPushButton#threadSummary:hover { color: #79c0ff; }

/* Primary chat-sidebar actions read as buttons rather than muted list rows. */
QPushButton#chatAddChannelButton {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 6px;
    color: #58a6ff; font-weight: 600; padding: 7px 10px; text-align: center;
}
QPushButton#chatAddChannelButton:hover {
    background-color: #1f2630; border-color: #58a6ff; color: #79c0ff;
}
QPushButton#chatAddChannelButton:pressed { background-color: #10151c; }
QPushButton#chatIntegrationButton {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 6px;
    color: #a5b4fc; font-weight: 600; padding: 7px 10px; text-align: center;
}
QPushButton#chatIntegrationButton:hover {
    background-color: #23253a; border-color: #5865f2; color: #c7d2fe;
}
QPushButton#chatIntegrationButton[connected="true"] {
    border-color: #238636; color: #3fb950;
}
QPushButton#chatIntegrationButton[bad="true"] {
    border-color: #da3633; color: #f85149;
}

#logFilterScroll, #logFilterScroll > QWidget,
#logFilterScroll > QWidget > QWidget { background: transparent; border: none; }

QPushButton#logFilterChip {
    background: transparent; border: none; padding: 0;
}

QPushButton#donateButton {
    background-color: #9945ff; border: 1px solid #b07bff; color: #ffffff;
    font-weight: 700;
}
QPushButton#donateButton:hover { background-color: #a85cff; }
QPushButton#donateButton:pressed { background-color: #7d34d6; }
QPushButton#socialButton {
    background: transparent; border: 1px solid #30363d; color: #8b949e;
    font-weight: 600; padding: 8px 12px;
}
QPushButton#socialButton:hover { color: #e6edf3; border-color: #6e7681; }

QPushButton#socialIconButton {
    background: transparent; border: 1px solid #30363d; color: #8b949e;
    border-radius: 6px; padding: 0;
}
QPushButton#socialIconButton:hover { border-color: #6e7681; }
#footerGitIdentity { color: #8b949e; font-size: 12px; }

#appStatusBar { background-color: #010409; border-top: 1px solid #30363d; }
#appStatusBar QLabel {
    background: transparent; color: #8b949e; font-size: 11px;
}
#appStatusBar QPushButton {
    background: transparent; border: none; color: #c9d1d9;
    font-size: 11px; padding: 0 4px;
}
#appStatusBar QPushButton:hover { color: #58a6ff; }
/* ForkMesh Ads "Sponsored" strip above the status bar. It borrows the status
   bar's surface and its muted greys so it reads as footer furniture, not as
   an alert: no accent fill, no icon, nothing that moves. */
#adBanner { background-color: #010409; border-top: 1px solid #30363d; }
#adBanner QLabel { background: transparent; }
#adBannerCaption {
    color: #8b949e; font-size: 10px; font-weight: 600;
    border: 1px solid #30363d; border-radius: 3px; padding: 0 4px;
}
#adBannerSponsor { color: #8b949e; font-size: 11px; }
#adBannerHeadline { color: #c9d1d9; font-size: 12px; font-weight: 600; }
#adBannerBody { color: #8b949e; font-size: 12px; }
QPushButton#adBannerCta {
    background-color: #21262d; border: 1px solid #30363d; border-radius: 6px;
    color: #c9d1d9; font-size: 12px; font-weight: 600; padding: 3px 10px;
}
QPushButton#adBannerCta:hover { border-color: #8b949e; color: #e6edf3; }
QPushButton#adBannerLink, QPushButton#adBannerHide {
    background: transparent; border: none; color: #8b949e;
    font-size: 11px; font-weight: 400; padding: 2px 4px;
}
QPushButton#adBannerLink:hover { color: #58a6ff; text-decoration: underline; }
QPushButton#adBannerHide:hover { color: #e6edf3; }
#statusVersionButton:checked { color: #58a6ff; font-weight: 700; }
#debugBar { background-color: #0d1117; border-top: 1px solid #30363d; }
#debugBarScroll, #debugBarContent { background: transparent; border: none; }
#debugBarSeparator { color: #30363d; }
#debugBarScroll QScrollBar:horizontal { height: 7px; }

#debugBarTools { background: transparent; }
#debugLogTail {
    background-color: #0d1117; border-top: 1px solid #30363d;
    color: #8b949e; padding: 2px 8px;
}

#statusBackgroundTasks { background: transparent; }

#navRail { background-color: #010409; border-right: 1px solid #30363d; }
#navRail QLabel { background: transparent; }
#navLogo { font-size: 18px; font-weight: 800; padding-bottom: 4px; }
QPushButton#navButton {
    background: transparent; border: none; color: #8b949e;
    font-size: 11px; font-weight: 600; border-radius: 6px; padding: 8px 2px;
}
QPushButton#navButton:hover { background-color: #161b22; color: #e6edf3; }
QPushButton#navButton:checked { background-color: #21262d; color: #e6edf3; }

QPushButton#repoTab {
    background: transparent; border: none; border-bottom: 2px solid transparent;
    color: #8b949e; font-weight: 600; padding: 6px 10px;
}
QPushButton#repoTab:hover { color: #e6edf3; }
QPushButton#repoTab:checked { color: #e6edf3; border-bottom: 2px solid #fd8c73; }

#fileTree { background-color: #0d1117; border: 1px solid #30363d; border-radius: 6px; }
#fileTree::item { padding: 3px 2px; color: #c9d1d9; }

#fileTree::item:selected { background-color: #1f6feb; color: #ffffff; padding: 3px 2px; }
#fileTabs::pane { border: 1px solid #30363d; border-radius: 6px; top: -1px; }
#fileTabs QTabBar::tab {
    background: #0d1117; color: #8b949e; padding: 6px 12px;
    border: 1px solid transparent; border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
#fileTabs QTabBar::tab:hover { color: #e6edf3; }
#fileTabs QTabBar::tab:selected {
    background: #161b22; color: #e6edf3; border-color: #30363d; border-bottom-color: #161b22;
}
#repoSettingsTabs::pane { border: 1px solid #30363d; border-radius: 6px; top: -1px; }
#repoSettingsTabs QTabBar::tab {
    background: #0d1117; color: #8b949e; padding: 7px 16px;
    border: 1px solid transparent; border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
#repoSettingsTabs QTabBar::tab:hover { color: #e6edf3; }
#repoSettingsTabs QTabBar::tab:selected {
    background: #161b22; color: #e6edf3; border-color: #30363d; border-bottom-color: #161b22;
}

/* The Network tabs pick a view the way the log range buttons pick a window,
   so they are drawn as the app's buttons rather than as a document tab strip:
   the same fill, border, radius and weight, with the open tab carrying the
   accent fill a checked button gets. */
#networkTabs::pane { border: none; }
#networkTabs QTabBar::tab {
    background-color: #21262d; color: #e6edf3; padding: 4px 12px;
    border: 1px solid #30363d; border-radius: 6px;
    margin: 0 6px 0 0; font-weight: 600;
}
#networkTabs QTabBar::tab:hover { background-color: #30363d; }
#networkTabs QTabBar::tab:selected {
    background-color: #1f6feb; border-color: #1f6feb; color: #ffffff;
}

/* Settings is one page: a section list on the left, a heading per section on
   the page, and search marks drawn by #settingsSearchHits, which must stay
   see-through over the controls it outlines. */
#settingsNav { background: transparent; border: none; }
#settingsNav::item { color: #8b949e; padding: 6px 10px; border-radius: 6px; margin: 1px 0; }
#settingsNav::item:hover { background-color: #161b22; color: #e6edf3; }
#settingsNav::item:selected { background-color: rgba(56, 139, 253, 0.15); color: #e6edf3; }
#settingsSectionTitle { font-size: 17px; font-weight: 600; color: #e6edf3; }
#settingsSectionRule { background-color: #21262d; }
#settingsSectionHeader { background: transparent; }
QToolButton#settingsSectionToggle { border: none; border-radius: 4px; background: transparent; padding: 0; }
QToolButton#settingsSectionToggle:hover { background-color: #21262d; }
#settingsNoMatches { color: #8b949e; padding: 12px 2px; }
#settingsSearchHits { background: transparent; }
#codeEditor {
    background-color: #0d1117; border: none; color: #e6edf3;
    font-family: monospace; font-size: 12px;
}
#diffView {
    background-color: #0d1117; border: none; border-radius: 0;
    color: #e6edf3; font-family: monospace; font-size: 12px;
}
#commitBar {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 6px;
}
#commitBar QLabel { background: transparent; border: none; }
#commitBarText { color: #e6edf3; }
#overviewList { background: transparent; border: none; }
#overviewList::item { padding: 5px 4px; color: #c9d1d9; }

#overviewList::item:selected { background-color: #1f6feb; color: #ffffff; padding: 5px 4px; }
#overviewList QHeaderView::section {
    background-color: #0d1117; color: #8b949e; padding: 4px 8px;
    border: none; border-bottom: 1px solid #21262d;
    border-right: 2px solid #010409; font-weight: 600;
}

#sizeBarTrack { background-color: #21262d; border-radius: 3px; }
#sizeBarFill { background-color: #3fb950; border-radius: 3px; }
#overviewMetricTrack { background-color: #21262d; border-radius: 2px; }
#overviewMetricFill { background-color: #3fb950; border-radius: 2px; }
#overviewLoadingPlaceholder { background-color: #30363d; border-radius: 3px; }
#overviewUpdatedTime { color: #8b949e; font-size: 10px; }
#readmeView {
    background: transparent; border: none; padding: 0; color: #e6edf3;
}

#repoHeaderTitle { font-size: 20px; font-weight: 400; }
#publicBadge {
    color: #8b949e; border: 1px solid #30363d; border-radius: 10px;
    padding: 0 8px; font-size: 11px;
}
#issueStatusPill {
    color: #ffffff; border-radius: 13px; padding: 5px 16px;
    font-size: 12px; font-weight: 700;
}
#issueStatusPill[status="open"] { background-color: #1f883d; }
#issueStatusPill[status="closed"] { background-color: #8250df; }
QPushButton#repoAction {
    background-color: #21262d; border: 1px solid #30363d; border-radius: 6px;
    padding: 4px 10px; font-size: 12px; font-weight: 600; color: #e6edf3;
}
QPushButton#repoAction:hover { background-color: #30363d; }
QPushButton#repoAction::menu-indicator { width: 0; }
#repoTabBar { border-bottom: 1px solid #30363d; }
#pullListActionBar { background: transparent; border: none; }
#pullListActionBarDivider {
    color: #30363d; background-color: #30363d; max-height: 1px;
}

#repoActivityRail { background-color: #010409; border-right: 1px solid #30363d; }
#appNavigationRail, #appNavigationRailContent {
    background-color: #0d1117; border-right: 1px solid #30363d;
}

#appNavigationRail QScrollBar:vertical { width: 6px; }
#appNavigationRail QScrollBar::handle:vertical { border-radius: 3px; }
#sideRail, #sideRailColumn, #sideRailScroll, #sideRailContent, #sideRailBrowserPanel { background-color: #0d1117; }
#sideRailColumn { border-left: 1px solid #30363d; }
#sideRailScroll QScrollBar:vertical { width: 6px; }
#sideRailScroll QScrollBar::handle:vertical { border-radius: 3px; }
#sideRailBrowserHandle:hover { background-color: #30363d; }
QLabel#sideRailBrowserTitle { color: #8b949e; font-size: 9px; font-weight: 600; }
QFrame#sideRailSeparator { color: #30363d; background-color: #30363d; border: none; max-height: 1px; }
QPushButton[sideRailTool="true"], QToolButton#sideRailItem {
    background: transparent; border: 1px solid transparent; border-radius: 10px; padding: 0;
}
QPushButton[sideRailTool="true"]:hover, QToolButton#sideRailItem:hover { background-color: #161b22; }
QPushButton[sideRailTool="true"]:checked { background-color: #1f2a37; border-color: #58a6ff; }
QPushButton#sideRailAddButton {
    background: transparent; border: 1px dashed #484f58; border-radius: 10px; padding: 0;
}
QPushButton#sideRailAddButton:hover { background-color: #161b22; border-color: #58a6ff; }
QLabel#terminalsTitle { font-size: 15px; font-weight: 600; }
QLabel#terminalsAgentStatus, QLabel#terminalsEmpty { color: #8b949e; }
#packageRail, #packageRailContent { background-color: #0d1117; }
#packageRail { border-left: 1px solid #30363d; }
#packageRail QScrollBar:vertical { width: 6px; }
#packageRail QScrollBar::handle:vertical { border-radius: 3px; }
QLabel#packageRailTitle, QLabel#teammateRailTitle { color: #8b949e; font-size: 9px; font-weight: 600; }
QPushButton#teammateRailAvatar {
    background: transparent; border: none; border-radius: 10px; padding: 0;
}
QPushButton#teammateRailAvatar:hover { background-color: #161b22; }
QLabel#teammateRailName {
    color: #e6edf3; font-size: 7px; font-weight: 600;
    background-color: rgba(13, 17, 23, 170); border-radius: 3px; padding: 0 2px;
}
QPushButton[teammateRailHud="true"] {
    background-color: rgba(13, 17, 23, 175); border: 1px solid rgba(139, 148, 158, 140);
    border-radius: 7px; padding: 0; color: #e6edf3; font-size: 10px; font-weight: 700;
}
QPushButton[teammateRailHud="true"]:hover {
    background-color: rgba(33, 38, 45, 235); border-color: #c9d1d9;
}
QLabel#teammateRailLastRun { color: #8b949e; font-size: 10px; }
QPushButton#teammateRailCreateButton {
    background: transparent; border: 1px dashed #484f58; border-radius: 22px;
    padding: 0;
}
QPushButton#teammateRailCreateButton:hover { background-color: #161b22; border-color: #58a6ff; }
QPushButton#teammateRailCreateButton[working="true"] { border: 1px solid #3fb950; }
QPushButton#teammateRailCreateButton:disabled { border-color: #30363d; }
QTableWidget#issueTable[nodesDirectory="true"]::item,
QTableWidget#issueTable[nodesDirectory="true"]::item:hover {
    padding: 1px 6px;
}
QTableWidget#issueTable[nodesDirectory="true"]::item:selected {
    background-color: #21262d; color: #e6edf3; padding: 1px 6px;
}

#railItemLabel {
    background: transparent; color: #8b949e; font-size: 10px; font-weight: 600;
}
#commitsList {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 6px;
}

#commitsList::item { padding: 8px; color: #e6edf3; }
#commitsList::item:hover { background-color: #161b22; padding: 8px; }
#commitsList::item:selected { background-color: #1f6feb; color: #ffffff; padding: 8px; }
#placeholderPanel { color: #e6edf3; font-size: 16px; }
#insightsPage QLabel { background: transparent; }
#insightsCard {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 8px;
    padding: 10px; color: #e6edf3;
}

#serverRail { background-color: #010409; border-right: 1px solid #30363d; }
QPushButton#serverButton {
    background-color: #21262d; border: 2px solid transparent;
    border-radius: 12px; padding: 0;
}
QPushButton#serverButton:hover { border-color: #30363d; border-radius: 12px; }
QPushButton#serverButton:checked { border-color: #2ea043; border-radius: 12px; }
QPushButton#serverAddButton {
    background-color: #21262d; border: none;
    border-radius: 12px; color: #3fb950; font-size: 22px; font-weight: 500;
    padding-bottom: 3px;
}
QPushButton#serverAddButton:hover { background-color: #2ea043; color: #ffffff; }
QPushButton#serverAddButton:pressed { background-color: #238636; }
QPushButton#serverFooterButton {
    background: transparent; border: none; color: #8b949e; font-size: 18px;

    border-radius: 13px;
}
QPushButton#serverFooterButton:hover { background-color: #161b22; color: #e6edf3; }

#breadcrumbBar { background-color: #0d1117; border-bottom: 1px solid #21262d; }
#windowChromeBar {
    background-color: #0d1117; border-bottom: 1px solid #21262d;
}
#appVersionLabel {
    background: transparent; color: #e6edf3; font-size: 13px; font-weight: 700;
}
#breadcrumb { background: transparent; font-size: 14px; font-weight: 600; }
#issueDetailOverlay { background-color: #0d1117; }
#connectionStatus { background: transparent; font-size: 13px; font-weight: 600; }

#connectionDot { border: 2px solid #0d1117; }

#relayJoinDot {
    background-color: #da3633; border: 1px solid #0d1117; border-radius: 5px;
}

#chromeDotDivider { background-color: #30363d; }

/* Timers package: the clock and its chips in the window chrome. A finished
   countdown or alarm shouts, a paused entry steps back, and a running one
   reads like the rest of the top bar. */
#timersNavStrip { background: transparent; }
QPushButton#timersClockButton {
    background: transparent; border: 1px solid transparent; border-radius: 6px;
    color: #8b949e; padding: 3px 5px;
}
QPushButton#timersClockButton:hover {
    background-color: #161b22; color: #e6edf3; border-color: #30363d;
}
QPushButton#timerChip, QPushButton#timerChipPaused, QPushButton#timerChipDone {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 6px;
    color: #e6edf3; font-size: 12px; font-weight: 600; padding: 3px 8px;
}
QPushButton#timerChip:hover { background-color: #21262d; }
QPushButton#timerChipPaused {
    background: transparent; border-style: dashed; color: #8b949e;
}
QPushButton#timerChipPaused:hover { background-color: #161b22; }
QPushButton#timerChipDone {
    background-color: #da3633; border-color: #f85149; color: #ffffff;
}
QPushButton#timerChipDone:hover { background-color: #f85149; }
QPushButton#relayJoinApproveButton {
    background-color: #da3633; color: #ffffff; border: none;
    border-radius: 6px; padding: 4px 10px; font-size: 12px; font-weight: 700;
}
QPushButton#relayJoinApproveButton:hover { background-color: #f85149; }
QPushButton#runnerSignInBadge {
    background-color: #da3633; color: #ffffff; border: none;
    border-radius: 6px; padding: 4px 10px; font-size: 12px; font-weight: 700;
}
QPushButton#runnerSignInBadge:hover { background-color: #f85149; }
QPushButton#runnerSignInBadge[severity="expiring"] {
    background-color: #9e6a03; color: #ffffff;
}
QPushButton#runnerSignInBadge[severity="expiring"]:hover {
    background-color: #bb8009;
}
QPushButton#backgroundRunnerBadge {
    background: transparent; border: 1px solid #30363d; border-radius: 6px;
    color: #8b949e; font-size: 12px; padding: 0 8px;
}
QPushButton#backgroundRunnerBadge:hover { background-color: #161b22; color: #e6edf3; }
QPushButton#backgroundRunnerBadge[severity="error"] { border-color: #da3633; }

#agentsNavBadge {
    color: #656d76; font-size: 13px; font-weight: 600;
}

#topNavBar { background: transparent; }
#navDivider { background-color: #21262d; border: none; }
QPushButton#topNavButton {
    background: transparent; border: 1px solid transparent; border-radius: 6px;
    color: #8b949e; font-size: 13px; font-weight: 600; padding: 5px 12px;
}
QPushButton#topNavButton:hover {
    background-color: #161b22; color: #e6edf3;
}
QPushButton#topNavButton:checked {
    background-color: #21262d; color: #e6edf3; border-color: #30363d;
}
QPushButton#floatingLogButton {
    background-color: #21262d; border: 1px solid #30363d; border-radius: 6px;
    color: #c9d1d9; font-size: 12px; font-weight: 600; padding: 4px 10px;
}
QPushButton#floatingLogButton:hover { background-color: #30363d; color: #e6edf3; }
QPushButton#windowChromeButton, QPushButton#windowChromeCloseButton,
QPushButton#packageActionsButton {
    background: transparent; border: 1px solid transparent; border-radius: 6px;
    color: #8b949e; padding: 0;
}
QPushButton#windowChromeButton:hover, QPushButton#packageActionsButton:hover {
    background-color: #161b22; color: #e6edf3; border-color: #30363d;
}
QPushButton#windowChromeCloseButton:hover {
    background-color: #da3633; color: #ffffff; border-color: #da3633;
}

QPushButton#relayOpenButton, QPushButton#navHistoryButton {
    background: transparent; border: none; border-radius: 8px; color: #8b949e;
}
QPushButton#nodeMenuButton, QPushButton#repoMenuButton {
    background: transparent; border: 1px solid #30363d; border-radius: 8px;
    color: #e6edf3; font-size: 15px; font-weight: 700; padding: 5px 12px;
}

QPushButton#projectMenuButton {
    background: transparent; border: none; border-radius: 8px; padding: 5px 4px;
}
QPushButton#relayMenuButton {
    background: transparent; border: none; border-radius: 8px;
    color: #e6edf3; font-size: 15px; font-weight: 700; padding: 5px 4px;
}
QPushButton#relayOpenButton:hover, QPushButton#navHistoryButton:hover,
QPushButton#projectMenuButton:hover,
QPushButton#relayMenuButton:hover,
QPushButton#nodeMenuButton:hover,
QPushButton#repoMenuButton:hover {
    background-color: #161b22; color: #e6edf3;
}
#nodeSwitchProgress { background: transparent; border: none; }
#nodeSwitchProgress::chunk { background-color: #58a6ff; border-radius: 1px; }
#navCaption { background: transparent; color: #8b949e; font-size: 13px; font-weight: 600; }

QPushButton#agentsMagicButton {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
        stop:0 #7c3aed, stop:0.5 #a855f7, stop:1 #22d3ee);
    border: 1px solid rgba(216,180,254,0.55); border-radius: 11px;
    color: #ffffff; font-size: 13px; font-weight: 700; padding: 5px 14px;
}
QPushButton#agentsMagicButton:hover {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
        stop:0 #8b5cf6, stop:0.5 #c084fc, stop:1 #67e8f9);
    border-color: #e9d5ff;
}
QPushButton#agentsMagicButton:checked {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
        stop:0 #6d28d9, stop:0.5 #9333ea, stop:1 #0891b2);
    border-color: #ffffff;
}

/* The agent activity window's toggles: lit while on, dimmed while off. */
QPushButton[activityToggle="true"] { color: #8b949e; }
QPushButton[activityToggle="true"]:checked {
    background-color: #12261a; border-color: #2ea043; color: #e6edf3;
}
QPushButton[activityToggle="true"]:checked:hover { background-color: #173420; }

#navSolanaBalance {
    background: transparent; border: none; color: #6e7681; padding: 0;
}
#agentPromptImage {
    background: rgba(48,54,61,0.35); border: 1px solid #30363d;
    border-radius: 6px; padding: 2px;
}
#agentPromptImage:hover { border-color: #58a6ff; }
QPushButton#pingPromptAction {
    background: rgba(88,166,255,0.12); border: 1px solid rgba(88,166,255,0.42);
    border-radius: 6px; color: #58a6ff; font-size: 11px; font-weight: 600; padding: 2px 6px;
}
QPushButton#pingPromptAction:hover { background: rgba(88,166,255,0.22); color: #79c0ff; }
#pageNoticeArea { background: transparent; }
#pageNoticeArea #inlineNotice {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 8px;
}
#pageNoticeArea #inlineNotice QLabel { background: transparent; color: #c9d1d9; font-size: 12px; }
#pageNoticeArea #inlineNotice[tone="danger"] { border-color: #f85149; }
#pageNoticeArea #inlineNotice[tone="danger"] QLabel { color: #ff7b72; }
#pageNoticeArea #inlineNotice[tone="accent"] { border-color: #388bfd; }
#pageNoticeArea #inlineNotice[tone="accent"] QLabel { color: #79c0ff; }
#pageNoticeArea #inlineNotice QPushButton#ghostButton {
    font-size: 11px; padding: 0 6px; min-height: 16px; max-height: 20px;
}
QPushButton#notificationButton, QPushButton#notificationButtonAlert {
    background: transparent; border: 1px solid #30363d; border-radius: 6px;
    padding: 2px 6px; font-size: 13px; color: #8b949e;
}
QPushButton#notificationButton:hover {
    background-color: #161b22; color: #e6edf3;
}
QPushButton#notificationButtonAlert {
    color: #d29922; border-color: #9e6a03; font-weight: 800;
}
QPushButton#notificationButtonAlert:hover {
    background-color: #1c1908; color: #f0b72f;
}

#homeCard {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 12px;
}
#homeCard QLabel { background: transparent; }
#homeTitle { font-size: 24px; font-weight: 800; }
#homeName { font-size: 18px; font-weight: 700; }
#homeStat { color: #8b949e; font-size: 14px; }
#homeScoreBoard {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 12px;
}
#homeScoreValue {
    color: #e3b341; font-size: 18px; font-weight: 800; background: transparent;
}
#homeGraph {
    background-color: #010409; border: 1px solid #30363d; border-radius: 8px;
    color: #58a6ff; font-family: monospace; font-size: 12px; padding: 10px;
}
#homeCard QListWidget {
    background: #0d1117; border: 1px solid #30363d; border-radius: 6px; padding: 4px;
}
#homeCard QListWidget::item { color: #c9d1d9; padding: 8px 7px; margin: 2px; }
QPushButton#primaryButton:disabled {
    background-color: #21262d; color: #6e7681; border-color: #30363d;
}

QPushButton#dangerButton {
    background: transparent; border: 1px solid #da3633; border-radius: 6px;
    color: #f85149; font-weight: 600;
}
QPushButton#dangerButton:hover { background-color: #da3633; color: #ffffff; }

QPushButton#successButton {
    background: transparent; border: 1px solid #2ea043; border-radius: 6px;
    color: #3fb950; font-weight: 600;
}
QPushButton#successButton:hover { background-color: #2ea043; color: #ffffff; }

#sidebar { background-color: #0d1117; border-right: 1px solid #30363d; }
#sidebar QLabel { background: transparent; }
#workspaceName {
    font-size: 17px; font-weight: 800; padding: 2px 0;
}
#statusLine { color: #8b949e; font-size: 11px; }
#suggestedAppStatusFound, #suggestedAppStatusMissing,
#suggestedAppStatusInProgress, #suggestedAppStatusProblem {
    font-size: 12px; font-weight: 700; padding: 5px 7px; border-radius: 5px;
}
#suggestedAppStatusFound {
    color: #3fb950; background: #0f2a1a; border: 1px solid #2ea043;
}
#suggestedAppStatusMissing, #suggestedAppStatusProblem {
    color: #ff7b72; background: #2d1215; border: 1px solid #da3633;
}
#suggestedAppStatusInProgress {
    color: #79c0ff; background: #0c2d4a; border: 1px solid #1f6feb;
}
#restartCountdown { color: #8b949e; font-size: 11px; background: transparent; }
#versionLabel { color: #6e7681; font-size: 11px; background: transparent; }
#sectionLabel {
    color: #8b949e; font-size: 11px; font-weight: 700;
    letter-spacing: 1px; padding-top: 8px;
}
#sidebar QListWidget {
    background: transparent; border: none; padding: 0;
}
#sidebar QListWidget::item {
    color: #c9d1d9; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QListWidget::item:hover {
    background-color: #161b22; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QListWidget::item:selected {
    background-color: #1f6feb; color: #ffffff; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QTreeWidget {
    background: transparent; border: none; padding: 0; outline: none;
}
#sidebar QTreeWidget::item {
    color: #c9d1d9; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QTreeWidget::item:hover {
    background-color: #161b22; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QTreeWidget::item:selected {
    background-color: #1f6feb; color: #ffffff; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QTreeWidget::branch { background: transparent; border-image: none; image: none; }

#notesList {
    background-color: transparent;
    border: none;
    padding: 0;
}
#notesList::item {
    color: #c9d1d9; border-radius: 6px; padding: 6px 8px; margin: 1px 0;
}
#notesList::item:hover {
    background-color: #161b22; border-radius: 6px; padding: 6px 8px; margin: 1px 0;
}
#notesList::item:selected {
    background-color: #238636; color: #ffffff; border-radius: 6px; padding: 6px 8px; margin: 1px 0;
}
QWidget#memberRow { background: transparent; }
QPushButton#memberNameButton {
    background: transparent; border: none; color: #c9d1d9;
    padding: 4px 6px; text-align: left; font-weight: 500;
}
QPushButton#memberNameButton:hover {
    background-color: #161b22; color: #e6edf3; border-radius: 6px;
}
QPushButton#memberDeleteButton {
    background: transparent; border: 1px solid #30363d; border-radius: 5px;
    color: #f85149; padding: 2px 6px; font-size: 11px; font-weight: 700;
}
QPushButton#memberDeleteButton:hover {
    background-color: #da3633; border-color: #da3633; color: #ffffff;
}
#issueQuickAdd {
    background-color: #0d1117; border: 1px solid #30363d;
    border-radius: 6px; padding: 4px 10px; font-size: 13px;
}
#issueQuickAdd:focus { border-color: #58a6ff; }
#promptWrapper {
    background-color: #0d1117; border: 1px solid rgba(57,211,83,0.55); border-radius: 6px;
}
#promptWrapper:focus-within { border-color: #39d353; }

#promptDragHandle { background: transparent; border: none; }
QPushButton#promptResetButton, QPushButton#promptChannelButton,
QPushButton#promptClearButton, QPushButton#promptCloudButton {
    background: transparent; border: none; padding: 0;
}
QPushButton#promptWorktreeCheck, QPushButton#promptVibeCheck {
    background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 0;
}
QPushButton#promptCloudButton:checked { background: rgba(88,166,255,0.22); border: 1px solid #58a6ff; border-radius: 4px; }
QPushButton#promptWorktreeCheck:checked { background: rgba(88,166,255,0.10); border-color: #58a6ff; }
QPushButton#promptVibeCheck:checked { background: rgba(163,113,247,0.16); border-color: #a371f7; }
#promptDetachWindow, #promptOverlayHost { background: transparent; }
#footerLogPanel {
    background-color: #0d1117;
    border: 1px solid rgba(57,211,83,0.55);
    border-radius: 6px;
}
#agentsActionBar {
    background: transparent;
    border-bottom: 1px solid #30363d;
}
#agentsActionBar QLabel { background: transparent; }
#promptWrapper #issueQuickAdd {
    background: transparent; border: 1px solid rgba(139,148,158,0.3); border-radius: 4px;
}
#promptWrapper #issueQuickAdd:focus { border: 1px solid rgba(139,148,158,0.3); }
QPushButton#quickAddSendIcon {
    background: transparent; border: none; color: #3fb950;
    padding: 4px; border-radius: 4px;
}
QPushButton#quickAddSendIcon:hover { color: #56d364; background: rgba(63,185,80,0.15); }

QPushButton#quickAddSendIcon[enterTarget="true"] {
    border: 1px solid #3fb950; background: rgba(63,185,80,0.08);
}

QPushButton#quickAddGenieButton {
    background: rgba(168,85,247,0.14); border: 1px solid rgba(192,132,252,0.55);
    color: #c084fc; padding: 4px; border-radius: 4px;
    font-size: 12px; font-weight: 700;
}
QPushButton#quickAddGenieButton:hover {
    background: rgba(168,85,247,0.28); border-color: #d8b4fe; color: #e9d5ff;
}
QPushButton#quickAddGenieButton[enterTarget="true"] {
    background: rgba(168,85,247,0.32); border: 1px solid #c084fc; color: #f3e8ff;
}
#issueSearch {
    background-color: #0d1117; border: 1px solid #30363d;
    border-radius: 6px; padding: 6px 10px;
}
#issueSearch:focus { border-color: #58a6ff; }
#globalSearch {
    background-color: #010409; border: 1px solid #30363d;
    border-radius: 6px; padding: 4px 8px; color: #e6edf3;
}
#globalSearch:focus { border-color: #58a6ff; }
#globalSearchPopup {
    background-color: #161b22; border: 1px solid #30363d;
    border-radius: 8px; padding: 4px; outline: none;
}
#globalSearchPopup::item { color: #c9d1d9; padding: 6px 8px; border-radius: 6px; }
#globalSearchPopup::item:selected { background-color: #1f6feb; color: #ffffff; }
#searchEmptyNotice { color: #8b949e; background: transparent; }
#searchResultsTitle { font-size: 18px; color: #e6edf3; }
#searchResultsTree {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 8px;
    padding: 4px; outline: none;
}
#searchResultsTree::item { padding: 4px 6px; color: #c9d1d9; }
#searchResultsTree::item:selected { background-color: #1f6feb; color: #ffffff; }
#saveSearchPrompt {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 8px;
}
#saveSearchPromptLabel { background: transparent; color: #e6edf3; }
#savedSearchPanel { background: transparent; }
#savedSearchList { background: transparent; border: none; outline: none; }
#savedSearchList::item { border-bottom: 1px solid #21262d; border-radius: 6px; }
#savedSearchList::item:hover { background-color: #161b22; }
#savedSearchList::item:selected { background-color: #1c2d41; }
#savedSearchRow, #savedSearchRow QLabel { background: transparent; }
#savedSearchQuery { color: #e6edf3; font-weight: 600; }
#savedSearchStats { color: #8b949e; font-size: 12px; }
#savedSearchNewBadge {
    background-color: #e3b341; color: #0d1117; border-radius: 8px;
    padding: 0px 6px; font-size: 11px; font-weight: 700;
}
#savedSearchSecurityBadge {
    background-color: #21262d; color: #f0b72f; border: 1px solid #9e6a03;
    border-radius: 8px; padding: 0px 6px; font-size: 11px; font-weight: 700;
}
#savedSearchSecurityBadge[alarming="true"] {
    background-color: #da3633; color: #ffffff; border-color: #f85149;
}
QCheckBox#savedSearchSecurityCheck { color: #8b949e; font-size: 12px; background: transparent; }
#securityAlarmBanner {
    background-color: #490202;
    border-bottom: 2px solid #f85149;
}
#securityAlarmBannerIcon { background: transparent; }
#securityAlarmBannerLabel {
    color: #ffdcd7; font-size: 13px; font-weight: 600; background: transparent;
}
QToolButton#savedSearchAlertToggle, QToolButton#savedSearchRemove {
    background: transparent; border: none; border-radius: 4px; padding: 3px;
}
QToolButton#savedSearchAlertToggle:hover, QToolButton#savedSearchRemove:hover {
    background-color: #21262d;
}
#issueTable {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 6px;
    gridline-color: #21262d;
}
QTableWidget#issueTable[repositoryGrid="true"] { gridline-color: #30363d; }
#issueTable::item { padding: 4px 8px; color: #c9d1d9; }

#issueTable::item:hover { padding: 4px 8px; }
#issueTable::item:selected { background-color: #1f6feb; color: #ffffff; padding: 4px 8px; }
#issueTable QHeaderView::section {
    background-color: #161b22; color: #8b949e; padding: 6px 8px;
    border: none; border-bottom: 1px solid #30363d;
    border-right: 2px solid #010409; font-weight: 700;
}

#issueBoardScroll { border: none; background: transparent; }
#issueBoardColumn {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 8px;
}
#issueBoardHeader {
    color: #c9d1d9; font-weight: 700; padding: 2px 2px 4px 2px;
}
#issueBoardList {
    background-color: transparent; border: none;
}
#issueBoardList::item {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 6px;
    color: #c9d1d9; padding: 8px 8px; margin: 3px 1px;
}
#issueBoardList::item:selected { border-color: #1f6feb; color: #c9d1d9; }

#chatHeader {
    background-color: #0d1117;
    border-bottom: 1px solid #30363d;
}
#chatHeader QLabel { background: transparent; }
#channelTitle { font-size: 16px; font-weight: 700; }
#agentPromptTitle {
    font-size: 16px; font-weight: 700;
}
#agentPromptTitleBlock {
    padding-bottom: 8px; border-bottom: 1px solid #30363d;
}
#encryptionLabel { color: #8b949e; font-size: 12px; }

#nodeProfilePanel { background: transparent; }
#nodeProfileContent { background-color: #0d1117; }
#profileBanner { border-radius: 16px; }
#profileName { font-size: 17px; font-weight: 800; }
#profileUserCard {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 10px;
}
#profileUserAvatar { border-radius: 12px; }
#profileUserName { color: #e6edf3; font-size: 16px; font-weight: 800; }
#profileNodeList {
    background-color: transparent; border: none; color: #c9d1d9;
    outline: none; font-weight: 600;
}
#profileNodeList::item {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 8px;
    padding: 5px 8px; margin: 2px 0;
}
QPushButton#profileActionButton {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 10px;
    color: #c9d1d9;
}
QPushButton#profileActionButton:hover {
    background-color: #1f2937; border-color: #58a6ff; color: #e6edf3;
}
QPushButton#profileActionButton:pressed { background-color: #0d1117; }
#statTile {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 9px;
    padding: 6px 4px; color: #e6edf3;
}
#profileCard {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 9px;
    padding: 7px 10px; color: #c9d1d9;
}
#profileMono {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 8px;
    padding: 6px 9px; color: #c9d1d9; font-family: monospace;
}
#profileQr {
    background-color: #ffffff; border-radius: 10px; padding: 8px;
}

#firewallBanner {
    background-color: #341a00;
    border-bottom: 1px solid #9e6a03;
}
#firewallBannerLabel {
    color: #f0b72f; font-size: 13px; background: transparent;
}
#meshDebugBar {
    background-color: #161b22;
    border-bottom: 1px solid #30363d;
}
#meshDebugLabel { color: #8b949e; font-size: 11px; background: transparent; }
#meshDebugCopy {
    color: #8b949e; font-size: 11px; background: transparent;
    border: 1px solid #30363d; border-radius: 4px; padding: 0 6px;
}
#meshDebugCopy:hover { color: #e6edf3; border-color: #8b949e; }

#chatUnreadBanner {
    background-color: #0d2136;
    border-bottom: 1px solid #1f6feb;
}
#chatUnreadBanner QLabel { color: #58a6ff; font-size: 13px; font-weight: 600; background: transparent; }
#chatUnreadBannerButton {
    background: transparent; border: none; color: #58a6ff; font-weight: 700;
    padding: 4px 10px; border-radius: 6px;
}
#chatUnreadBannerButton:hover { background-color: #1f6feb; color: #ffffff; }
#chatSidebarScroll, #chatSidebarRooms { background: transparent; border: none; }
#chatWorkspaceHeader { border-bottom: 1px solid #30363d; }
#chatWorkspaceMenu {
    background: transparent; border: none; border-radius: 6px; color: #e6edf3;
    font-size: 15px; font-weight: 800; text-align: left; padding: 4px 8px;
}
#chatWorkspaceMenu:hover { background-color: #21262d; }
#chatUserPanel { border-top: 1px solid #30363d; }
#chatMembersPanel { border-left: 1px solid #30363d; }
#chatMembersHeading { color: #8b949e; font-size: 11px; font-weight: 600; padding: 0px 16px 4px 16px; }
#chatMembersGroupHeading { color: #8b949e; font-size: 12px; font-weight: 600; padding: 16px 8px 4px 8px; }
#chatMemberCard:hover { background-color: #21262d; border-radius: 6px; }
#chatDayDividerRule, #channelTopicDivider { background-color: #30363d; border: none; }
#chatDayDividerLabel { color: #8b949e; font-size: 12px; font-weight: 600; }
#channelHash { color: #8b949e; font-size: 20px; }
#composerAttachButton { background: transparent; border: 1px solid #30363d; border-radius: 12px; }
#composerAttachButton:hover { background-color: #21262d; }
#chatWorkspaceUser { color: #8b949e; font-size: 12px; }
#chatComposeButton { background: transparent; border: 1px solid #30363d; border-radius: 17px; }
#chatComposeButton:hover { background-color: #21262d; }
#chatNavButton, #chatSectionToggle {
    background: transparent; border: none; color: #8b949e; text-align: left;
    padding: 4px 14px; font-size: 13px;
}
#chatNavButton:hover, #chatSectionToggle:hover { color: #e6edf3; background-color: #21262d; }
#chatNavButton[unread="true"] { color: #e6edf3; font-weight: 700; }
#chatNavBadge {
    background-color: #cf222e; color: #ffffff; border-radius: 9px; font-size: 11px;
    font-weight: 700; padding: 0px 6px; min-height: 18px; max-height: 18px;
}
#chatSidebarDivider { background-color: #30363d; border: none; }
#chatSectionAddButton { background: transparent; border: none; border-radius: 6px; }
#chatSectionAddButton:hover { background-color: #21262d; }
#channelTopic { color: #8b949e; font-size: 12px; }
#chatHeaderMeta, #chatHeaderIconButton {
    background: transparent; border: none; border-radius: 6px; color: #8b949e; padding: 0px 4px;
}
#chatHeaderMeta:hover, #chatHeaderIconButton:hover { background-color: #21262d; color: #e6edf3; }
#chatHeaderIconButton:checked { background-color: #21262d; color: #e6edf3; }
#composerToolButton {
    background: transparent; border: none; border-radius: 4px; color: #8b949e; font-size: 14px; padding: 0px;
}
#composerToolButton:hover { background-color: #21262d; color: #e6edf3; }
#composerSendButton { background-color: #238636; border: none; border-radius: 4px; }
#solanaBanner {
    background-color: #12261a;
    border-bottom: 1px solid #2ea043;
}
#solanaBannerLabel {
    color: #56d364; font-size: 13px; font-weight: 600; background: transparent;
}
#walletVerifyBanner {
    background-color: #2b210a;
    border-bottom: 1px solid #bb8009;
}
#walletVerifyBanner QLabel { background: transparent; }
#walletVerifyTitle { color: #e3b341; font-size: 15px; font-weight: 800; }
#walletVerifyBody { color: #d8c08a; font-size: 13px; }
#messageView, #messageContainer {
    background-color: #0d1117; border: none;
}
#typingLabel {
    background-color: #0d1117; color: #8b949e; font-size: 12px;
    padding: 0 18px;
}
#messageText { color: #e6edf3; }
#fileChip {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 6px;
}
#reactionChip {
    background-color: #161b22; border: 1px solid #21262d; border-radius: 18px;
    padding: 2px 9px; font-size: 12px; color: #e6edf3;
}
#reactionChip:hover { border-color: #58a6ff; background-color: #1c2433; }
#reactionAdd {
    background: transparent; border: 1px solid transparent; border-radius: 12px;
    padding: 2px 7px;
}
#reactionAdd:hover { border-color: #30363d; background-color: #161b22; }
#reactionPicker {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 10px;
}

#agentMetaPopup {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 10px;
}
#reactionPickerButton { background: transparent; border: none; border-radius: 8px; }
#reactionPickerButton:hover { background-color: #21262d; }
#messageAction {
    background: transparent; border: none; border-radius: 6px;
    color: #8b949e; font-size: 11px; padding: 2px 7px;
}
#messageAction:hover { color: #e6edf3; }
#messageAction::menu-indicator { image: none; width: 0; }
#iconButton {
    background: transparent; border: none; font-size: 18px; padding: 2px 6px;
}
#iconButton:hover { background-color: #21262d; border-radius: 6px; }

#settingsTitle { font-size: 20px; font-weight: 800; }
#avatarPreview {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 12px;
    color: #6e7681; font-size: 11px;
}
#emailVerifiedBadge {
    background-color: #0f2a1a; border: 1px solid #238636; border-radius: 10px;
    color: #3fb950; padding: 2px 8px; font-size: 11px; font-weight: 700;
}
#networkLog, #networkLogPopoutView {
    background-color: #010409; border: none;
    color: #8b949e; font-family: monospace; font-size: 12px;
}
#logTimelineChart { background-color: #010409; border-radius: 8px; }
#logTimelineSummary { color: #8b949e; font-size: 12px; }
#logRangeButton { padding: 4px 10px; }
#logRangeButton:checked {
    color: #ffffff; background-color: #1f6feb; border-color: #1f6feb;
}
#logDock { background: transparent; border: none; }
#logDock QLabel { background: transparent; }
#composerBar { background-color: #0d1117; border-top: 1px solid #30363d; }
#composerInputRow {
    background-color: #010409; border: 1px solid #30363d; border-radius: 10px;
}
#messageInput {
    background-color: transparent; border: none;
    padding: 9px 4px; font-size: 14px;
}

#scmMessageInput {
    background-color: transparent; border: 1px solid #30363d;
    border-radius: 4px; selection-background-color: #1f6feb;
}
#scmMessageInput:focus { border-color: #2da44e; }
#emojiPickerButton {
    border: none; background: transparent; font-size: 18px;
    padding: 3px; border-radius: 6px;
}
#emojiPickerButton:hover { background-color: #21262d; }

QScrollBar:vertical {
    background: transparent; width: 10px; margin: 0;
}
QScrollBar::handle:vertical {
    background: #30363d; border-radius: 5px; min-height: 30px;
}
QScrollBar::handle:vertical:hover { background: #484f58; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal {
    background: transparent; height: 10px; margin: 0;
}
QScrollBar::handle:horizontal {
    background: #30363d; border-radius: 5px; min-width: 30px;
}
QScrollBar::handle:horizontal:hover { background: #484f58; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }

QSplitter::handle { width: 1px; height: 1px; background: #30363d; }
QSplitter::handle:hover, QSplitter::handle:pressed { background: palette(highlight); }

QMessageBox, QInputDialog, QDialog { background-color: #161b22; }
QDialog QLabel { background: transparent; }

QPushButton[buttonSize="sm"] {
    min-height: 24px; max-height: 28px; padding: 3px 8px;
    font-size: 12px; border-radius: 6px;
}
QPushButton[buttonSize="xs"] {
    min-height: 18px; max-height: 22px; padding: 1px 7px;
    font-size: 11px; border-radius: 5px;
}
QPushButton#repoTab {
    background: transparent;
    border: none;
    border-bottom: 2px solid transparent;
    border-radius: 0;
    color: #8b949e;
    font-weight: 600;
    padding: 6px 10px;
}
QPushButton#repoTab:hover { color: #e6edf3; background: transparent; }
QPushButton#repoTab:checked {
    color: #e6edf3;
    border-bottom: 2px solid #2ea043;
    border-radius: 0;
    background: transparent;
}
QPushButton#repoAction {
    background-color: #21262d;
    border: 1px solid #30363d;
    border-radius: 6px;
    padding: 4px 10px;
    font-size: 12px;
    font-weight: 600;
    color: #e6edf3;
    min-height: 24px;
    max-height: 28px;
}
QPushButton#repoAction:hover { background-color: #30363d; }
QPushButton#repoAction::menu-indicator { width: 0; }
QPushButton#serverAddButton {
    background-color: #0d1117;
    border: 1px dashed #30363d;
    border-radius: 12px;
    color: #8b949e;
    font-size: 20px;
    font-weight: 700;
    padding: 0;
    text-align: center;
}
QPushButton#serverAddButton:hover { color: #2ea043; border-color: #2ea043; background-color: #0d1117; }
#sectionCard, #settingsCard {
    background-color: #161b22;
    border: 1px solid #30363d;
    border-radius: 8px;
}
#sectionCard QLabel, #settingsCard QLabel { background: transparent; }

QPushButton#shortcutCard {
    background-color: #161b22;
    border: 1px solid #30363d;
    border-radius: 8px;
    text-align: left;
}
QPushButton#shortcutCard:hover { border-color: #2ea043; background-color: #1b2129; }
#teammatesTopBar { background: transparent; }
QFrame#teammateDashboardSummary {
    background-color: #18142a; border: 1px solid #6e40c9;
    border-radius: 9px; margin-bottom: 8px;
}
QFrame#teammateDashboardSummary QLabel { background: transparent; }
#teammateDashboardTitle { color: #f0f6fc; font-size: 14px; font-weight: 750; }
#teammateDashboardSummaryText { color: #b1bac4; font-size: 10px; }
#teammateDashboardMainPoints { color: #e6edf3; font-size: 11px; font-weight: 600; }
QPushButton#teammateDashboardGenerate, QPushButton#teammateDashboardContinue {
    color: #ffffff; background-color: #8250df; border: 1px solid #a371f7;
    border-radius: 7px; padding: 4px 8px; font-size: 10px; font-weight: 700;
}
QPushButton#teammateDashboardGenerate:hover, QPushButton#teammateDashboardContinue:hover { background-color: #6e40c9; }
QPushButton#teammateDashboardContinue:disabled { color: #8b949e; background-color: #21262d; border-color: #30363d; }
QPushButton#teammateDashboardBriefingDetail,
QPushButton#teammateDashboardBriefingChoice,
QPushButton#teammateDashboardSourceLink {
    color: #c9d1d9; background-color: #0d1117; border: 1px solid #30363d;
    border-radius: 6px; padding: 3px 6px; font-size: 9px; font-weight: 650;
}
QPushButton#teammateDashboardBriefingDetail:hover,
QPushButton#teammateDashboardBriefingChoice:hover,
QPushButton#teammateDashboardSourceLink:hover { color: #f0f6fc; border-color: #8b949e; }
QFrame#teammateSkillUsageCard, QFrame#teammatePerformanceCard {
    background-color: #161b22; border: 1px solid #30363d;
    border-radius: 9px;
}
QFrame#teammateSkillUsageCard QLabel, QFrame#teammatePerformanceCard QLabel { background: transparent; }
#teammateBriefingSidebar, #teammateBriefingContributors,
#teammateBriefingContributorsScroll { background-color: #161b22; }
#teammateBriefingFollowUps { background: transparent; }
QFrame#teammateBriefingSidebar {
    border: 1px solid #30363d; border-radius: 9px;
}
QFrame#teammateBriefingHero {
    background-color: #18142a; border: 1px solid #6e40c9; border-radius: 10px;
}
QFrame#teammateBriefingSection, QFrame#teammateBriefingContribution {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 9px;
}
QFrame#teammateBriefingContribution[hasContribution="false"] { border-style: dashed; }
QFrame#teammateBriefingMetric {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 7px;
}
QFrame#teammateBriefingHero QLabel, QFrame#teammateBriefingSection QLabel,
QFrame#teammateBriefingContribution QLabel, QFrame#teammateBriefingMetric QLabel { background: transparent; }
#teammateBriefingEyebrow { color: #a371f7; font-size: 10px; font-weight: 750; }
#teammateBriefingTitle { color: #f0f6fc; font-size: 21px; font-weight: 750; }
#teammateBriefingIntro { color: #b1bac4; font-size: 11px; }
#teammateBriefingRailTitle, #teammateBriefingSectionHeading {
    color: #f0f6fc; font-size: 15px; font-weight: 750;
}
#teammateBriefingSectionTitle, #teammateBriefingContributionTitle {
    color: #f0f6fc; font-size: 13px; font-weight: 700;
}
#teammateBriefingMemberName, #teammateBriefingContributorName {
    color: #e6edf3; font-size: 12px; font-weight: 700;
}
#teammateBriefingContributorHandle { color: #f0f6fc; font-size: 11px; font-weight: 750; }
#teammateBriefingContributorRole { color: #8b949e; font-size: 10px; }
#teammateBriefingBody { color: #c9d1d9; font-size: 11px; }
#teammateBriefingMainPoints { color: #e6edf3; font-size: 15px; font-weight: 600; }
#teammateBriefingMuted, #teammateBriefingContributorMeta {
    color: #8b949e; font-size: 10px;
}
#teammateBriefingMetricValue { color: #f0f6fc; font-size: 16px; font-weight: 750; }
#teammateBriefingMetricLabel { color: #8b949e; font-size: 9px; font-weight: 650; }
#teammateBriefingStatus {
    color: #aff5b4; background-color: #17351f; border: 1px solid #246b35;
    border-radius: 7px; padding: 2px 6px; font-size: 9px; font-weight: 700;
}
#teammateBriefingStatus[kind="scheduled"] { color: #d2a8ff; background-color: #2b1d46; border-color: #5a32a3; }
#teammateBriefingStatus[kind="waiting"] { color: #f2cc60; background-color: #3d2f12; border-color: #6e541c; }
#teammateBriefingStatus[kind="idle"] { color: #8b949e; background-color: #21262d; border-color: #444c56; }
QPushButton#teammateBriefingGenerate {
    color: #ffffff; background-color: #8250df; border: 1px solid #a371f7;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 700;
}
QPushButton#teammateBriefingGenerate:hover { background-color: #6e40c9; }
#teammateBriefingChevron { color: #8b949e; }
QPushButton#teammateBriefingLink {
    color: #79c0ff; background: transparent; border: 0;
    padding: 1px 2px; font-size: 11px; font-weight: 700;
}
QPushButton#teammateBriefingLink:hover { color: #a5d6ff; background-color: #21262d; }
QPushButton#teammateBriefingRelatedLink {
    color: #c9d1d9; background-color: #21262d; border: 1px solid #30363d;
    border-radius: 7px; padding: 3px 7px; font-size: 10px; font-weight: 650;
}
QPushButton#teammateBriefingRelatedLink:hover { color: #a5d6ff; border-color: #58a6ff; }
QPushButton#teammateBriefingChoice {
    color: #c9d1d9; background-color: #0d1117; border: 1px solid #30363d;
    border-radius: 7px; padding: 6px 9px; font-size: 11px; font-weight: 650;
}
QPushButton#teammateBriefingChoice:hover { color: #f0f6fc; border-color: #8b949e; }
QPushButton#teammateBriefingChoice:checked { color: #ffffff; background-color: #2b1d46; border-color: #a371f7; }
QPushButton#teammateBriefingRunAgain {
    color: #ffffff; background-color: #8250df; border: 1px solid #a371f7;
    border-radius: 7px; padding: 6px 10px; font-size: 11px; font-weight: 700;
}
QPushButton#teammateBriefingRunAgain:hover { background-color: #6e40c9; }
QPushButton#teammateBriefingRunAgain:disabled { color: #8b949e; background-color: #21262d; border-color: #30363d; }
#teammateSkillUsageTitle, #teammatePerformanceTitle { color: #c9d1d9; font-size: 13px; font-weight: 650; }
#teammateSkillUsageTotal, #teammatePerformanceTotal { color: #f0f6fc; font-size: 25px; font-weight: 500; }
#teammatePerformanceGrade, #teammateTaskGradeLabel { color: #8b949e; font-size: 10px; font-weight: 650; }
QComboBox#teammateTaskGrade, QComboBox#agentTeammateTaskGrade { min-width: 162px; font-size: 11px; }
#teammateSkillUsageRange {
    color: #8b949e; background-color: #0d1117;
    border: 1px solid #30363d; border-radius: 7px;
    padding: 3px 7px; font-size: 10px;
}
QPushButton#teammateCard {
    background-color: #161b22; border: 1px solid #30363d;
    border-radius: 14px; text-align: left;
}
QPushButton#teammateCard[viewMode="list"] { border-radius: 9px; }
QPushButton#teammateCard[status="scheduled"] { border-color: #5a32a3; }
QPushButton#teammateCard[status="idle"] { border-color: #51441f; }
QPushButton#teammateCard[status="working"] { border-color: #246b35; }
QPushButton#teammateCard:hover { border-color: #3fb950; background-color: #1b2129; }
QPushButton#teammateCard QLabel { background: transparent; }
QWidget#teammateTableCell { border-right: 1px solid #30363d; }
QWidget#teammateTableCell[lastColumn="true"] { border-right: 0; }
QPushButton#teammateCard[viewMode="grid"] { border-color: transparent; background-color: #161b22; }
QPushButton#teammateCard[viewMode="grid"]:hover { border-color: #30363d; background-color: #1b2129; }
QProgressBar#teammateBoxProgress { border: 0; border-radius: 2px; background-color: #30363d; }
QProgressBar#teammateBoxProgress::chunk { border-radius: 2px; background-color: #2f81f7; }
#teammateName { font-size: 17px; font-weight: 750; }
#teammateHandle { color: #a371f7; font-size: 13px; font-weight: 700; }
#teammateHeroHandle, #teammateMentionPreview { color: #a371f7; font-size: 11px; }
#teammateSpecialty { font-size: 13px; font-weight: 650; color: #e6edf3; }
#teammateFocus, #teammateLastActive { color: #8b949e; font-size: 11px; }
#teammateModel, #teammateSchedule, #teammateActivity, #teammateCardMeta { font-size: 11px; color: #8b949e; }
#teammateDescription { color: #c9d1d9; font-size: 12px; }
#teammateOutput { color: #e6edf3; font-size: 12px; }
#teammateSkillsEmpty { color: #8b949e; font-size: 11px; font-style: italic; }
#teammateSkillAssignment {
    color: #c9d1d9; background-color: #0d1117; border: 1px solid #30363d;
    border-radius: 7px; padding: 7px 9px; font-size: 11px;
}
QPlainTextEdit#teammatePromptInput, QPlainTextEdit#teammateProfilePromptInput {
    color: #e6edf3; background-color: #0d1117; border: 1px solid #30363d;
    border-radius: 7px; padding: 4px 6px; font-size: 11px;
}
QPlainTextEdit#teammatePromptInput:focus, QPlainTextEdit#teammateProfilePromptInput:focus { border-color: #2ea043; }
QPlainTextEdit#teammatePromptInput:disabled, QPlainTextEdit#teammateProfilePromptInput:disabled {
    color: #6e7681; background-color: #161b22;
}
QPushButton#teammatePromptSend, QPushButton#teammateProfilePromptSend {
    color: #c9d1d9; background-color: #21262d; border: 1px solid #30363d;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 700;
}
QPushButton#teammatePromptSend:hover, QPushButton#teammateProfilePromptSend:hover {
    color: #ffffff; background-color: #2ea043; border-color: #2ea043;
}
QPushButton#teammatePromptSend:disabled, QPushButton#teammateProfilePromptSend:disabled {
    color: #6e7681; background-color: #161b22; border-color: #30363d;
}
#teammateEyebrow, #teammateFieldLabel {
    color: #8b949e; font-size: 10px; font-weight: 700;
    text-transform: uppercase;
}
QPushButton#teammateAgentLink, QPushButton#teammateCardLink {
    color: #79c0ff; background: transparent; border: 0;
    padding: 2px 3px; font-size: 11px; font-weight: 650;
}
QPushButton#teammateAgentLink:hover, QPushButton#teammateCardLink:hover {
    color: #a5d6ff; background-color: #21262d;
}
QPushButton#teammateRunButton {
    color: #aff5b4; background-color: #17351f; border: 1px solid #246b35;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 700;
}
QPushButton#teammateRunButton:hover {
    color: #ffffff; background-color: #1f6f34; border-color: #3fb950;
}
QPushButton#teammateStartButton {
    color: #ffffff; background-color: #238636; border: 1px solid #2ea043;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 750;
}
QPushButton#teammateStartButton:hover { background-color: #2ea043; border-color: #3fb950; }
QPushButton#teammateScheduleButton {
    color: #d2a8ff; background-color: #2b1d46; border: 1px solid #6e40c9;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 750;
}
QPushButton#teammateScheduleButton:hover { color: #ffffff; background-color: #6e40c9; }
QPushButton#teammateRunButton[running="true"], QPushButton#teammateStopButton {
    color: #ffb3ad; background-color: #3d1719; border: 1px solid #8e2f2f;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 700;
}
QPushButton#teammateRunButton[running="true"]:hover, QPushButton#teammateStopButton:hover {
    color: #ffffff; background-color: #6e2025; border-color: #f85149;
}
QLabel#teammatePill {
    color: #c9d1d9; background-color: #21262d; border: 1px solid #30363d;
    border-radius: 7px; padding: 2px 6px; font-size: 10px;
}
QLabel#teammatePill[kind="working"] { color: #aff5b4; background-color: #17351f; border-color: #246b35; }
QLabel#teammatePill[kind="scheduled"] { color: #d2a8ff; background-color: #2b1d46; border-color: #5a32a3; }
QLabel#teammatePill[kind="waiting"] { color: #f2cc60; background-color: #3d2f12; border-color: #6e541c; }
QLabel#teammatePill[kind="beta"] { color: #d2a8ff; background-color: #2b1d46; border-color: #8957e5; font-weight: 750; }
QLabel#teammatePill[kind="model"] { color: #79c0ff; background-color: #132b45; border-color: #1f6feb; }
QLabel#teammatePill[kind="model-empty"] { color: #8b949e; background-color: #21262d; border-color: #444c56; }
QLabel#teammatePill[kind="skill"] { color: #aff5b4; background-color: #17351f; border-color: transparent; }
QLabel#teammatePill[kind="cadence"] { color: #d2a8ff; background-color: #2b1d46; border-color: #5a32a3; }
QLabel#teammatePill[kind="watch"] { color: #f2cc60; background-color: #3d2f12; border-color: #6e541c; }
QLabel#teammatePill[kind="route"] { color: #79c0ff; background-color: #132b45; border-color: #1f6feb; }
QLabel#teammatePill[kind="duty"] { color: #c9d1d9; background-color: #21262d; border-color: #444c56; }
#teammateInvocation {
    background-color: #0d2117; border: 1px solid #246b35;
    border-radius: 8px;
}
#teammateInvocationEyebrow {
    color: #7ee787; font-size: 9px; font-weight: 750;
}
#teammateInvocationTitle { color: #e6edf3; font-size: 12px; font-weight: 700; }
#teammateInvocationCadence { color: #8b949e; font-size: 10px; }
QFrame#teammateInvocation[embedded="true"] {
    background: transparent; border: 0; border-top: 1px solid #6e40c9;
    border-radius: 0;
}
QFrame#teammateInvocation[embedded="true"] QLabel#teammateInvocationEyebrow { color: #d2a8ff; }
QFrame#teammateInvocation[embedded="true"] QLabel#teammateInvocationTitle { color: #f0f6fc; }
QFrame#teammateInvocation[embedded="true"][inline="true"] {
    border-top: 0; border-left: 1px solid #6e40c9;
}
#teammateDivider { color: #30363d; background-color: #30363d; max-height: 1px; }
#teammateModelsHost, #teammateSkillsHost, #teammateDutiesHost, #teammateProfileSkillsHost,
#teammateProfileDutiesHost, #teammateInvocationGraphic {
    background: transparent;
}
#teammateStatusBanner {
    background-color: #21262d; border: 1px solid #30363d; border-radius: 9px;
}
#teammateStatusBanner[kind="scheduled"] { background-color: #2b1d46; border-color: #6e40c9; }
#teammateStatusBanner[kind="idle"] { background-color: #2b2416; border-color: #7d5d1b; }
#teammateStatusBanner[kind="working"] { background-color: #17351f; border-color: #2f7d40; }
#teammateStatusBanner[kind="waiting"] { background-color: #3d2f12; border-color: #6e541c; }
#teammateStatusTitle { color: #f0f6fc; font-size: 14px; font-weight: 800; }
#teammateStatusDetail { color: #b1bac4; font-size: 10px; }
#teammateEmpty, #teammateHero, #teammateSection {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 10px;
}
#teammateHero[kind="scheduled"] { border-color: #6e40c9; }
#teammateHero[kind="working"] { border-color: #2f7d40; }
#teammateHero[kind="waiting"] { border-color: #6e541c; }
#teammateSidePanel {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 10px;
}
#teammateSideScroll { background: transparent; border: 0; }
#teammatePanelTitle, #teammateEditorPanelTitle {
    font-size: 15px; font-weight: 750; background: transparent;
}
QPushButton#teammatePanelClose {
    color: #8b949e; background: transparent; border: 0; border-radius: 6px;
    padding: 4px;
}
QPushButton#teammatePanelClose:hover { color: #f0f6fc; background-color: #21262d; }
#teammateEmptyTitle { font-size: 17px; font-weight: 700; background: transparent; }
#teammateHero QLabel, #teammateSection QLabel { background: transparent; }
#teammateHeroName { font-size: 25px; font-weight: 800; }
#teammateHeroRole { color: #c9d1d9; font-size: 14px; font-weight: 600; }
#teammateSectionTitle { font-size: 14px; font-weight: 700; }
#teammateWorkTitle { font-size: 13px; font-weight: 650; }
#teammateActivityDot { background-color: #3fb950; border-radius: 4px; }
#teammateActivityLine { color: #c9d1d9; font-size: 12px; }
#teammateHomeChatScroll, #teammateHomeChat, #teammateHomeWidgets, #teammateHomeInfo,
#teammateHomeAvatarHost { background: transparent; border: 0; }
#teammateHomeYou {
    background-color: #1f3a5f; border: 1px solid #2f5d93; border-radius: 10px;
}
#teammateHomeReply, #teammateHomeWidget, #genieSaid {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 9px;
}
#teammateHomeReply[status="running"] { border-color: #2f7d40; }
#teammateHomeReply[status="failed"] { border-color: #8e1519; }
#genieSaid[verdict="wrong"] { border-color: #8e1519; }
#genieSaid[verdict="accurate"] { border-color: #2f7d40; }
#teammateHomeProposal {
    background-color: #2b1d46; border: 1px dashed #8957e5; border-radius: 9px;
}
#teammateHomeYou QLabel, #teammateHomeReply QLabel, #teammateHomeWidget QLabel,
#teammateHomeProposal QLabel, #genieSaid QLabel { background: transparent; }
#teammateHomeYouText, #teammateHomeReplyText, #teammateHomeWidgetText { color: #e6edf3; font-size: 12px; }
#teammateHomeWhen { color: #8b949e; font-size: 10px; }
#teammateHomeChatEmpty, #teammateHomeWidgetsEmpty { color: #8b949e; font-size: 12px; padding: 18px 8px; }
#teammateHomeGoalValue { color: #c9d1d9; font-size: 12px; font-weight: 650; }
QPlainTextEdit#teammateHomeNotes, QPlainTextEdit#teammateHomeComposer {
    background-color: #0d1117; border: 1px solid #30363d; border-radius: 8px; padding: 4px;
}
#teammateLinkedShortcut { color: #79c0ff; font-size: 12px; }
#teammateInstructions, #teammateScheduleMonitor, #teammateAgentRoute,
#teammateCollaborationSummary { color: #c9d1d9; font-size: 12px; }
#teammateSchedulePreview {
    color: #aff5b4; background-color: #17351f; border: 1px solid #246b35;
    border-radius: 7px; padding: 7px 9px;
}
#teammateRelayPreview {
    color: #c9d1d9; background-color: #0d1117; border: 1px solid #30363d;
    border-radius: 7px; padding: 6px 8px; font-size: 11px;
}
QGroupBox#teammateFormSection {
    background-color: #161b22; border: 1px solid #30363d;
    border-radius: 9px; margin-top: 9px; font-weight: 700;
}
QGroupBox#teammateFormSection::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; }
QListWidget#teammateModelPicker { background-color: #0d1117; border: 1px solid #30363d; border-radius: 8px; }
QListWidget#teammateModelPicker::item { border: 1px solid transparent; border-radius: 8px; padding: 5px; }
QListWidget#teammateModelPicker::item:selected { background-color: #17351f; border-color: #2ea043; color: #e6edf3; }
#skillScopePill {
    color: #d2a8ff; background-color: #2b1d46; border: 1px solid #5a32a3;
    border-radius: 6px; padding: 2px 5px; font-size: 9px; font-weight: 750;
}
#skillCardFooter { color: #6e7681; font-size: 10px; }
#shortcutRunStats { color: #8b949e; font-size: 10px; }
#shortcutRunEta { color: #8b949e; font-size: 10px; }
QProgressBar#shortcutRunProgress { border: 0; border-radius: 2px; background-color: #30363d; }
QProgressBar#shortcutRunProgress::chunk { border-radius: 2px; background-color: #2f81f7; }
#shortcutRunPill {
    color: #7ee787; background-color: #12261a; border: 1px solid #238636;
    border-radius: 6px; padding: 2px 5px; font-size: 9px; font-weight: 750;
}
#shortcutKindPill {
    color: #79c0ff; background-color: #0d2440; border: 1px solid #1f6feb;
    border-radius: 6px; padding: 2px 5px; font-size: 9px; font-weight: 750;
}
#settingsTitle { font-size: 20px; font-weight: 800; }
#avatarPreview {
    background-color: #0d1117;
    border: 1px solid #30363d;
    border-radius: 12px;
    color: #6e7681;
    font-size: 11px;
}
#networkLog, #networkLogPopoutView {
    background-color: #010409;
    border: none;
    color: #8b949e;
    font-family: monospace;
    font-size: 12px;
}
#logTimelineChart { background-color: #010409; border-radius: 8px; }
#logTimelineSummary { color: #8b949e; font-size: 12px; }
#logRangeButton { padding: 4px 10px; }
#logRangeButton:checked {
    color: #ffffff; background-color: #1f6feb; border-color: #1f6feb;
}
#logDock { background: transparent; border: none; }
#issueQuickAdd {
    background-color: #0d1117;
    border: 1px dashed #30363d;
    border-radius: 8px;
    padding: 5px 8px;
    font-size: 12px;
    min-height: 38px;
    max-height: 38px;
}
#issueQuickAdd:focus { border-color: #58a6ff; }
#promptWrapper {
    background-color: #0d1117; border: 1px solid rgba(57,211,83,0.55); border-radius: 8px;
}
#promptWrapper:focus-within { border-color: #39d353; }

#promptDragHandle { background: transparent; border: none; }
QPushButton#promptResetButton, QPushButton#promptChannelButton,
QPushButton#promptClearButton, QPushButton#promptCloudButton {
    background: transparent; border: none; padding: 0;
}
QPushButton#promptWorktreeCheck, QPushButton#promptVibeCheck {
    background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 0;
}
QPushButton#promptCloudButton:checked { background: rgba(88,166,255,0.22); border: 1px solid #58a6ff; border-radius: 4px; }
QPushButton#promptWorktreeCheck:checked { background: rgba(88,166,255,0.10); border-color: #58a6ff; }
QPushButton#promptVibeCheck:checked { background: rgba(163,113,247,0.16); border-color: #a371f7; }
#promptDetachWindow, #promptOverlayHost { background: transparent; }
#footerLogPanel {
    background-color: #0d1117;
    border: 1px solid rgba(57,211,83,0.55);
    border-radius: 8px;
}
#promptWrapper #issueQuickAdd {
    background: transparent; border: 1px solid rgba(139,148,158,0.3); border-radius: 4px;
    min-height: 38px; max-height: 38px;
}
#promptWrapper #issueQuickAdd:focus { border: 1px solid rgba(139,148,158,0.3); }
QPushButton#quickAddSendIcon {
    background: transparent; border: none; color: #3fb950;
    padding: 4px; border-radius: 4px;
}
QPushButton#quickAddSendIcon:hover { color: #56d364; background: rgba(63,185,80,0.15); }

QPushButton#quickAddSendIcon[enterTarget="true"] {
    border: 1px solid #3fb950; background: rgba(63,185,80,0.08);
}

QPushButton#quickAddGenieButton {
    background: rgba(168,85,247,0.14); border: 1px solid rgba(192,132,252,0.55);
    color: #c084fc; padding: 4px; border-radius: 4px;
    font-size: 12px; font-weight: 700;
}
QPushButton#quickAddGenieButton:hover {
    background: rgba(168,85,247,0.28); border-color: #d8b4fe; color: #e9d5ff;
}
QPushButton#quickAddGenieButton[enterTarget="true"] {
    background: rgba(168,85,247,0.32); border: 1px solid #c084fc; color: #f3e8ff;
}

QCheckBox#quickAddCreateIssueCheck::indicator,
QCheckBox#quickAddAgentCheck::indicator {
    width: 16px; height: 16px; border-radius: 4px;
    border: 2px solid #30363d; background: #0d1117;
}
QCheckBox#quickAddCreateIssueCheck::indicator:checked,
QCheckBox#quickAddAgentCheck::indicator:checked {
    border-color: #2ea043; background: #2ea043;
    image: url(:/icons/octicons/check-white.svg);
}
#quickAddAgentBox {
    border: none; background: transparent;
    font-size: 12px;
}

QPushButton#quickAddSlashButton {
    background: #0d1117; border: 1px solid #30363d; border-radius: 5px;
    color: #8b949e; font-weight: 600; font-size: 12px; padding: 0;
}
QPushButton#quickAddSlashButton:hover { border-color: #58a6ff; color: #e6edf3; }

#slashActionsPopup {
    background: #161b22; border: 1px solid #30363d; border-radius: 8px;
}
QLineEdit#slashActionsFilter {
    background: transparent; border: none; border-bottom: 1px solid #30363d;
    border-radius: 0; padding: 10px 12px; color: #e6edf3; font-size: 13px;
}
#slashActionsScroll { background: transparent; border: none; }
#slashActionsScroll > QWidget > QWidget { background: transparent; }
QLabel#slashActionsHeader {
    color: #8b949e; font-size: 11px; font-weight: 600;
    padding: 8px 12px 4px 12px;
}
QFrame#slashActionRow { background: transparent; border-radius: 6px; }
QFrame#slashActionRow[slashSelected="true"] { background: #1f2937; }
QLabel#slashActionRowLabel { color: #e6edf3; font-size: 13px; background: transparent; }
QLabel#slashActionRowValue { color: #8b949e; font-size: 12px; background: transparent; }
QToolButton#slashEffortDot {
    background: transparent; border: 2px solid #6e7681; border-radius: 5px;
}
QToolButton#slashEffortDot:checked { background: #e6edf3; border-color: #e6edf3; }
QCheckBox#slashToggle::indicator {
    width: 28px; height: 16px; border-radius: 8px; border: none; background: #30363d;
}
QCheckBox#slashToggle::indicator:checked { background: #2ea043; }
#issuePageTitle {
    font-size: 26px;
    font-weight: 400;
    color: #e6edf3;
    background: transparent;
}
#issueDivider { background-color: #30363d; }
QPushButton#issueIconButton {
    background: transparent;
    border: none;
    border-radius: 6px;
    padding: 4px;
}
QPushButton#issueIconButton:hover { background-color: #21262d; }
QPushButton#inlineHelpButton {
    background: transparent;
    border: none;
    border-radius: 6px;
    padding: 4px;
}
QPushButton#inlineHelpButton:hover { background-color: #21262d; }
QPushButton#logFrequencyPromptButton {
    background: transparent;
    border: none;
    border-radius: 6px;
    padding: 2px;
}
QPushButton#logFrequencyPromptButton:hover { background-color: #21262d; }
#issuePageScroll {
    background: transparent;
    border: none;
}
/* Exact-class selector on purpose: plain container widgets go transparent,
   but QPushButton/QLabel keep their own rules. A bare "QWidget" here also
   matches subclasses and outranks QPushButton#primaryButton, which painted
   the thread's Comment/Merge buttons white-on-white. */
#issuePageScroll .QWidget { background: transparent; }
#issueAvatar {
    background-color: #21262d;
    border: 1px solid #30363d;
    border-radius: 18px;
    color: #e6edf3;
    font-size: 11px;
    font-weight: 800;
}
#issueCommentTitle {
    font-size: 16px;
    font-weight: 700;
    color: #e6edf3;
    background: transparent;
}
#issueCommentEditor {
    background-color: #0d1117;
    border: 1px solid #30363d;
    border-radius: 8px;
}
#issueCommentCard {
    background-color: #0d1117;
    border: 1px solid #2ea043;
    border-radius: 8px;
}
#issueCommentEditor QPlainTextEdit#markdownSource,
#issueCommentEditor QTextBrowser#markdownPreview {
    border: none;
    border-top-left-radius: 0;
    border-top-right-radius: 0;
}
#issueCommentEditor QPushButton#markdownTab {
    border-top-color: transparent;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
#issueTimelineRow, #issueTimelineIndent, #issueReactionsRow { background: transparent; }
#issueTimelineCard {
    background-color: #0d1117;
    border: 1px solid #2ea043;
    border-radius: 8px;
}
#issueTimelineCard QLabel { background: transparent; }
#issueTimelineHeader {
    background-color: #0f2a1a;
    border-bottom: 1px solid #238636;
    border-top-left-radius: 8px;
    border-top-right-radius: 8px;
}
#issueTimelineHeader QLabel {
    background: transparent;
    color: #c9d1d9;
}
#issueActivityAvatar {
    background-color: #21262d;
    border: 1px solid #30363d;
    border-radius: 12px;
    color: #e6edf3;
    font-size: 9px;
    font-weight: 800;
}
#issueActivityCard {
    background-color: #161b22;
    border: 1px solid #30363d;
    border-radius: 8px;
    padding: 7px 10px;
}
#issueLabelPill {
    background-color: #161b22;
    border: 1px solid;
    border-radius: 10px;
    padding: 2px 8px;
    font-size: 11px;
    font-weight: 600;
}
#issueDetailTabs::pane { border: 0; }
#issueAttachmentPreview {
    background-color: #010409;
    border: 1px solid #30363d;
    border-radius: 6px;
    padding: 6px;
}
QToolButton#issueActionButton, QPushButton#issueActionButton {
    background: transparent;
    border: none;
    border-radius: 6px;
    color: #c9d1d9;
    font-weight: 800;
    padding: 4px 8px;
}
QToolButton#issueActionButton:hover, QPushButton#issueActionButton:hover { background-color: rgba(255,255,255,24); }
QToolButton#issueActionButton::menu-indicator { image: none; width: 0; }
#issueChecklist, #issueChecklistRow, #issueChecklistMarkdown {
    background: transparent;
}
QLabel#issueChecklistLabel[completed="true"] { color: #8b949e; }
#issueDependencies { background: transparent; border-top: 1px solid #30363d; }
#issueSubIssues { background: transparent; border: 1px solid #30363d; border-radius: 6px; }
#issueParentRef { background: transparent; }
QLabel#issueParentCaption { color: #8b949e; font-size: 12px; padding: 4px 0; }
QPushButton#issueParentPill {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 12px;
    color: #c9d1d9; font-size: 12px; font-weight: 600; padding: 3px 10px;
}
QPushButton#issueParentPill:hover { background-color: #1f2630; border-color: #8b949e; }
QPushButton#issueParentPill[status="closed"], QPushButton#issueParentPill[status="unknown"] { color: #8b949e; }
QLabel#issueDependenciesHeading {
    color: #8b949e; font-size: 12px; font-weight: 700; padding-top: 8px;
}
QPushButton#issueDependencyRow {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 6px;
    text-align: left; padding: 0;
}
QPushButton#issueDependencyRow:hover { background-color: #1f2630; border-color: #8b949e; }
QLabel#issueDependencyNumber { color: #8b949e; font-size: 12px; font-weight: 600; }
QLabel#issueDependencyTitle { color: #c9d1d9; font-weight: 600; }
QLabel#issueDependencyTitle[status="closed"], QLabel#issueDependencyTitle[status="unknown"] { color: #8b949e; }
QLabel#issueDependencyStatus {
    border-radius: 9px; padding: 2px 8px; font-size: 11px; font-weight: 700;
    color: #ffffff;
}
QLabel#issueDependencyStatus[status="open"] { background-color: #1f883d; }
QLabel#issueDependencyStatus[status="closed"] { background-color: #8250df; }
QLabel#issueDependencyStatus[status="unknown"] {
    background-color: transparent; border: 1px solid #30363d; color: #8b949e;
}
QPushButton#issueChecklistDrag {
    background: transparent; border: none; border-radius: 6px; color: #6e7681;
    font-size: 18px; font-weight: 700; padding: 0;
}
QPushButton#issueChecklistDrag:hover, QPushButton#issueChecklistDrag:focus {
    background-color: #21262d; color: #c9d1d9;
}
QPushButton#issueChecklistConvert, QPushButton#issueChecklistAttached {
    background-color: #161b22; border: 1px solid #30363d; border-radius: 12px;
    color: #8b949e; font-size: 11px; font-weight: 650; padding: 4px 8px;
}
QPushButton#issueChecklistConvert:hover {
    background-color: #1f2630; border-color: #3fb950; color: #7ee787;
}
QPushButton#issueChecklistAttached {
    background-color: #171529; border-color: #6e40c9; color: #d2a8ff;
}
QPushButton#issueChecklistAttached:hover {
    background-color: #211b3a; border-color: #a371f7; color: #e2c5ff;
}
#issueSidebar { background: transparent; }
#issueSidebar QLabel { background: transparent; }
#issueSidebarHeading {
    color: #8b949e;
    font-size: 12px;
    font-weight: 700;
    background: transparent;
}
#issueSidebarDivider { background-color: #30363d; }
QPushButton#issueSidebarAction, QPushButton#issueDangerLink {
    background: transparent;
    border: none;
    color: #c9d1d9;
    font-weight: 500;
    padding: 6px 2px;
    text-align: left;
}
QPushButton#issueSidebarAction:hover { color: #e6edf3; }
QPushButton#issueDangerLink { color: #f85149; }
QPushButton#issueDangerLink:hover { color: #ff7b72; }
QMenu {
    background-color: #161b22;
    border: 1px solid #30363d;
    border-radius: 8px;
    padding: 6px;
    color: #e6edf3;
}
QMenu::item {
    padding: 7px 26px 7px 22px;
    border-radius: 6px;
}
QMenu::item:selected { background-color: #21262d; }
QListWidget#assigneePickerList, QListWidget#reporterPickerList {
    color: #e6edf3;
}
QListWidget#assigneePickerList::item, QListWidget#reporterPickerList::item {
    color: #e6edf3;
    padding: 3px 4px;
    border-radius: 6px;
}
QListWidget#assigneePickerList::item:hover, QListWidget#reporterPickerList::item:hover,
QListWidget#assigneePickerList::item:selected, QListWidget#reporterPickerList::item:selected {
    background-color: #21262d;
    color: #e6edf3;
}
QMenu::separator {
    height: 1px;
    background: #30363d;
    margin: 6px 0;
}
QLineEdit#issueSearch, QComboBox#issueControlSm {
    background-color: #0d1117;
    border: 1px solid #30363d;
    border-radius: 8px;
    padding: 4px 8px;
    font-size: 12px;
    min-height: 24px;
    max-height: 28px;
}
QLineEdit#issueSearch:focus, QComboBox#issueControlSm:focus { border-color: #58a6ff; }

QLineEdit#messageInput {
    background-color: #0d1117;
    border: 1px solid #484f58;
    border-radius: 8px;
    padding: 6px 10px;
}
QLineEdit#messageInput:focus { border-color: #58a6ff; }
QPlainTextEdit#issueComposerSm {
    background-color: #161b22;
    color: #e6edf3;
    border: 1px solid #484f58;
    border-radius: 8px;
    padding: 8px;
    selection-background-color: rgba(46, 160, 67, 46);
}
QPlainTextEdit#issueComposerSm:focus { border: 1px solid #58a6ff; }
#issueComposeSidebar { background: transparent; }
QPushButton#markdownTab {
    background-color: #161b22;
    border: 1px solid #30363d;
    border-bottom: none;
    border-radius: 0;
    color: #8b949e;
    padding: 8px 16px;
}
QPushButton#markdownTab:checked {
    background-color: #0d1117;
    color: #e6edf3;
}
QToolButton#markdownTool {
    background: transparent;
    border: none;
    border-radius: 4px;
    color: #8b949e;
    padding: 5px 7px;
    font-weight: 700;
}
QToolButton#markdownTool:hover {
    background-color: #21262d;
    color: #e6edf3;
}
QPlainTextEdit#markdownSource, QTextBrowser#markdownPreview {
    background-color: #0d1117;
    color: #e6edf3;
    border: 1px solid #30363d;
    border-radius: 6px;
    padding: 14px 16px;
    selection-background-color: #1f6feb;
}
QPlainTextEdit#markdownSource:focus { border-color: #58a6ff; }
#mentionPopup {
    background-color: #161b22;
    color: #e6edf3;
    border: 1px solid #30363d;
    border-radius: 6px;
    outline: 0;
}
#mentionPopup::item { padding: 4px 10px; }
#mentionPopup::item:selected { background-color: #1f6feb; color: #ffffff; }
#issueTable {
    background-color: #0d1117;
    alternate-background-color: #161b22;
    border: 1px solid #30363d;
    border-radius: 8px;
    gridline-color: #21262d;
    selection-background-color: #1f6feb;
    selection-color: #ffffff;
}
QTableWidget#issueTable[repositoryGrid="true"] { gridline-color: #30363d; }
#issueTable::item { padding: 6px 8px; color: #c9d1d9; }
#issueTable::item:hover { padding: 6px 8px; }
#issueTable::item:selected { background-color: #1f6feb; color: #ffffff; padding: 6px 8px; }
#issueTable QHeaderView::section {
    background-color: #161b22;
    color: #8b949e;
    padding: 6px 8px;
    border: none;
    border-bottom: 1px solid #30363d;
    border-right: 2px solid #010409;
    font-weight: 700;
}
#issueTable QTableCornerButton::section {
    background-color: #161b22;
    border: none;
    border-bottom: 1px solid #30363d;
}
#issueList {
    background-color: #0d1117; border: none; outline: 0;
    selection-background-color: transparent; gridline-color: transparent;
}
#issueList::item { padding: 0; border: none; }
#issueList::item:selected { background: transparent; color: #e6edf3; }

#organizationTasksTable {
    background-color: #0d1117;
    alternate-background-color: #0d1117;
    border: none;
    gridline-color: transparent;
    selection-background-color: transparent;
    selection-color: #ffffff;
    outline: 0;
}
#actionWorkflowList {
    background-color: #0d1117;
    alternate-background-color: #161b22;
    border: 1px solid #30363d;
    border-radius: 8px;
    outline: 0;
    padding: 4px;
}
#actionWorkflowList::item {
    border-radius: 6px;
    color: #c9d1d9;
    padding: 7px 10px;
}
#actionWorkflowList::item:hover {
    background-color: #161b22;
    color: #e6edf3;
}
#actionWorkflowList::item:selected {
    background-color: #238636;
    color: #ffffff;
}
#codeEditor {
    background-color: #0d1117;
    border: 1px solid #30363d;
    border-radius: 8px;
    color: #e6edf3;
    /* A QSS family list only resolves to a face that is installed; the generic
       alias at the end does not fall back through fontconfig, so name the
       usual Linux monospace faces too or the editor paints proportional. */
    font-family: "SF Mono", "Menlo", "Consolas", "DejaVu Sans Mono",
        "Liberation Mono", "Noto Sans Mono", monospace;
    font-size: 12px;
    selection-background-color: #264f78;
    selection-color: #ffffff;
}
#codeLineNumberArea {
    background-color: #0d1117;
    border-right: 1px solid #21262d;
}
#diffView {
    background-color: #0d1117;
    border: none;
    border-radius: 0;
    color: #e6edf3;
    font-family: monospace;
    font-size: 12px;
}
#readmeView {
    background: transparent;
    border: none;
    padding: 0;
    color: #e6edf3;
}

QPlainTextEdit#actionLog {
    background-color: #ffffff;
    border: 1px solid #d1d5db;
    border-radius: 8px;
    color: #1f2937;
    padding: 10px 12px;
    selection-background-color: #2563eb;
    selection-color: #ffffff;
}
QToolButton#agentStatusPill {
    background-color: transparent;
    border: 1px solid #d1d5db;
    border-radius: 24px;
    padding: 0px 10px 0px 3px;
    font-size: 11px;
    font-weight: 600;

    color: #e6edf3;
}
QToolButton#agentStatusPill:hover { background-color: #21262d; }
QToolButton#agentStatusPill[outcomeTone="success"] { border-color: #3fb950; }
QToolButton#agentStatusPill[outcomeTone="failure"] { border-color: #f85149; }
QToolButton#agentStatusPill[outcomeTone="pending"] { border-color: #e3742f; }
#agentNetPanel {
    background-color: #f9fafb;
    border: 1px solid #d1d5db;
    border-radius: 8px;
    padding: 8px 12px;
    color: #374151;
}

QAbstractItemView::item { border: 0px; }
QAbstractItemView::item:selected { background-color: palette(highlight); color: palette(highlighted-text); }
#coveSharedWithList { selection-background-color: #238636; selection-color: #ffffff; }
#issueTable { selection-background-color: #238636; selection-color: #ffffff; }
#issueTable::item:selected,
#overviewList::item:selected,
#fileTree::item:selected,
#commitsList::item:selected,
#coveSharedWithList::item:selected,
#sidebar QListWidget::item:selected,
#sidebar QTreeWidget::item:selected { background-color: #238636; color: #ffffff; }
QComboBox QAbstractItemView { selection-background-color: #238636; selection-color: #0d1117; }
QComboBox QAbstractItemView::item:selected { color: #0d1117; }

QPushButton#scrollJump {
    background-color: #161b22; color: #e6edf3;
    border: 1px solid #30363d; border-radius: 15px;
    font-size: 12px; font-weight: 700;
}
QPushButton#scrollJump:hover { background-color: #1f2630; }

/* Armed force deploy. A checked QPushButton is otherwise indistinguishable
   from an idle one under this sheet, and this toggle decides whether the next
   deployment may skip its change check, so it carries the danger accent while
   it is on. */
QPushButton#siteDeployForceButton:checked {
    background-color: #da3633; border-color: #da3633; color: #ffffff;
}
QPushButton#siteDeployForceButton:checked:hover { background-color: #f85149; }

)";

inline const char *kLightStyleSheet = R"(
* { outline: none; }
QWidget {
    background-color: #ffffff;
    color: #1f2328;
    /* No font-family here: a family in this rule beats every widget-level
       setFont(), which silently turned each monospace log, code and transcript
       view into the UI face. main.cpp picks the UI family once and sets it as
       the application font, so widgets that never call setFont() still inherit
       it, while a QFont::Monospace setFont() now wins. */
    font-size: 14px;
}
#appWindowFrame { background-color: #d0d7de; }
QToolTip {
    background-color: #ffffff; color: #1f2328;
    border: 1px solid #d0d7de; padding: 4px;
}

#setupCard {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 12px;
}
#appTitle { font-size: 26px; font-weight: 800; background: transparent; }
#appTitleAccent { color: #cf222e; }
#appSubtitle { color: #656d76; background: transparent; }
#setupCard QLabel { background: transparent; }
#onboardingCard {
    background-color: #ffffff;
    border: 1px solid #afb8c1;
    border-radius: 18px;
}
#onboardingCard QLabel, #onboardingWelcomeFeatures { background: transparent; }
#onboardingBrand {
    color: #1a7f37; font-size: 11px; font-weight: 850; letter-spacing: 2px;
}
#onboardingKicker {
    color: #0969da; font-size: 11px; font-weight: 800; letter-spacing: 1px;
}
#onboardingTitle {
    color: #1f2328; font-size: 27px; font-weight: 850;
}
#onboardingBody { color: #424a53; font-size: 14px; }
#onboardingFeatureCard {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 10px;
}
#onboardingFeatureCard QLabel { background: transparent; }
#onboardingFeatureBadge {
    background-color: #0969da; color: #ffffff;
    border-radius: 14px; font-size: 10px; font-weight: 850;
}
#onboardingFeatureCopy { color: #424a53; font-size: 12px; }
#onboardingDetails {
    color: #424a53; background-color: #f6f8fa;
    border: 1px solid #d0d7de; border-radius: 9px; padding: 11px;
}
#onboardingHint { color: #656d76; font-size: 12px; }
#onboardingProgressLabel { color: #656d76; font-size: 11px; font-weight: 700; }
QProgressBar#onboardingProgress {
    background-color: #d0d7de; border: none; border-radius: 2px;
}
QProgressBar#onboardingProgress::chunk {
    background-color: #1a7f37; border-radius: 2px;
}
QPushButton#onboardingCloseButton {
    background: transparent; border: none; color: #656d76;
    border-radius: 15px; padding: 0; font-size: 22px; font-weight: 400;
}
QPushButton#onboardingCloseButton:hover { background: #d0d7de; color: #1f2328; }
#onboardingTrackRow { background: transparent; }
QPushButton#onboardingTrackButton {
    background-color: #f6f8fa; color: #424a53; border: 1px solid #d0d7de;
    border-radius: 10px; padding: 11px 14px; text-align: left;
    font-size: 12px; font-weight: 600;
}
QPushButton#onboardingTrackButton:hover { border-color: #0969da; color: #1f2328; }
QPushButton#onboardingTrackButton:focus { border-color: #0969da; }
QPushButton#onboardingTrackButton:pressed { background-color: #eaeef2; }
#calendarAgentCard {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 10px;
}
#calendarAgentCard QLabel { background: transparent; }
#calendarAgentHeading {
    font-size: 16px; font-weight: 700; color: #1f2328;
}
#ledgerWalletCard {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 10px;
}
#ledgerWalletCard QLabel { background: transparent; }
#ledgerWalletLabel {
    color: #1a7f37; font-size: 10px; font-weight: 750; letter-spacing: 1px;
}
#ledgerWalletAge { color: #6e7681; font-size: 10px; }
#ledgerWalletAmount { font-size: 19px; font-weight: 750; color: #1f2328; }
#ledgerWalletUsd { color: #656d76; font-size: 12px; font-weight: 650; }
QPushButton#ledgerWalletRefresh {
    background: transparent; border: 1px solid #d0d7de; border-radius: 11px;
    padding: 0px;
}
QPushButton#ledgerWalletRefresh:hover { border-color: #0969da; }
#ledgerWalletAddress { color: #656d76; font-size: 11px; }
#ledgerWalletNote { color: #6e7681; font-size: 11px; }
#ledgerWalletWarning { color: #9a6700; font-size: 11px; font-weight: 650; }
#ledgerEntryForm {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 10px;
}
#ledgerEntryForm QLabel, #ledgerEntryForm QCheckBox { background: transparent; }
#ledgerPayoutHoldPanel {
    background-color: #fff8c5;
    border: 1px solid #d4a72c;
    border-radius: 10px;
}
#ledgerPayoutHoldPanel QLabel { background: transparent; }
#ledgerPayoutHoldTitle { color: #7d4e00; font-weight: 750; }
#ledgerPayoutHoldDetails { color: #3b2f16; font-size: 12px; }
QPushButton#ledgerPayoutUndoButton { color: #cf222e; border-color: #ff8182; }
QPushButton#ledgerPayoutUndoButton:hover {
    color: #ffffff; background-color: #cf222e; border-color: #cf222e;
}
#ledgerTotals { color: #656d76; font-size: 12px; font-weight: 650; }
QTextEdit#calendarAgentPrompt {
    background-color: #ffffff; color: #1f2328;
    border: 1px solid #d0d7de; border-radius: 7px;
    padding: 8px; selection-background-color: #0969da;
    selection-color: #ffffff;
}
QTextEdit#calendarAgentPrompt:focus { border-color: #0969da; }
#calendarDetailsScroll, #calendarDetailsScroll > QWidget,
#calendarDetailsScroll > QWidget > QWidget {
    background: transparent; border: none;
}
QRadioButton { background: transparent; spacing: 8px; padding: 4px 0; }
QRadioButton::indicator {
    width: 16px; height: 16px; border-radius: 9px;
    border: 2px solid #d0d7de; background: #ffffff;
}
QRadioButton::indicator:checked { border-color: #0969da; background: #0969da; }
QCheckBox { background: transparent; spacing: 8px; padding: 4px 0; }
QCheckBox::indicator {
    width: 16px; height: 16px; border-radius: 4px;
    border: 2px solid #d0d7de; background: #ffffff;
}
QCheckBox::indicator:checked { border-color: #0969da; background: #0969da; }
#modeHint { color: #656d76; font-size: 12px; padding-left: 26px; background: transparent; }

#relayHostEdit { color: #8c959f; }
#relayHostEdit:focus { color: #1f2328; }

#wizardStep { color: #1a7f37; font-size: 11px; font-weight: 700; letter-spacing: 1px; background: transparent; }
#wizardTitle { font-size: 20px; font-weight: 800; background: transparent; }

QLineEdit, QSpinBox, QComboBox {
    background-color: #ffffff;
    border: 1px solid #d0d7de;
    border-radius: 6px;
    padding: 8px 10px;

    min-height: 19px;
    selection-background-color: #0969da;
    selection-color: #ffffff;
}
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #0969da; }
QLineEdit:disabled, QSpinBox:disabled { color: #8c959f; border-color: #eaeef2; }
QSpinBox::up-button, QSpinBox::down-button { width: 0; }
QComboBox QAbstractItemView {
    background-color: #ffffff; border: 1px solid #d0d7de;
    selection-background-color: #0969da; selection-color: #1f2328; color: #1f2328;
}
QComboBox QAbstractItemView::item:selected { color: #1f2328; }
QComboBox#quickAddAgentSelector, QComboBox#quickAddModelSelector, QComboBox#quickAddAgentModelSelector, QComboBox#quickAddModeSelector, QComboBox#quickAddSpeedSelector {
    border: none;
    background-color: transparent;
    padding: 0px 4px 0px 8px;
}
QComboBox#quickAddAgentSelector:focus, QComboBox#quickAddModelSelector:focus, QComboBox#quickAddAgentModelSelector:focus, QComboBox#quickAddModeSelector:focus, QComboBox#quickAddSpeedSelector:focus {
    border: none;
    background-color: rgba(9, 105, 218, 0.08);
}
QComboBox#quickAddAgentSelector::drop-down, QComboBox#quickAddModelSelector::drop-down, QComboBox#quickAddAgentModelSelector::drop-down {
    border: none;
    width: 20px;
}
QComboBox#quickAddAgentSelector::down-arrow, QComboBox#quickAddModelSelector::down-arrow, QComboBox#quickAddAgentModelSelector::down-arrow {
    image: url(:/icons/octicons/chevron-down.svg);
    width: 16px;
    height: 16px;
}

QComboBox#quickAddModeSelector, QComboBox#quickAddSpeedSelector {
    padding: 0px 4px;
}
QComboBox#quickAddModeSelector::drop-down, QComboBox#quickAddSpeedSelector::drop-down {
    border: none; width: 0px;
}
QComboBox#quickAddModeSelector::down-arrow, QComboBox#quickAddSpeedSelector::down-arrow {
    image: none; width: 0px; height: 0px;
}

QPushButton {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 6px;
    padding: 8px 16px;
    font-weight: 600;
}
QPushButton:hover { background-color: #f3f4f6; }
QPushButton:pressed { background-color: #ebecf0; }
QPushButton#primaryButton {
    background-color: #1f883d; border: 1px solid #1a7f37; color: #ffffff;
}
QPushButton#primaryButton:hover { background-color: #1a7f37; }
QPushButton#primaryButton:pressed { background-color: #187733; }
QPushButton#scmSyncButton {
    background-color: #0969da; border: 1px solid #0969da; color: #ffffff;
    padding: 6px 12px;
}
QPushButton#scmSyncButton:hover { background-color: #0867c4; }
QPushButton#scmSyncButton:pressed { background-color: #0759ad; }
QPushButton#scmSyncButton:disabled {
    background-color: #8cbae8; border-color: #8cbae8; color: #ffffff;
}
QPushButton#ghostButton {
    background: transparent; border: none; color: #656d76;
    font-weight: 500; padding: 4px 8px; text-align: left;
}
QPushButton#ghostButton:hover { color: #1f2328; }
QPushButton#ghostButton[needsPull="true"] {
    background-color: #fff8c5; border: 1px solid #bf8700;
    border-radius: 6px; color: #9a6700;
}
QPushButton#ghostButton[needsPull="true"]:hover {
    background-color: #fae17d; color: #7d4e00;
}
/* "Play" under a chat video card, beside a ghost "Save". */
QPushButton#videoPlayButton {
    background: transparent; border: none; color: #0969da;
    font-weight: 600; padding: 4px 8px;
}
QPushButton#videoPlayButton:hover { color: #0a5cc7; }
/* "carol  3 replies" under a message, behind the repliers' faces. A quiet
   link, not the chunky default push button. */
QPushButton#threadSummary {
    background: transparent; border: none; color: #0969da;
    font-size: 12px; font-weight: 600; padding: 1px 2px; text-align: left;
}
QPushButton#threadSummary:hover { color: #0a5cc7; }

/* Primary chat-sidebar actions read as buttons rather than muted list rows. */
QPushButton#chatAddChannelButton {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 6px;
    color: #0969da; font-weight: 600; padding: 7px 10px; text-align: center;
}
QPushButton#chatAddChannelButton:hover {
    background-color: #eef4fb; border-color: #0969da; color: #0757ba;
}
QPushButton#chatAddChannelButton:pressed { background-color: #e6ebf1; }
QPushButton#chatIntegrationButton {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 6px;
    color: #5865f2; font-weight: 600; padding: 7px 10px; text-align: center;
}
QPushButton#chatIntegrationButton:hover {
    background-color: #f0f1ff; border-color: #5865f2; color: #4752c4;
}
QPushButton#chatIntegrationButton[connected="true"] {
    border-color: #1a7f37; color: #1a7f37;
}
QPushButton#chatIntegrationButton[bad="true"] {
    border-color: #cf222e; color: #cf222e;
}

#logFilterScroll, #logFilterScroll > QWidget,
#logFilterScroll > QWidget > QWidget { background: transparent; border: none; }

QPushButton#logFilterChip {
    background: transparent; border: none; padding: 0;
}

#navRail { background-color: #f6f8fa; border-right: 1px solid #d0d7de; }
#navRail QLabel { background: transparent; }
#navLogo { font-size: 18px; font-weight: 800; padding-bottom: 4px; }
QPushButton#navButton {
    background: transparent; border: none; color: #656d76;
    font-size: 11px; font-weight: 600; border-radius: 6px; padding: 8px 2px;
}
QPushButton#navButton:hover { background-color: #eaeef2; color: #1f2328; }
QPushButton#navButton:checked { background-color: #eaeef2; color: #1f2328; }

QPushButton#repoTab {
    background: transparent; border: none; border-bottom: 2px solid transparent;
    color: #656d76; font-weight: 600; padding: 6px 10px;
}
QPushButton#repoTab:hover { color: #1f2328; }
QPushButton#repoTab:checked { color: #1f2328; border-bottom: 2px solid #fd8c73; }

#fileTree { background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 6px; }
#fileTree::item { padding: 3px 2px; color: #1f2328; }

#fileTree::item:selected { background-color: #0969da; color: #ffffff; padding: 3px 2px; }
#fileTabs::pane { border: 1px solid #d0d7de; border-radius: 6px; top: -1px; }
#fileTabs QTabBar::tab {
    background: #ffffff; color: #656d76; padding: 6px 12px;
    border: 1px solid transparent; border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
#fileTabs QTabBar::tab:hover { color: #1f2328; }
#fileTabs QTabBar::tab:selected {
    background: #f6f8fa; color: #1f2328; border-color: #d0d7de; border-bottom-color: #f6f8fa;
}
#repoSettingsTabs::pane { border: 1px solid #d0d7de; border-radius: 6px; top: -1px; }
#repoSettingsTabs QTabBar::tab {
    background: #ffffff; color: #656d76; padding: 7px 16px;
    border: 1px solid transparent; border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
#repoSettingsTabs QTabBar::tab:hover { color: #1f2328; }
#repoSettingsTabs QTabBar::tab:selected {
    background: #f6f8fa; color: #1f2328; border-color: #d0d7de; border-bottom-color: #f6f8fa;
}

/* The Network tabs pick a view the way the log range buttons pick a window,
   so they are drawn as the app's buttons rather than as a document tab strip:
   the same fill, border, radius and weight, with the open tab carrying the
   accent fill a checked button gets. */
#networkTabs::pane { border: none; }
#networkTabs QTabBar::tab {
    background-color: #f6f8fa; color: #1f2328; padding: 4px 12px;
    border: 1px solid #d0d7de; border-radius: 6px;
    margin: 0 6px 0 0; font-weight: 600;
}
#networkTabs QTabBar::tab:hover { background-color: #eaeef2; }
#networkTabs QTabBar::tab:selected {
    background-color: #0969da; border-color: #0969da; color: #ffffff;
}

/* Settings is one page: a section list on the left, a heading per section on
   the page, and search marks drawn by #settingsSearchHits, which must stay
   see-through over the controls it outlines. */
#settingsNav { background: transparent; border: none; }
#settingsNav::item { color: #656d76; padding: 6px 10px; border-radius: 6px; margin: 1px 0; }
#settingsNav::item:hover { background-color: #f6f8fa; color: #1f2328; }
#settingsNav::item:selected { background-color: rgba(9, 105, 218, 0.1); color: #1f2328; }
#settingsSectionTitle { font-size: 17px; font-weight: 600; color: #1f2328; }
#settingsSectionRule { background-color: #d8dee4; }
#settingsSectionHeader { background: transparent; }
QToolButton#settingsSectionToggle { border: none; border-radius: 4px; background: transparent; padding: 0; }
QToolButton#settingsSectionToggle:hover { background-color: #eaeef2; }
#settingsNoMatches { color: #656d76; padding: 12px 2px; }
#settingsSearchHits { background: transparent; }
#codeEditor {
    background-color: #ffffff; border: none; color: #1f2328;
    font-family: monospace; font-size: 12px;
}
#diffView {
    background-color: #ffffff; border: none; border-radius: 0;
    color: #1f2328; font-family: monospace; font-size: 12px;
}
#commitBar {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 6px;
}
#commitBar QLabel { background: transparent; border: none; }
#commitBarText { color: #1f2328; }
#overviewList { background: transparent; border: none; }
#overviewList::item { padding: 5px 4px; color: #1f2328; }

#overviewList::item:selected { background-color: #0969da; color: #ffffff; padding: 5px 4px; }
#overviewList QHeaderView::section {
    background-color: #ffffff; color: #656d76; padding: 4px 8px;
    border: none; border-bottom: 1px solid #d8dee4;
    border-right: 2px solid #afb8c1; font-weight: 600;
}

#sizeBarTrack { background-color: #eaeef2; border-radius: 3px; }
#sizeBarFill { background-color: #2da44e; border-radius: 3px; }
#overviewMetricTrack { background-color: #eaeef2; border-radius: 2px; }
#overviewMetricFill { background-color: #2da44e; border-radius: 2px; }
#overviewLoadingPlaceholder { background-color: #d8dee4; border-radius: 3px; }
#overviewUpdatedTime { color: #656d76; font-size: 10px; }
#readmeView {
    background: transparent; border: none; padding: 0; color: #1f2328;
}

#repoHeaderTitle { font-size: 20px; font-weight: 400; }
#publicBadge {
    color: #656d76; border: 1px solid #d0d7de; border-radius: 10px;
    padding: 0 8px; font-size: 11px;
}
#issueStatusPill {
    color: #ffffff; border-radius: 13px; padding: 5px 16px;
    font-size: 12px; font-weight: 700;
}
#issueStatusPill[status="open"] { background-color: #1a7f37; }
#issueStatusPill[status="closed"] { background-color: #8250df; }
QPushButton#repoAction {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 6px;
    padding: 4px 10px; font-size: 12px; font-weight: 600; color: #1f2328;
}
QPushButton#repoAction:hover { background-color: #f3f4f6; }
QPushButton#repoAction::menu-indicator { width: 0; }
#repoTabBar { border-bottom: 1px solid #d0d7de; }
#pullListActionBar { background: transparent; border: none; }
#pullListActionBarDivider {
    color: #d0d7de; background-color: #d0d7de; max-height: 1px;
}

#repoActivityRail { background-color: #f6f8fa; border-right: 1px solid #d0d7de; }
#appNavigationRail, #appNavigationRailContent {
    background-color: #ffffff; border-right: 1px solid #d0d7de;
}

#appNavigationRail QScrollBar:vertical { width: 6px; }
#appNavigationRail QScrollBar::handle:vertical { border-radius: 3px; }
#sideRail, #sideRailColumn, #sideRailScroll, #sideRailContent, #sideRailBrowserPanel { background-color: #ffffff; }
#sideRailBrowserHandle:hover { background-color: #d0d7de; }
#sideRailScroll QScrollBar:vertical { width: 6px; }
#sideRailScroll QScrollBar::handle:vertical { border-radius: 3px; }
QLabel#sideRailBrowserTitle { color: #656d76; font-size: 9px; font-weight: 600; }
QFrame#sideRailSeparator { color: #d0d7de; background-color: #d0d7de; border: none; max-height: 1px; }
QPushButton[sideRailTool="true"], QToolButton#sideRailItem {
    background: transparent; border: 1px solid transparent; border-radius: 10px; padding: 0;
}
QPushButton[sideRailTool="true"]:hover, QToolButton#sideRailItem:hover { background-color: #f6f8fa; }
QPushButton[sideRailTool="true"]:checked { background-color: #ddf4ff; border-color: #0969da; }
QPushButton#sideRailAddButton {
    background: transparent; border: 1px dashed #afb8c1; border-radius: 10px; padding: 0;
}
QLabel#terminalsTitle { font-size: 15px; font-weight: 600; }
QPushButton#sideRailAddButton:hover { background-color: #f6f8fa; border-color: #0969da; }
QLabel#terminalsAgentStatus, QLabel#terminalsEmpty { color: #57606a; }
#packageRail, #packageRailContent { background-color: #ffffff; }
#packageRail { border-left: 1px solid #d0d7de; }
#packageRail QScrollBar:vertical { width: 6px; }
#packageRail QScrollBar::handle:vertical { border-radius: 3px; }
QLabel#packageRailTitle, QLabel#teammateRailTitle { color: #656d76; font-size: 9px; font-weight: 600; }
#sideRailColumn { border-left: 1px solid #d0d7de; }
QPushButton#teammateRailAvatar {
    background: transparent; border: none; border-radius: 10px; padding: 0;
}
QPushButton#teammateRailAvatar:hover { background-color: #f6f8fa; }
QLabel#teammateRailName {
    color: #1f2328; font-size: 7px; font-weight: 600;
    background-color: rgba(255, 255, 255, 185); border-radius: 3px; padding: 0 2px;
}
QPushButton[teammateRailHud="true"] {
    background-color: rgba(255, 255, 255, 190); border: 1px solid rgba(140, 149, 159, 150);
    border-radius: 7px; padding: 0; color: #24292f; font-size: 10px; font-weight: 700;
}
QPushButton[teammateRailHud="true"]:hover {
    background-color: rgba(246, 248, 250, 245); border-color: #57606a;
}
QLabel#teammateRailLastRun { color: #656d76; font-size: 10px; }
QPushButton#teammateRailCreateButton {
    background: transparent; border: 1px dashed #afb8c1; border-radius: 22px;
    padding: 0;
}
QPushButton#teammateRailCreateButton:hover { background-color: #f6f8fa; border-color: #0969da; }
QPushButton#teammateRailCreateButton[working="true"] { border: 1px solid #1a7f37; }
QPushButton#teammateRailCreateButton:disabled { border-color: #d0d7de; }
QTableWidget#issueTable[nodesDirectory="true"]::item,
QTableWidget#issueTable[nodesDirectory="true"]::item:hover {
    padding: 1px 6px;
}
QTableWidget#issueTable[nodesDirectory="true"]::item:selected {
    background-color: #eaeef2; color: #1f2328; padding: 1px 6px;
}

#railItemLabel {
    background: transparent; color: #656d76; font-size: 10px; font-weight: 600;
}
#commitsList {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 6px;
}

#commitsList::item { padding: 8px; color: #1f2328; }
#commitsList::item:hover { background-color: #f6f8fa; padding: 8px; }
#commitsList::item:selected { background-color: #0969da; color: #ffffff; padding: 8px; }
#placeholderPanel { color: #1f2328; font-size: 16px; }
#insightsPage QLabel { background: transparent; }
#insightsCard {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 8px;
    padding: 10px; color: #1f2328;
}

#serverRail { background-color: #f6f8fa; border-right: 1px solid #d0d7de; }
QPushButton#serverButton {
    background-color: #ffffff; border: 2px solid transparent;
    border-radius: 12px; padding: 0;
}
QPushButton#serverButton:hover { border-color: #d0d7de; border-radius: 12px; }
QPushButton#serverButton:checked { border-color: #1f883d; border-radius: 12px; }
QPushButton#serverAddButton {
    background-color: #eaeef2; border: none;
    border-radius: 12px; color: #1f883d; font-size: 22px; font-weight: 500;
    padding-bottom: 3px;
}
QPushButton#serverAddButton:hover { background-color: #1f883d; color: #ffffff; }
QPushButton#serverAddButton:pressed { background-color: #1a7f37; }
QPushButton#serverFooterButton {
    background: transparent; border: none; color: #656d76; font-size: 18px;

    border-radius: 13px;
}
QPushButton#serverFooterButton:hover { background-color: #eaeef2; color: #1f2328; }

#breadcrumbBar { background-color: #ffffff; border-bottom: 1px solid #d8dee4; }
#windowChromeBar {
    background-color: #ffffff; border-bottom: 1px solid #d8dee4;
}
#appVersionLabel {
    background: transparent; color: #1f2328; font-size: 13px; font-weight: 700;
}
#issueDetailOverlay { background-color: #ffffff; }
#globalSearch {
    background-color: #ffffff; border: 1px solid #d0d7de;
    border-radius: 6px; padding: 4px 8px; color: #1f2328;
}
#globalSearch:focus { border-color: #0969da; }
#globalSearchPopup {
    background-color: #ffffff; border: 1px solid #d0d7de;
    border-radius: 8px; padding: 4px; outline: none;
}
#globalSearchPopup::item { color: #1f2328; padding: 6px 8px; border-radius: 6px; }
#globalSearchPopup::item:selected { background-color: #0969da; color: #ffffff; }
#searchEmptyNotice { color: #656d76; background: transparent; }
QPushButton#windowChromeButton, QPushButton#windowChromeCloseButton,
QPushButton#packageActionsButton {
    background: transparent; border: 1px solid transparent; border-radius: 6px;
    color: #656d76; padding: 0;
}
QPushButton#windowChromeButton:hover, QPushButton#packageActionsButton:hover {
    background-color: #f6f8fa; color: #1f2328; border-color: #d0d7de;
}
QPushButton#windowChromeCloseButton:hover {
    background-color: #cf222e; color: #ffffff; border-color: #cf222e;
}
#searchResultsTitle { font-size: 18px; color: #1f2328; }
#searchResultsTree {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 8px;
    padding: 4px; outline: none;
}
#searchResultsTree::item { padding: 4px 6px; color: #1f2328; }
#searchResultsTree::item:selected { background-color: #0969da; color: #ffffff; }
#saveSearchPrompt {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 8px;
}
#saveSearchPromptLabel { background: transparent; color: #1f2328; }
#savedSearchPanel { background: transparent; }
#savedSearchList { background: transparent; border: none; outline: none; }
#savedSearchRow, #savedSearchRow QLabel { background: transparent; }
#savedSearchList::item { border-bottom: 1px solid #d8dee4; border-radius: 6px; }
#savedSearchList::item:hover { background-color: #f6f8fa; }
#savedSearchList::item:selected { background-color: #ddf4ff; }
#savedSearchQuery { color: #1f2328; font-weight: 600; }
#savedSearchStats { color: #656d76; font-size: 12px; }
#savedSearchNewBadge {
    background-color: #bf8700; color: #ffffff; border-radius: 8px;
    padding: 0px 6px; font-size: 11px; font-weight: 700;
}
#savedSearchSecurityBadge {
    background-color: #fff8c5; color: #7d4e00; border: 1px solid #d4a72c;
    border-radius: 8px; padding: 0px 6px; font-size: 11px; font-weight: 700;
}
#savedSearchSecurityBadge[alarming="true"] {
    background-color: #cf222e; color: #ffffff; border-color: #a40e26;
}
QCheckBox#savedSearchSecurityCheck { color: #656d76; font-size: 12px; background: transparent; }
#securityAlarmBanner {
    background-color: #ffebe9;
    border-bottom: 2px solid #cf222e;
}
#securityAlarmBannerIcon { background: transparent; }
#securityAlarmBannerLabel {
    color: #82071e; font-size: 13px; font-weight: 600; background: transparent;
}
QToolButton#savedSearchAlertToggle, QToolButton#savedSearchRemove {
    background: transparent; border: none; border-radius: 4px; padding: 3px;
}
QToolButton#savedSearchAlertToggle:hover, QToolButton#savedSearchRemove:hover {
    background-color: #eaeef2;
}
#breadcrumb { background: transparent; color: #656d76; font-size: 14px; font-weight: 600; }
#connectionStatus { background: transparent; color: #656d76; font-size: 13px; font-weight: 600; }

#connectionDot { border: 2px solid #ffffff; }

#relayJoinDot {
    background-color: #cf222e; border: 1px solid #ffffff; border-radius: 5px;
}

#chromeDotDivider { background-color: #d0d7de; }

/* Timers package: the clock and its chips in the window chrome. */
#timersNavStrip { background: transparent; }
QPushButton#timersClockButton {
    background: transparent; border: 1px solid transparent; border-radius: 6px;
    color: #656d76; padding: 3px 5px;
}
QPushButton#timersClockButton:hover {
    background-color: #eaeef2; color: #1f2328; border-color: #d0d7de;
}
QPushButton#timerChip, QPushButton#timerChipPaused, QPushButton#timerChipDone {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 6px;
    color: #1f2328; font-size: 12px; font-weight: 600; padding: 3px 8px;
}
QPushButton#timerChip:hover { background-color: #eaeef2; }
QPushButton#timerChipPaused {
    background: transparent; border-style: dashed; color: #656d76;
}
QPushButton#timerChipPaused:hover { background-color: #eaeef2; }
QPushButton#timerChipDone {
    background-color: #cf222e; border-color: #cf222e; color: #ffffff;
}
QPushButton#timerChipDone:hover { background-color: #a40e26; }
QPushButton#relayJoinApproveButton {
    background-color: #cf222e; color: #ffffff; border: none;
    border-radius: 6px; padding: 4px 10px; font-size: 12px; font-weight: 700;
}
QPushButton#relayJoinApproveButton:hover { background-color: #a40e26; }
QPushButton#runnerSignInBadge {
    background-color: #cf222e; color: #ffffff; border: none;
    border-radius: 6px; padding: 4px 10px; font-size: 12px; font-weight: 700;
}
QPushButton#runnerSignInBadge:hover { background-color: #a40e26; }
QPushButton#runnerSignInBadge[severity="expiring"] {
    background-color: #9a6700; color: #ffffff;
}
QPushButton#runnerSignInBadge[severity="expiring"]:hover {
    background-color: #7d4e00;
}
QPushButton#backgroundRunnerBadge {
    background: transparent; border: 1px solid #d0d7de; border-radius: 6px;
    color: #656d76; font-size: 12px; padding: 0 8px;
}
QPushButton#backgroundRunnerBadge:hover { background-color: #f6f8fa; color: #1f2328; }
QPushButton#backgroundRunnerBadge[severity="error"] { border-color: #cf222e; }

#agentsNavBadge {
    color: #8b949e; font-size: 13px; font-weight: 600;
}

#topNavBar { background: transparent; }
#navDivider { background-color: #d0d7de; border: none; }
QPushButton#topNavButton {
    background: transparent; border: 1px solid transparent; border-radius: 6px;
    color: #656d76; font-size: 13px; font-weight: 600; padding: 5px 12px;
}
QPushButton#topNavButton:hover {
    background-color: #eaeef2; color: #1f2328;
}
QPushButton#topNavButton:checked {
    background-color: #eaeef2; color: #1f2328; border-color: #d0d7de;
}
QPushButton#floatingLogButton {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 6px;
    color: #24292f; font-size: 12px; font-weight: 600; padding: 4px 10px;
}
QPushButton#floatingLogButton:hover { background-color: #f3f4f6; color: #1f2328; }

QPushButton#relayOpenButton, QPushButton#navHistoryButton {
    background: transparent; border: none; border-radius: 8px; color: #656d76;
}
QPushButton#nodeMenuButton, QPushButton#repoMenuButton {
    background: transparent; border: 1px solid #d0d7de; border-radius: 8px;
    color: #1f2328; font-size: 15px; font-weight: 700; padding: 5px 12px;
}

QPushButton#projectMenuButton {
    background: transparent; border: none; border-radius: 8px; padding: 5px 4px;
}
QPushButton#relayMenuButton {
    background: transparent; border: none; border-radius: 8px;
    color: #1f2328; font-size: 15px; font-weight: 700; padding: 5px 4px;
}
QPushButton#relayOpenButton:hover, QPushButton#navHistoryButton:hover,
QPushButton#projectMenuButton:hover,
QPushButton#relayMenuButton:hover,
QPushButton#nodeMenuButton:hover,
QPushButton#repoMenuButton:hover {
    background-color: #eaeef2; color: #1f2328;
}
#nodeSwitchProgress { background: transparent; border: none; }
#nodeSwitchProgress::chunk { background-color: #0969da; border-radius: 1px; }
#navCaption { background: transparent; color: #656d76; font-size: 13px; font-weight: 600; }

QPushButton#agentsMagicButton {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
        stop:0 #6d28d9, stop:0.5 #9333ea, stop:1 #0891b2);
    border: 1px solid rgba(109,40,217,0.45); border-radius: 11px;
    color: #ffffff; font-size: 13px; font-weight: 700; padding: 5px 14px;
}
QPushButton#agentsMagicButton:hover {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
        stop:0 #7c3aed, stop:0.5 #a855f7, stop:1 #06b6d4);
    border-color: #7c3aed;
}
QPushButton#agentsMagicButton:checked {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
        stop:0 #5b21b6, stop:0.5 #7e22ce, stop:1 #0e7490);
    border-color: #1f2328;
}

/* The agent activity window's toggles: lit while on, dimmed while off. */
QPushButton[activityToggle="true"] { color: #656d76; }
QPushButton[activityToggle="true"]:checked {
    background-color: #dafbe1; border-color: #1a7f37; color: #1f2328;
}
QPushButton[activityToggle="true"]:checked:hover { background-color: #c6f2d0; }

#navSolanaBalance {
    background: transparent; border: none; color: #6e7781; padding: 0;
}
#agentPromptImage {
    background: #f6f8fa; border: 1px solid #d0d7de;
    border-radius: 6px; padding: 2px;
}
#agentPromptImage:hover { border-color: #0969da; }
QPushButton#pingPromptAction {
    background: rgba(9,105,218,0.08); border: 1px solid rgba(9,105,218,0.35);
    border-radius: 6px; color: #0969da; font-size: 11px; font-weight: 600; padding: 2px 6px;
}
QPushButton#pingPromptAction:hover { background: rgba(9,105,218,0.16); color: #0550ae; }
#pageNoticeArea { background: transparent; }
#pageNoticeArea #inlineNotice {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 8px;
}
#pageNoticeArea #inlineNotice QLabel { background: transparent; color: #1f2328; font-size: 12px; }
#pageNoticeArea #inlineNotice[tone="danger"] { border-color: #cf222e; }
#pageNoticeArea #inlineNotice[tone="danger"] QLabel { color: #cf222e; }
#pageNoticeArea #inlineNotice[tone="accent"] { border-color: #0969da; }
#pageNoticeArea #inlineNotice[tone="accent"] QLabel { color: #0969da; }
#pageNoticeArea #inlineNotice QPushButton#ghostButton {
    font-size: 11px; padding: 0 6px; min-height: 16px; max-height: 20px;
}
QPushButton#notificationButton, QPushButton#notificationButtonAlert {
    background: transparent; border: 1px solid #d0d7de; border-radius: 6px;
    padding: 2px 6px; font-size: 13px; color: #656d76;
}
QPushButton#notificationButton:hover {
    background-color: #f6f8fa; color: #1f2328;
}
QPushButton#notificationButtonAlert {
    color: #9a6700; border-color: #d4a72c; font-weight: 800;
}
QPushButton#notificationButtonAlert:hover {
    background-color: #fff8c5; color: #7d4e00;
}

#homeCard {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 12px;
}
#homeCard QLabel { background: transparent; }
#homeTitle { font-size: 24px; font-weight: 800; }
#homeName { font-size: 18px; font-weight: 700; }
#homeStat { color: #656d76; font-size: 14px; }
#homeScoreBoard {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 12px;
}
#homeScoreValue {
    color: #9a6700; font-size: 18px; font-weight: 800; background: transparent;
}
#homeGraph {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 8px;
    color: #0969da; font-family: monospace; font-size: 12px; padding: 10px;
}
#homeCard QListWidget {
    background: #f6f8fa; border: 1px solid #d0d7de; border-radius: 6px; padding: 4px;
}
#homeCard QListWidget::item { color: #1f2328; padding: 8px 7px; margin: 2px; }
QPushButton#primaryButton:disabled {
    background-color: #eaeef2; color: #8c959f; border-color: #d0d7de;
}

QPushButton#dangerButton {
    background: transparent; border: 1px solid #cf222e; border-radius: 6px;
    color: #cf222e; font-weight: 600;
}
QPushButton#dangerButton:hover { background-color: #cf222e; color: #ffffff; }

QPushButton#successButton {
    background: transparent; border: 1px solid #1a7f37; border-radius: 6px;
    color: #1a7f37; font-weight: 600;
}
QPushButton#successButton:hover { background-color: #1a7f37; color: #ffffff; }

#sidebar { background-color: #ffffff; border-right: 1px solid #d0d7de; }
#sidebar QLabel { background: transparent; }
#workspaceName {
    font-size: 17px; font-weight: 800; padding: 2px 0;
}
#statusLine { color: #656d76; font-size: 11px; }
#suggestedAppStatusFound, #suggestedAppStatusMissing,
#suggestedAppStatusInProgress, #suggestedAppStatusProblem {
    font-size: 12px; font-weight: 700; padding: 5px 7px; border-radius: 5px;
}
#suggestedAppStatusFound {
    color: #116329; background: #dafbe1; border: 1px solid #1a7f37;
}
#suggestedAppStatusMissing, #suggestedAppStatusProblem {
    color: #a40e26; background: #ffebe9; border: 1px solid #cf222e;
}
#suggestedAppStatusInProgress {
    color: #0550ae; background: #ddf4ff; border: 1px solid #0969da;
}
#restartCountdown { color: #656d76; font-size: 11px; background: transparent; }
#versionLabel { color: #8c959f; font-size: 11px; background: transparent; }
#sectionLabel {
    color: #656d76; font-size: 11px; font-weight: 700;
    letter-spacing: 1px; padding-top: 8px;
}
#sidebar QListWidget {
    background: transparent; border: none; padding: 0;
}
#sidebar QListWidget::item {
    color: #1f2328; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QListWidget::item:hover {
    background-color: #f6f8fa; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QListWidget::item:selected {
    background-color: #0969da; color: #ffffff; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QTreeWidget {
    background: transparent; border: none; padding: 0; outline: none;
}
#sidebar QTreeWidget::item {
    color: #1f2328; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QTreeWidget::item:hover {
    background-color: #f6f8fa; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QTreeWidget::item:selected {
    background-color: #0969da; color: #ffffff; border-radius: 6px; padding: 5px 8px; margin: 1px 0;
}
#sidebar QTreeWidget::branch { background: transparent; border-image: none; image: none; }

#notesList {
    background-color: transparent;
    border: none;
    padding: 0;
}
#notesList::item {
    color: #1f2328; border-radius: 6px; padding: 6px 8px; margin: 1px 0;
}
#notesList::item:hover {
    background-color: #f6f8fa; border-radius: 6px; padding: 6px 8px; margin: 1px 0;
}
#notesList::item:selected {
    background-color: #1f883d; color: #ffffff; border-radius: 6px; padding: 6px 8px; margin: 1px 0;
}
QWidget#memberRow { background: transparent; }
QPushButton#memberNameButton {
    background: transparent; border: none; color: #1f2328;
    padding: 4px 6px; text-align: left; font-weight: 500;
}
QPushButton#memberNameButton:hover {
    background-color: #f6f8fa; color: #0969da; border-radius: 6px;
}
QPushButton#memberDeleteButton {
    background: transparent; border: 1px solid #d0d7de; border-radius: 5px;
    color: #cf222e; padding: 2px 6px; font-size: 11px; font-weight: 700;
}
QPushButton#memberDeleteButton:hover {
    background-color: #cf222e; border-color: #cf222e; color: #ffffff;
}
#issueQuickAdd {
    background-color: #ffffff; border: 1px solid #d0d7de;
    border-radius: 6px; padding: 4px 10px; font-size: 13px;
}
#issueQuickAdd:focus { border-color: #0969da; }
#promptWrapper {
    background-color: #ffffff; border: 1px solid rgba(26,127,55,0.5); border-radius: 6px;
}
#promptWrapper:focus-within { border-color: #1a7f37; }

#promptDragHandle { background: transparent; border: none; }
QPushButton#promptResetButton, QPushButton#promptChannelButton,
QPushButton#promptClearButton, QPushButton#promptCloudButton {
    background: transparent; border: none; padding: 0;
}
QPushButton#promptWorktreeCheck, QPushButton#promptVibeCheck {
    background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 0;
}
QPushButton#promptCloudButton:checked { background: rgba(9,105,218,0.14); border: 1px solid #0969da; border-radius: 4px; }
QPushButton#promptWorktreeCheck:checked { background: rgba(9,105,218,0.10); border-color: #0969da; }
QPushButton#promptVibeCheck:checked { background: rgba(130,80,223,0.16); border-color: #8250df; }
#promptDetachWindow, #promptOverlayHost { background: transparent; }
#footerLogPanel {
    background-color: #ffffff;
    border: 1px solid rgba(26,127,55,0.5);
    border-radius: 6px;
}
#agentsActionBar {
    background: transparent;
    border-bottom: 1px solid #d0d7de;
}
#agentsActionBar QLabel { background: transparent; }
#promptWrapper #issueQuickAdd {
    background: transparent; border: 1px solid rgba(87,96,106,0.3); border-radius: 4px;
}
#promptWrapper #issueQuickAdd:focus { border: 1px solid rgba(87,96,106,0.3); }
QPushButton#quickAddSendIcon {
    background: transparent; border: none; color: #1a7f37;
    padding: 4px; border-radius: 4px;
}
QPushButton#quickAddSendIcon:hover { color: #1a7f37; background: rgba(26,127,55,0.12); }

QPushButton#quickAddSendIcon[enterTarget="true"] {
    border: 1px solid #1a7f37; background: rgba(26,127,55,0.08);
}

QPushButton#quickAddGenieButton {
    background: rgba(124,58,237,0.10); border: 1px solid rgba(124,58,237,0.45);
    color: #7c3aed; padding: 4px; border-radius: 4px;
    font-size: 12px; font-weight: 700;
}
QPushButton#quickAddGenieButton:hover {
    background: rgba(124,58,237,0.20); border-color: #7c3aed; color: #5b21b6;
}
QPushButton#quickAddGenieButton[enterTarget="true"] {
    background: rgba(124,58,237,0.22); border: 1px solid #7c3aed; color: #5b21b6;
}
#issueSearch {
    background-color: #ffffff; border: 1px solid #d0d7de;
    border-radius: 6px; padding: 6px 10px;
}
#issueSearch:focus { border-color: #0969da; }
#issueTable {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 6px;
    gridline-color: #d8dee4;
}
QTableWidget#issueTable[repositoryGrid="true"] { gridline-color: #d0d7de; }
#issueTable::item { padding: 4px 8px; color: #1f2328; }
#issueTable::item:hover { padding: 4px 8px; }
#issueTable::item:selected { background-color: #0969da; color: #ffffff; padding: 4px 8px; }
#issueTable QHeaderView::section {
    background-color: #f6f8fa; color: #656d76; padding: 6px 8px;
    border: none; border-bottom: 1px solid #d0d7de;
    border-right: 2px solid #afb8c1; font-weight: 700;
}

#issueBoardScroll { border: none; background: transparent; }
#issueBoardColumn {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 8px;
}
#issueBoardHeader {
    color: #1f2328; font-weight: 700; padding: 2px 2px 4px 2px;
}
#issueBoardList {
    background-color: transparent; border: none;
}
#issueBoardList::item {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 6px;
    color: #1f2328; padding: 8px 8px; margin: 3px 1px;
}
#issueBoardList::item:selected { border-color: #0969da; color: #1f2328; }

#chatHeader {
    background-color: #ffffff;
    border-bottom: 1px solid #d0d7de;
}
#chatHeader QLabel { background: transparent; }
#channelTitle { font-size: 16px; font-weight: 700; }
#agentPromptTitle {
    font-size: 16px; font-weight: 700;
}
#agentPromptTitleBlock {
    padding-bottom: 8px; border-bottom: 1px solid #d0d7de;
}
#encryptionLabel { color: #656d76; font-size: 12px; }

#nodeProfilePanel { background: transparent; }
#nodeProfileContent { background-color: #ffffff; }
#profileBanner { border-radius: 16px; }
#profileName { font-size: 17px; font-weight: 800; }
#profileUserCard {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 10px;
}
#profileUserAvatar { border-radius: 12px; }
#profileUserName { color: #1f2328; font-size: 16px; font-weight: 800; }
#profileNodeList {
    background-color: transparent; border: none; color: #1f2328;
    outline: none; font-weight: 600;
}
#profileNodeList::item {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 8px;
    padding: 5px 8px; margin: 2px 0;
}
QPushButton#profileActionButton {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 10px;
    color: #1f2328;
}
QPushButton#profileActionButton:hover {
    background-color: #eaeef2; border-color: #0969da; color: #1f2328;
}
QPushButton#profileActionButton:pressed { background-color: #e1e6eb; }
#statTile {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 9px;
    padding: 6px 4px; color: #1f2328;
}
#profileCard {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 9px;
    padding: 7px 10px; color: #1f2328;
}
#profileMono {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 8px;
    padding: 6px 9px; color: #1f2328; font-family: monospace;
}
#profileQr {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 10px;
    padding: 8px;
}

#firewallBanner {
    background-color: #fff8c5;
    border-bottom: 1px solid #d4a72c;
}
#firewallBannerLabel {
    color: #7d4e00; font-size: 13px; background: transparent;
}
#meshDebugBar {
    background-color: #f6f8fa;
    border-bottom: 1px solid #d0d7de;
}
#meshDebugLabel { color: #656d76; font-size: 11px; background: transparent; }
#meshDebugCopy {
    color: #656d76; font-size: 11px; background: transparent;
    border: 1px solid #d0d7de; border-radius: 4px; padding: 0 6px;
}
#meshDebugCopy:hover { color: #1f2328; border-color: #656d76; }

#chatUnreadBanner {
    background-color: #ddf4ff;
    border-bottom: 1px solid #54aeff;
}
#chatUnreadBanner QLabel { color: #0969da; font-size: 13px; font-weight: 600; background: transparent; }
#chatUnreadBannerButton {
    background: transparent; border: none; color: #0969da; font-weight: 700;
    padding: 4px 10px; border-radius: 6px;
}
#chatUnreadBannerButton:hover { background-color: #0969da; color: #ffffff; }
#chatSidebarScroll, #chatSidebarRooms { background: transparent; border: none; }
#chatWorkspaceHeader { border-bottom: 1px solid #d0d7de; }
#chatWorkspaceMenu {
    background: transparent; border: none; border-radius: 6px; color: #1f2328;
    font-size: 15px; font-weight: 800; text-align: left; padding: 4px 8px;
}
#chatWorkspaceMenu:hover { background-color: #eaeef2; }
#chatUserPanel { border-top: 1px solid #d0d7de; }
#chatMembersPanel { border-left: 1px solid #d0d7de; }
#chatMembersHeading { color: #656d76; font-size: 11px; font-weight: 600; padding: 0px 16px 4px 16px; }
#chatMembersGroupHeading { color: #656d76; font-size: 12px; font-weight: 600; padding: 16px 8px 4px 8px; }
#chatMemberCard:hover { background-color: #eaeef2; border-radius: 6px; }
#chatDayDividerRule, #channelTopicDivider { background-color: #d0d7de; border: none; }
#chatDayDividerLabel { color: #656d76; font-size: 12px; font-weight: 600; }
#channelHash { color: #656d76; font-size: 20px; }
#composerAttachButton { background: transparent; border: 1px solid #d0d7de; border-radius: 12px; }
#composerAttachButton:hover { background-color: #eaeef2; }
#chatWorkspaceUser { color: #656d76; font-size: 12px; }
#chatComposeButton { background: transparent; border: 1px solid #d0d7de; border-radius: 17px; }
#chatComposeButton:hover { background-color: #eaeef2; }
#chatNavButton, #chatSectionToggle {
    background: transparent; border: none; color: #656d76; text-align: left;
    padding: 4px 14px; font-size: 13px;
}
#chatNavButton:hover, #chatSectionToggle:hover { color: #1f2328; background-color: #eaeef2; }
#chatNavButton[unread="true"] { color: #1f2328; font-weight: 700; }
#chatNavBadge {
    background-color: #cf222e; color: #ffffff; border-radius: 9px; font-size: 11px;
    font-weight: 700; padding: 0px 6px; min-height: 18px; max-height: 18px;
}
#chatSidebarDivider { background-color: #d0d7de; border: none; }
#chatSectionAddButton { background: transparent; border: none; border-radius: 6px; }
#chatSectionAddButton:hover { background-color: #eaeef2; }
#channelTopic { color: #656d76; font-size: 12px; }
#chatHeaderMeta, #chatHeaderIconButton {
    background: transparent; border: none; border-radius: 6px; color: #656d76; padding: 0px 4px;
}
#chatHeaderMeta:hover, #chatHeaderIconButton:hover { background-color: #eaeef2; color: #1f2328; }
#chatHeaderIconButton:checked { background-color: #eaeef2; color: #1f2328; }
#composerToolButton {
    background: transparent; border: none; border-radius: 4px; color: #656d76; font-size: 14px; padding: 0px;
}
#composerToolButton:hover { background-color: #eaeef2; color: #1f2328; }
#composerSendButton { background-color: #1f883d; border: none; border-radius: 4px; }
#solanaBanner {
    background-color: #dafbe1;
    border-bottom: 1px solid #1f883d;
}
#solanaBannerLabel {
    color: #1a7f37; font-size: 13px; font-weight: 600; background: transparent;
}
#walletVerifyBanner {
    background-color: #fff8c5;
    border-bottom: 1px solid #d4a72c;
}
#walletVerifyBanner QLabel { background: transparent; }
#walletVerifyTitle { color: #7d4e00; font-size: 15px; font-weight: 800; }
#walletVerifyBody { color: #6a4b16; font-size: 13px; }
#messageView, #messageContainer {
    background-color: #ffffff; border: none;
}
#typingLabel {
    background-color: #ffffff; color: #656d76; font-size: 12px;
    padding: 0 18px;
}
#messageText { color: #1f2328; }
#fileChip {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 6px;
}
#reactionChip {
    background-color: #f6f8fa; border: 1px solid #e5e7eb; border-radius: 18px;
    padding: 2px 9px; font-size: 12px; color: #1f2328;
}
#reactionChip:hover { border-color: #0969da; background-color: #eef4fb; }
#reactionAdd {
    background: transparent; border: 1px solid transparent; border-radius: 12px;
    padding: 2px 7px;
}
#reactionAdd:hover { border-color: #d0d7de; background-color: #f6f8fa; }
#reactionPicker {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 10px;
}

#agentMetaPopup {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 10px;
}
#reactionPickerButton { background: transparent; border: none; border-radius: 8px; }
#reactionPickerButton:hover { background-color: #eaeef2; }
#messageAction {
    background: transparent; border: none; border-radius: 6px;
    color: #656d76; font-size: 11px; padding: 2px 7px;
}
#messageAction:hover { color: #1f2328; }
#messageAction::menu-indicator { image: none; width: 0; }
#iconButton {
    background: transparent; border: none; font-size: 18px; padding: 2px 6px;
}
#iconButton:hover { background-color: #eaeef2; border-radius: 6px; }

#settingsTitle { font-size: 20px; font-weight: 800; }
#avatarPreview {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 12px;
    color: #8c959f; font-size: 11px;
}
#emailVerifiedBadge {
    background-color: #dafbe1; border: 1px solid #2da44e; border-radius: 10px;
    color: #1a7f37; padding: 2px 8px; font-size: 11px; font-weight: 700;
}
#networkLog, #networkLogPopoutView {
    background-color: #ffffff; border: none;
    color: #1f2328; font-family: monospace; font-size: 12px;
}
#logTimelineChart { background-color: #ffffff; border-radius: 8px; }
#logTimelineSummary { color: #656d76; font-size: 12px; }
#logRangeButton { padding: 4px 10px; }
#logRangeButton:checked {
    color: #ffffff; background-color: #0969da; border-color: #0969da;
}
#logDock { background: transparent; border: none; }
#logDock QLabel { background: transparent; }

#appStatusBar { background-color: #f6f8fa; border-top: 1px solid #d0d7de; }
#appStatusBar QLabel {
    background: transparent; color: #656d76; font-size: 11px;
}
#appStatusBar QPushButton {
    background: transparent; border: none; color: #1f2328;
    font-size: 11px; padding: 0 4px;
}
#appStatusBar QPushButton:hover { color: #0969da; }
/* ForkMesh Ads "Sponsored" strip above the status bar. It borrows the status
   bar's surface and its muted greys so it reads as footer furniture, not as
   an alert: no accent fill, no icon, nothing that moves. */
#adBanner { background-color: #f6f8fa; border-top: 1px solid #d0d7de; }
#adBanner QLabel { background: transparent; }
#adBannerCaption {
    color: #656d76; font-size: 10px; font-weight: 600;
    border: 1px solid #d0d7de; border-radius: 3px; padding: 0 4px;
}
#adBannerSponsor { color: #656d76; font-size: 11px; }
#adBannerHeadline { color: #1f2328; font-size: 12px; font-weight: 600; }
#adBannerBody { color: #656d76; font-size: 12px; }
QPushButton#adBannerCta {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 6px;
    color: #1f2328; font-size: 12px; font-weight: 600; padding: 3px 10px;
}
QPushButton#adBannerCta:hover { border-color: #656d76; color: #1f2328; }
QPushButton#adBannerLink, QPushButton#adBannerHide {
    background: transparent; border: none; color: #656d76;
    font-size: 11px; font-weight: 400; padding: 2px 4px;
}
QPushButton#adBannerLink:hover { color: #0969da; text-decoration: underline; }
QPushButton#adBannerHide:hover { color: #1f2328; }
#statusVersionButton:checked { color: #0969da; font-weight: 700; }
#debugBar { background-color: #ffffff; border-top: 1px solid #d0d7de; }
#debugBarScroll, #debugBarContent { background: transparent; border: none; }
#debugBarSeparator { color: #d0d7de; }
#debugBarScroll QScrollBar:horizontal { height: 7px; }

#debugBarTools { background: transparent; }
#debugLogTail {
    background-color: #ffffff; border-top: 1px solid #d0d7de;
    color: #57606a; padding: 2px 8px;
}

#statusBackgroundTasks { background: transparent; }

#composerBar { background-color: #ffffff; border-top: 1px solid #d0d7de; }
#composerInputRow {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 10px;
}
#messageInput {
    background-color: transparent; border: none;
    padding: 9px 4px; font-size: 14px;
}
#scmMessageInput {
    background-color: transparent; border: 1px solid #d0d7de;
    border-radius: 4px; selection-background-color: #0969da;
}
#scmMessageInput:focus { border-color: #1a7f37; }
#emojiPickerButton {
    border: none; background: transparent; font-size: 18px;
    padding: 3px; border-radius: 6px;
}
#emojiPickerButton:hover { background-color: #eaeef2; }

QScrollBar:vertical {
    background: transparent; width: 10px; margin: 0;
}
QScrollBar::handle:vertical {
    background: #d0d7de; border-radius: 5px; min-height: 30px;
}
QScrollBar::handle:vertical:hover { background: #afb8c1; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
QScrollBar:horizontal {
    background: transparent; height: 10px; margin: 0;
}
QScrollBar::handle:horizontal {
    background: #d0d7de; border-radius: 5px; min-width: 30px;
}
QScrollBar::handle:horizontal:hover { background: #afb8c1; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: none; }

QSplitter::handle { width: 1px; height: 1px; background: #d0d7de; }
QSplitter::handle:hover, QSplitter::handle:pressed { background: palette(highlight); }

QMessageBox, QInputDialog, QDialog { background-color: #ffffff; }
QDialog QLabel { background: transparent; }

QPushButton[buttonSize="sm"] {
    min-height: 24px; max-height: 28px; padding: 3px 8px;
    font-size: 12px; border-radius: 6px;
}
QPushButton[buttonSize="xs"] {
    min-height: 18px; max-height: 22px; padding: 1px 7px;
    font-size: 11px; border-radius: 5px;
}
QPushButton#repoTab {
    background: transparent;
    border: none;
    border-bottom: 2px solid transparent;
    border-radius: 0;
    color: #656d76;
    font-weight: 600;
    padding: 6px 10px;
}
QPushButton#repoTab:hover { color: #1f2328; background: transparent; }
QPushButton#repoTab:checked {
    color: #1f2328;
    border-bottom: 2px solid #1f883d;
    border-radius: 0;
    background: transparent;
}
QPushButton#repoAction {
    background-color: #eaeef2;
    border: 1px solid #d0d7de;
    border-radius: 6px;
    padding: 4px 10px;
    font-size: 12px;
    font-weight: 600;
    color: #1f2328;
    min-height: 24px;
    max-height: 28px;
}
QPushButton#repoAction:hover { background-color: #d0d7de; }
QPushButton#repoAction::menu-indicator { width: 0; }
QPushButton#serverAddButton {
    background-color: #ffffff;
    border: 1px dashed #d0d7de;
    border-radius: 12px;
    color: #656d76;
    font-size: 20px;
    font-weight: 700;
    padding: 0;
    text-align: center;
}
QPushButton#serverAddButton:hover { color: #1f883d; border-color: #1f883d; background-color: #ffffff; }
#sectionCard, #settingsCard {
    background-color: #ffffff;
    border: 1px solid #d0d7de;
    border-radius: 8px;
}
#sectionCard QLabel, #settingsCard QLabel { background: transparent; }

QPushButton#shortcutCard {
    background-color: #ffffff;
    border: 1px solid #d0d7de;
    border-radius: 8px;
    text-align: left;
}
QPushButton#shortcutCard:hover { border-color: #2ea043; background-color: #f6f8fa; }
#teammatesTopBar { background: transparent; }
QFrame#teammateDashboardSummary {
    background-color: #f5f0ff; border: 1px solid #8250df;
    border-radius: 9px; margin-bottom: 8px;
}
QFrame#teammateDashboardSummary QLabel { background: transparent; }
#teammateDashboardTitle { color: #1f2328; font-size: 14px; font-weight: 750; }
#teammateDashboardSummaryText { color: #57606a; font-size: 10px; }
#teammateDashboardMainPoints { color: #24292f; font-size: 11px; font-weight: 600; }
QPushButton#teammateDashboardGenerate, QPushButton#teammateDashboardContinue {
    color: #ffffff; background-color: #8250df; border: 1px solid #6639ba;
    border-radius: 7px; padding: 4px 8px; font-size: 10px; font-weight: 700;
}
QPushButton#teammateDashboardGenerate:hover, QPushButton#teammateDashboardContinue:hover { background-color: #6639ba; }
QPushButton#teammateDashboardContinue:disabled { color: #8c959f; background-color: #f6f8fa; border-color: #d0d7de; }
QPushButton#teammateDashboardBriefingDetail,
QPushButton#teammateDashboardBriefingChoice,
QPushButton#teammateDashboardSourceLink {
    color: #24292f; background-color: #ffffff; border: 1px solid #d0d7de;
    border-radius: 6px; padding: 3px 6px; font-size: 9px; font-weight: 650;
}
QPushButton#teammateDashboardBriefingDetail:hover,
QPushButton#teammateDashboardBriefingChoice:hover,
QPushButton#teammateDashboardSourceLink:hover { background-color: #f6f8fa; border-color: #8c959f; }
QFrame#teammateSkillUsageCard, QFrame#teammatePerformanceCard {
    background-color: #ffffff; border: 1px solid #d0d7de;
    border-radius: 9px;
}
QFrame#teammateSkillUsageCard QLabel, QFrame#teammatePerformanceCard QLabel { background: transparent; }
#teammateBriefingSidebar, #teammateBriefingContributors,
#teammateBriefingContributorsScroll { background-color: #ffffff; }
#teammateBriefingFollowUps { background: transparent; }
QFrame#teammateBriefingSidebar {
    border: 1px solid #d0d7de; border-radius: 9px;
}
QFrame#teammateBriefingHero {
    background-color: #f5f0ff; border: 1px solid #8250df; border-radius: 10px;
}
QFrame#teammateBriefingSection, QFrame#teammateBriefingContribution {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 9px;
}
QFrame#teammateBriefingContribution[hasContribution="false"] { border-style: dashed; }
QFrame#teammateBriefingMetric {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 7px;
}
QFrame#teammateBriefingHero QLabel, QFrame#teammateBriefingSection QLabel,
QFrame#teammateBriefingContribution QLabel, QFrame#teammateBriefingMetric QLabel { background: transparent; }
#teammateBriefingEyebrow { color: #6639ba; font-size: 10px; font-weight: 750; }
#teammateBriefingTitle { color: #1f2328; font-size: 21px; font-weight: 750; }
#teammateBriefingIntro { color: #57606a; font-size: 11px; }
#teammateBriefingRailTitle, #teammateBriefingSectionHeading {
    color: #1f2328; font-size: 15px; font-weight: 750;
}
#teammateBriefingSectionTitle, #teammateBriefingContributionTitle {
    color: #1f2328; font-size: 13px; font-weight: 700;
}
#teammateBriefingMemberName, #teammateBriefingContributorName {
    color: #24292f; font-size: 12px; font-weight: 700;
}
#teammateBriefingContributorHandle { color: #1f2328; font-size: 11px; font-weight: 750; }
#teammateBriefingContributorRole { color: #656d76; font-size: 10px; }
#teammateBriefingBody { color: #424a53; font-size: 11px; }
#teammateBriefingMainPoints { color: #24292f; font-size: 15px; font-weight: 600; }
#teammateBriefingMuted, #teammateBriefingContributorMeta {
    color: #656d76; font-size: 10px;
}
#teammateBriefingMetricValue { color: #1f2328; font-size: 16px; font-weight: 750; }
#teammateBriefingMetricLabel { color: #656d76; font-size: 9px; font-weight: 650; }
#teammateBriefingStatus {
    color: #116329; background-color: #dafbe1; border: 1px solid #82e596;
    border-radius: 7px; padding: 2px 6px; font-size: 9px; font-weight: 700;
}
#teammateBriefingStatus[kind="scheduled"] { color: #6639ba; background-color: #f0e7ff; border-color: #d8b9ff; }
#teammateBriefingStatus[kind="waiting"] { color: #7d4e00; background-color: #fff8c5; border-color: #d4a72c; }
#teammateBriefingStatus[kind="idle"] { color: #656d76; background-color: #f6f8fa; border-color: #d0d7de; }
QPushButton#teammateBriefingGenerate {
    color: #ffffff; background-color: #8250df; border: 1px solid #6639ba;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 700;
}
QPushButton#teammateBriefingGenerate:hover { background-color: #6639ba; }
#teammateBriefingChevron { color: #656d76; }
QPushButton#teammateBriefingLink {
    color: #0969da; background: transparent; border: 0;
    padding: 1px 2px; font-size: 11px; font-weight: 700;
}
QPushButton#teammateBriefingLink:hover { color: #0550ae; background-color: #f6f8fa; }
QPushButton#teammateBriefingRelatedLink {
    color: #24292f; background-color: #f6f8fa; border: 1px solid #d0d7de;
    border-radius: 7px; padding: 3px 7px; font-size: 10px; font-weight: 650;
}
QPushButton#teammateBriefingRelatedLink:hover { color: #0550ae; border-color: #0969da; }
QPushButton#teammateBriefingChoice {
    color: #24292f; background-color: #ffffff; border: 1px solid #d0d7de;
    border-radius: 7px; padding: 6px 9px; font-size: 11px; font-weight: 650;
}
QPushButton#teammateBriefingChoice:hover { background-color: #f6f8fa; border-color: #8c959f; }
QPushButton#teammateBriefingChoice:checked { color: #6639ba; background-color: #f5f0ff; border-color: #8250df; }
QPushButton#teammateBriefingRunAgain {
    color: #ffffff; background-color: #8250df; border: 1px solid #6639ba;
    border-radius: 7px; padding: 6px 10px; font-size: 11px; font-weight: 700;
}
QPushButton#teammateBriefingRunAgain:hover { background-color: #6639ba; }
QPushButton#teammateBriefingRunAgain:disabled { color: #8c959f; background-color: #f6f8fa; border-color: #d0d7de; }
#teammateSkillUsageTitle, #teammatePerformanceTitle { color: #424a53; font-size: 13px; font-weight: 650; }
#teammateSkillUsageTotal, #teammatePerformanceTotal { color: #1f2328; font-size: 25px; font-weight: 500; }
#teammatePerformanceGrade, #teammateTaskGradeLabel { color: #656d76; font-size: 10px; font-weight: 650; }
QComboBox#teammateTaskGrade, QComboBox#agentTeammateTaskGrade { min-width: 162px; font-size: 11px; }
#teammateSkillUsageRange {
    color: #656d76; background-color: #f6f8fa;
    border: 1px solid #d0d7de; border-radius: 7px;
    padding: 3px 7px; font-size: 10px;
}
QPushButton#teammateCard {
    background-color: #ffffff; border: 1px solid #d0d7de;
    border-radius: 14px; text-align: left;
}
QPushButton#teammateCard[viewMode="list"] { border-radius: 9px; }
QPushButton#teammateCard[status="scheduled"] { border-color: #8250df; }
QPushButton#teammateCard[status="idle"] { border-color: #d4a72c; }
QPushButton#teammateCard[status="working"] { border-color: #2da44e; }
QPushButton#teammateCard:hover { border-color: #1a7f37; background-color: #f6f8fa; }
QPushButton#teammateCard QLabel { background: transparent; }
QWidget#teammateTableCell { border-right: 1px solid #d8dee4; }
QWidget#teammateTableCell[lastColumn="true"] { border-right: 0; }
QPushButton#teammateCard[viewMode="grid"] { border-color: transparent; background-color: #f6f8fa; }
QPushButton#teammateCard[viewMode="grid"]:hover { border-color: #d0d7de; background-color: #eef1f4; }
QProgressBar#teammateBoxProgress { border: 0; border-radius: 2px; background-color: #d0d7de; }
QProgressBar#teammateBoxProgress::chunk { border-radius: 2px; background-color: #0969da; }
#teammateName { font-size: 17px; font-weight: 750; }
#teammateHandle { color: #6639ba; font-size: 13px; font-weight: 700; }
#teammateHeroHandle, #teammateMentionPreview { color: #6639ba; font-size: 11px; }
#teammateSpecialty { font-size: 13px; font-weight: 650; color: #1f2328; }
#teammateFocus, #teammateLastActive { color: #656d76; font-size: 11px; }
#teammateModel, #teammateSchedule, #teammateActivity, #teammateCardMeta { font-size: 11px; color: #656d76; }
#teammateDescription { color: #424a53; font-size: 12px; }
#teammateOutput { color: #1f2328; font-size: 12px; }
#teammateSkillsEmpty { color: #656d76; font-size: 11px; font-style: italic; }
#teammateSkillAssignment {
    color: #424a53; background-color: #f6f8fa; border: 1px solid #d0d7de;
    border-radius: 7px; padding: 7px 9px; font-size: 11px;
}
QPlainTextEdit#teammatePromptInput, QPlainTextEdit#teammateProfilePromptInput {
    color: #1f2328; background-color: #ffffff; border: 1px solid #d0d7de;
    border-radius: 7px; padding: 4px 6px; font-size: 11px;
}
QPlainTextEdit#teammatePromptInput:focus, QPlainTextEdit#teammateProfilePromptInput:focus { border-color: #1a7f37; }
QPlainTextEdit#teammatePromptInput:disabled, QPlainTextEdit#teammateProfilePromptInput:disabled {
    color: #8c959f; background-color: #f6f8fa;
}
QPushButton#teammatePromptSend, QPushButton#teammateProfilePromptSend {
    color: #24292f; background-color: #f6f8fa; border: 1px solid #d0d7de;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 700;
}
QPushButton#teammatePromptSend:hover, QPushButton#teammateProfilePromptSend:hover {
    color: #ffffff; background-color: #1a7f37; border-color: #1a7f37;
}
QPushButton#teammatePromptSend:disabled, QPushButton#teammateProfilePromptSend:disabled {
    color: #8c959f; background-color: #f6f8fa; border-color: #d0d7de;
}
#teammateEyebrow, #teammateFieldLabel { color: #656d76; font-size: 10px; font-weight: 700; }
QPushButton#teammateAgentLink, QPushButton#teammateCardLink {
    color: #0969da; background: transparent; border: 0;
    padding: 2px 3px; font-size: 11px; font-weight: 650;
}
QPushButton#teammateAgentLink:hover, QPushButton#teammateCardLink:hover {
    color: #0550ae; background-color: #f6f8fa;
}
QPushButton#teammateRunButton {
    color: #116329; background-color: #dafbe1; border: 1px solid #82e596;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 700;
}
QPushButton#teammateRunButton:hover {
    color: #ffffff; background-color: #1a7f37; border-color: #1a7f37;
}
QPushButton#teammateStartButton {
    color: #ffffff; background-color: #1f883d; border: 1px solid #1a7f37;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 750;
}
QPushButton#teammateStartButton:hover { background-color: #1a7f37; border-color: #116329; }
QPushButton#teammateScheduleButton {
    color: #6639ba; background-color: #f0e7ff; border: 1px solid #8250df;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 750;
}
QPushButton#teammateScheduleButton:hover { color: #ffffff; background-color: #8250df; }
QPushButton#teammateRunButton[running="true"], QPushButton#teammateStopButton {
    color: #cf222e; background-color: #ffebe9; border: 1px solid #ff8182;
    border-radius: 7px; padding: 5px 9px; font-size: 11px; font-weight: 700;
}
QPushButton#teammateRunButton[running="true"]:hover, QPushButton#teammateStopButton:hover {
    color: #ffffff; background-color: #cf222e; border-color: #cf222e;
}
QLabel#teammatePill {
    color: #424a53; background-color: #f6f8fa; border: 1px solid #d0d7de;
    border-radius: 7px; padding: 2px 6px; font-size: 10px;
}
QLabel#teammatePill[kind="working"] { color: #116329; background-color: #dafbe1; border-color: #82e596; }
QLabel#teammatePill[kind="scheduled"] { color: #6639ba; background-color: #f0e7ff; border-color: #d8b9ff; }
QLabel#teammatePill[kind="waiting"] { color: #7d4e00; background-color: #fff8c5; border-color: #d4a72c; }
QLabel#teammatePill[kind="beta"] { color: #6639ba; background-color: #f0e7ff; border-color: #8250df; font-weight: 750; }
QLabel#teammatePill[kind="model"] { color: #0969da; background-color: #ddf4ff; border-color: #54aeff; }
QLabel#teammatePill[kind="model-empty"] { color: #656d76; background-color: #f6f8fa; border-color: #d0d7de; }
QLabel#teammatePill[kind="skill"] { color: #116329; background-color: #dafbe1; border-color: transparent; }
QLabel#teammatePill[kind="cadence"] { color: #6639ba; background-color: #f0e7ff; border-color: #d8b9ff; }
QLabel#teammatePill[kind="watch"] { color: #7d4e00; background-color: #fff8c5; border-color: #d4a72c; }
QLabel#teammatePill[kind="route"] { color: #0969da; background-color: #ddf4ff; border-color: #54aeff; }
QLabel#teammatePill[kind="duty"] { color: #424a53; background-color: #f6f8fa; border-color: #d0d7de; }
#teammateInvocation {
    background-color: #f0fff4; border: 1px solid #82e596;
    border-radius: 8px;
}
#teammateInvocationEyebrow {
    color: #116329; font-size: 9px; font-weight: 750;
}
#teammateInvocationTitle { color: #1f2328; font-size: 12px; font-weight: 700; }
#teammateInvocationCadence { color: #656d76; font-size: 10px; }
QFrame#teammateInvocation[embedded="true"] {
    background: transparent; border: 0; border-top: 1px solid #d8b9ff;
    border-radius: 0;
}
QFrame#teammateInvocation[embedded="true"] QLabel#teammateInvocationEyebrow { color: #6639ba; }
QFrame#teammateInvocation[embedded="true"] QLabel#teammateInvocationTitle { color: #1f2328; }
QFrame#teammateInvocation[embedded="true"][inline="true"] {
    border-top: 0; border-left: 1px solid #d8b9ff;
}
#teammateDivider { color: #d8dee4; background-color: #d8dee4; max-height: 1px; }
#teammateModelsHost, #teammateSkillsHost, #teammateDutiesHost, #teammateProfileSkillsHost,
#teammateProfileDutiesHost, #teammateInvocationGraphic {
    background: transparent;
}
#teammateStatusBanner {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 9px;
}
#teammateStatusBanner[kind="scheduled"] { background-color: #f0e7ff; border-color: #8250df; }
#teammateStatusBanner[kind="idle"] { background-color: #fff8c5; border-color: #bf8700; }
#teammateStatusBanner[kind="working"] { background-color: #dafbe1; border-color: #2da44e; }
#teammateStatusBanner[kind="waiting"] { background-color: #fff8c5; border-color: #d4a72c; }
#teammateStatusTitle { color: #1f2328; font-size: 14px; font-weight: 800; }
#teammateStatusDetail { color: #57606a; font-size: 10px; }
#teammateEmpty, #teammateHero, #teammateSection {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 10px;
}
#teammateHero[kind="scheduled"] { border-color: #8250df; }
#teammateHero[kind="working"] { border-color: #2da44e; }
#teammateHero[kind="waiting"] { border-color: #d4a72c; }
#teammateSidePanel {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 10px;
}
#teammateSideScroll { background: transparent; border: 0; }
#teammatePanelTitle, #teammateEditorPanelTitle {
    font-size: 15px; font-weight: 750; background: transparent;
}
QPushButton#teammatePanelClose {
    color: #656d76; background: transparent; border: 0; border-radius: 6px;
    padding: 4px;
}
QPushButton#teammatePanelClose:hover { color: #1f2328; background-color: #f3f4f6; }
#teammateEmptyTitle { font-size: 17px; font-weight: 700; background: transparent; }
#teammateHero QLabel, #teammateSection QLabel { background: transparent; }
#teammateHeroName { font-size: 25px; font-weight: 800; }
#teammateHeroRole { color: #424a53; font-size: 14px; font-weight: 600; }
#teammateSectionTitle { font-size: 14px; font-weight: 700; }
#teammateWorkTitle { font-size: 13px; font-weight: 650; }
#teammateActivityDot { background-color: #1a7f37; border-radius: 4px; }
#teammateActivityLine { color: #424a53; font-size: 12px; }
#teammateHomeChatScroll, #teammateHomeChat, #teammateHomeWidgets, #teammateHomeInfo,
#teammateHomeAvatarHost { background: transparent; border: 0; }
#teammateHomeYou {
    background-color: #ddf4ff; border: 1px solid #54aeff; border-radius: 10px;
}
#teammateHomeReply, #teammateHomeWidget, #genieSaid {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 9px;
}
#teammateHomeReply[status="running"] { border-color: #2da44e; }
#teammateHomeReply[status="failed"] { border-color: #cf222e; }
#genieSaid[verdict="wrong"] { border-color: #cf222e; }
#genieSaid[verdict="accurate"] { border-color: #2da44e; }
#teammateHomeProposal {
    background-color: #fbefff; border: 1px dashed #8250df; border-radius: 9px;
}
#teammateHomeYou QLabel, #teammateHomeReply QLabel, #teammateHomeWidget QLabel,
#teammateHomeProposal QLabel, #genieSaid QLabel { background: transparent; }
#teammateHomeYouText, #teammateHomeReplyText, #teammateHomeWidgetText { color: #1f2328; font-size: 12px; }
#teammateHomeWhen { color: #656d76; font-size: 10px; }
#teammateHomeChatEmpty, #teammateHomeWidgetsEmpty { color: #656d76; font-size: 12px; padding: 18px 8px; }
#teammateHomeGoalValue { color: #424a53; font-size: 12px; font-weight: 650; }
QPlainTextEdit#teammateHomeNotes, QPlainTextEdit#teammateHomeComposer {
    background-color: #ffffff; border: 1px solid #d0d7de; border-radius: 8px; padding: 4px;
}
#teammateLinkedShortcut { color: #0969da; font-size: 12px; }
#teammateInstructions, #teammateScheduleMonitor, #teammateAgentRoute,
#teammateCollaborationSummary { color: #424a53; font-size: 12px; }
#teammateSchedulePreview {
    color: #116329; background-color: #dafbe1; border: 1px solid #82e596;
    border-radius: 7px; padding: 7px 9px;
}
#teammateRelayPreview {
    color: #424a53; background-color: #f6f8fa; border: 1px solid #d0d7de;
    border-radius: 7px; padding: 6px 8px; font-size: 11px;
}
QGroupBox#teammateFormSection {
    background-color: #ffffff; border: 1px solid #d0d7de;
    border-radius: 9px; margin-top: 9px; font-weight: 700;
}
QGroupBox#teammateFormSection::title { subcontrol-origin: margin; left: 12px; padding: 0 5px; }
QListWidget#teammateModelPicker { background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 8px; }
QListWidget#teammateModelPicker::item { border: 1px solid transparent; border-radius: 8px; padding: 5px; }
QListWidget#teammateModelPicker::item:selected { background-color: #dafbe1; border-color: #1a7f37; color: #1f2328; }
#skillScopePill {
    color: #6639ba; background-color: #f0e7ff; border: 1px solid #d8b9ff;
    border-radius: 6px; padding: 2px 5px; font-size: 9px; font-weight: 750;
}
#skillCardFooter { color: #8c959f; font-size: 10px; }
#shortcutRunStats { color: #57606a; font-size: 10px; }
#shortcutRunEta { color: #57606a; font-size: 10px; }
QProgressBar#shortcutRunProgress { border: 0; border-radius: 2px; background-color: #d0d7de; }
QProgressBar#shortcutRunProgress::chunk { border-radius: 2px; background-color: #0969da; }
#shortcutRunPill {
    color: #1a7f37; background-color: #dafbe1; border: 1px solid #6fdd8b;
    border-radius: 6px; padding: 2px 5px; font-size: 9px; font-weight: 750;
}
#shortcutKindPill {
    color: #0969da; background-color: #ddf4ff; border: 1px solid #54aeff;
    border-radius: 6px; padding: 2px 5px; font-size: 9px; font-weight: 750;
}
#settingsTitle { font-size: 20px; font-weight: 800; }
#avatarPreview {
    background-color: #ffffff;
    border: 1px solid #d0d7de;
    border-radius: 12px;
    color: #6e7681;
    font-size: 11px;
}
#networkLog, #networkLogPopoutView {
    background-color: #ffffff;
    border: none;
    color: #1f2328;
    font-family: monospace;
    font-size: 12px;
}
#logTimelineChart { background-color: #ffffff; border-radius: 8px; }
#logTimelineSummary { color: #656d76; font-size: 12px; }
#logRangeButton { padding: 4px 10px; }
#logRangeButton:checked {
    color: #ffffff; background-color: #0969da; border-color: #0969da;
}
#logDock { background: transparent; border: none; }
#issueQuickAdd {
    background-color: #ffffff;
    border: 1px dashed #d0d7de;
    border-radius: 8px;
    padding: 5px 8px;
    font-size: 12px;
    min-height: 38px;
    max-height: 38px;
}
#issueQuickAdd:focus { border-color: #0969da; }
#promptWrapper {
    background-color: #ffffff; border: 1px solid rgba(26,127,55,0.5); border-radius: 8px;
}
#promptWrapper:focus-within { border-color: #1a7f37; }

#promptDragHandle { background: transparent; border: none; }
QPushButton#promptResetButton, QPushButton#promptChannelButton,
QPushButton#promptClearButton, QPushButton#promptCloudButton {
    background: transparent; border: none; padding: 0;
}
QPushButton#promptWorktreeCheck, QPushButton#promptVibeCheck {
    background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 0;
}
QPushButton#promptCloudButton:checked { background: rgba(9,105,218,0.14); border: 1px solid #0969da; border-radius: 4px; }
QPushButton#promptWorktreeCheck:checked { background: rgba(9,105,218,0.10); border-color: #0969da; }
QPushButton#promptVibeCheck:checked { background: rgba(130,80,223,0.16); border-color: #8250df; }
#promptDetachWindow, #promptOverlayHost { background: transparent; }
#footerLogPanel {
    background-color: #ffffff;
    border: 1px solid rgba(26,127,55,0.5);
    border-radius: 8px;
}
#promptWrapper #issueQuickAdd {
    background: transparent; border: 1px solid rgba(87,96,106,0.3); border-radius: 4px;
    min-height: 38px; max-height: 38px;
}
#promptWrapper #issueQuickAdd:focus { border: 1px solid rgba(87,96,106,0.3); }
QPushButton#quickAddSendIcon {
    background: transparent; border: none; color: #1a7f37;
    padding: 4px; border-radius: 4px;
}
QPushButton#quickAddSendIcon:hover { color: #1a7f37; background: rgba(26,127,55,0.12); }

QPushButton#quickAddSendIcon[enterTarget="true"] {
    border: 1px solid #1a7f37; background: rgba(26,127,55,0.08);
}

QPushButton#quickAddGenieButton {
    background: rgba(124,58,237,0.10); border: 1px solid rgba(124,58,237,0.45);
    color: #7c3aed; padding: 4px; border-radius: 4px;
    font-size: 12px; font-weight: 700;
}
QPushButton#quickAddGenieButton:hover {
    background: rgba(124,58,237,0.20); border-color: #7c3aed; color: #5b21b6;
}
QPushButton#quickAddGenieButton[enterTarget="true"] {
    background: rgba(124,58,237,0.22); border: 1px solid #7c3aed; color: #5b21b6;
}

QCheckBox#quickAddCreateIssueCheck::indicator,
QCheckBox#quickAddAgentCheck::indicator {
    width: 16px; height: 16px; border-radius: 4px;
    border: 2px solid #d0d7de; background: #ffffff;
}
QCheckBox#quickAddCreateIssueCheck::indicator:checked,
QCheckBox#quickAddAgentCheck::indicator:checked {
    border-color: #1a7f37; background: #1a7f37;
    image: url(:/icons/octicons/check-white.svg);
}
#quickAddAgentBox {
    border: none; background: transparent;
    font-size: 12px;
}

QPushButton#quickAddSlashButton {
    background: #ffffff; border: 1px solid #d0d7de; border-radius: 5px;
    color: #57606a; font-weight: 600; font-size: 12px; padding: 0;
}
QPushButton#quickAddSlashButton:hover { border-color: #0969da; color: #1f2328; }
#slashActionsPopup {
    background: #ffffff; border: 1px solid #d0d7de; border-radius: 8px;
}
QLineEdit#slashActionsFilter {
    background: transparent; border: none; border-bottom: 1px solid #d0d7de;
    border-radius: 0; padding: 10px 12px; color: #1f2328; font-size: 13px;
}
#slashActionsScroll { background: transparent; border: none; }
#slashActionsScroll > QWidget > QWidget { background: transparent; }
QLabel#slashActionsHeader {
    color: #6e7781; font-size: 11px; font-weight: 600;
    padding: 8px 12px 4px 12px;
}
QFrame#slashActionRow { background: transparent; border-radius: 6px; }
QFrame#slashActionRow[slashSelected="true"] { background: #eaeef2; }
QLabel#slashActionRowLabel { color: #1f2328; font-size: 13px; background: transparent; }
QLabel#slashActionRowValue { color: #6e7781; font-size: 12px; background: transparent; }
QToolButton#slashEffortDot {
    background: transparent; border: 2px solid #8c959f; border-radius: 5px;
}
QToolButton#slashEffortDot:checked { background: #1f2328; border-color: #1f2328; }
QCheckBox#slashToggle::indicator {
    width: 28px; height: 16px; border-radius: 8px; border: none; background: #d0d7de;
}
QCheckBox#slashToggle::indicator:checked { background: #1a7f37; }
#issuePageTitle {
    font-size: 26px;
    font-weight: 400;
    color: #1f2328;
    background: transparent;
}
#issueDivider { background-color: #d0d7de; }
QPushButton#issueIconButton {
    background: transparent;
    border: none;
    border-radius: 6px;
    padding: 4px;
}
QPushButton#issueIconButton:hover { background-color: #eaeef2; }
QPushButton#inlineHelpButton {
    background: transparent;
    border: none;
    border-radius: 6px;
    padding: 4px;
}
QPushButton#inlineHelpButton:hover { background-color: #eaeef2; }
QPushButton#logFrequencyPromptButton {
    background: transparent;
    border: none;
    border-radius: 6px;
    padding: 2px;
}
QPushButton#logFrequencyPromptButton:hover { background-color: #eaeef2; }
#issuePageScroll {
    background: transparent;
    border: none;
}
/* Exact-class selector on purpose: plain container widgets go transparent,
   but QPushButton/QLabel keep their own rules. A bare "QWidget" here also
   matches subclasses and outranks QPushButton#primaryButton, which painted
   the thread's Comment/Merge buttons white-on-white. */
#issuePageScroll .QWidget { background: transparent; }
#issueAvatar {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 18px;
    color: #1f2328;
    font-size: 11px;
    font-weight: 800;
}
#issueCommentTitle {
    font-size: 16px;
    font-weight: 700;
    color: #1f2328;
    background: transparent;
}
#issueCommentEditor {
    background-color: #ffffff;
    border: 1px solid #d0d7de;
    border-radius: 8px;
}
#issueCommentCard {
    background-color: #ffffff;
    border: 1px solid #1a7f37;
    border-radius: 8px;
}
#issueCommentEditor QPlainTextEdit#markdownSource,
#issueCommentEditor QTextBrowser#markdownPreview {
    border: none;
    border-top-left-radius: 0;
    border-top-right-radius: 0;
}
#issueCommentEditor QPushButton#markdownTab {
    border-top-color: transparent;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
#issueTimelineRow, #issueTimelineIndent, #issueReactionsRow { background: transparent; }
#issueTimelineCard {
    background-color: #ffffff;
    border: 1px solid #1a7f37;
    border-radius: 8px;
}
#issueTimelineCard QLabel { background: transparent; }
#issueTimelineHeader {
    background-color: #dafbe1;
    border-bottom: 1px solid #1a7f37;
    border-top-left-radius: 8px;
    border-top-right-radius: 8px;
}
#issueTimelineHeader QLabel {
    background: transparent;
    color: #1f2328;
}
#issueActivityAvatar {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 12px;
    color: #1f2328;
    font-size: 9px;
    font-weight: 800;
}
#issueActivityCard {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 8px;
    padding: 7px 10px;
}
#issueLabelPill {
    background-color: #f6f8fa;
    border: 1px solid;
    border-radius: 10px;
    padding: 2px 8px;
    font-size: 11px;
    font-weight: 600;
}
#issueDetailTabs::pane { border: 0; }
#issueAttachmentPreview {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 6px;
    padding: 6px;
}
QToolButton#issueActionButton, QPushButton#issueActionButton {
    background: transparent;
    border: none;
    border-radius: 6px;
    color: #656d76;
    font-weight: 800;
    padding: 4px 8px;
}
QToolButton#issueActionButton:hover, QPushButton#issueActionButton:hover { background-color: #cceaff; }
QToolButton#issueActionButton::menu-indicator { image: none; width: 0; }
#issueChecklist, #issueChecklistRow, #issueChecklistMarkdown {
    background: transparent;
}
QLabel#issueChecklistLabel[completed="true"] { color: #656d76; }
#issueDependencies { background: transparent; border-top: 1px solid #d0d7de; }
#issueSubIssues { background: transparent; border: 1px solid #d0d7de; border-radius: 6px; }
#issueParentRef { background: transparent; }
QLabel#issueParentCaption { color: #656d76; font-size: 12px; padding: 4px 0; }
QPushButton#issueParentPill {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 12px;
    color: #1f2328; font-size: 12px; font-weight: 600; padding: 3px 10px;
}
QPushButton#issueParentPill:hover { background-color: #eaeef2; border-color: #8c959f; }
QPushButton#issueParentPill[status="closed"], QPushButton#issueParentPill[status="unknown"] { color: #656d76; }
QLabel#issueDependenciesHeading {
    color: #656d76; font-size: 12px; font-weight: 700; padding-top: 8px;
}
QPushButton#issueDependencyRow {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 6px;
    text-align: left; padding: 0;
}
QPushButton#issueDependencyRow:hover { background-color: #eaeef2; border-color: #8c959f; }
QLabel#issueDependencyNumber { color: #656d76; font-size: 12px; font-weight: 600; }
QLabel#issueDependencyTitle { color: #1f2328; font-weight: 600; }
QLabel#issueDependencyTitle[status="closed"], QLabel#issueDependencyTitle[status="unknown"] { color: #656d76; }
QLabel#issueDependencyStatus {
    border-radius: 9px; padding: 2px 8px; font-size: 11px; font-weight: 700;
    color: #ffffff;
}
QLabel#issueDependencyStatus[status="open"] { background-color: #1a7f37; }
QLabel#issueDependencyStatus[status="closed"] { background-color: #8250df; }
QLabel#issueDependencyStatus[status="unknown"] {
    background-color: transparent; border: 1px solid #d0d7de; color: #656d76;
}
QPushButton#issueChecklistDrag {
    background: transparent; border: none; border-radius: 6px; color: #8c959f;
    font-size: 18px; font-weight: 700; padding: 0;
}
QPushButton#issueChecklistDrag:hover, QPushButton#issueChecklistDrag:focus {
    background-color: #ddf4ff; color: #0969da;
}
QPushButton#issueChecklistConvert, QPushButton#issueChecklistAttached {
    background-color: #f6f8fa; border: 1px solid #d0d7de; border-radius: 12px;
    color: #57606a; font-size: 11px; font-weight: 650; padding: 4px 8px;
}
QPushButton#issueChecklistConvert:hover {
    background-color: #dafbe1; border-color: #1a7f37; color: #1a7f37;
}
QPushButton#issueChecklistAttached {
    background-color: #fbefff; border-color: #8250df; color: #6639ba;
}
QPushButton#issueChecklistAttached:hover {
    background-color: #f3e8ff; border-color: #6639ba; color: #512a97;
}
#issueSidebar { background: transparent; }
#issueSidebar QLabel { background: transparent; }
#issueSidebarHeading {
    color: #57606a;
    font-size: 12px;
    font-weight: 700;
    background: transparent;
}
#issueSidebarDivider { background-color: #d0d7de; }
QPushButton#issueSidebarAction, QPushButton#issueDangerLink {
    background: transparent;
    border: none;
    color: #1f2328;
    font-weight: 500;
    padding: 6px 2px;
    text-align: left;
}
QPushButton#issueSidebarAction:hover { color: #0969da; }
QPushButton#issueDangerLink { color: #cf222e; }
QPushButton#issueDangerLink:hover { color: #a40e26; }
QMenu {
    background-color: #ffffff;
    border: 1px solid #d0d7de;
    border-radius: 8px;
    padding: 6px;
    color: #1f2328;
}
QMenu::item {
    padding: 7px 26px 7px 22px;
    border-radius: 6px;
}
QMenu::item:selected { background-color: #f6f8fa; }
QListWidget#assigneePickerList, QListWidget#reporterPickerList {
    color: #1f2328;
}
QListWidget#assigneePickerList::item, QListWidget#reporterPickerList::item {
    color: #1f2328;
    padding: 3px 4px;
    border-radius: 6px;
}
QListWidget#assigneePickerList::item:hover, QListWidget#reporterPickerList::item:hover,
QListWidget#assigneePickerList::item:selected, QListWidget#reporterPickerList::item:selected {
    background-color: #eaeef2;
    color: #1f2328;
}
QMenu::separator {
    height: 1px;
    background: #d0d7de;
    margin: 6px 0;
}
QLineEdit#issueSearch, QComboBox#issueControlSm {
    background-color: #ffffff;
    border: 1px solid #d0d7de;
    border-radius: 8px;
    padding: 4px 8px;
    font-size: 12px;
    min-height: 24px;
    max-height: 28px;
}
QLineEdit#issueSearch:focus, QComboBox#issueControlSm:focus { border-color: #0969da; }

QLineEdit#messageInput {
    background-color: #ffffff;
    border: 1px solid #afb8c1;
    border-radius: 8px;
    padding: 6px 10px;
}
QLineEdit#messageInput:focus { border-color: #0969da; }
QPlainTextEdit#issueComposerSm {
    background-color: #ffffff;
    color: #1f2328;
    border: 1px solid #d0d7de;
    border-radius: 8px;
    padding: 8px;
    selection-background-color: rgba(46, 160, 67, 46);
}
QPlainTextEdit#issueComposerSm:focus { border: 1px solid #0969da; }
#issueComposeSidebar { background: transparent; }
QPushButton#markdownTab {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-bottom: none;
    border-radius: 0;
    color: #656d76;
    padding: 8px 16px;
}
QPushButton#markdownTab:checked {
    background-color: #ffffff;
    color: #1f2328;
}
QToolButton#markdownTool {
    background: transparent;
    border: none;
    border-radius: 4px;
    color: #656d76;
    padding: 5px 7px;
    font-weight: 700;
}
QToolButton#markdownTool:hover {
    background-color: #eaeef2;
    color: #1f2328;
}
QPlainTextEdit#markdownSource, QTextBrowser#markdownPreview {
    background-color: #ffffff;
    color: #1f2328;
    border: 1px solid #d0d7de;
    border-radius: 6px;
    padding: 14px 16px;
    selection-background-color: #0969da;
    selection-color: #ffffff;
}
QPlainTextEdit#markdownSource:focus { border-color: #0969da; }
#mentionPopup {
    background-color: #ffffff;
    color: #1f2328;
    border: 1px solid #d0d7de;
    border-radius: 6px;
    outline: 0;
}
#mentionPopup::item { padding: 4px 10px; }
#mentionPopup::item:selected { background-color: #0969da; color: #ffffff; }
#issueTable {
    background-color: #ffffff;
    alternate-background-color: #ffffff;
    border: 1px solid #d0d7de;
    border-radius: 8px;
    gridline-color: #eaeef2;
    selection-background-color: #0969da;
    selection-color: #ffffff;
}
QTableWidget#issueTable[repositoryGrid="true"] { gridline-color: #d0d7de; }
#issueTable::item { padding: 6px 8px; color: #1f2328; }
#issueTable::item:hover { padding: 6px 8px; }
#issueTable::item:selected { background-color: #0969da; color: #ffffff; padding: 6px 8px; }
#issueTable QHeaderView::section {
    background-color: #ffffff;
    color: #656d76;
    padding: 6px 8px;
    border: none;
    border-bottom: 1px solid #d0d7de;
    border-right: 2px solid #afb8c1;
    font-weight: 700;
}
#issueTable QTableCornerButton::section {
    background-color: #ffffff;
    border: none;
    border-bottom: 1px solid #d0d7de;
}
#issueList {
    background-color: #ffffff; border: none; outline: 0;
    selection-background-color: transparent; gridline-color: transparent;
}
#issueList::item { padding: 0; border: none; }
#issueList::item:selected { background: transparent; color: #1f2328; }

#organizationTasksTable {
    background-color: #ffffff;
    alternate-background-color: #ffffff;
    border: none;
    gridline-color: transparent;
    selection-background-color: transparent;
    selection-color: #ffffff;
    outline: 0;
}
#actionWorkflowList {
    background-color: #ffffff;
    alternate-background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 8px;
    outline: 0;
    padding: 4px;
}
#actionWorkflowList::item {
    border-radius: 6px;
    color: #1f2328;
    padding: 7px 10px;
}
#actionWorkflowList::item:hover {
    background-color: #f6f8fa;
    color: #1f2328;
}
#actionWorkflowList::item:selected {
    background-color: #1f883d;
    color: #ffffff;
}
#codeEditor {
    background-color: #ffffff;
    border: 1px solid #d0d7de;
    border-radius: 8px;
    color: #1f2328;
    /* A QSS family list only resolves to a face that is installed; the generic
       alias at the end does not fall back through fontconfig, so name the
       usual Linux monospace faces too or the editor paints proportional. */
    font-family: "SF Mono", "Menlo", "Consolas", "DejaVu Sans Mono",
        "Liberation Mono", "Noto Sans Mono", monospace;
    font-size: 12px;
    selection-background-color: #0969da;
    selection-color: #ffffff;
}
#codeLineNumberArea {
    background-color: #ffffff;
    border-right: 1px solid #eaeef2;
}
#diffView {
    background-color: #ffffff;
    border: none;
    border-radius: 0;
    color: #1f2328;
    font-family: monospace;
    font-size: 12px;
}
#readmeView {
    background: transparent;
    border: none;
    padding: 0;
    color: #1f2328;
}

QPlainTextEdit#actionLog {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 8px;
    color: #1f2328;
    padding: 10px 12px;
    selection-background-color: #0969da;
    selection-color: #ffffff;
}
QToolButton#agentStatusPill {
    background-color: transparent;
    border: 1px solid #d0d7de;
    border-radius: 24px;
    padding: 0px 10px 0px 3px;
    font-size: 11px;
    font-weight: 600;
}
QToolButton#agentStatusPill:hover { background-color: #f6f8fa; }
QToolButton#agentStatusPill[outcomeTone="success"] { border-color: #3fb950; }
QToolButton#agentStatusPill[outcomeTone="failure"] { border-color: #f85149; }
QToolButton#agentStatusPill[outcomeTone="pending"] { border-color: #e3742f; }
#agentNetPanel {
    background-color: #f6f8fa;
    border: 1px solid #d0d7de;
    border-radius: 8px;
    padding: 8px 12px;
}

QAbstractItemView::item { border: 0px; }
QAbstractItemView::item:selected { background-color: palette(highlight); color: palette(highlighted-text); }
#coveSharedWithList { selection-background-color: #1f883d; selection-color: #ffffff; }
#issueTable { selection-background-color: #1f883d; selection-color: #ffffff; }
#issueTable::item:selected,
#overviewList::item:selected,
#fileTree::item:selected,
#commitsList::item:selected,
#coveSharedWithList::item:selected,
#sidebar QListWidget::item:selected,
#sidebar QTreeWidget::item:selected { background-color: #1f883d; color: #ffffff; }
QComboBox QAbstractItemView { selection-background-color: #1f883d; selection-color: #1f2328; }
QComboBox QAbstractItemView::item:selected { color: #1f2328; }

QPushButton#scrollJump {
    background-color: #f6f8fa; color: #1f2328;
    border: 1px solid #d0d7de; border-radius: 15px;
    font-size: 12px; font-weight: 700;
}
QPushButton#scrollJump:hover { background-color: #eef1f5; }

/* Armed force deploy; see the dark sheet. */
QPushButton#siteDeployForceButton:checked {
    background-color: #cf222e; border-color: #cf222e; color: #ffffff;
}
QPushButton#siteDeployForceButton:checked:hover { background-color: #a40e26; }

)";

inline const char *styleSheetForDark(bool dark)
{
    return dark ? kStyleSheet : kLightStyleSheet;
}

inline QString styleSheetForPalette(bool dark,
                                    const QMap<QString, QString> &palette,
                                    const QString &overlay = {})
{
    QString style = QString::fromLatin1(styleSheetForDark(dark));
    for (auto it = palette.constBegin(); it != palette.constEnd(); ++it)
        style.replace(it.key(), it.value(), Qt::CaseInsensitive);
    if (!overlay.isEmpty()) {
        style += QLatin1Char('\n');
        style += overlay;
    }
    return style;
}

} // namespace Theme
