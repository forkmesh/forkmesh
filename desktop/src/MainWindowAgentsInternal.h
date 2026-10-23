#pragma once

// File-local helpers shared by the MainWindowAgent*.cpp translation units
// (the Agents page was one 20k-line file). Include only from those files:
// everything here sits in an anonymous namespace, so each including TU gets
// its own internal-linkage copy, exactly as when they lived in one .cpp.


#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "AccountCapability.h"
#include "ForkMeshPackageRegistry.h"
#include "RepoStatsStore.h"
#include "AgentCliLocator.h"
#include "AgentTicker.h"
#include "AgentJail.h"
#include "AgentCapabilityBroker.h"
#include "AgentCapabilityStore.h"
#include "AgentPermissionController.h"
#include "AgentPromptImages.h"
#include "AgentToolCatalog.h"
#include "AgentToolEfficiencyBoard.h"
#include "AgentGuardrailBoard.h"
#include "AgentToolPolicy.h"
#include "AgentRuntimeRegistry.h"
#include "AgentResumeIdentity.h"
#include "AgentWorktree.h"
#include "FlowLayout.h"
#include "KebabHeaderView.h"
#include "CodexAppServerSession.h"
#include "NodeEventSocket.h"
#include "UsageLimitCalendar.h"

#include <QActionGroup>
#include <QComboBox>
#include <QDebug>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTabBar>
#include <QTextLayout>
#include <QTextOption>
#include <QUrl>
#include <QEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QButtonGroup>
#include <QFrame>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QThreadPool>

#include <algorithm>
#include <limits>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#else
#include <csignal>
#endif

class QPainterPath;

namespace forkmesh::ui {
bool paintWorkspaceSessionRow(QPainter *painter,
                              const QStyleOptionViewItem &option,
                              const QModelIndex &index, bool hovered,
                              QColor *fill, QPainterPath *shape);
}

using namespace forkmesh::ui;

namespace {

template <typename T>
inline void releaseOffGuiThread(T &&value)
{
    using Held = std::decay_t<T>;
    QThreadPool::globalInstance()->start(
        [held = Held(std::forward<T>(value))]() mutable { Held drop = std::move(held); });
}

inline QString agentMergeBase(const AgentSession &s);

inline QString usageExhaustedSetting(const QString &providerKey,
                             const QString &windowKey);

inline QString usageResetSetting(const QString &providerKey,
                         const QString &windowKey);

inline QString briefFailureReason(const AgentSession &session)
{
    QString why = session.lastError.simplified();
    why = why.left(120);
    return why;
}

inline QString agentStatusBadgeText(const AgentSession &session)
{
    QString state;
    if (session.merged)
        state = QStringLiteral("merged");
    else if (session.status == AgentStatus::Success)
        state = QStringLiteral("done");
    else if (session.status == AgentStatus::Failed)
        state = QStringLiteral("failed");
    else if (session.status == AgentStatus::Stopped)
        state = QStringLiteral("stopped");
    else if (session.finishingBackgroundWork())
        state = QStringLiteral("finishing");
    else if (session.status == AgentStatus::Running)
        state = QStringLiteral("running");
    else if (session.status == AgentStatus::Waiting)
        state = QStringLiteral("waiting");
    else if (session.awaitingStart())
        state = QStringLiteral("paused");
    else if (session.status == AgentStatus::Queued)
        state = QStringLiteral("queued");
    else
        state = QStringLiteral("idle");
    const QString modelWord = agentModelShortLabel(session.model);
    if (modelWord.isEmpty()) {
        state[0] = state.at(0).toUpper();
        return state;
    }
    return QStringLiteral("%1 is: %2").arg(modelWord, state);
}

constexpr int kAgentStatusPillIconPx = 44;

// What a bare Continue carries: the same words whether the session is being
// resumed from a stored conversation or nudged into a process that is still up.
inline QString continueWhereLeftOffPrompt()
{
    return QStringLiteral("Continue where you left off.");
}

inline void parseAgentOwnedLog(const QByteArray &output, QSet<QString> *paths,
                        QStringList *commits = nullptr)
{
    if (!paths)
        return;
    for (const QString &raw : QString::fromUtf8(output).split(
             QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QChar(0x1e))) {
            if (commits)
                commits->append(line.mid(1));
            continue;
        }
        paths->insert(line);
    }
}

inline bool readAgentOwnedPaths(const QString &gitDir, const QString &base,
                         const QString &branch, QSet<QString> *paths)
{
    if (!paths || gitDir.isEmpty() || base.isEmpty() || branch.isEmpty())
        return false;
    QByteArray out;
    if (!runGitCapture(
            gitDir,
            {QStringLiteral("log"), QStringLiteral("--no-merges"),
             QStringLiteral("--cherry-pick"), QStringLiteral("--right-only"),
             QStringLiteral("--format=%x1e%h %s"),
             QStringLiteral("--name-only"),
             base + QStringLiteral("...") + branch},
            &out, nullptr))
        return false;
    parseAgentOwnedLog(out, paths);
    return true;
}

inline QStringList literalPathspecs(const QSet<QString> &paths)
{
    QStringList sorted = paths.values();
    std::sort(sorted.begin(), sorted.end());
    for (QString &path : sorted)
        path.prepend(QStringLiteral(":(literal)"));
    return sorted;
}

inline QHash<QString, QString> backgroundWorktrees(const QString &gitDir)
{
    QHash<QString, QString> result;
    QByteArray out;
    if (gitDir.isEmpty() ||
        !runGitCapture(gitDir,
                       {QStringLiteral("worktree"), QStringLiteral("list"),
                        QStringLiteral("--porcelain")},
                       &out, nullptr))
        return result;
    QString path;
    for (const QByteArray &raw : out.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.startsWith(QLatin1String("worktree "))) {
            path = line.mid(9).trimmed();
        } else if (line.startsWith(QLatin1String("branch refs/heads/")) &&
                   !path.isEmpty() &&
                   QDir(path).absolutePath() != QDir(gitDir).absolutePath()) {
            result.insert(line.mid(18).trimmed(), path);
        } else if (line.isEmpty()) {
            path.clear();
        }
    }
    return result;
}

inline QStringList localProviderCredentialValues()
{
    // Asked for once per streamed transcript event (redaction) and per log
    // refresh. Two JSON credential files plus a QSettings construction each
    // time was the bulk of the per-event cost; the answer only moves when a
    // credential file is rewritten (token refresh) or a key setting changes,
    // so serve a copy keyed on the files' stamps for up to a second.
    struct Memo {
        QString stamp;
        qint64 atMs = 0;
        QStringList values;
    };
    static Memo memo;
    static QMutex memoMutex;
    const QString claudePath = agentAccountCredentialPath(
        QStringLiteral("claude-code"),
        activeAgentAccount(QStringLiteral("claude-code")).configDir);
    const QString codexPath = agentAccountCredentialPath(
        QStringLiteral("codex"),
        activeAgentAccount(QStringLiteral("codex")).configDir);
    const auto fileStamp = [](const QString &path) {
        const QFileInfo info(path);
        return path + QLatin1Char('@') +
               QString::number(info.lastModified().toMSecsSinceEpoch()) +
               QLatin1Char('/') + QString::number(info.size());
    };
    const QString stamp = fileStamp(claudePath) + QLatin1Char('|') + fileStamp(codexPath);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    {
        QMutexLocker lock(&memoMutex);
        if (memo.atMs > 0 && memo.stamp == stamp && nowMs - memo.atMs < 1000)
            return memo.values;
    }
    QStringList values;
    QFile credentials(claudePath);
    if (credentials.open(QIODevice::ReadOnly)) {
        const QJsonObject oauth =
            QJsonDocument::fromJson(credentials.readAll())
                .object()
                .value(QStringLiteral("claudeAiOauth"))
                .toObject();
        values << oauth.value(QStringLiteral("accessToken")).toString()
               << oauth.value(QStringLiteral("refreshToken")).toString();
    }
    QFile codexAuth(codexPath);
    if (codexAuth.open(QIODevice::ReadOnly)) {
        const QJsonObject auth =
            QJsonDocument::fromJson(codexAuth.readAll()).object();
        const QJsonObject tokens = auth.value(QStringLiteral("tokens")).toObject();
        values << tokens.value(QStringLiteral("access_token")).toString()
               << tokens.value(QStringLiteral("refresh_token")).toString()
               << tokens.value(QStringLiteral("openAiApiKey")).toString()
               << auth.value(QStringLiteral("access_token")).toString();
    }
    const QSettings settings;
    values << settings.value(kClaudeApiKeySetting).toString()
           << settings.value(kClaudeAdminKeySetting).toString()
           << settings.value(kCodexApiKeySetting).toString()
           << settings.value(kOpenAiAdminKeySetting).toString()
           // Genie's remote-MCP bearer credential rides inside the
           // opening prompt, so it would otherwise land in the transcript, the
           // run log and the sealed snapshot pushed to the relay.
           << settings.value(kGenieTokenSetting).toString();
    values << qEnvironmentVariable("ANTHROPIC_API_KEY")
           << qEnvironmentVariable("ANTHROPIC_AUTH_TOKEN")
           << qEnvironmentVariable("CLAUDE_CODE_OAUTH_TOKEN")
           << qEnvironmentVariable("OPENAI_API_KEY");
    values.removeAll(QString());
    values.removeDuplicates();
    QMutexLocker lock(&memoMutex);
    memo.stamp = stamp;
    memo.atMs = nowMs;
    memo.values = values;
    return values;
}

