#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

// The provider-neutral inventory of coding-agent tool families ForkMesh
// tracks. Every runtime tool call (Claude Code, Codex app-server, MCP) is
// folded into one of these families so the efficiency board, permission
// prompts, and future host tools share one vocabulary regardless of which
// runtime executed the call.
//
// The inventory mirrors the complete public tool surface of the reference
// coding agent (28 canonical + browser/computer/image/speech + 3 hidden
// control tools) plus two ForkMesh-specific buckets: `mcp` for extension
// tools that match no family, and `other` for anything unrecognised.
namespace AgentToolAvailability {
// Executed today by a bundled runtime and observed through the transcript.
inline const QString Native = QStringLiteral("native");
// Reaches ForkMesh only through an optional Marketplace package or an MCP
// server; observed when that package is installed and used.
inline const QString Package = QStringLiteral("package");
// No runtime path yet; listed so coverage gaps stay visible.
inline const QString Planned = QStringLiteral("planned");
} // namespace AgentToolAvailability

namespace AgentToolBaseline {
// How the ledger estimates the naive-alternative cost for a call. Families
// without a measurable baseline report consumed tokens only.
inline const QString None = QStringLiteral("none");
// Whole-file size versus the bytes the tool actually returned.
inline const QString PartialRead = QStringLiteral("partial-read");
// Rewriting the whole file versus the anchored patch the model emitted.
inline const QString AnchoredEdit = QStringLiteral("anchored-edit");
// Reading every file named in the result versus the matches returned.
inline const QString MatchedFiles = QStringLiteral("matched-files");
} // namespace AgentToolBaseline

struct AgentToolFamily {
    QString id;
    QString displayName;
    QString category;
    QString summary;
    QString availability;  // AgentToolAvailability::*
    QString availabilityNote;
    QString baseline;      // AgentToolBaseline::*
    QString expectedSavings; // planning estimate shown until data exists
    bool hidden = false;   // internal control tools
};

const QVector<AgentToolFamily> &agentToolFamilies();
const AgentToolFamily *agentToolFamily(const QString &id);
QStringList agentToolFamilyIds();
QStringList agentToolCategories();

// Maps a runtime tool name (plus its input, used for MCP heuristics) to a
// family id. Unknown names map to "other"; MCP tools that match no family
// map to "mcp".
QString agentToolFamilyFor(const QString &runtimeToolName,
                           const QJsonObject &input = QJsonObject());
