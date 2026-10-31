#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

namespace forkmesh::workspaceshortcuts {

enum class Action {
    NewSession,
    NextSession,
    PreviousSession,
    NextSessionAlt,
    PreviousSessionAlt,
    ToggleDiff,
    ToggleTerminal,
    ToggleBrowser,
    ClosePanel,
    ModeMenu,
    ModelMenu,
    EffortMenu,
    CycleMode,
    ShowShortcuts,
};

struct Binding {
    Action action;
    QKeySequence keys;
    QString label;
    bool bound = true;
};

inline Qt::Modifier physicalControl()
{
#ifdef Q_OS_MACOS
    return Qt::META;
#else
    return Qt::CTRL;
#endif
}

inline QList<Binding> bindings()
{
    const Qt::Modifier control = physicalControl();
    return {
        {Action::NewSession, QKeySequence(Qt::CTRL | Qt::Key_N),
         QStringLiteral("New session"), false},
        {Action::NextSession, QKeySequence(control | Qt::Key_Tab),
         QStringLiteral("Next session")},
        {Action::PreviousSession, QKeySequence(control | Qt::SHIFT | Qt::Key_Backtab),
         QStringLiteral("Previous session")},
        {Action::NextSessionAlt, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketRight),
         QStringLiteral("Next session")},
        {Action::PreviousSessionAlt,
         QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_BracketLeft),
         QStringLiteral("Previous session")},
        {Action::ToggleDiff, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_D),
         QStringLiteral("Show or hide the diff")},
        {Action::ToggleTerminal, QKeySequence(control | Qt::Key_QuoteLeft),
         QStringLiteral("Show or hide the terminal")},
        {Action::ToggleBrowser, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B),
         QStringLiteral("Show or hide the browser")},
        {Action::ClosePanel, QKeySequence(Qt::CTRL | Qt::Key_Backslash),
         QStringLiteral("Close the side panel")},
        {Action::ModeMenu, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_M),
         QStringLiteral("Permission mode menu")},
        {Action::ModelMenu, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_I),
         QStringLiteral("Model menu")},
        {Action::EffortMenu, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_E),
         QStringLiteral("Effort menu")},
        {Action::CycleMode, QKeySequence(Qt::SHIFT | Qt::Key_Backtab),
         QStringLiteral("Next permission mode (in the prompt)"), false},
        {Action::ShowShortcuts, QKeySequence(Qt::CTRL | Qt::Key_Slash),
         QStringLiteral("Show keyboard shortcuts")},
    };
}

inline QString keysText(const QKeySequence &keys)
{
    QString text = keys.toString(QKeySequence::NativeText);
    text.replace(QStringLiteral("Shift+Backtab"), QStringLiteral("Shift+Tab"));
    text.replace(QStringLiteral("Backtab"), QStringLiteral("Shift+Tab"));
    return text;
}

}