inline QString agentAccountLabelFor(const QString &provider, const QString &accountId)
{
    if (accountId.isEmpty())
        return QString();
    for (const AgentAccountProfile &profile : agentAccountProfiles(provider))
        if (profile.id == accountId)
            return profile.label.trimmed();
    return QString();
}

inline QString redactProviderCredentials(QString text,
                                  const QStringList &credentialValues)
{
    for (const QString &secret : credentialValues) {
        if (secret.size() >= 8 && text.contains(secret))
            text.replace(secret, QStringLiteral("***"));
    }
    static const QRegularExpression bearerHeader(
        QStringLiteral(
            "(?i)(authorization\\s*:\\s*bearer\\s+)[A-Za-z0-9._~+/-]{8,}"));
    static const QRegularExpression credentialAssignment(
        QStringLiteral(
            "(?i)([\"']?(?:accessToken|refreshToken|apiKey|"
            "anthropicApiKey|openAiApiKey|authorization)[\"']?"
            "\\s*[:=]\\s*[\"'])[^\"'\\r\\n]+"));
    static const QRegularExpression skToken(
        QStringLiteral("\\bsk-[A-Za-z0-9_-]{20,}"));
    // Tool results can be megabytes long.  Avoid three full regular-expression
    // passes when none of the credential spellings occur; matched content is
    // still redacted by the same expressions below.
    if (text.contains(QStringLiteral("authorization"), Qt::CaseInsensitive))
        text.replace(bearerHeader, QStringLiteral("\\1***"));
    bool mayContainAssignment = false;
    for (const QString &key :
         {QStringLiteral("accessToken"), QStringLiteral("refreshToken"),
          QStringLiteral("apiKey"), QStringLiteral("anthropicApiKey"),
          QStringLiteral("openAiApiKey"), QStringLiteral("authorization")}) {
        if (text.contains(key, Qt::CaseInsensitive)) {
            mayContainAssignment = true;
            break;
        }
    }
    if (mayContainAssignment)
        text.replace(credentialAssignment, QStringLiteral("\\1***"));
    if (text.contains(QStringLiteral("sk-")))
        text.replace(skToken, QStringLiteral("sk-***"));
    return text;
}

inline QJsonValue redactProviderCredentials(
    const QJsonValue &value, const QStringList &credentialValues)
{
    if (value.isString())
        return redactProviderCredentials(
            value.toString(), credentialValues);
    if (value.isArray()) {
        QJsonArray result;
        for (const QJsonValue &entry : value.toArray()) {
            result.append(
                redactProviderCredentials(entry, credentialValues));
        }
        return result;
    }
    if (!value.isObject())
        return value;
    QJsonObject result;
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString key = it.key().toLower();
        const bool sensitive =
            key == QLatin1String("accesstoken") ||
            key == QLatin1String("refreshtoken") ||
            key == QLatin1String("apikey") ||
            key == QLatin1String("anthropicapikey") ||
            key == QLatin1String("openaiapikey") ||
            key == QLatin1String("authorization") ||
            key == QLatin1String("credentials");
        result.insert(
            it.key(), sensitive ? QJsonValue(QStringLiteral("***"))
                                : redactProviderCredentials(
                                      it.value(), credentialValues));
    }
    return result;
}

constexpr int kAgentBranchRole = Qt::UserRole + 33;

constexpr int kAgentBranchFilesRole = Qt::UserRole + 34;

constexpr int kAgentBranchDirtyRole = Qt::UserRole + 35;

constexpr int kAgentBranchWorktreeRole = Qt::UserRole + 36;

constexpr int kAgentConflictRole = Qt::UserRole + 37;

constexpr int kAgentConflictSessionRole = Qt::UserRole + 38;

constexpr int kAgentAddedRole = Qt::UserRole + 39;

constexpr int kAgentRemovedRole = Qt::UserRole + 40;

constexpr int kAgentAheadRole = Qt::UserRole + 41;

constexpr int kAgentBehindRole = Qt::UserRole + 42;

constexpr int kAgentImagesRole = Qt::UserRole + 43;

constexpr int kAgentImagesSessionRole = Qt::UserRole + 44;

constexpr int kAgentRunToggleRole = Qt::UserRole + 45;

constexpr int kAgentRowChipsRole = Qt::UserRole + 46;

constexpr int kAgentSpinAngleRole = Qt::UserRole + 47;

constexpr int kAgentUnreadRole = Qt::UserRole + 48;

constexpr int kAgentFolderRole = Qt::UserRole + 49;

constexpr int kAgentFolderCollapsedRole = Qt::UserRole + 50;

constexpr int kAgentFolderSummaryRole = Qt::UserRole + 51;

constexpr int kAgentRowFolderRole = Qt::UserRole + 52;

constexpr int kAgentTickerRole = Qt::UserRole + 53;

inline QString agentTickerText(int sessionId, const QString &prose)
{
    static QHash<int, std::pair<QString, QString>> cache;
    auto it = cache.find(sessionId);
    if (it != cache.end() && it->first == prose)
        return it->second;
    const QString line = forkmesh::ui::agentTickerLine(prose);
    cache.insert(sessionId, {prose, line});
    return line;
}

inline const QString kAgentFolderMimeType =
    QStringLiteral("application/x-forkmesh-agent-folder");

// What the hover button at the end of a row does for that session.
enum AgentRunToggle {
    AgentRunToggleNone = 0,  // nothing this click could start or stop
    AgentRunToggleStart = 1, // stopped or failed: resume it
    AgentRunToggleStop = 2,  // running, waiting or queued: stop it
};

constexpr int kAgentThumbnailPx = 20;

inline QString agentMergeBase(const AgentSession &s)
{
    return s.baseBranch.isEmpty() ? QStringLiteral("main") : s.baseBranch;
}

inline QString agentStatusLabel(const AgentSession &s)
{
    if (s.merged)
        return QStringLiteral("merged");
    // A run whose turn has ended but whose build/test tree is still going is
    // not done, and must not read as if it were.
    if (s.finishingBackgroundWork())
        return QStringLiteral("Running — %1 background task%2 still %3")
            .arg(s.backgroundProcesses)
            .arg(s.backgroundProcesses == 1 ? QString() : QStringLiteral("s"),
                 s.backgroundProcessesBusy ? QStringLiteral("working")
                                           : QStringLiteral("open"));
    return agentStatusText(s.status);
}

inline QString agentStatusCellIconName(const AgentSession &s)
{
    if (s.merged)
        return QStringLiteral("git-merge");
    if (s.genieInFlight())
        return QStringLiteral("sparkle");
    if (s.status == AgentStatus::Running)
        return QStringLiteral("sync");
    if (s.status == AgentStatus::Success)
        return QStringLiteral("check-circle");
    if (s.status == AgentStatus::Stopped)
        return QStringLiteral("stop");
    if (s.status == AgentStatus::Waiting)
        return QStringLiteral("hand");
    if (s.status == AgentStatus::Failed)
        return QStringLiteral("x");
    if (s.status == AgentStatus::Queued)
        return QStringLiteral("history");
    return QString();
}

struct AgentProviderGlyph {
    QString icon; // empty: nothing to draw and nothing to say
    QColor tint;
    QString text; // the hover card's line for it
};

inline AgentProviderGlyph agentProviderGlyph(const QString &provider)
{
    const QColor claude("#d97757");
    const QColor openai("#10a37f");
    if (provider == QLatin1String("claude-code"))
        return {QStringLiteral("terminal"), claude,
                QStringLiteral("Claude Code — run by the `claude` CLI")};
    if (agentIsCodexProvider(provider))
        return {QStringLiteral("terminal"), openai,
                QStringLiteral("Codex — run by the `codex` CLI")};
    if (agentIsClaudeProvider(provider))
        return {QStringLiteral("cloud"), claude,
                QStringLiteral("Claude API — run against Anthropic's API")};
    if (agentUsesOpenAiKey(provider))
        return {QStringLiteral("cloud"), openai,
                QStringLiteral("OpenAI API — run against OpenAI's API")};
    if (agentIsCloudflareAiProvider(provider))
        return {QStringLiteral("cloud"), QColor("#f6821f"),
                QStringLiteral(
                    "Cloudflare AI — run against the relay's Workers AI")};
    if (agentIsLocalLlamaProvider(provider))
        return {QStringLiteral("terminal"), QColor("#7fb069"),
                QStringLiteral(
                    "Local Llama — run against the installed local model")};
    return {QString(), QColor(), QString()};
}

constexpr int kAgentIdentityCirclePx = 20;

constexpr int kAgentStatusGlyphPx = 20;

constexpr int kAgentTeammateBadgePx = 9; // teammate portrait on the avatar

constexpr int kAgentCloudBadgePx = 11; // cloud mark on a mirror-run avatar

// The cloud mark's blue: the sky, and nothing else in the row uses it.
inline QColor agentCloudBadgeColor()
{
    return Theme::nextOr(Theme::Role::AccentFg,
                         QColor(currentThemeIsDark() ? "#58a6ff" : "#0969da"));
}

constexpr int kAgentLeadGlyphGapPx = 4;

constexpr int kAgentLeadGlyphsPx =
    kAgentIdentityCirclePx + kAgentLeadGlyphGapPx + kAgentStatusGlyphPx;

constexpr int kAgentIdentityArtworkPx = kAgentIdentityCirclePx;

