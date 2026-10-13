#pragma once

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

// Operator policy for switching tool families off when they perform worse
// than the alternative. The desktop stores the disabled family ids; every
// runtime launch carries them in AgentRuntimeLaunch.options so each adapter
// can translate the policy into whatever its runtime understands:
//
//   Claude Code   --disallowedTools with the family's runtime tool names
//   Codex         -c config overrides for the tools it can switch off, and
//                 the host permission broker denying shell execution
//
// Families a runtime cannot switch off are still tracked: the ledger counts
// calls made while a family is off so the board shows whether the policy
// was honoured.
namespace AgentToolPolicy {

// QSettings key holding the disabled family ids.
inline const QString DisabledFamiliesSetting =
    QStringLiteral("agents/disabledToolFamilies");
// AgentRuntimeLaunch.options key carrying the disabled family ids (array of
// strings) to a runtime adapter.
inline const QString LaunchOption = QStringLiteral("disabledToolFamilies");

// Keeps only known catalog family ids, sorted and unique.
QStringList normalizeDisabledFamilies(const QStringList &raw);
QStringList disabledFamiliesFromOptions(const QJsonObject &options);
QJsonObject optionsWithDisabledFamilies(QJsonObject options,
                                        const QStringList &disabled);

inline const QString AllowlistOption = QStringLiteral("allowedToolFamilies");
QStringList allowedFamiliesFromOptions(const QJsonObject &options, bool *active = nullptr);
QJsonObject optionsWithAllowedFamilies(QJsonObject options, bool active,
                                       const QStringList &allowed);
QStringList familiesOutsideAllowlist(const QStringList &allowedFamilies);
QStringList claudeAvailableTools(const QStringList &allowedFamilies,
                                 const QStringList &disabledFamilies);

// Claude Code tool names to pass through --disallowedTools.
QStringList claudeDisallowedTools(const QStringList &disabledFamilies);
// Codex `-c key=value` overrides (each entry is one override).
QStringList codexConfigOverrides(const QStringList &disabledFamilies);
// Whether the host broker must deny shell execution for this policy.
bool deniesProcessExecution(const QStringList &disabledFamilies);

// One-line description of how a family is enforced when switched off, for
// tooltips and notices.
QString enforcementNote(const QString &family);

} // namespace AgentToolPolicy