inline QPixmap agentLeadGlyphPixmap(const AgentSession &session,
                             const QPixmap &pullAuthorAvatar = QPixmap(),
                             qreal activityAngle = 0.0)
{
    QPixmap artwork = pullAuthorAvatar;
    if (artwork.isNull())
        artwork = agentStatusModelIcon(session).pixmap(
            kAgentIdentityArtworkPx, kAgentIdentityArtworkPx);
    if (artwork.isNull())
        return QPixmap();
    QPixmap out = crispIconPixmap(kAgentLeadGlyphsPx, kAgentIdentityCirclePx,
                                  iconDevicePixelRatio());
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawPixmap(QRect(0, 0, kAgentIdentityArtworkPx,
                       kAgentIdentityArtworkPx), artwork);
    // A run a teammate started carries that teammate's portrait in the corner
    // of its avatar, so the list says who is working without opening anything.
    if (session.hasTeammateAttribution()) {
        const QPixmap portrait = teammateAvatarPixmap(
            session.teammateAvatar, session.provider, session.model,
            kAgentTeammateBadgePx);
        if (!portrait.isNull()) {
            const QRectF badge(kAgentIdentityArtworkPx - kAgentTeammateBadgePx,
                               kAgentIdentityArtworkPx - kAgentTeammateBadgePx,
                               kAgentTeammateBadgePx, kAgentTeammateBadgePx);
            // A rim in the row's own background parts the two round faces;
            // without it the portrait reads as a smudge on the avatar's edge.
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(currentThemeIsDark() ? "#0d1117" : "#ffffff"));
            p.drawEllipse(badge.adjusted(-1.2, -1.2, 1.2, 1.2));
            p.drawPixmap(badge.toRect(), portrait);
        }
    }
    // A run on one of the account's mirrors wears a cloud on the top corner of
    // its avatar — the teammate portrait keeps the bottom one — so the list
    // says where the work is happening before anything is opened.
    if (session.cloud) {
        const QRectF badge(kAgentIdentityArtworkPx - kAgentCloudBadgePx, 0,
                           kAgentCloudBadgePx, kAgentCloudBadgePx);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(currentThemeIsDark() ? "#0d1117" : "#ffffff"));
        p.drawEllipse(badge.adjusted(-1.2, -1.2, 1.2, 1.2));
        themedOcticon(QStringLiteral("cloud"), agentCloudBadgeColor(),
                      kAgentCloudBadgePx)
            .paint(&p, badge.toRect());
    }
    const bool running = !session.merged && session.status == AgentStatus::Running;

    const QString statusIcon = agentStatusCellIconName(session);
    if (!statusIcon.isEmpty()) {
        const QRect statusRect(kAgentIdentityCirclePx + kAgentLeadGlyphGapPx,
                               (kAgentIdentityCirclePx - kAgentStatusGlyphPx) / 2,
                               kAgentStatusGlyphPx, kAgentStatusGlyphPx);
        if (running) {
            p.drawPixmap(statusRect.topLeft(), agentRunningFireballPixmap(
                                                   kAgentStatusGlyphPx,
                                                   activityAngle));
        } else {
            const QIcon icon = themedOcticon(statusIcon,
                                             agentStatusIconColor(session),
                                             kAgentStatusGlyphPx);
            icon.paint(&p, statusRect);
        }
    }
    p.end();
    return out;
}

inline QString agentHoverRow(const QString &icon, const QColor &tint, const QString &text)
{
    return QStringLiteral(
               "<tr><td style='padding:1px 7px 1px 0; vertical-align:middle;'>"
               "%1</td><td style='padding:1px 0; white-space:nowrap;'>%2</td></tr>")
        .arg(octiconMarkup(icon, 13, tint), text.toHtmlEscaped());
}

inline void applyAgentStatusCell(QTableWidgetItem *cell, const AgentSession &s,
                          const AgentDiffStat &stat = AgentDiffStat(),
                          const QString &base = QString(), int sessionId = 0,
                          const QPixmap &pullAuthorAvatar = QPixmap(),
                          qreal activityAngle = 0.0,
                          int runToggle = AgentRunToggleNone)
{
    const qint64 updatedMs = qMax(qMax(s.createdAtMs, s.startedAtMs),
                                  qMax(s.finishedAtMs, s.mergedAtMs));
    cell->setData(Qt::DisplayRole,
                  updatedMs > 0 ? formatShortRelativeTime(updatedMs / 1000)
                                : QStringLiteral("-"));
    cell->setData(Qt::UserRole, s.id);
    cell->setData(kTableSortRole, s.id);
    cell->setData(kAgentSpinAngleRole, activityAngle);
    const QString statusIcon = agentStatusCellIconName(s);
    const QPixmap lead = agentLeadGlyphPixmap(s, pullAuthorAvatar, activityAngle);
    cell->setIcon(lead.isNull() ? QIcon() : QIcon(lead));
    cell->setData(kAgentBranchRole, s.branchName);
    cell->setData(kAgentBranchFilesRole, stat.files);
    cell->setData(kAgentBranchDirtyRole, stat.dirty);
    cell->setData(kAgentBranchWorktreeRole, stat.worktree);
    cell->setData(kAgentAddedRole, stat.added);
    cell->setData(kAgentRemovedRole, stat.removed);
    cell->setData(kAgentAheadRole, stat.ahead);
    cell->setData(kAgentBehindRole, stat.behind);
    cell->setData(kAgentConflictRole, stat.conflicted);
    cell->setData(kAgentConflictSessionRole, sessionId);
    cell->setData(kAgentRunToggleRole, runToggle);
    QString tip = QStringLiteral(
        "<table cellspacing='0' cellpadding='0' style='border-collapse:collapse;'>");
    auto addTip = [&tip](const QString &icon, const QColor &tint,
                         const QString &text) {
        tip += agentHoverRow(icon, tint, text);
    };
    if (s.cloud) {
        // A run nothing has started is waiting, not running somewhere: say so,
        // because that is the row "Continue on this desktop" is offered on.
        const bool waiting = s.status == AgentStatus::Queued;
        addTip(QStringLiteral("cloud"), agentCloudBadgeColor(),
               s.cloudNode.isEmpty()
                   ? QStringLiteral("Cloud agent — waiting for one of your "
                                    "nodes to claim it")
               : waiting && s.cloudHere
                   ? QStringLiteral("Cloud agent — queued from the web for "
                                    "this desktop")
               : waiting
                   ? QStringLiteral("Cloud agent — waiting for %1 to claim it")
                         .arg(s.cloudNode)
                   : QStringLiteral("Cloud agent — runs on %1, not on this "
                                    "desktop")
                         .arg(s.cloudNode));
    } else {
        addTip(QStringLiteral("person"), Theme::remapColor(QColor("#8b949e")),
               QStringLiteral("Agent #%1").arg(s.id));
    }
    if (s.hasTeammateAttribution())
        addTip(QStringLiteral("people"),
               Theme::nextOr(Theme::Role::FgMuted, QColor("#a371f7")),
               QStringLiteral("%1 — started by %2")
                   .arg(s.teammateIds.isEmpty() ? QStringLiteral("Special agent")
                                                : QStringLiteral("Special agents run"))
                   .arg(s.teammateName.isEmpty() ? s.teammateId
                                                 : s.teammateName));
    const AgentProviderGlyph providerGlyph = agentProviderGlyph(s.provider);
    if (!providerGlyph.icon.isEmpty())
        addTip(providerGlyph.icon, providerGlyph.tint, providerGlyph.text);
    if (updatedMs > 0)
        addTip(QStringLiteral("history"), Theme::remapColor(QColor("#8b949e")),
               QStringLiteral("Updated %1")
                   .arg(QDateTime::fromMSecsSinceEpoch(updatedMs).toString(
                       QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    addTip(statusIcon.isEmpty() ? QStringLiteral("circle-slash") : statusIcon,
           agentStatusIconColor(s), agentStatusLabel(s));
    // Hovering the row puts its controls at the end of it, so say what each
    // one does before it is pressed.
    if (runToggle == AgentRunToggleStop)
        addTip(QStringLiteral("circle-slash"), QColor("#f85149"),
               QStringLiteral("Click the stop button at the end of the row to "
                              "stop this agent"));
    else if (runToggle == AgentRunToggleStart)
        addTip(QStringLiteral("play"),
               Theme::nextOr(Theme::Role::AccentFg, QColor("#3fb950")),
               QStringLiteral("Click the play button at the end of the row to "
                              "start this agent again"));
    addTip(QStringLiteral("trash"), QColor("#f85149"),
           s.cloud
               ? QStringLiteral("Click the trash button at the end of the row "
                                "to delete this cloud session from the relay")
               : QStringLiteral("Click the trash button at the end of the row "
                                "to delete this agent"));
    if (s.genie)
        addTip(QStringLiteral("sparkle"),
               Theme::nextOr(Theme::Role::FgMuted, QColor(Theme::kGenie)),
               QStringLiteral("Genie — working the organization's "
                              "shared task list from the website's remote MCP"));
    if (!s.merged && s.status == AgentStatus::Queued)
        addTip(QStringLiteral("history"), QColor("#d29922"),
               QStringLiteral("Queued — starts when one of the %1 running "
                              "agent slots frees up (Settings → Agents)")
                   .arg(maxRunningAgents()));
    if (s.merged)
        addTip(QStringLiteral("git-merge"),
               Theme::nextOr(Theme::Role::FgMuted, QColor("#a371f7")),
               QStringLiteral("Worktree/PR merged into %1%2")
                   .arg(agentMergeBase(s),
                        s.mergedAtMs > 0
                            ? QStringLiteral(" on %1").arg(
                                  QDateTime::fromMSecsSinceEpoch(s.mergedAtMs)
                                      .toString(QStringLiteral("MMM d  hh:mm")))
                            : QString()));
    if (!s.branchName.isEmpty()) {
        addTip(QStringLiteral("git-branch"),
               Theme::nextOr(Theme::Role::FgMuted, QColor("#3fb950")),
               QStringLiteral("Click the branch button to review %1 in the "
                              "Git view")
                   .arg(s.branchName));
        if (stat.files >= 0)
            addTip(QStringLiteral("file-diff"),
                   Theme::nextOr(Theme::Role::FgMuted, QColor("#58a6ff")),
                   QStringLiteral("%1 file%2 changed")
                       .arg(stat.files)
                       .arg(stat.files == 1 ? QString() : QStringLiteral("s")));
        if (stat.worktree.isEmpty())
            addTip(QStringLiteral("worktree"), Theme::remapColor(QColor("#8b949e")),
                   QStringLiteral("No worktree checked out — the branch "
                                  "button's grey ring means this session's "
                                  "checkout is gone"));
        else
            addTip(QStringLiteral("worktree"),
                   Theme::nextOr(Theme::Role::AccentFg, QColor("#58a6ff")),
                   QStringLiteral("Worktree: %1 — the branch button's "
                                  "blue ring means this checkout is still on disk")
                       .arg(stat.worktree));
        if (stat.dirty > 0)
            addTip(QStringLiteral("diff"), QColor("#d29922"),
                   QStringLiteral("%1 uncommitted change%2 in the worktree "
                                  "— what the amber dot on the branch "
                                  "button's corner counts")
                       .arg(stat.dirty)
                       .arg(stat.dirty == 1 ? QString() : QStringLiteral("s")));
        else if (stat.dirty == 0)
            addTip(QStringLiteral("check-circle"), QColor("#3fb950"),
                   QStringLiteral("Worktree is clean — no dot on the "
                                  "branch button"));
    }
    if (stat.conflicted)
        addTip(QStringLiteral("alert"),
               Theme::nextOr(Theme::Role::Warning, QColor("#e3742f")),
               QStringLiteral("Conflicts with %1 — the branch button turns amber; "
                              "click the orange alert on it to have this agent "
                              "merge base in and resolve")
                   .arg(base.isEmpty() ? QStringLiteral("base") : base));
    else if (stat.behind > 0)
        addTip(QStringLiteral("download"),
               Theme::nextOr(Theme::Role::Warning, QColor("#e3742f")),
               QStringLiteral("The ⬇ arrow beside the branch button "
                              "(and its amber ring): this branch is %1 commit%2 "
                              "behind %3 and needs base merged in to catch up")
                   .arg(stat.behind)
                   .arg(stat.behind == 1 ? QString() : QStringLiteral("s"))
                   .arg(base.isEmpty() ? QStringLiteral("base") : base));
    if (stat.added >= 0 && stat.removed >= 0)
        addTip(QStringLiteral("diff"), Theme::remapColor(QColor("#8b949e")),
               QStringLiteral("%1 line%2 added · %3 removed")
                   .arg(stat.added)
                   .arg(stat.added == 1 ? QString() : QStringLiteral("s"))
                   .arg(stat.removed));
    if (stat.ahead >= 0 && stat.behind >= 0)
        addTip(QStringLiteral("git-compare"),
               Theme::nextOr(Theme::Role::FgMuted, QColor("#a371f7")),
               QString::fromUtf8("%1 ahead · %2 behind %3")
                   .arg(stat.ahead)
                   .arg(stat.behind)
                   .arg(base.isEmpty() ? QStringLiteral("base") : base));
    tip += QStringLiteral("</table>");
    cell->setToolTip(tip);
}

enum AgentRowChipSlot {
    AgentChipStatus,
    AgentChipProvider,
    AgentChipHost,
    AgentChipTeammate,
    AgentChipPriority,
    AgentChipAge,
    AgentChipSync,
    AgentChipFiles,
    AgentChipChurn,
    AgentChipDirty,
    AgentChipBranch,
    AgentChipSlotCount,
};

inline const QString &agentRowChipFireballIcon()
{
    static const QString icon = QStringLiteral("agent-fireball");
    return icon;
}

inline QVariantList agentRowChips(const AgentSession &s,
                                  const AgentDiffStat &stat)
{
    QVariantList chips;
    auto add = [&chips](AgentRowChipSlot slot, const QString &icon,
                        const QColor &tint, const QString &word) {
        chips.append(QStringList{icon, tint.name(), word, QString::number(slot)});
    };
    const QColor grey = Theme::remapColor(QColor("#8b949e"));
    const QColor amber = Theme::nextOr(Theme::Role::Warning, QColor("#e3742f"));
    const QString statusIcon = agentStatusCellIconName(s);
    const bool running = !s.merged && s.status == AgentStatus::Running &&
                         !s.genieInFlight();
    add(AgentChipStatus,
        running                ? agentRowChipFireballIcon()
        : statusIcon.isEmpty() ? QStringLiteral("circle-slash")
                               : statusIcon,
        running ? agentRunningFireballColor() : agentStatusIconColor(s),
        s.merged ? QStringLiteral("Merged") : agentStatusText(s.status));
    const AgentProviderGlyph provider = agentProviderGlyph(s.provider);
    if (!provider.icon.isEmpty())
        add(AgentChipProvider, provider.icon, provider.tint,
            agentIsLocalLlamaProvider(s.provider)
                ? QStringLiteral("Llama")
                : provider.text.section(QLatin1Char(' '), 0, 0));
    if (s.cloud)
        add(AgentChipHost, QStringLiteral("cloud"), agentCloudBadgeColor(),
            QStringLiteral("Cloud"));
    else
        add(AgentChipHost, QStringLiteral("device-desktop"), grey,
            QStringLiteral("Desktop"));
    if (s.hasTeammateAttribution()) {
        const QString who = s.teammateName.trimmed().section(QLatin1Char(' '), 0, 0);
        add(AgentChipTeammate, QStringLiteral("people"),
            Theme::nextOr(Theme::Role::FgMuted, QColor("#a371f7")),
            who.isEmpty() ? QStringLiteral("Special agent") : who);
    }
    if (s.id > 0)
        add(AgentChipPriority, QStringLiteral("sort-desc"), grey,
            QStringLiteral("P%1").arg(qBound(1, s.priority, 99), 2, 10,
                                      QLatin1Char('0')));
    const qint64 updatedMs = qMax(qMax(s.createdAtMs, s.startedAtMs),
                                  qMax(s.finishedAtMs, s.mergedAtMs));
    if (updatedMs > 0)
        add(AgentChipAge, QStringLiteral("history"), grey,
            formatShortRelativeTime(updatedMs / 1000));
    if (!s.branchName.isEmpty()) {
        if (stat.conflicted)
            add(AgentChipSync, QStringLiteral("alert"), amber,
                QStringLiteral("Conflicts"));
        else if (stat.behind > 0)
            add(AgentChipSync, QStringLiteral("download"), amber,
                QStringLiteral("Behind"));
        if (stat.files >= 0)
            add(AgentChipFiles, QStringLiteral("file-diff"),
                Theme::nextOr(Theme::Role::FgMuted, QColor("#58a6ff")),
                QString::number(stat.files));
        if (stat.added >= 0 && stat.removed >= 0 &&
            (stat.added > 0 || stat.removed > 0))
            add(AgentChipChurn, QStringLiteral("diff"), grey,
                QString::fromUtf8("+%1\xE2\x88\x92%2")
                    .arg(stat.added)
                    .arg(stat.removed));
        if (stat.dirty > 0)
            add(AgentChipDirty, QStringLiteral("diff"), QColor("#d29922"),
                QStringLiteral("Dirty"));
        add(AgentChipBranch, QStringLiteral("git-branch"),
            Theme::nextOr(Theme::Role::FgMuted, QColor("#3fb950")),
            stat.worktree.isEmpty() ? QStringLiteral("Branch")
                                    : QStringLiteral("Worktree"));
    }
    return chips;
}

inline qint64 agentEffectiveDurationMs(const AgentSession &s)
{
    const qint64 ran = s.totalRunMs(QDateTime::currentMSecsSinceEpoch());
    // While a run is open, durationMs belongs to the turn that reported before
    // it — tens of milliseconds against a session that has been going twenty
    // minutes — and taking it made the Stats row say "0s" and the Speed row
    // five figures of tok/s. The clock is the honest answer until the CLI's own
    // figure is final.
    if (s.runInFlight())
        return ran > 0 ? ran : s.durationMs;
    return s.durationMs > 0 ? s.durationMs : ran;
}

inline double agentTokensPerSecond(const AgentSession &s, qint64 tokens)
{
    const qint64 durationMs = agentEffectiveDurationMs(s);
    if (s.status != AgentStatus::Running || tokens <= 0 || durationMs <= 0)
        return 0.0;
    return tokens * 1000.0 / static_cast<double>(durationMs);
}

constexpr double kAgentFastTokensPerSecond = 30.0;

static constexpr int kAgentIdColumn = 0;

static constexpr int kAgentIssueColumn = 1;

// The last column: the issue title, its attachment thumbnail, and the row's own
// controls. Pointing anywhere at an agent fades in a start/stop toggle,
// priority shortcuts, and a delete button at the end of its row — right-aligned,
// so they line up down the list instead of following the status glyph in and
// out of the left margin.
// They fade in over the title rather than reserving a strip out of it: controls
// that only one row at a time ever shows should not shorten every
// title in the list.
class AgentRowActionsDelegate : public SelectionBorderRowDelegate
{
public:
    AgentRowActionsDelegate(QAbstractItemView *view,
                            std::function<void(int)> onThumbnailClick,
                            std::function<void(int)> onRunToggle,
                            std::function<void(int)> onMoveToTop,
                            std::function<void(int)> onMoveToBottom,
                            std::function<void(int)> onDelete)
        : SelectionBorderRowDelegate(view), m_itemView(view),
          m_onThumbnailClick(std::move(onThumbnailClick)),
          m_onRunToggle(std::move(onRunToggle)),
          m_onMoveToTop(std::move(onMoveToTop)),
          m_onMoveToBottom(std::move(onMoveToBottom)),
          m_onDelete(std::move(onDelete))
    {
        // The controls appear while the pointer is anywhere on the agent's
        // row, so both the row it left and the row it entered must repaint.
        m_hoverRepaint = true;
        // Drives the fade-in. Armed only while a reveal is in flight, so a
        // table nobody is pointing at costs nothing.
        m_revealTimer.setInterval(kRevealTickMs);
        connect(&m_revealTimer, &QTimer::timeout, this,
                &AgentRowActionsDelegate::stepReveal);
        m_tickerClock.start();
        m_tickerTimer.setInterval(kTickerTickMs);
        connect(&m_tickerTimer, &QTimer::timeout, this,
                &AgentRowActionsDelegate::stepTickers);
        if (QAbstractItemModel *model = view->model()) {
            const auto stale = [this] { m_chipColumnsStale = true; };
            connect(model, &QAbstractItemModel::dataChanged, this,
                    [this](const QModelIndex &, const QModelIndex &,
                           const QList<int> &roles) {
                        if (roles.isEmpty() || roles.contains(kAgentRowChipsRole))
                            m_chipColumnsStale = true;
                    });
            connect(model, &QAbstractItemModel::rowsInserted, this, stale);
            connect(model, &QAbstractItemModel::rowsRemoved, this, stale);
            connect(model, &QAbstractItemModel::modelReset, this, stale);
            connect(model, &QAbstractItemModel::layoutChanged, this, stale);
        }
    }

    // True while a row's controls are still fading in or out. Tests grabbing
    // the hovered row wait this out before comparing pixels.
    bool revealing() const { return m_revealTimer.isActive(); }

    // Where the row's controls are drawn, measured from the end of the row.
    // Public so the window's test hooks can click exactly where a user would.
    static QRect deleteButtonRect(const QRect &cell)
    {
        return QRect(cell.right() - kActionMargin - kActionSize + 1,
                     cell.center().y() - kActionSize / 2, kActionSize,
                     kActionSize);
    }

    static QRect runToggleButtonRect(const QRect &cell,
                                     bool includePriorityControls = true)
    {
        return includePriorityControls
                   ? moveToTopButtonRect(cell).translated(-(kActionSize + kActionGap),
                                                          0)
                   : moveToBottomButtonRect(cell);
    }

    static QRect moveToTopButtonRect(const QRect &cell)
    {
        return moveToBottomButtonRect(cell).translated(-(kActionSize + kActionGap),
                                                        0);
    }

    static QRect moveToBottomButtonRect(const QRect &cell)
    {
        return deleteButtonRect(cell).translated(-(kActionSize + kActionGap), 0);
    }

    static QRect unreadDotRect(const QRect &cell, bool twoLines)
    {
        const int centerY = twoLines ? cell.top() + cell.height() / 4
                                     : cell.center().y();
        return QRect(cell.left() + (kUnreadGutterPx - kUnreadDotPx) / 2 + 1,
                     centerY - kUnreadDotPx / 2, kUnreadDotPx, kUnreadDotPx);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QColor cardFill;
        QPainterPath cardShape;
        if (forkmesh::ui::paintWorkspaceSessionRow(painter, option, index,
                                                   rowHovered(option, index),
                                                   &cardFill, &cardShape)) {
            painter->save();
            painter->setClipPath(cardShape);
            m_washOverride = cardFill;
            paintRowActions(painter, option, index);
            m_washOverride = QColor();
            painter->restore();
            return;
        }
        const QVariantList chips = index.data(kAgentRowChipsRole).toList();
        const bool unread = index.data(kAgentUnreadRole).toBool();
        QStyleOptionViewItem content = contentOption(option);
        if (unread)
            content.font.setWeight(QFont::DemiBold);
        if (chips.isEmpty()) {
            content.state &= ~(QStyle::State_MouseOver | QStyle::State_Selected);
            const QString ticker = index.data(kAgentTickerRole).toString();
            const QRect lane = ticker.isEmpty() ? QRect() : tickerLane(content, index);
            if (lane.isValid()) {
                QStyleOptionViewItem title(content);
                title.rect.setRight(lane.left() - kTickerGap);
                QStyledItemDelegate::paint(painter, title, index);
                paintTicker(painter, lane, index, ticker, content.font);
            } else {
                forgetTicker(sessionOf(index));
                QStyledItemDelegate::paint(painter, content, index);
            }
            paintRowSelectionBorder(painter, option, index);
        } else {
            QStyleOptionViewItem title(content);
            title.rect.setBottom(option.rect.top() + option.rect.height() / 2);
            title.state &= ~QStyle::State_Selected;
            const int inset = titleTextLeft(title, index) - chipLineLeft(content);
            if (inset > 0) {
                title.rect.translate(-inset, 0);
                title.rect.setRight(option.rect.right());
            }
            SelectionBorderRowDelegate::paint(painter, title, index);
            paintRowChips(painter, content, chips,
                          leadIndex(index).data(kAgentSpinAngleRole).toReal());
            paintRowSelectionBorder(painter, option, index);
        }
        if (unread) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(currentThemeIsDark() ? "#58a6ff" : "#0969da"));
            painter->drawEllipse(unreadDotRect(option.rect, !chips.isEmpty()));
            painter->restore();
        }
        if (paintRowActions(painter, option, index)) {
            // The wash behind the controls lies over the end of the row's
            // selection border, which the base delegate drew first. Put it
            // back, or a selected row loses its right-hand corner while it is
            // hovered.
            paintRowSelectionBorder(painter, option, index);
        }
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::MouseButtonRelease) {
            auto *me = static_cast<QMouseEvent *>(event);
            const int action = actionAt(option.rect, me->pos(), index);
            if (me->button() == Qt::LeftButton && action != ActionNone) {
                // Swallowing the press keeps these pure one-click controls:
                // without it the same click also drags the selection onto the
                // row and loads its transcript.
                if (event->type() == QEvent::MouseButtonPress) {
                    setPressed(index.row(), action);
                    return true;
                }
                const int session = sessionOf(index);
                setPressed(-1, ActionNone);
                if (session != 0) {
                    // Both handlers pump the event loop — a confirmation, a
                    // git teardown — and can rebuild the table this event was
                    // dispatched from. Let it finish first.
                    std::function<void(int)> handler;
                    if (action == ActionDelete)
                        handler = m_onDelete;
                    else if (action == ActionRunToggle)
                        handler = m_onRunToggle;
                    else if (action == ActionMoveToTop)
                        handler = m_onMoveToTop;
                    else if (action == ActionMoveToBottom)
                        handler = m_onMoveToBottom;
                    if (handler) {
                        QTimer::singleShot(0, this,
                                           [fn = std::move(handler), session] {
                                               fn(session);
                                           });
                        return true;
                    }
                }
            }
            if (event->type() == QEvent::MouseButtonRelease)
                setPressed(-1, ActionNone);
        }
        if (event->type() == QEvent::MouseButtonRelease && m_onThumbnailClick) {
            auto *me = static_cast<QMouseEvent *>(event);
            const int sessionId = index.data(kAgentImagesSessionRole).toInt();
            if (me->button() == Qt::LeftButton && sessionId > 0 &&
                !index.data(kAgentImagesRole).toStringList().isEmpty() &&
                thumbnailRect(option, index).contains(me->pos())) {
                m_onThumbnailClick(sessionId);
                return true;
            }
        }
        return SelectionBorderRowDelegate::editorEvent(event, model, option, index);
    }

private:
    static constexpr int kUnreadGutterPx = 12;
    static constexpr int kUnreadDotPx = 7;

    static QStyleOptionViewItem contentOption(const QStyleOptionViewItem &option)
    {
        QStyleOptionViewItem content(option);
        content.rect.setLeft(option.rect.left() + kUnreadGutterPx);
        return content;
    }

    static constexpr int kChipGlyphPx = 12;
    static constexpr int kChipGlyphGap = 3;
    static constexpr int kChipGap = 10;
    static constexpr int kChipFireballBleedPx = 2;

    static int chipLineLeft(const QStyleOptionViewItem &option)
    {
        const QStyle *style =
            option.widget ? option.widget->style() : QApplication::style();
        return option.rect.left() +
               style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr,
                                  option.widget) +
               1;
    }

    int titleTextLeft(const QStyleOptionViewItem &option,
                      const QModelIndex &index) const
    {
        QStyleOptionViewItem opt(option);
        initStyleOption(&opt, index);
        const QStyle *style =
            opt.widget ? opt.widget->style() : QApplication::style();
        const QRect text =
            style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
        return text.isValid() ? text.left() : option.rect.left();
    }

    static QFont chipFont(const QFont &base)
    {
        QFont font = base;
        if (font.pixelSize() > 0)
            font.setPixelSize(qMax(9, font.pixelSize() - 1));
        else
            font.setPointSizeF(qMax(7.0, font.pointSizeF() - 1.0));
        return font;
    }

    static int chipSlot(const QStringList &parts)
    {
        bool ok = false;
        const int slot = parts.value(3).toInt(&ok);
        return ok && slot >= 0 && slot < AgentChipSlotCount ? slot : -1;
    }

    const QList<int> &chipColumnWidths(const QFont &font) const
    {
        const QString fontKey = font.key();
        const QFontMetrics fm(font);
        if (!m_chipColumnsStale && fontKey == m_chipColumnsFontKey &&
            m_chipColumnWidths.size() == AgentChipSlotCount)
            return m_chipColumnWidths;
        QList<int> widths(AgentChipSlotCount, 0);
        if (const QAbstractItemModel *model =
                m_itemView ? m_itemView->model() : nullptr) {
            for (int row = 0; row < model->rowCount(); ++row) {
                const QVariantList chips =
                    model->index(row, kAgentIssueColumn)
                        .data(kAgentRowChipsRole)
                        .toList();
                for (const QVariant &chip : chips) {
                    const QStringList parts = chip.toStringList();
                    const int slot = chipSlot(parts);
                    if (slot < 0)
                        continue;
                    widths[slot] = qMax(widths[slot],
                                        kChipGlyphPx + kChipGlyphGap +
                                            fm.horizontalAdvance(parts.at(2)));
                }
            }
        }
        const bool moved = !m_chipColumnWidths.isEmpty() &&
                           m_chipColumnWidths != widths;
        m_chipColumnWidths = widths;
        m_chipColumnsFontKey = fontKey;
        m_chipColumnsStale = false;
        if (moved && m_itemView)
            m_itemView->viewport()->update();
        return m_chipColumnWidths;
    }

    void paintRowChips(QPainter *painter, const QStyleOptionViewItem &option,
                       const QVariantList &chips, qreal spinAngle) const
    {
        const int margin = chipLineLeft(option) - option.rect.left();
        const int top = option.rect.top() + option.rect.height() / 2;
        const QRect line(option.rect.left() + margin, top,
                         option.rect.width() - 2 * margin,
                         option.rect.bottom() - top - 2);
        const QFont font = chipFont(option.font);
        const QFontMetrics fm(font);
        QList<QStringList> bySlot(AgentChipSlotCount);
        for (const QVariant &chip : chips) {
            const QStringList parts = chip.toStringList();
            const int slot = chipSlot(parts);
            if (slot >= 0)
                bySlot[slot] = parts;
        }
        const QList<int> &widths = chipColumnWidths(font);
        painter->save();
        painter->setFont(font);
        painter->setPen(QColor(currentThemeIsDark() ? "#8b949e" : "#656d76"));
        int x = line.left();
        for (int slot = 0; slot < AgentChipSlotCount; ++slot) {
            const int column = qMax(widths.value(slot), 0);
            const QStringList &parts = bySlot.at(slot);
            if (column == 0 && parts.isEmpty())
                continue;
            if (!parts.isEmpty()) {
                const int textWidth = fm.horizontalAdvance(parts.at(2));
                if (x + kChipGlyphPx + kChipGlyphGap + textWidth > line.right() + 1)
                    break;
                const QRect glyph(x, line.center().y() - kChipGlyphPx / 2,
                                  kChipGlyphPx, kChipGlyphPx);
                if (parts.at(0) == agentRowChipFireballIcon()) {
                    const QRect orbit = glyph.adjusted(-kChipFireballBleedPx,
                                                       -kChipFireballBleedPx,
                                                       kChipFireballBleedPx,
                                                       kChipFireballBleedPx);
                    painter->drawPixmap(orbit, agentRunningFireballPixmap(
                                                   orbit.width(), spinAngle));
                } else {
                    themedOcticon(parts.at(0), QColor(parts.at(1)), kChipGlyphPx)
                        .paint(painter, glyph);
                }
                painter->drawText(QRect(x + kChipGlyphPx + kChipGlyphGap,
                                        line.top(), textWidth, line.height()),
                                  Qt::AlignLeft | Qt::AlignVCenter, parts.at(2));
            }
            x += column + kChipGap;
        }
        painter->restore();
    }

    static constexpr int kTickerTickMs = 33;
    static constexpr int kTickerGap = 10;
    static constexpr int kTickerMinLanePx = 80;
    static constexpr int kTickerTitleSharePct = 45;

    QRect tickerLane(const QStyleOptionViewItem &content,
                     const QModelIndex &index) const
    {
        const QFontMetrics fm(content.font);
        const int margin = chipLineLeft(content) - content.rect.left();
        const int right = content.rect.right() - margin;
        const int natural = titleTextLeft(content, index) +
                            fm.horizontalAdvance(index.data(Qt::DisplayRole).toString()) +
                            margin;
        const int floor =
            content.rect.left() + content.rect.width() * kTickerTitleSharePct / 100;
        const int titleEnd =
            qMax(floor, qMin(natural, right - kTickerMinLanePx - kTickerGap));
        const QRect lane(titleEnd + kTickerGap, content.rect.top(),
                         right - titleEnd - kTickerGap + 1, content.rect.height());
        return lane.width() >= kTickerMinLanePx ? lane : QRect();
    }

    void paintTicker(QPainter *painter, const QRect &lane, const QModelIndex &index,
                     const QString &text, const QFont &base) const
    {
        const int session = sessionOf(index);
        const QFont font = chipFont(base);
        const QFontMetrics fm(font);
        if (font.key() != m_tickerFontKey) {
            m_tickers.clear();
            m_tickerFontKey = font.key();
        }
        const qint64 now = m_tickerClock.elapsed();
        forkmesh::ui::AgentTickerTrain &train = m_tickers[session];
        train.setLine(text, fm.horizontalAdvance(text), lane.width(), now);
        const QList<forkmesh::ui::AgentTickerTrain::Car> cars =
            train.layout(lane.width(), now);
        painter->save();
        painter->setClipRect(lane, Qt::IntersectClip);
        painter->setFont(font);
        painter->setPen(QColor(currentThemeIsDark() ? "#8b949e" : "#656d76"));
        for (const forkmesh::ui::AgentTickerTrain::Car &car : cars)
            painter->drawText(QRectF(lane.left() + car.left, lane.top(),
                                     car.width + 1, lane.height()),
                              Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                              car.text);
        painter->restore();
        m_tickerLanes.insert(index.row(), lane);
        if (!m_tickerTimer.isActive())
            m_tickerTimer.start();
    }

    void forgetTicker(int session) const { m_tickers.remove(session); }

    void stepTickers()
    {
        if (m_tickerLanes.isEmpty() || !m_itemView) {
            m_tickerTimer.stop();
            return;
        }
        const QHash<int, QRect> lanes = std::exchange(m_tickerLanes, {});
        for (const QRect &lane : lanes)
            m_itemView->viewport()->update(lane);
    }

    static constexpr int kActionSize = 20;  // one round button
    static constexpr int kActionGap = 5;    // between the two
    static constexpr int kActionMargin = 7; // to the end of the row
    static constexpr int kActionGlyphPx = 12;
    static constexpr int kActionFadePx = 22; // title -> controls cross-fade

    enum RowAction {
        ActionNone,
        ActionRunToggle,
        ActionMoveToTop,
        ActionMoveToBottom,
        ActionDelete,
    };

    // The lead cell carries the session and what a click on the run control
    // would do; the controls themselves are painted over here, in the last
    // column, where the end of the row is.
    static QModelIndex leadIndex(const QModelIndex &idx)
    {
        return idx.sibling(idx.row(), kAgentIdColumn);
    }

    static int sessionOf(const QModelIndex &idx)
    {
        return leadIndex(idx).data(Qt::UserRole).toInt();
    }

    static int runToggleMode(const QModelIndex &idx)
    {
        return leadIndex(idx).data(kAgentRunToggleRole).toInt();
    }

    QRect thumbnailRect(const QStyleOptionViewItem &option,
                        const QModelIndex &index) const
    {
        QStyleOptionViewItem opt = contentOption(option);
        initStyleOption(&opt, index);
        const QStyle *style =
            opt.widget ? opt.widget->style() : QApplication::style();
        const QRect rect = style->subElementRect(QStyle::SE_ItemViewItemDecoration,
                                                 &opt, opt.widget);
        return rect.isValid() ? rect.adjusted(-2, -2, 2, 2) : rect;
    }

    // Which control the point is on, or ActionNone. Only answers for a row
    // whose controls are actually showing: an invisible hotspot is a trap.
    int actionAt(const QRect &cell, const QPoint &pos,
                 const QModelIndex &index) const
    {
        // Externally started sessions carry a negative id (see
        // externalTempIdFor); they can still be dropped from the list.
        if (revealFor(index.row()) <= 0.0 || sessionOf(index) == 0)
            return ActionNone;
        // A pane too narrow to draw them in draws neither, so neither answers.
        if ((sessionOf(index) > 0 ? moveToTopButtonRect(cell)
                                  : deleteButtonRect(cell))
                .left() < cell.left())
            return ActionNone;
        if (deleteButtonRect(cell).contains(pos))
            return ActionDelete;
        if (sessionOf(index) > 0 && moveToTopButtonRect(cell).contains(pos))
            return ActionMoveToTop;
        if (sessionOf(index) > 0 && moveToBottomButtonRect(cell).contains(pos))
            return ActionMoveToBottom;
        if (runToggleMode(index) != AgentRunToggleNone &&
            runToggleButtonRect(cell, sessionOf(index) > 0).contains(pos))
            return ActionRunToggle;
        return ActionNone;
    }

    // Hover is per-cell in QTableView, but the controls belong to the agent:
    // the pointer anywhere on the row reveals them.
    bool rowHovered(const QStyleOptionViewItem &option,
                    const QModelIndex &index) const
    {
        return (option.state & QStyle::State_MouseOver) ||
               hoveredRow() == index.row();
    }

    // One click at the end of a hovered row stops a live agent or starts a
    // stopped one — the per-row half of the toolbar's Stop all / Start all —
    // and the trash beside it deletes the session outright.
    bool paintRowActions(QPainter *painter, const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
    {
        const bool wanted = rowHovered(option, index) && sessionOf(index) != 0;
        const qreal reveal = driveReveal(index.row(), wanted);
        if (reveal <= 0.0)
            return false;
        const int mode = runToggleMode(index);
        const bool canReprioritize = sessionOf(index) > 0;
        const QRect run = runToggleButtonRect(option.rect, canReprioritize);
        const QRect moveTop = moveToTopButtonRect(option.rect);
        const QRect moveBottom = moveToBottomButtonRect(option.rect);
        const QRect trash = deleteButtonRect(option.rect);
        const QRect firstAction =
            canReprioritize ? (mode != AgentRunToggleNone ? run : moveTop)
                            : (mode != AgentRunToggleNone ? run : trash);
        if (trash.isEmpty() || firstAction.left() < option.rect.left())
            return false;
        rememberHotspots(index.row(), option.rect);
        const bool dark = currentThemeIsDark();
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setOpacity(reveal);
        // The title runs the full width of the row until the pointer arrives —
        // reserving this strip on every row would shorten every title for two
        // buttons almost none of them are showing. Fading the row's own
        // background in under the controls clears the few characters they
        // stand on, and only while they are up. The background is the plain
        // one: this list draws its selection as a border, and hovering paints
        // no fill, so there is no other colour a row can be.
        const int washFrom = firstAction.left() - kActionMargin;
        QLinearGradient wash(washFrom - kActionFadePx, 0, washFrom, 0);
        const QColor base =
            m_washOverride.isValid()
                ? m_washOverride
                : Theme::remapColor(QColor(dark ? "#0d1117" : "#ffffff"));
        QColor clear = base;
        clear.setAlpha(0);
        wash.setColorAt(0.0, clear);
        wash.setColorAt(1.0, base);
        painter->fillRect(QRect(washFrom - kActionFadePx, option.rect.top(),
                                kActionFadePx, option.rect.height()),
                          wash);
        painter->fillRect(QRect(washFrom, option.rect.top(),
                                option.rect.right() - washFrom + 1,
                                option.rect.height()),
                          base);
        if (mode != AgentRunToggleNone) {
            const bool stop = mode == AgentRunToggleStop;
            const QColor accent =
                stop ? QColor(dark ? "#f85149" : "#cf222e")
                     : Theme::nextOr(Theme::Role::AccentFg,
                                     QColor(dark ? "#3fb950" : "#1a7f37"));
            const QRectF pad =
                paintActionPad(painter, run, accent, index.row(), ActionRunToggle);
            painter->setPen(Qt::NoPen);
            painter->setBrush(accent);
            const QPointF c = pad.center();
            if (stop) {
                const qreal side = pad.width() * 0.38;
                painter->drawRoundedRect(
                    QRectF(c.x() - side / 2, c.y() - side / 2, side, side), 1.6,
                    1.6);
            } else {
                // Nudged right by the sliver that keeps a triangle looking
                // centred in a circle, which its bounding box alone does not.
                const qreal h = pad.height() * 0.44;
                const qreal w = h * 0.88;
                QPolygonF play;
                play << QPointF(c.x() - w / 2 + 0.8, c.y() - h / 2)
                     << QPointF(c.x() - w / 2 + 0.8, c.y() + h / 2)
                     << QPointF(c.x() + w / 2 + 0.8, c.y());
                painter->drawPolygon(play);
            }
        }
        if (canReprioritize) {
            const QColor priorityAccent = Theme::nextOr(
                Theme::Role::FgSecondary, QColor(dark ? "#58a6ff" : "#0969da"));
            const QRectF topPad = paintActionPad(
                painter, moveTop, priorityAccent, index.row(), ActionMoveToTop);
            const QRectF bottomPad = paintActionPad(
                painter, moveBottom, priorityAccent, index.row(), ActionMoveToBottom);
            const auto paintPriorityIcon = [&](const QString &icon,
                                               const QRectF &pad) {
                themedOcticon(icon, priorityAccent, kActionGlyphPx)
                    .paint(painter,
                           QRect(qRound(pad.center().x()) - kActionGlyphPx / 2,
                                 qRound(pad.center().y()) - kActionGlyphPx / 2,
                                 kActionGlyphPx, kActionGlyphPx));
            };
            paintPriorityIcon(QStringLiteral("chevron-up"), topPad);
            paintPriorityIcon(QStringLiteral("chevron-down"), bottomPad);
        }
        const QColor trashAccent(dark ? "#f85149" : "#cf222e");
        const QRectF pad =
            paintActionPad(painter, trash, trashAccent, index.row(), ActionDelete);
        themedOcticon(QStringLiteral("trash"), trashAccent, kActionGlyphPx)
            .paint(painter, QRect(qRound(pad.center().x()) - kActionGlyphPx / 2,
                                  qRound(pad.center().y()) - kActionGlyphPx / 2,
                                  kActionGlyphPx, kActionGlyphPx));
        painter->restore();
        return true;
    }

    // A filled pad rather than an outlined ring: it reads as one control at
    // this size, and states its intent by tint instead of by a hairline.
    QRectF paintActionPad(QPainter *painter, const QRect &rect,
                          const QColor &accent, int row, int action) const
    {
        const bool dark = currentThemeIsDark();
        const bool pressed = m_pressedRow == row && m_pressedAction == action;
        const bool hot = pressed || (m_hotRow == row && m_hotAction == action);
        const QRectF pad(rect);
        QColor fill = accent;
        fill.setAlpha(pressed ? (dark ? 104 : 88)
                              : hot ? (dark ? 70 : 58) : (dark ? 46 : 40));
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawEllipse(pad);
        if (hot) {
            QColor rim = accent;
            rim.setAlpha(pressed ? 224 : 150);
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(rim, 1.0));
            painter->drawEllipse(pad.adjusted(0.5, 0.5, -0.5, -0.5));
        }
        return pad;
    }

    // --- the reveal ------------------------------------------------------
    //
    // One row fades at a time: whichever row the pointer is on climbs to 1,
    // and the row it left falls back to 0. Both ends are read off the hover
    // state the row is painted with, so a table rebuild or a scroll under a
    // still pointer can never strand a control on a row nobody is over.

    static constexpr int kRevealTickMs = 16;
    static constexpr qreal kRevealStep = 0.2; // 0 -> 1 in ~80 ms

    qreal revealFor(int row) const
    {
        return row >= 0 && row == m_revealRow ? m_reveal : 0.0;
    }

    qreal driveReveal(int row, bool wanted) const
    {
        if (wanted) {
            if (m_revealRow != row) {
                const int leaving = m_revealRow;
                m_revealRow = row;
                m_reveal = 0.0;
                if (leaving >= 0)
                    updateRowBand(leaving);
            }
            m_revealTarget = 1.0;
        } else if (m_revealRow == row) {
            m_revealTarget = 0.0;
        } else {
            return 0.0;
        }
        if (!revealSettled() && !m_revealTimer.isActive())
            m_revealTimer.start();
        return m_reveal;
    }

    bool revealSettled() const
    {
        return qAbs(m_reveal - m_revealTarget) < 0.001;
    }

    void stepReveal()
    {
        const int row = m_revealRow;
        if (row < 0) {
            m_revealTimer.stop();
            return;
        }
        m_reveal = m_revealTarget > m_reveal
                       ? qMin(m_revealTarget, m_reveal + kRevealStep)
                       : qMax(m_revealTarget, m_reveal - kRevealStep);
        if (revealSettled()) {
            m_reveal = m_revealTarget;
            m_revealTimer.stop();
            if (m_revealTarget <= 0.0) {
                m_revealRow = -1;
                clearHotspots(); // no controls left to point at
            }
        }
        updateRowBand(row);
    }

    // The whole row, not just this cell: the row's other columns share the
    // repaint anyway, and the selection border spans them.
    void updateRowBand(int row) const
    {
        if (!m_itemView || !m_itemView->model() || row < 0)
            return;
        QWidget *viewport = m_itemView->viewport();
        const QModelIndex idx = m_itemView->model()->index(row, kAgentIdColumn);
        if (!viewport || !idx.isValid())
            return;
        const QRect cell = m_itemView->visualRect(idx);
        if (cell.isNull())
            return;
        viewport->update(QRect(0, cell.top(), viewport->width(), cell.height()));
    }

    void setPressed(int row, int action)
    {
        if (m_pressedRow == row && m_pressedAction == action)
            return;
        const int was = m_pressedRow;
        m_pressedRow = row;
        m_pressedAction = action;
        updateRowBand(was);
        updateRowBand(row);
    }

    // QTableView only repaints when the hovered *cell* changes, so sliding
    // along one row onto a control would otherwise light it up only on rows
    // that happen to be animating something else. Remember where the controls
    // were drawn and repaint the row the moment the pointer crosses an edge.
    void rememberHotspots(int row, const QRect &cell) const
    {
        m_hotRow = row;
        m_hotCell = cell;
    }

    void clearHotspots() const
    {
        m_hotRow = -1;
        m_hotCell = QRect();
        setHotAction(ActionNone);
    }

    // The row is one big click target that selects the agent, so a control has
    // to say for itself that it does something else.
    void setHotAction(int action) const
    {
        if (action == m_hotAction)
            return;
        m_hotAction = action;
        if (QWidget *viewport = m_itemView ? m_itemView->viewport() : nullptr) {
            if (action != ActionNone)
                viewport->setCursor(Qt::PointingHandCursor);
            else
                viewport->unsetCursor();
        }
    }

    bool eventFilter(QObject *obj, QEvent *event) override
    {
        if (event->type() == QEvent::MouseMove && m_hotRow >= 0 &&
            !m_hotCell.isEmpty()) {
            const auto *me = static_cast<QMouseEvent *>(event);
            const QModelIndex idx =
                m_itemView && m_itemView->model()
                    ? m_itemView->model()->index(m_hotRow, kAgentIssueColumn)
                    : QModelIndex();
            const int action =
                idx.isValid() ? actionAt(m_hotCell, me->pos(), idx) : ActionNone;
            if (action != m_hotAction) {
                setHotAction(action);
                updateRowBand(m_hotRow);
            }
            if (m_pressedRow >= 0 && !(QApplication::mouseButtons() & Qt::LeftButton))
                setPressed(-1, ActionNone);
        }
        if (event->type() == QEvent::Leave)
            clearHotspots();
        return SelectionBorderRowDelegate::eventFilter(obj, event);
    }

    QAbstractItemView *m_itemView = nullptr;
    std::function<void(int)> m_onThumbnailClick;
    std::function<void(int)> m_onRunToggle;
    std::function<void(int)> m_onMoveToTop;
    std::function<void(int)> m_onMoveToBottom;
    std::function<void(int)> m_onDelete;
    mutable QTimer m_revealTimer;
    mutable int m_revealRow = -1;
    mutable qreal m_reveal = 0.0;
    mutable qreal m_revealTarget = 0.0;
    int m_pressedRow = -1;
    int m_pressedAction = ActionNone;
    mutable int m_hotRow = -1;
    mutable QRect m_hotCell;
    mutable QList<int> m_chipColumnWidths;
    mutable QString m_chipColumnsFontKey;
    mutable bool m_chipColumnsStale = true;
    mutable int m_hotAction = ActionNone;
    mutable QColor m_washOverride;
    mutable QTimer m_tickerTimer;
    QElapsedTimer m_tickerClock;
    mutable QHash<int, forkmesh::ui::AgentTickerTrain> m_tickers;
    mutable QHash<int, QRect> m_tickerLanes;
    mutable QString m_tickerFontKey;
};

constexpr int kAgentTitleMaxLines = 3;

class WrappedTitleLabel : public QLabel
{
public:
    explicit WrappedTitleLabel(const QString &text, QWidget *parent = nullptr)
        : QLabel(parent)
    {
        setWordWrap(true);
        setAlignment(Qt::AlignLeft | Qt::AlignTop);
        QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        setTextInteractionFlags(Qt::TextSelectableByMouse);
        setFullText(text);
    }

    void setFullText(const QString &text)
    {
        m_full = text.simplified();
        setToolTip(m_full);
        refreshLines();
    }

    QString fullText() const { return m_full; }

    QSize minimumSizeHint() const override
    {
        QSize s = QLabel::minimumSizeHint();
        s.setWidth(0);
        return s;
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QLabel::resizeEvent(event);
        refreshLines();
    }

    void changeEvent(QEvent *event) override
    {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange)
            refreshLines();
    }

private:
    void refreshLines()
    {
        const int avail = width() - contentsMargins().left() -
                          contentsMargins().right() - 2 * margin();
        QString shown = m_full;
        if (avail > 0) {
            QTextLayout layout(m_full, font());
            QTextOption option = layout.textOption();
            option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
            layout.setTextOption(option);
            layout.beginLayout();
            int lastStart = 0;
            int lines = 0;
            bool more = false;
            for (QTextLine line = layout.createLine(); line.isValid();
                 line = layout.createLine()) {
                if (lines == kAgentTitleMaxLines) {
                    more = true;
                    break;
                }
                line.setLineWidth(avail);
                lastStart = line.textStart();
                ++lines;
            }
            layout.endLayout();
            if (more)
                shown = m_full.left(lastStart) +
                        fontMetrics().elidedText(m_full.mid(lastStart),
                                                 Qt::ElideRight, avail);
        }
        if (text() != shown)
            QLabel::setText(shown);
    }

    QString m_full;
};

inline void setAgentTitleText(QLabel *label, const QString &text)
{
    if (auto *wrapped = dynamic_cast<WrappedTitleLabel *>(label))
        wrapped->setFullText(text);
    else if (label)
        label->setText(text);
}

// Re-caption a rail button, re-widening it the way railActionButton sized it.
inline void setRailActionCaption(ActivityRailButton *button, const QString &icon,
                          const QString &caption)
{
    if (!button || button->text() == caption)
        return; // the queue readout refreshes on every action-state pass
    QFont f = QGuiApplication::font();
    f.setPixelSize(10);
    f.setWeight(QFont::DemiBold);
    button->setFixedSize(
        qMax(railItemWidth(), QFontMetrics(f).horizontalAdvance(caption) + 12),
        kRailItemHeight);
    button->setGlyph(icon, caption);
}

// Re-caption an agent detail action, whatever kind of button it is. The action
// row is built from ActivityRailButtons, which paint their own caption: plain
// setText() leaves both the drawn label and the width railActionButton fixed for
// the original caption stale, so a numbered caption never reaches the screen.
inline void setAgentActionCaption(QPushButton *button, const QString &icon,
                           const QString &caption)
{
    if (auto *rail = dynamic_cast<ActivityRailButton *>(button))
        setRailActionCaption(rail, icon, caption);
    else if (button)
        button->setText(caption);
}

// Which of this provider's accounts new agents run as, named the way the
// account menu numbers them: "Claude 1", "Claude 2", "Codex 1", …
inline QString agentAccountSlotLabel(const QString &provider)
{
    const QString base = agentIsCodexProvider(provider)
                             ? QStringLiteral("Codex")
                             : QStringLiteral("Claude");
    const QList<AgentAccountProfile> profiles = agentAccountProfiles(provider);
    const QString activeId = activeAgentAccount(provider).id;
    for (int i = 0; i < profiles.size(); ++i) {
        if (profiles.at(i).id == activeId)
            return QStringLiteral("%1 %2").arg(base).arg(i + 1);
    }
    return base;
}

inline QString usageExhaustedSetting(const QString &providerKey,
                              const QString &windowKey)
{
    if (providerKey == QLatin1String("claude")) {
        if (windowKey == QLatin1String("5h"))
            return kClaudeUsage5hExhaustedSetting;
        if (windowKey == QLatin1String("weekly"))
            return kClaudeUsageWeekExhaustedSetting;
        if (windowKey == QLatin1String("fable"))
            return kClaudeUsageFableExhaustedSetting;
    } else if (providerKey == QLatin1String("codex")) {
        if (windowKey == QLatin1String("5h"))
            return kCodexUsage5hExhaustedSetting;
        if (windowKey == QLatin1String("weekly"))
            return kCodexUsageWeekExhaustedSetting;
    }
    return QString();
}

inline QString usageResetSetting(const QString &providerKey, const QString &windowKey)
{
    if (providerKey == QLatin1String("claude")) {
        if (windowKey == QLatin1String("5h"))
            return kClaudeUsage5hResetSetting;
        if (windowKey == QLatin1String("weekly"))
            return kClaudeUsageWeekResetSetting;
        if (windowKey == QLatin1String("fable"))
            return kClaudeUsageFableResetSetting;
    } else if (providerKey == QLatin1String("codex")) {
        if (windowKey == QLatin1String("5h"))
            return kCodexUsage5hResetSetting;
        if (windowKey == QLatin1String("weekly"))
            return kCodexUsageWeekResetSetting;
    }
    return QString();
}

// A concise, safe-to-display snapshot of the processes still owned by an
// agent. SystemStats intentionally supplies only kernel command names rather
// than full argument strings: a command line can contain prompt text, paths or
// credentials, while the command and PID are enough to identify a straggler.
inline QStringList agentSubprocessLabels(
    const QList<SystemStats::DescendantProcess> &processes)
{
    QStringList labels;
    constexpr int kShownProcesses = 4;
    for (int i = 0; i < processes.size() && i < kShownProcesses; ++i) {
        const SystemStats::DescendantProcess &process = processes.at(i);
        labels << QStringLiteral("%1 (PID %2)")
                      .arg(process.command.isEmpty()
                               ? QStringLiteral("process")
                               : process.command)
                      .arg(process.pid);
    }
    if (processes.size() > kShownProcesses)
        labels << QStringLiteral("+%1 more").arg(processes.size() - kShownProcesses);
    return labels;
}

} // namespace
